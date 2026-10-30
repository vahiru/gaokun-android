#!/usr/bin/env python3
"""Offline framing for the observed 40x196 SC image read; never accesses hardware.
The MCU payload is encoded data, not a ready-to-display fingerprint image.
"""
import argparse
from pathlib import Path
import struct

PIXELS = 40 * 196
ENCODED_BYTES = PIXELS * 2
MCU_REPLY_BYTES = ENCODED_BYTES + 5
TA_REPLY_BYTES = MCU_REPLY_BYTES + 12


def request_frames():
    payload = b'\x34\x00' + ENCODED_BYTES.to_bytes(2, 'big')
    mcu = b'\x02\x00\x05\x8b' + payload
    check = 0
    for value in mcu[1:]:
        check ^= value
    mcu += bytes([check])
    return struct.pack('<III', 0x8021, len(mcu), 0) + mcu, struct.pack('<III', 0x8020, 0, MCU_REPLY_BYTES)


def validate_reply(raw):
    if len(raw) != TA_REPLY_BYTES:
        raise ValueError('expected exactly 15697 bytes of TA response')
    frame = raw[12:]
    length = int.from_bytes(frame[1:3], 'big')
    if length != ENCODED_BYTES + 1 or frame[3] != 4:
        raise ValueError('wrong MCU payload length or status')
    check = 0
    for value in frame[1:length + 3]:
        check ^= value
    if frame[length + 3] != check:
        raise ValueError('bad MCU XOR checksum')
    # The Windows parser does not verify byte0; preserve it as evidence.
    return {'frame_byte0': frame[0], 'encoded_bytes': ENCODED_BYTES,
            'pixels': PIXELS, 'block_bytes': 16,
            'decoded_image_verified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reply', type=Path)
    args = parser.parse_args()
    if args.reply:
        print(validate_reply(args.reply.read_bytes()))
    else:
        write, read = request_frames()
        print('TA write:', write.hex(' '))
        print('TA read:', read.hex(' '))
        print('Expected TA response:', TA_REPLY_BYTES)
        print('Framing only; initialization and decoding remain required.')


if __name__ == '__main__':
    main()
