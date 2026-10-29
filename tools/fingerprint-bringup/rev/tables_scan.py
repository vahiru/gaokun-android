#!/usr/bin/env python3
"""Broad xref scan: ADRP + any load/store/ADD touching a VA range; plus string reads.

fingerpr.mbn: file = VA + 0x3000.
"""
import struct
import sys

MBN = "/Users/vahiru/Projects/gaokun-android/tools/fingerprint-bringup/rev/cabs/qctree/fingerpr.mbn"
CODE_VA_LO, CODE_VA_HI = 0x0, 0x226000

with open(MBN, "rb") as f:
    data = f.read()


def va2off(va):
    return va + 0x3000


def read_va(va, n):
    return data[va2off(va):va2off(va) + n]


def cstr(va, maxlen=120):
    off = va2off(va)
    end = data.find(b"\x00", off, off + maxlen)
    if end < 0:
        return None
    return data[off:end].decode("ascii", "replace")


# ---- strings around the 0x403 metadata ----
print("=== strings near metadata records ===")
for va in (0x1E7487, 0x1E74A0, 0x1E74B9, 0x1E774F, 0x1E7770, 0x1E7786):
    print(f"  {va:#x}: {cstr(va)!r}")

# also dump g_request_type_table entries (14 u64 ptrs)
print("\n=== g_request_type_table @0x226478 (14 ptrs) ===")
for i in range(14):
    p = struct.unpack_from("<Q", read_va(0x226478 + i * 8, 8))[0]
    print(f"  [{i:2d}] {p:#x}  {cstr(p)!r}")

print("\n=== g_session_result_table @0x2265a8 (5 ptrs) ===")
for i in range(5):
    p = struct.unpack_from("<Q", read_va(0x2265a8 + i * 8, 8))[0]
    print(f"  [{i}] {p:#x}  {cstr(p)!r}")

print("\n=== g_algo_operation_table @0x226440 (7 ptrs) ===")
for i in range(7):
    p = struct.unpack_from("<Q", read_va(0x226440 + i * 8, 8))[0]
    print(f"  [{i}] {p:#x}  {cstr(p)!r}")

print("\n=== g_cmd_to_device_table @0x226540 (13 ptrs) ===")
for i in range(13):
    p = struct.unpack_from("<Q", read_va(0x226540 + i * 8, 8))[0]
    print(f"  [{i:2d}] {p:#x}  {cstr(p)!r}")

# ---- ADRP + memop scan ----
# targets: the tables themselves and the pointer-list slots
ranges = {
    "g_request_type_table": (0x226478, 0x226478 + 0x70),
    "g_session_result_table": (0x2265A8, 0x2265A8 + 0x28),
    "g_algo_operation_table": (0x226440, 0x226440 + 0x38),
    "g_cmd_to_device_table": (0x226540, 0x226540 + 0x68),
    "ptrlist@0x361248": (0x361248, 0x3612D0),
}

code = read_va(CODE_VA_LO, CODE_VA_HI - CODE_VA_LO)
n = len(code) // 4

print("\n=== ADRP+memop/ADD refs into table ranges ===")
for name, (lo, hi) in ranges.items():
    hits = []
    for i in range(n - 1):
        w0 = struct.unpack_from("<I", code, i * 4)[0]
        if (w0 & 0x9F000000) != 0x90000000:  # ADRP
            continue
        rd = w0 & 0x1F
        immlo = (w0 >> 29) & 3
        immhi = (w0 >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & (1 << 20):
            imm -= 1 << 21
        pc = CODE_VA_LO + i * 4
        page = (pc & ~0xFFF) + (imm << 12)
        w1 = struct.unpack_from("<I", code, i * 4 + 4)[0]
        ea = None
        kind = None
        # ADD imm (64): 10010001
        if (w1 & 0xFF800000) == 0x91000000 and ((w1 >> 5) & 0x1F) == rd:
            imm12 = (w1 >> 10) & 0xFFF
            if (w1 >> 22) & 1:
                imm12 <<= 12
            ea = page + imm12
            kind = "ADD"
        # LDR/STR unsigned imm 64-bit: 1x111000/1x111001 with size=11
        elif (w1 & 0x3B000000) == 0x39000000 and ((w1 >> 5) & 0x1F) == rd:
            imm12 = (w1 >> 10) & 0xFFF
            if (w1 >> 30) & 3 == 3:  # 64-bit
                ea = page + imm12 * 8
            else:
                ea = page + imm12 * 4
            kind = "LDR/STR" if ((w1 >> 22) & 1) == 0 else "LDR/STR?"  # rough
        if ea is not None and lo <= ea < hi:
            hits.append((pc, ea, kind))
    print(f"{name}: {len(hits)} hits")
    for pc, ea, kind in hits[:30]:
        print(f"    {kind} ea={ea:#x} @ VA {pc:#x}")
