#!/usr/bin/env python3
"""Disassemble ARM64 bytes around a VA in fingerpr.mbn (ELF: file = VA + 0x3000).

Usage: elf_dasm.py <mbn> <va> [before-insns] [after-insns]
"""
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

BASE = 0x3000  # file = VA + BASE


def main():
    path, va_s = sys.argv[1], sys.argv[2]
    before = int(sys.argv[3], 0) if len(sys.argv) > 3 else 40
    after = int(sys.argv[4], 0) if len(sys.argv) > 4 else 20
    with open(path, "rb") as f:
        data = f.read()
    target = int(va_s, 0)
    off = target + BASE
    print(f"# target VA 0x{target:x} -> file off 0x{off:x}")
    start_off = max(0, off - before * 4)
    end_off = off + after * 4
    base_va = start_off - BASE
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = False
    for insn in md.disasm(data[start_off:end_off], base_va):
        mark = ">>" if insn.address == target else "  "
        print(f"{mark} 0x{insn.address:x}  {insn.bytes.hex():8s}  {insn.mnemonic} {insn.op_str}")


if __name__ == "__main__":
    main()
