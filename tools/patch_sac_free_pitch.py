#!/usr/bin/env python3
"""Free vertical mouse look for the 3rd-person StringAndCageCamera (2026-11-15).

Instruction-only patch (bytecode size unchanged) of scripts\\camera\\stringandcagecamera.lua:
 1. main  [28] HeightOffsetUpperBound = K24 (0.7)  -> K14 (1.5)   ~56 deg looking down
 2. main  [30] HeightOffsetLowerBound = K24 (0.7)  -> K44 (1.0)   ~45 deg looking up
 3. proto70 [149] LOADK R14, K6 (0.0)  -> GETTABLE R14, R0, 'ManualHeightOffset'
    i.e. GetRollingAverage(M, 0.0, 20) becomes GetRollingAverage(M, M, 20) = M:
    the manual camera height no longer springs back to centre.
usage: patch_sac_free_pitch.py in.lua out.lua
"""
import sys, os, inspect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lua51_disasm as L

src = inspect.getsource(L.parse_function).replace(
    "    sizecode = r.uint32()\n", "    sizecode = r.uint32()\n    f['code_off'] = r.pos\n")
exec(src, L.__dict__)

def enc_abc(op, a, b, c):
    return L.OPNAMES.index(op) | (a << 6) | (c << 14) | (b << 23)

data = bytearray(open(sys.argv[1], 'rb').read())
hdr, main = L.parse(bytes(data))
p70 = main['protos'][70]

def check(f, pc, expect):
    got = L.decode_instr(f['code'][pc])[:4]
    assert got == expect, (pc, got, expect)

def put(f, pc, word):
    off = f['code_off'] + 4 * pc
    data[off:off+4] = word.to_bytes(4, 'little')

K = main['constants']
assert K[23] == 'HeightOffsetUpperBound' and K[25] == 'HeightOffsetLowerBound'
assert abs(K[14] - 1.5) < 1e-6 and abs(K[44] - 1.0) < 1e-6
check(main, 28, ('SETTABLE', 0, 256+23, 256+24))
check(main, 30, ('SETTABLE', 0, 256+25, 256+24))
put(main, 28, enc_abc('SETTABLE', 0, 256+23, 256+14))
put(main, 30, enc_abc('SETTABLE', 0, 256+25, 256+44))

assert p70['constants'][27] == 'ManualHeightOffset' and p70['constants'][33] == 20.0
check(p70, 149, ('LOADK', 14, None, None))
assert L.decode_instr(p70['code'][149])[4] == 6
check(p70, 148, ('GETTABLE', 13, 0, 256+27))
put(p70, 149, enc_abc('GETTABLE', 14, 0, 256+27))

open(sys.argv[2], 'wb').write(data)
print("patched OK", len(data))
