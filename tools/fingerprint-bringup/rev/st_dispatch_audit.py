#!/usr/bin/env python3
"""Read-only, hash-pinned ST dispatcher/ACK audit; never accesses a device.

Uses Thumb instructions and startup copy literals to locate the actual command
table. Optional trace checks only short protocol responses, not image payloads.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS

BASE = 0x08060000
SHA = '9cc81095c955175045e178f9de99960306a07c45a16d4385b0854c81b1aba79b'
DEFAULT = Path.home() / '.cache/fingerprint-20261030-check/firmware/st-ft9769-app-202303152035.bin'


def inspect(path):
    data = path.read_bytes()
    if len(data) != 0x12c04 or hashlib.sha256(data).hexdigest() != SHA:
        raise ValueError('expected exact extracted ST image; refusing unpinned interpretation')
    def word(address):
        return struct.unpack_from('<I', data, address - BASE)[0]
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
    def instructions(start, end):
        return {i.address: i for i in md.disasm(data[start-BASE:end-BASE], start)}
    code = {}
    for a, b in [(0x08060b18, 0x08060b72), (0x080606d0, 0x0806071a),
                 (0x0806072c, 0x0806081a), (0x08060290, 0x080602d0),
                 (0x080661c0, 0x080661d4)]:
        code.update(instructions(a, b))
    def check(address, mnemonic, operands):
        i = code[address]
        if (i.mnemonic, i.op_str) != (mnemonic, operands):
            raise ValueError(f'unexpected instruction at {address:#x}: {i.mnemonic} {i.op_str}')
    # RAM table is not a flash literal: reset copies .data into SRAM.
    source, destination, end = (word(a) for a in (0x0806216c, 0x08062170, 0x08062174))
    if (source, destination, end) != (0x08072940, 0x20000000, 0x200002c4):
        raise ValueError('unexpected startup data mapping')
    table_ram = word(0x08060b7c)
    table_flash = source + table_ram - destination
    if table_ram != 0x20000088 or table_flash != 0x080729c8:
        raise ValueError('unexpected command table pointer')
    check(0x08060b34, 'ldrb', 'r1, [r0, #3]')
    check(0x08060b3a, 'ldrb.w', 'r5, [r2, r3, lsl #3]')
    check(0x08060b46, 'ldr', 'r3, [r3, #4]')
    check(0x08060b48, 'blx', 'r3')
    check(0x08060b6c, 'cmp', 'r3, #0xf')
    check(0x08060b6e, 'bne', '#0x8060b3a')
    check(0x08060b70, 'b', '#0x8060b4a')
    check(0x08060b4a, 'mov.w', 'r2, #0x408')
    check(0x08060b50, 'movs', 'r1, #0')
    check(0x08060b52, 'bl', '#0x806bdf4')
    check(0x08060b5a, 'strb.w', 'r2, [r3, #0x408]')
    check(0x08060b5e, 'str.w', 'r2, [r3, #0x40c]')
    table = [struct.unpack_from('<II', data, table_flash - BASE + 8*i) for i in range(15)]
    expected = [0x30,0x31,0x32,0x33,0x3a,0x3b,0x3d,0x3f,0x83,0x84,0x87,0x88,0x8a,0x8b,0x8e]
    if [cmd for cmd, _ in table] != expected:
        raise ValueError('unexpected command table')
    #87's input pointer is discarded before allocation; no index/value read.
    check(0x080606d2, 'cbnz', 'r0, #0x80606e2')
    check(0x080606e2, 'movs', 'r1, #0x40')
    check(0x080606e4, 'movs', 'r0, #0')
    check(0x080606e6, 'bl', '#0x80662ec')
    check(0x080606f8, 'movs', 'r2, #0x40')
    check(0x080606fa, 'movs', 'r1, #0')
    check(0x080606fc, 'bl', '#0x806bdf4')
    check(0x08060700, 'cpsid', 'i')
    check(0x08060702, 'cpsie', 'i')
    check(0x08060704, 'movs', 'r2, #4')
    check(0x08060706, 'adds', 'r0, r4, r2')
    check(0x08060708, 'movs', 'r1, #0')
    check(0x0806070a, 'bl', '#0x8060290')
    #88/index20 returns literalA5/5A; it does not inspect a ready flag.
    check(0x0806073e, 'ldrb', 'r5, [r0, #4]')
    check(0x08060760, 'cmp', 'r5, #0x20')
    check(0x08060762, 'bne', '#0x8060770')
    check(0x08060764, 'movs', 'r3, #0xa5')
    check(0x08060766, 'strb', 'r3, [r4, #4]')
    check(0x08060768, 'movs', 'r3, #0x5a')
    check(0x0806076a, 'strb', 'r3, [r4, #5]')
    check(0x0806076c, 'movs', 'r1, #2')
    check(0x0806076e, 'b', '#0x8060792')
    check(0x08060794, 'movs', 'r2, #4')
    check(0x08060798, 'bl', '#0x8060290')
    # Shared reply: header02, BE(payload length+1), status, XOR, send length+5.
    check(0x08060290, 'movs', 'r3, #2')
    check(0x08060294, 'strb', 'r3, [r0, #-0x4]')
    check(0x0806029a, 'adds', 'r3, r1, #1')
    check(0x080602ac, 'strh', 'r3, [r0, #-0x3]')
    check(0x080602b0, 'strb', 'r2, [r0, #-0x1]')
    check(0x080602b8, 'bl', '#0x80661c0')
    check(0x080661d0, 'eors', 'r0, r2')
    check(0x080602bc, 'strb', 'r0, [r4, r6]')
    check(0x080602c0, 'adds', 'r2, r5, #5')
    check(0x080602c8, 'bl', '#0x806122c')
    return {
        'firmware_sha256': SHA,
        'table_ram': hex(table_ram), 'table_flash': hex(table_flash),
        'commands': [{'command': hex(c), 'handler_thumb': hex(h)} for c,h in table],
        'unknown_89': '15-entry miss branches to RX cleanup; no reply call on that path',
        'command87': 'input index/value not read; empty status04 ACK only',
        'command88_index20': 'constant payload a5 5a, not a hardware readiness measurement',
        'reply87_hex': reply(b'').hex(),
        'reply88_index20_hex': reply(bytes.fromhex('a5 5a')).hex(),
        'limitations': [
            'Same embedded version string is not a device firmware byte-for-byte readback.',
            'Unknown-command branch proves no command-specific reply is queued, not what SPI hardware must shift out.',
            'Repeating the last reply checksum is consistent with observed fill bytes; peripheral/DMA idle behavior is not established here.'
        ]
    }


def reply(payload):
    frame = bytes([2]) + (len(payload)+1).to_bytes(2,'big') + bytes([4]) + payload
    checksum = 0
    for b in frame[1:]: checksum ^= b
    return frame + bytes([checksum])


def correlate_trace(path):
    current = None
    previous = None
    records = []
    for line in path.read_text().splitlines():
        m = re.search(r'MCU cmd=0x([0-9a-f]+) index=0x([0-9a-f]+)', line)
        if m: current = tuple(int(x,16) for x in m.groups())
        m = re.search(r'fpinit MCU reply: 00000000: ([0-9a-f ]+)$', line)
        if not m: continue
        packet = bytes.fromhex(m.group(1))
        if current == (0x88,0x20):
            if packet != reply(bytes.fromhex('a5 5a')):
                raise ValueError('trace direct20 reply differs from pinned constant-frame model')
            previous = packet
        elif current == (0x89,0x1d):
            records.append({'packet_hex':packet.hex(),
                            'prior_direct20_hex': previous.hex() if previous else None,
                            'all_bytes_repeat_prior_tail': bool(previous and packet and all(b==previous[-1] for b in packet))})
    return records


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--firmware', type=Path, default=DEFAULT)
    ap.add_argument('--trace', type=Path)
    args = ap.parse_args()
    report = inspect(args.firmware)
    if args.trace: report['trace_correlations'] = correlate_trace(args.trace)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
