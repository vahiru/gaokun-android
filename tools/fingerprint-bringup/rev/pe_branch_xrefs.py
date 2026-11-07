#!/usr/bin/env python3
"""Find direct AArch64 B/BL references in PE executable sections (not indirect calls)."""
import argparse
import struct
from pe import pe


def branches(data, sections, targets):
    for name, base, raw, size, flags in sections:
        if not flags & 0x20000000:
            continue
        if raw + size > len(data):
            raise ValueError(f'truncated section {name}')
        for offset in range(0, size - 3, 4):
            word = struct.unpack_from('<I', data, raw + offset)[0]
            opcode = word & 0xfc000000
            if opcode not in (0x14000000, 0x94000000):
                continue
            displacement = word & 0x3ffffff
            if displacement & 0x2000000:
                displacement -= 0x4000000
            pc = base + offset
            target = pc + 4 * displacement
            if target in targets:
                yield pc, 'BL' if opcode == 0x94000000 else 'B', target, name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    parser.add_argument('target', nargs='+', type=lambda x: int(x, 0))
    args = parser.parse_args()
    data, _, sections = pe(args.image)
    header = struct.unpack_from('<I', data, 0x3c)[0]
    if data[header:header + 4] != b'PE\0\0' or struct.unpack_from('<H', data, header + 4)[0] != 0xaa64:
        parser.error('expected AArch64 PE')
    for pc, kind, target, section in branches(data, sections, set(args.target)):
        print(f'{pc:#x} {kind} -> {target:#x} [{section}]')


if __name__ == '__main__':
    main()
