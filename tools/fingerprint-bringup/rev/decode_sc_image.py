#!/usr/bin/env python3
"""Decode one fixed SC payload locally; print statistics, never pixels or keys.
The checks below are only the initial Windows 0x6f00 checks, not the complete
image-processing algorithm or proof of a usable fingerprint.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
from Crypto.Cipher import AES
from pe import pe, off_of

PIN = '90dfeb2c23523a382f9310c16e6ad5d4ed33247b7868a955521ce7b70a710fb0'
SIZE = 15680


def decode(payload, dll):
    if len(payload) != SIZE:
        raise ValueError('expected exactly 15680 encoded bytes')
    data, base, sections = pe(dll)
    if hashlib.sha256(data).hexdigest() != PIN:
        raise ValueError('unreviewed DLL hash')
    key_at, _ = off_of(sections, base + 0x24060)
    iv_at, _ = off_of(sections, base + 0x24070)
    if key_at is None or iv_at is None:
        raise ValueError('missing reviewed AES constants')
    plain = AES.new(data[key_at:key_at + 16], AES.MODE_CBC, data[iv_at:iv_at + 16]).decrypt(payload)
    return plain, statistics(plain)


def statistics(plain):
    if len(plain) != SIZE:
        raise ValueError('wrong decoded length')
    pixels = struct.unpack('<7840H', plain)
    low, high = min(pixels), max(pixels)
    outside = sum(x > 4096 for x in pixels)
    return {'samples': len(pixels), 'min': low, 'max': high,
            'over_4096': outside, 'uniform': low == high,
            'basic_checks_passed': low != high and outside <= 100,
            'complete_image_processing_verified': False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('payload', type=Path)
    p.add_argument('--dll', type=Path, default=Path(__file__).parent / 'cabs/ftwbio/FtWbioDriverUmdf.dll')
    p.add_argument('--decoded', type=Path, help='optional new private raw LE16 file; refuses overwrite')
    a = p.parse_args()
    plain, stats = decode(a.payload.read_bytes(), a.dll)
    if a.decoded:
        fd = os.open(a.decoded, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as f:
            f.write(plain)
    print(json.dumps(stats, indent=2))

if __name__ == '__main__':
    main()
