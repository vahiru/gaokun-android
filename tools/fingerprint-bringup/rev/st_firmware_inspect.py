#!/usr/bin/env python3
"""Offline inspection of the pinned embedded STM firmware; no device access."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN

BASE = 0x08060000
SIZE = 0x12c04
PIN = '9cc81095c955175045e178f9de99960306a07c45a16d4385b0854c81b1aba79b'
DEFAULT = Path.home() / '.cache/fingerprint-20261030-check/firmware/st-ft9769-app-202303152035.bin'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--firmware', type=Path, default=DEFAULT)
    p.add_argument('--disassemble', nargs=2, type=lambda value: int(value, 0),
                   metavar=('START_VA', 'END_VA'))
    a = p.parse_args()
    data = a.firmware.read_bytes()
    if len(data) != SIZE or hashlib.sha256(data).hexdigest() != PIN:
        p.error('unreviewed firmware size/hash')
    if a.disassemble:
        start, end = a.disassemble
        if not BASE <= start < end <= BASE + SIZE or start % 2 or end % 2:
            p.error('expected an aligned, in-range Thumb window')
        md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
        for i in md.disasm(data[start-BASE:end-BASE], start):
            print(f'{i.address:#010x}  {i.mnemonic:9s} {i.op_str}')
        return
    commands = []
    for offset in range(0x129c8, 0x12a40, 8):
        cmd, ptr = struct.unpack_from('<II', data, offset)
        if cmd > 255 or not ptr & 1 or not BASE <= (ptr & ~1) < BASE + SIZE:
            p.error('unexpected command table entry')
        commands.append({'command': hex(cmd), 'handler_thumb_address': hex(ptr),
                         'handler_code_address': hex(ptr & ~1)})
    print(json.dumps({'sha256': PIN, 'size': len(data), 'flash_base': hex(BASE),
                      'data_lma': hex(struct.unpack_from('<I', data, 0x216c)[0]),
                      'data_vma_start': hex(struct.unpack_from('<I', data, 0x2170)[0]),
                      'data_vma_end': hex(struct.unpack_from('<I', data, 0x2174)[0]),
                      'command_table_runtime_address': '0x20000088',
                      'dispatcher_limit': 15, 'commands': commands}, indent=2))


if __name__ == '__main__':
    main()
