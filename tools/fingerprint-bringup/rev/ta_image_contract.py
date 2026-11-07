#!/usr/bin/env python3
"""Offline fixed TA image-export contract. Contains no device execution interface.

802C is deliberately not constructible: its unchecked failure paths are not
yet suitable for an automatic diagnostic. 802D export requires an independently
confirmed initialized processed-image buffer; an ACK alone does not prove it.
"""
import json
import struct

IMAGE_BYTES = 40 * 196
RESPONSE_BYTES = 12 + IMAGE_BYTES


def build_request(command=0x802d, response_bytes=RESPONSE_BYTES):
    if command != 0x802d:
        raise ValueError('only reviewed 802D full-image export can be described; 802C/algorithm/template modes unsupported')
    if response_bytes != RESPONSE_BYTES:
        raise ValueError('require exact 7852-byte response; short/threshold/oversized modes not reviewed')
    return struct.pack('<III', command, 0, IMAGE_BYTES)


def validate_reply(request_after, response, response_before, *, transport_status):
    if transport_status != 0:
        raise ValueError('transport did not complete successfully')
    if len(request_after) != 12 or struct.unpack('<III', request_after) != (0x5aa5, 0, 0):
        raise ValueError('unexpected request ACK or header mutation')
    if len(response) != RESPONSE_BYTES or len(response_before) != RESPONSE_BYTES:
        raise ValueError('wrong exact response size')
    if response[:12] != response_before[:12]:
        raise ValueError('unexpected response header mutation')
    if response[12:] == response_before[12:]:
        raise ValueError('image output remained untouched; NULL/uninitialized image state is not success')
    return {'transport_and_export_contract_passed': True, 'exported_bytes': IMAGE_BYTES,
            'image_quality_verified': False, 'template_or_authentication_result': False,
            'source_state_requires_independent_confirmation': True}


if __name__ == '__main__':
    print(json.dumps({'request_hex': build_request().hex(' '),
                      'request_bytes': 12, 'response_bytes': RESPONSE_BYTES,
                      'payload_disclosed': False, 'executes_commands': False,
                      'eligible_for_device_execution': False,
                      '802c_supported': False,
                      'prerequisite': 'independently verified TA processed-image buffer, same live TA session'}, indent=2))
