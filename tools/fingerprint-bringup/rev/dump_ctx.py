#!/usr/bin/env python3
"""Dump context around pointer-constant hits, annotate with ELF symbol names.

fingerpr.mbn: file = VA + 0x3000.
SYMTAB VA 0x3677d0 (2026 entries × 24B), STRTAB VA 0x3735c0.
"""
import struct

MBN = "/Users/vahiru/Projects/gaokun-android/tools/fingerprint-bringup/rev/cabs/qctree/fingerpr.mbn"

SYMTAB_VA = 0x3677D0
STRTAB_VA = 0x3735C0
NSYM = 2026


def va2off(va):
    return va + 0x3000


with open(MBN, "rb") as f:
    data = f.read()


def read_va(va, n):
    o = va2off(va)
    return data[o:o + n]


# build symbol lookup: st_value -> list of (name, size)
syms = {}
symtab = read_va(SYMTAB_VA, NSYM * 24)
strtab_off = va2off(STRTAB_VA)
for i in range(NSYM):
    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", symtab, i * 24)
    if st_name == 0:
        continue
    end = data.index(b"\x00", strtab_off + st_name)
    name = data[strtab_off + st_name:end].decode("ascii", "replace")
    syms.setdefault(st_value, []).append((name, st_size))


def sym_at(va):
    best = None
    for v, lst in syms.items():
        for name, size in lst:
            if v <= va < v + max(size, 1):
                if best is None or v > best[0]:
                    best = (v, name, size)
    return best


HITS = [0x361268, 0x3623d0, 0x363238, 0x361298, 0x362760, 0x3632c8]

for h in HITS:
    print(f"\n===== file {h:#x} / VA {h - 0x3000:#x} =====")
    s = sym_at(h - 0x3000)
    if s:
        print(f"  inside symbol: {s[1]} @ {s[0]:#x} size {s[2]:#x} (offset +{h - 0x3000 - s[0]:#x})")
    base = (h - 0x20) & ~7
    for a in range(base, h + 0x28, 8):
        q = struct.unpack_from("<Q", data, va2off(a))[0]
        marker = "  <<" if a == h else "   "
        ann = ""
        if q in syms:
            ann = " -> " + ", ".join(f"{n}(sz={sz:#x})" for n, sz in syms[q])
        else:
            s2 = sym_at(q) if q < 0x400000 else None
            if s2:
                ann = f" -> inside {s2[1]}+{q - s2[0]:#x}"
        # try string if points into rodata-ish
        if 0x226000 <= q < 0x361000:
            try:
                off = va2off(q)
                end = data.index(b"\x00", off)
                if 0 < end - off < 80:
                    ann += f'  str="{data[off:end].decode("ascii", "replace")}"'
            except Exception:
                pass
        print(f"{marker} {a:#x}: {q:#018x}{ann}")
