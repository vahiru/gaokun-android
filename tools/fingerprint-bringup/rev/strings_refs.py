#!/usr/bin/env python3
"""Extract ADRP+ADD string refs and compare immediates in a code region.

Usage: strings_refs.py <mbn> <va-start> <va-end>
"""
import struct
import sys

BASE = 0x3000

path, s, e = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
with open(path, "rb") as f:
    data = f.read()


def cstr(va, maxlen=96):
    off = va + BASE
    if off >= len(data):
        return None
    end = data.find(b"\x00", off, off + maxlen)
    if end < 0:
        return None
    return data[off:end].decode("ascii", "replace")


code = data[s + BASE:e + BASE]
n = len(code) // 4
regs = {}
for i in range(n):
    pc = s + i * 4
    w = struct.unpack_from("<I", code, i * 4)[0]
    # ADRP
    if (w & 0x9F000000) == 0x90000000:
        rd = w & 0x1F
        immlo = (w >> 29) & 3
        immhi = (w >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & (1 << 20):
            imm -= 1 << 21
        regs[rd] = ("page", (pc & ~0xFFF) + (imm << 12))
        continue
    # ADD imm 64/32
    if (w & 0xFF800000) in (0x91000000, 0x11000000):
        rd = w & 0x1F
        rn = (w >> 5) & 0x1F
        imm12 = (w >> 10) & 0xFFF
        if (w >> 22) & 1:
            imm12 <<= 12
        if rn in regs and regs[rn][0] == "page":
            regs[rd] = ("val", regs[rn][1] + imm12)
        else:
            regs.pop(rd, None)
        continue
    # SUB imm (for cmp-style) — skip
    # MOVZ/MOVN
    if (w & 0xFF800000) in (0x52800000, 0x72800000):
        rd = w & 0x1F
        imm16 = (w >> 5) & 0xFFFF
        hw = (w >> 21) & 3
        regs[rd] = ("imm", imm16 << (hw * 16))
        continue
    # CMP imm = SUBS wzr
    if (w & 0xFF800000) == 0x71000000 and (w & 0x1F) == 0x1F:
        rn = (w >> 5) & 0x1F
        imm12 = (w >> 10) & 0xFFF
        if (w >> 22) & 1:
            imm12 <<= 12
        src = regs.get(rn)
        note = f"cmp {src}" if src else ""
        print(f"  {pc:#x}: CMP w{rn}, #{imm12}  {note}")
        continue
    # print resolved string loads
    if (w & 0xFFE0FC00) == 0x91000000:  # ADD Xd, Xn, #imm (already handled)
        pass

# second pass: ADD resolving to string
regs = {}
print("=== ADRP+ADD -> string ===")
for i in range(n):
    pc = s + i * 4
    w = struct.unpack_from("<I", code, i * 4)[0]
    if (w & 0x9F000000) == 0x90000000:
        rd = w & 0x1F
        immlo = (w >> 29) & 3
        immhi = (w >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & (1 << 20):
            imm -= 1 << 21
        regs[rd] = (pc & ~0xFFF) + (imm << 12)
        continue
    if (w & 0xFF800000) in (0x91000000, 0x11000000):
        rd = w & 0x1F
        rn = (w >> 5) & 0x1F
        imm12 = (w >> 10) & 0xFFF
        if (w >> 22) & 1:
            imm12 <<= 12
        if rn in regs:
            va = regs[rn] + imm12
            st = cstr(va)
            if st and all(32 <= ord(c) < 127 for c in st) and len(st) >= 3:
                print(f"  {pc:#x}: x{rd} = {va:#x}  \"{st}\"")
        continue
