#!/usr/bin/env python3
"""Disassemble ARM64 bytes around a VA in a PE. Usage: dasm.py <pe> <va> [before-insns] [after-insns]"""
import sys

sys.path.insert(0, "/Users/vahiru/Projects/gaokun-android/tools/fingerprint-bringup/rev")
from pe import pe, off_of, va_of  # noqa: E402
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN  # noqa: E402


def main():
    path, va_s = sys.argv[1], sys.argv[2]
    before = int(sys.argv[3], 0) if len(sys.argv) > 3 else 40
    after = int(sys.argv[4], 0) if len(sys.argv) > 4 else 20
    data, ib, secs = pe(path)
    target = int(va_s, 0)
    off, sec = off_of(secs, target)
    print(f"# target VA 0x{target:x} -> file off 0x{off:x} (section {sec})")
    start_off = max(0, off - before * 4)
    end_off = off + after * 4
    base_va, _n = va_of(secs, start_off)
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = False
    for insn in md.disasm(data[start_off:end_off], base_va):
        mark = ">>" if insn.address == target else "  "
        print(f"{mark} 0x{insn.address:x}  {insn.bytes.hex():8s}  {insn.mnemonic} {insn.op_str}")


if __name__ == "__main__":
    main()
