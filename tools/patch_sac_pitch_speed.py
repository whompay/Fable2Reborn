#!/usr/bin/env python3
"""Fast, smooth vertical look for StringAndCageCamera (2026-11-15, after patch_sac_free_pitch.py).
Instruction-only (size unchanged):
 1. main [26]  HeightAdjustSpeed = K22 (0.05) -> K133 (0.4)   x8 = fable2_cam_rate_scale
 2. main [18]  HeightDamp        = K14 (1.5)  -> K29 (0.0)    no height smoothing lag
 3. proto113 [11] LT 0.1 < y  -> 0 < y                         no 0.1 dead zone on pitch
    proto113 [13] LT y < -0.1 -> y < 0
usage: patch_sac_pitch_speed.py in.lua out.lua
"""
import sys, os, inspect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lua51_disasm as L
src = inspect.getsource(L.parse_function).replace(
    "    sizecode = r.uint32()\n", "    sizecode = r.uint32()\n    f['code_off'] = r.pos\n")
exec(src, L.__dict__)
def enc(op, a, b, c): return L.OPNAMES.index(op) | (a << 6) | (c << 14) | (b << 23)
data = bytearray(open(sys.argv[1], 'rb').read())
hdr, main = L.parse(bytes(data))
def check(f, pc, e):
    g = L.decode_instr(f['code'][pc])[:4]; assert g == e, (pc, g, e)
def put(f, pc, w):
    o = f['code_off'] + 4 * pc; data[o:o+4] = w.to_bytes(4, 'little')
K = main['constants']
assert K[21] == 'HeightAdjustSpeed' and K[15] == 'HeightDamp'
assert abs(K[133] - 0.4) < 1e-6 and K[29] == 0.0
check(main, 26, ('SETTABLE', 0, 256+21, 256+22)); put(main, 26, enc('SETTABLE', 0, 256+21, 256+133))
check(main, 18, ('SETTABLE', 0, 256+15, 256+14)); put(main, 18, enc('SETTABLE', 0, 256+15, 256+29))
p = main['protos'][113]; Kp = p['constants']
assert Kp[1] == 'GAME_ACTION_ZOOM_CAMERA' and Kp[35] == 0.0
check(p, 11, ('LT', 1, 256+4, 3)); put(p, 11, enc('LT', 1, 256+35, 3))
check(p, 13, ('LT', 0, 3, 256+5)); put(p, 13, enc('LT', 0, 3, 256+35))
open(sys.argv[2], 'wb').write(data); print("patched OK", len(data))
