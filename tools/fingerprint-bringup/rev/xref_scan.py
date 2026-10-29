#!/usr/bin/env python3
"""Scan ARM64 code for ADRP+ADD references to given data VAs, and raw pointer constants.

fingerpr.mbn: file = VA + 0x3000 (all PH).
Usage: xref_scan.py <mbn> <va> [va...]
"""
import struct
import sys

MBN = "/Users/vahiru/Projects/gaokun-android/tools/fingerprint-bringup/rev/cabs/qctree/fingerpr.mbn"
# RX code: VA 0..0x226000  (file 0x3000..0x229000)
CODE_VA_LO, CODE_VA_HI = 0x0, 0x226000


def va2off(va):
    return va + 0x3000


def main():
    targets = [int(a, 16) for a in sys.argv[2:]] or [0x226478, 0x2265a8]
    with open(sys.argv[1] if len(sys.argv) > 1 else MBN, "rb") as f:
        data = f.read()

    # 1) raw pointer constants anywhere in file
    print("=== raw u64 pointer constants ===")
    for t in targets:
        pat = struct.pack("<Q", t)
        idx = 0
        while True:
            i = data.find(pat, idx)
            if i < 0:
                break
            print(f"  VA target {t:#x} found at file {i:#x} (VA {i - 0x3000:#x})")
            idx = i + 1
        # also u32 truncated (low 32 bits in a pointer-sized slot unlikely, but check)
        pat4 = struct.pack("<I", t & 0xFFFFFFFF)
        idx = 0
        cnt = 0
        while cnt < 20:
            i = data.find(pat4, idx)
            if i < 0:
                break
            # skip if already covered by u64 hit (aligned context)
            if data[i:i + 8] != pat:
                print(f"  u32 low-half {t & 0xFFFFFFFF:#x} at file {i:#x} (VA {i - 0x3000:#x})")
                cnt += 1
            idx = i + 1

    # 2) ADRP + ADD pairs in code
    print("=== ADRP+ADD code references ===")
    code_off = va2off(CODE_VA_LO)
    code = data[code_off:va2off(CODE_VA_HI)]
    n = len(code) // 4
    for t in targets:
        hits = []
        for i in range(n - 1):
            w0 = struct.unpack_from("<I", code, i * 4)[0]
            w1 = struct.unpack_from("<I", code, i * 4 + 4)[0]
            # ADRP: op 1xx10000
            if (w0 & 0x9F000000) != 0x90000000:
                continue
            rd = w0 & 0x1F
            immlo = (w0 >> 29) & 3
            immhi = (w0 >> 5) & 0x7FFFF
            imm = (immhi << 2) | immlo
            if imm & (1 << 20):
                imm -= 1 << 21
            pc = CODE_VA_LO + i * 4
            page = (pc & ~0xFFF) + (imm << 12)
            # ADD imm 64-bit: 10010001 sh imm12 Rn Rd
            if (w1 & 0xFF800000) != 0x91000000:
                continue
            rn = (w1 >> 5) & 0x1F
            rd2 = w1 & 0x1F
            if rn != rd or rd2 != rd:
                continue
            imm12 = (w1 >> 10) & 0xFFF
            sh = (w1 >> 22) & 1
            if sh:
                imm12 <<= 12
            addr = page + imm12
            if addr == t:
                hits.append(pc)
        print(f"  target {t:#x}: {len(hits)} hits")
        for h in hits[:40]:
            print(f"    ADRP+ADD at VA {h:#x}")

    # 3) LDR literal (PC-relative 64-bit loads) pointing at targets
    print("=== LDR literal (64-bit) code references ===")
    for t in targets:
        hits = []
        for i in range(n):
            w0 = struct.unpack_from("<I", code, i * 4)[0]
            # LDR Xt literal: 01 011 V 00 imm19 Rt  (64-bit: V=0, opc=01)
            if (w0 & 0xFF000000) != 0x58000000:
                continue
            imm19 = (w0 >> 5) & 0x7FFFF
            if imm19 & (1 << 18):
                imm19 -= 1 << 19
            pc = CODE_VA_LO + i * 4
            addr = pc + imm19 * 4
            if addr == t:
                hits.append(pc)
        print(f"  target {t:#x}: {len(hits)} hits")
        for h in hits[:40]:
            print(f"    LDR literal at VA {h:#x}")


if __name__ == "__main__":
    main()
