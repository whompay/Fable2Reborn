#!/usr/bin/env python3
"""Patch the 8 inner JMP instructions in main.proto14 of generictriggers'
compiled bytecode (the Ancestors' Chest reward-granting function) so the
PlayerWebsiteUnlocks.IsItemUnlocked() result no longer gates whether each
of the 8 website items is added to the chest. The IsItemUnlocked() call
itself is left completely intact (still executes, its result is just no
longer read) -- narrowest possible patch: only the JMP target field of 8
existing instructions changes, nothing else in the file moves or resizes.

Each targeted JMP directly follows a `TEST A=1 B=0 C=0` that tests the
CALL result of IsItemUnlocked() and, on a falsy result, jumps forward
6 instructions to skip the grant block. We retarget each JMP's sBx to 0
(jump to pc+1, i.e. straight into the grant block that already immediately
follows), so the grant block runs unconditionally once the item's
Gameflow.On lineX flag is 'NotGiven' -- the SAME one-time flag gate that
was already there and is completely untouched, so rewards still only
grant once and existing saves still work normally.
"""
import struct, sys, hashlib

sys.path.insert(0, '.')
from lua51_disasm import Reader, decode_instr, OPNAMES

# JMP pcs (0-indexed) inside main.proto14 whose target must become pc+1.
# See generictriggers.disasm.txt lines around pc 74-213 for the evidence:
# each is the JMP immediately after the `TEST A=1 B=0 C=0` that follows a
# PlayerWebsiteUnlocked.IsItemUnlocked(...) CALL.
TARGET_PCS = {
    81:  ("MascotHat",   88),
    99:  ("MascotCoat",  106),
    117: ("MascotBoots", 124),
    135: ("Book",        142),
    153: ("Doll",        160),
    171: ("Dye",         178),
    189: ("FaceTattoo",  196),
    207: ("TorsoTattoo", 214),
}

MAXARG_sBx = 131071

def find_proto14_code_offset(data):
    """Re-parse the file, but track the byte offset of each function's code
    array (right after its 4-byte sizecode field), and descend into
    main.protos[14] specifically."""
    r = Reader(data)
    sig = r.bytes(4)
    assert sig == b'\x1bLua'
    version = r.u8(); fmt = r.u8(); endian = r.u8()
    size_int = r.u8(); size_size_t = r.u8(); size_instr = r.u8()
    lua_number_size = r.u8(); integral = r.u8()
    header = dict(size_size_t=size_size_t, lua_number_size=lua_number_size)

    def parse_function(track_path, want_path):
        f = {}
        f['source'] = r.lstring(header['size_size_t'])
        f['linedefined'] = r.int32()
        f['lastlinedefined'] = r.int32()
        f['nups'] = r.u8(); f['numparams'] = r.u8()
        f['is_vararg'] = r.u8(); f['maxstacksize'] = r.u8()
        sizecode = r.uint32()
        code_offset = r.pos
        code = []
        for _ in range(sizecode):
            code.append(r.uint32())
        f['code'] = code
        found_offset = code_offset if track_path == want_path else None
        sizek = r.uint32()
        for _ in range(sizek):
            t = r.u8()
            if t == 0: pass
            elif t == 1: r.u8()
            elif t == 3:
                if header['lua_number_size'] == 8: r.double()
                else: r.pos += 4
            elif t == 4: r.lstring(header['size_size_t'])
            else: raise ValueError(f"unknown const type {t}")
        sizep = r.uint32()
        for i in range(sizep):
            sub_offset = parse_function(track_path + [i], want_path)
            if sub_offset is not None:
                found_offset = sub_offset
        sizelineinfo = r.uint32()
        for _ in range(sizelineinfo): r.int32()
        sizelocvars = r.uint32()
        for _ in range(sizelocvars):
            r.lstring(header['size_size_t']); r.int32(); r.int32()
        sizeupvalues = r.uint32()
        for _ in range(sizeupvalues): r.lstring(header['size_size_t'])
        return found_offset

    offset = parse_function([], [14])
    assert r.pos == len(data), (r.pos, len(data))
    return offset

def main():
    src = sys.argv[1]
    dst = sys.argv[2]
    data = bytearray(open(src, 'rb').read())
    orig_sha = hashlib.sha256(data).hexdigest()

    code_offset = find_proto14_code_offset(bytes(data))
    assert code_offset is not None, "could not locate main.proto14 code array"
    print(f"main.proto14 code array starts at byte offset {code_offset}", file=sys.stderr)

    for pc, (label, old_target) in sorted(TARGET_PCS.items()):
        word_off = code_offset + pc * 4
        word = struct.unpack_from('<I', data, word_off)[0]
        opname, A, B, C, sBx = decode_instr(word)
        assert opname == "JMP", f"pc={pc} expected JMP, got {opname} (word={word:#010x})"
        # decode_instr already returns the SIGNED sBx for AsBx-mode ops (JMP included)
        # -- do not subtract MAXARG_sBx again here.
        actual_target = pc + 1 + sBx
        assert actual_target == old_target, f"pc={pc} {label}: expected target {old_target}, found {actual_target}"
        assert A == 0, f"pc={pc} {label}: unexpected A={A} (expected 0, leaving untouched otherwise)"
        # Rebuild instruction with Bx = MAXARG_sBx (sBx=0), same opcode(22)/A(0).
        new_word = (22) | (A << 6) | (MAXARG_sBx << 14)
        new_opname, new_A, _, _, new_sBx = decode_instr(new_word)
        new_target = pc + 1 + new_sBx
        assert new_opname == "JMP" and new_A == 0 and new_target == pc + 1, "re-encode sanity check failed"
        struct.pack_into('<I', data, word_off, new_word)
        print(f"patched pc={pc:4d} ({label:<11s}) JMP target {old_target} -> {new_target}  "
              f"(word {word:#010x} -> {new_word:#010x})", file=sys.stderr)

    open(dst, 'wb').write(data)
    new_sha = hashlib.sha256(data).hexdigest()
    print(f"orig sha256={orig_sha}", file=sys.stderr)
    print(f"new  sha256={new_sha}", file=sys.stderr)
    print(f"orig size={len(open(src,'rb').read())} new size={len(data)} (must match -- no resize)", file=sys.stderr)

if __name__ == '__main__':
    main()
