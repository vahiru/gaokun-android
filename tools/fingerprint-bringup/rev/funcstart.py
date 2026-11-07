#!/usr/bin/env python3
"""Find likely ARM64 function start for an address inside an unknown function.

Usage: funcstart.py <pe-path> <va> [max-back]

Scans backwards from VA for 4-aligned prologue signatures:
  BTI C        5f 24 03 d5
  PACIBSP      7f 23 03 d5
  PACIASP      1f 23 03 d5
  STP X29,X30,[SP,#-imm]!   fd 7b xx a9
  SUB SP,SP,#imm            ff 43 xx d1
Prints candidates nearest-first.
"""
import sys

sys.path.insert(0, "/Users/vahiru/Projects/gaokun-android/tools/fingerprint-bringup/rev")
from pe import pe, off_of, va_of  # noqa: E402


def main():
    path, va_s = sys.argv[1], sys.argv[2]
    max_back = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x2000
    data, ib, secs = pe(path)
    target = int(va_s, 0)
    off, _sec = off_of(secs, target)
    if off is None:
        sys.exit(f"VA 0x{target:x} not in any section")
    start = max(0, off - max_back)
    cand = []
    for o in range(start - start % 4, off + 1, 4):
        b = data[o:o + 4]
        if len(b) < 4:
            continue
        va, _n = va_of(secs, o)
        w = int.from_bytes(b, "little")
        rank = None
        if b in (b"\x5f\x24\x03\xd5", b"\x7f\x23\x03\xd5", b"\x1f\x23\x03\xd5"):
            rank = 0  # BTI C / PACIBSP / PACIASP
        elif w & 0xFFC003E0 in (0xA98003E0, 0xA9C003E0) and w & 0x1F == 0x1D:
            rank = 1  # STP X29, Xt, [SP, #imm]!  /  [SP], #imm
        elif w & 0xFFC003FF == 0xD10003FF:
            rank = 2  # SUB SP, SP, #imm
        if rank is not None:
            cand.append((va, rank, b.hex()))
    cand.sort(key=lambda t: (abs(t[0] - target), t[1]))
    for va, rank, hx in cand[:15]:
        print(f"0x{va:x}  rank={rank}  {hx}")


if __name__ == "__main__":
    main()
