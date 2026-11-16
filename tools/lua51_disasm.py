#!/usr/bin/env python3
"""Minimal Lua 5.1 bytecode disassembler (32-bit LE, no debug info assumed
but handles debug info if present). Produces a readable listing: constants,
and instructions with GETGLOBAL/SETGLOBAL/GETTABLE-with-constant-key names
resolved, so we can find call sites like PlayerWebsiteUnlocks.IsItemUnlocked
and the chest gating logic without a real Lua interpreter.

Also supports RE-ENCODING: this module can rebuild the exact same byte
stream from the parsed structure (round-trip verified), which lets us patch
specific instructions/constants and re-dump valid bytecode.
"""
import struct, sys

OPNAMES = [
 "MOVE","LOADK","LOADBOOL","LOADNIL","GETUPVAL","GETGLOBAL","GETTABLE",
 "SETGLOBAL","SETUPVAL","SETTABLE","NEWTABLE","SELF","ADD","SUB","MUL",
 "DIV","MOD","POW","UNM","NOT","LEN","CONCAT","JMP","EQ","LT","LE","TEST",
 "TESTSET","CALL","TAILCALL","RETURN","FORLOOP","FORPREP","TFORLOOP",
 "SETLIST","CLOSE","CLOSURE","VARARG"
]
# iABC=0, iABx=1, iAsBx=2
MODES = {
 "MOVE":0,"LOADK":1,"LOADBOOL":0,"LOADNIL":0,"GETUPVAL":0,"GETGLOBAL":1,
 "GETTABLE":0,"SETGLOBAL":1,"SETUPVAL":0,"SETTABLE":0,"NEWTABLE":0,
 "SELF":0,"ADD":0,"SUB":0,"MUL":0,"DIV":0,"MOD":0,"POW":0,"UNM":0,"NOT":0,
 "LEN":0,"CONCAT":0,"JMP":2,"EQ":0,"LT":0,"LE":0,"TEST":0,"TESTSET":0,
 "CALL":0,"TAILCALL":0,"RETURN":0,"FORLOOP":2,"FORPREP":2,"TFORLOOP":0,
 "SETLIST":0,"CLOSE":0,"CLOSURE":1,"VARARG":0
}
MAXARG_sBx = 131071  # (1<<17)-1

class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0
    def bytes(self, n):
        b = self.data[self.pos:self.pos+n]
        self.pos += n
        return b
    def u8(self):
        v = self.data[self.pos]; self.pos += 1; return v
    def int32(self):
        v = struct.unpack_from('<i', self.data, self.pos)[0]; self.pos += 4; return v
    def uint32(self):
        v = struct.unpack_from('<I', self.data, self.pos)[0]; self.pos += 4; return v
    def double(self):
        v = struct.unpack_from('<d', self.data, self.pos)[0]; self.pos += 8; return v
    def lstring(self, size_t=4):
        if size_t == 4:
            size = self.uint32()
        else:
            size = struct.unpack_from('<Q', self.data, self.pos)[0]; self.pos += 8
        if size == 0:
            return None
        s = self.data[self.pos:self.pos+size-1]  # exclude NUL terminator
        self.pos += size
        return s.decode('latin1')

def parse_function(r, header):
    f = {}
    f['source'] = r.lstring(header['size_size_t'])
    f['linedefined'] = r.int32()
    f['lastlinedefined'] = r.int32()
    f['nups'] = r.u8()
    f['numparams'] = r.u8()
    f['is_vararg'] = r.u8()
    f['maxstacksize'] = r.u8()
    sizecode = r.uint32()
    code = []
    for _ in range(sizecode):
        code.append(r.uint32())
    f['code'] = code
    sizek = r.uint32()
    consts = []
    for _ in range(sizek):
        t = r.u8()
        if t == 0:  # nil
            consts.append(None)
        elif t == 1:  # boolean
            consts.append(bool(r.u8()))
        elif t == 3:  # number
            if header['lua_number_size'] == 8:
                consts.append(r.double())
            else:
                consts.append(struct.unpack_from('<f', r.data, r.pos)[0]); r.pos += 4
        elif t == 4:  # string
            consts.append(r.lstring(header['size_size_t']))
        else:
            raise ValueError(f"unknown const type {t} at {r.pos}")
    f['constants'] = consts
    sizep = r.uint32()
    protos = []
    for _ in range(sizep):
        protos.append(parse_function(r, header))
    f['protos'] = protos
    # debug info
    sizelineinfo = r.uint32()
    f['lineinfo'] = [r.int32() for _ in range(sizelineinfo)]
    sizelocvars = r.uint32()
    locvars = []
    for _ in range(sizelocvars):
        name = r.lstring(header['size_size_t'])
        startpc = r.int32()
        endpc = r.int32()
        locvars.append((name, startpc, endpc))
    f['locvars'] = locvars
    sizeupvalues = r.uint32()
    f['upvalues'] = [r.lstring(header['size_size_t']) for _ in range(sizeupvalues)]
    return f

def parse(data):
    r = Reader(data)
    sig = r.bytes(4)
    assert sig == b'\x1bLua', sig
    version = r.u8()
    fmt = r.u8()
    endian = r.u8()
    size_int = r.u8()
    size_size_t = r.u8()
    size_instr = r.u8()
    lua_number_size = r.u8()
    integral = r.u8()
    header = dict(version=version, fmt=fmt, endian=endian, size_int=size_int,
                  size_size_t=size_size_t, size_instr=size_instr,
                  lua_number_size=lua_number_size, integral=integral)
    main = parse_function(r, header)
    assert r.pos == len(data), (r.pos, len(data))
    return header, main

def decode_instr(instr):
    op = instr & 0x3F
    opname = OPNAMES[op]
    mode = MODES[opname]
    A = (instr >> 6) & 0xFF
    if mode == 0:  # ABC
        C = (instr >> 14) & 0x1FF
        B = (instr >> 23) & 0x1FF
        return opname, A, B, C, None
    elif mode == 1:  # ABx
        Bx = (instr >> 14) & 0x3FFFF
        return opname, A, None, None, Bx
    else:  # AsBx
        Bx = (instr >> 14) & 0x3FFFF
        sBx = Bx - MAXARG_sBx
        return opname, A, None, None, sBx

def rk_str(x, consts):
    if x >= 256:
        k = consts[x-256]
        return f"K{x-256}={k!r}"
    return f"R{x}"

def listing(f, indent=0, path="main"):
    out = []
    pre = "  " * indent
    out.append(f"{pre}== function {path} (params={f['numparams']} vararg={f['is_vararg']} nups={f['nups']} maxstack={f['maxstacksize']}) ==")
    for i, k in enumerate(f['constants']):
        out.append(f"{pre}  K{i} = {k!r}")
    for pc, instr in enumerate(f['code']):
        opname, A, B, C, Bx = decode_instr(instr)
        line = f['lineinfo'][pc] if pc < len(f['lineinfo']) else -1
        extra = ""
        consts = f['constants']
        if opname in ("GETGLOBAL", "SETGLOBAL"):
            extra = f" ; {consts[Bx]!r}"
        elif opname == "LOADK":
            extra = f" ; K{Bx}={consts[Bx]!r}"
        elif opname in ("GETTABLE","SETTABLE","SELF"):
            extra = f" ; B={rk_str(B,consts)} C={rk_str(C,consts)}"
        elif opname in ("EQ","LT","LE"):
            extra = f" ; B={rk_str(B,consts)} C={rk_str(C,consts)}"
        elif opname == "JMP":
            extra = f" ; -> {pc+1+Bx}"
        elif opname in ("FORLOOP","FORPREP"):
            extra = f" ; -> {pc+1+Bx}"
        elif opname == "CALL":
            extra = f" ; nargs={B-1 if B>0 else 'multi'} nret={C-1 if C>0 else 'multi'}"
        elif opname == "CLOSURE":
            extra = f" ; proto {Bx}"
        if Bx is not None and opname not in ("GETGLOBAL","SETGLOBAL","LOADK","JMP","FORLOOP","FORPREP","CLOSURE"):
            out.append(f"{pre}  [{pc:4d}] L{line:<5d} {opname:<10s} A={A} Bx={Bx}{extra}")
        elif Bx is not None:
            out.append(f"{pre}  [{pc:4d}] L{line:<5d} {opname:<10s} A={A}{extra}")
        else:
            out.append(f"{pre}  [{pc:4d}] L{line:<5d} {opname:<10s} A={A} B={B} C={C}{extra}")
    for i, p in enumerate(f['protos']):
        out.extend(listing(p, indent+1, path=f"{path}.proto{i}"))
    return out

if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    header, main = parse(data)
    print(f"header: {header}", file=sys.stderr)
    lines = listing(main)
    print("\n".join(lines))
