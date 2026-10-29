#!/usr/bin/env python3
"""Decode a regular-file dump of misc_hos; never opens a block device."""
import argparse
import json
from pathlib import Path
import stat
import struct
import zlib

SIZE = 4 * 1024 * 1024
HEADER = 65536
DATA = 69632
FORMAT = '<8sIIQQQIIQQ'


def decode(raw):
    if len(raw) != SIZE:
        raise ValueError('expected a complete 4 MiB misc_hos dump')
    headers = []
    for slot in (0, 1):
        block = raw[HEADER + slot * 512:HEADER + slot * 512 + 64]
        magic, version, size, capacity, total, generation, crc, reserved, start, dropped = struct.unpack(FORMAT, block)
        clean = block[:40] + bytes(4) + block[44:]
        if (magic != b'GK3HLOG1' or version != 1 or size != 64 or capacity != SIZE - DATA
                or reserved or not generation or generation % 2 != slot
                or zlib.crc32(clean) != crc):
            continue
        headers.append(dict(slot=slot, capacity=capacity, total=total, generation=generation,
                            start_monotonic_ns=start, kmsg_dropped_notifications=dropped))
    if not headers:
        raise ValueError('no valid logger header (not initialized or both headers damaged)')
    best = max(headers, key=lambda h: h['generation'])
    n, capacity = best['total'], best['capacity']
    data = raw[DATA:]
    if n <= capacity:
        output = data[:n]
    else:
        position = n % capacity
        output = data[position:] + data[:position]
    best.update(valid_headers=len(headers), wrapped=n > capacity, recovered_bytes=len(output),
                limitation='Header CRC validates metadata; power loss may leave an incomplete data tail.')
    return best, output


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args()
    if not stat.S_ISREG(a.input.stat().st_mode):
        p.error('input must be a regular local dump, not a block device')
    info, log = decode(a.input.read_bytes())
    a.out.mkdir(parents=True, exist_ok=False)
    (a.out / 'kmsg.txt').write_bytes(log)
    (a.out / 'report.json').write_text(json.dumps(info, indent=2) + '\n')
    print(f"Recovered {len(log)} bytes; wrapped={info['wrapped']}")


if __name__ == '__main__':
    main()
