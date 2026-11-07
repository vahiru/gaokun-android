#!/usr/bin/env python3
"""Offline fixed Windows SC info queries; no arbitrary register/command input."""
import argparse
from functools import reduce
import json
from operator import xor
from pathlib import Path
import struct

QUERIES = {
    'dimension0': (0x14, 1),
    'dimension1': (0x15, 1),
    'device_status': (0x20, 2),
}


def describe(name):
    index, count = QUERIES[name]
    frame = bytes((2, 0, 3, 0x88, index, count))
    frame += bytes((reduce(xor, frame[1:], 0),))
    return {
        'query': name, 'index': index, 'count': count,
        'mcu_request_hex': frame.hex(' '),
        'ta_write_request_hex': (struct.pack('<III', 0x8021, 7, 0) + frame).hex(' '),
        'ta_write_response_size': 12,
        'ta_read_request_hex': struct.pack('<III', 0x8020, 0, count + 5).hex(' '),
        'ta_read_response_size': count + 17,
        'windows_delay_before_read_ms': 10,
        'windows_delay_after_read_ms': 11,
        'windows_upper_layer_extra_delay_ms': 1,
        'windows_transport_delay_after_each_send_ms': 10,
        'total_explicit_transaction_delay_ms': 21,
        'runtime_tested': False,
    }


def parse(name, response):
    _, count = QUERIES[name]
    if len(response) != count + 17:
        raise ValueError('wrong complete TA response size')
    frame = response[12:]
    length = int.from_bytes(frame[1:3], 'big')
    if length != count + 1:
        raise ValueError('MCU payload size does not match the fixed query')
    if frame[3] != 4:
        raise ValueError('MCU did not return status 0x04')
    if reduce(xor, frame[1:length + 3], 0) != frame[length + 3]:
        raise ValueError('MCU checksum mismatch')
    data = frame[4:4 + count]
    result = {'query': name, 'payload_hex': data.hex(' '),
              'value_little_endian': int.from_bytes(data, 'little'),
              'frame_type_unverified': frame[0]}
    if name.startswith('dimension'):
        if data[0] == 0:
            raise ValueError('Windows parameter check rejects a zero dimension')
        result['note'] = 'Sensor dimension in Windows return order; width/height assignment not established'
    else:
        result['windows_ready_value_matches'] = data == b'\xa5\x5a'
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('query', choices=tuple(QUERIES))
    p.add_argument('--response', type=Path, help='complete raw TA response, including 12-byte header')
    a = p.parse_args()
    result = describe(a.query)
    if a.response:
        try:
            result['response'] = parse(a.query, a.response.read_bytes())
        except ValueError as exc:
            p.error(str(exc))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
