#!/usr/bin/env python3
"""Offline Windows-QSEE SC version frame and response checker. Never calls adb."""
import argparse
import functools
import json
import operator
from pathlib import Path
import struct

MAX_VERSION = 64
MCU_READ_SIZE = MAX_VERSION + 5


def request_description():
    # FtWbioDriverUmdf.dll 0x180006404..0x180006410 passes cmd=0x30,
    # empty payload to 0x1800077a8. 0x180011950 frames that for QSEE.
    frame = bytes.fromhex('02 00 01 30 31')
    return {
        'scope': 'SC firmware version, not a sensor chip ID or proof of initialization',
        'mcu_request_hex': frame.hex(' '),
        'ta_write_request_hex': (struct.pack('<III', 0x8021, len(frame), 0) + frame).hex(' '),
        'ta_write_response_size': 12,
        'windows_delay_ms': 20,
        'windows_transport_delay_after_each_send_ms': 10,
        'total_explicit_write_to_read_delay_ms': 30,
        'total_explicit_transaction_delay_ms': 40,
        'ta_read_request_hex': struct.pack('<III', 0x8020, 0, MCU_READ_SIZE).hex(' '),
        'ta_read_response_size': 12 + MCU_READ_SIZE,
        'runtime_tested': False,
    }


def parse_mcu_response(frame):
    if len(frame) != MCU_READ_SIZE:
        raise ValueError('expected exactly 69 bytes from the SPI-read payload')
    length = int.from_bytes(frame[1:3], 'big')
    if not 1 <= length <= MAX_VERSION + 1:
        raise ValueError('MCU length outside 1..65')
    if frame[3] != 4:
        raise ValueError(f'MCU status is 0x{frame[3]:02x}, expected 0x04')
    payload_size = length - 1
    if not payload_size:
        raise ValueError('empty firmware version payload')
    checksum = functools.reduce(operator.xor, frame[1:length + 3], 0)
    if frame[length + 3] != checksum:
        raise ValueError('MCU XOR checksum mismatch')
    payload = frame[4:4 + payload_size]
    return {
        'frame_type_unverified': frame[0],
        'payload_hex': payload.hex(' '),
        'payload_ascii': payload.decode('ascii', errors='backslashreplace'),
        'length': length,
        'status': frame[3],
        'checksum_valid': True,
        'note': 'Framing validated; identity/freshness and SPI setup require independent evidence',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--response', type=Path,
                        help='optional raw 81-byte TA response (12-byte header plus 69 bytes)')
    args = parser.parse_args()
    result = request_description()
    if args.response:
        data = args.response.read_bytes()
        if len(data) != 81:
            parser.error('TA response must contain exactly 81 bytes')
        try:
            result['response'] = parse_mcu_response(data[12:])
        except ValueError as exc:
            parser.error(str(exc))
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()
