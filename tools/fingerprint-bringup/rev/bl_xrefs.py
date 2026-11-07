#!/usr/bin/env python3
"""Find BL/B call sites to a target VA in fingerpr.mbn (file = VA + 0x3000).

Usage: bl_xrefs.py <mbn> <target-va>
"""
import struct
import sys

BASE = 0x3000
CODE_VA_LO, CODE_VA_HI = 0x0, 0x226000

path = sys.argv[1]
target = int(sys.argv[2], 0)
with open(path, "rb") as f:
    data = f.read()

code = data[CODE_VA_LO + BASE:CODE_VA_HI + BASE]
n = len(code) // 4
hits = []
for i in range(n):
    w = struct.unpack_from("<I", code, i * 4)[0]
    if (w & 0xFC000000) not in (0x94000000, 0x14000000):  # BL / B
        continue
    imm26 = w & 0x3FFFFFF
    if imm26 & (1 << 25):
        imm26 -= 1 << 26
    pc = CODE_VA_LO + i * 4
    dest = pc + imm26 * 4
    if dest == target:
        hits.append((pc, "BL" if (w >> 31) else "B"))
print(f"{len(hits)} call sites -> {target:#x}")
for pc, kind in hits:
    print(f"  {kind} @ {pc:#x}")
