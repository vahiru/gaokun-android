#!/usr/bin/env python3
"""Resume verified full-OTA partition extraction using HTTP ranges (local files only).

Requires a previously recorded payload offset and trusted metadata hash/size.
Only REPLACE/BZ/XZ/ZERO operations are accepted; no delta source or device access.
"""
import argparse
import base64
import bz2
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
import lzma
import os
from pathlib import Path
import re
import struct
import time
import urllib.request

spec = importlib.util.spec_from_file_location('payload', Path(__file__).with_name('payload-extract.py'))
pb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pb)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
    return h.digest()


def fetch(url, start, size):
    if size <= 0 or start < 0:
        raise ValueError('invalid range')
    for attempt in range(5):
        try:
            request = urllib.request.Request(url, headers={'Range': f'bytes={start}-{start+size-1}',
                                                            'Accept-Encoding': 'identity'})
            with urllib.request.urlopen(request, timeout=45) as r:
                match = re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)', r.headers.get('Content-Range', ''))
                if r.status != 206 or not match or tuple(map(int, match.groups()[:2])) != (start, start+size-1):
                    raise ValueError('server did not honor exact byte range')
                data = r.read(size + 1)
                if len(data) != size:
                    raise ValueError('truncated/oversized range response')
                return data
        except (OSError, ValueError) as e:
            if attempt == 4:
                raise RuntimeError(f'range {start}+{size} failed') from e
            time.sleep(attempt + 1)


def operations(part, blocksize, size):
    result, ranges = [], []
    for raw in pb.get(part, 8):
        fields = pb.fields(raw)
        kind = pb.one(fields, 1, 0)
        offset, length = pb.one(fields, 2, 0), pb.one(fields, 3, 0)
        checksum = pb.one(fields, 8)
        extents = [(pb.one(pb.fields(e), 1, 0)*blocksize,
                    pb.one(pb.fields(e), 2, 0)*blocksize) for e in pb.get(fields, 6)]
        if kind not in (0, 1, 6, 8) or not extents:
            raise ValueError(f'unsupported operation {kind} or empty extents')
        if kind != 6 and (length <= 0 or not isinstance(checksum, bytes) or len(checksum) != 32):
            raise ValueError('data operation lacks length/hash')
        for start, count in extents:
            if count <= 0 or start < 0 or start + count > size:
                raise ValueError('destination outside partition')
            ranges.append((start, start + count))
        result.append((kind, offset, length, checksum, extents))
    end = 0
    for start, stop in sorted(ranges):
        if start != end:
            raise ValueError('overlap or gap in full partition operations')
        end = stop
    if end != size:
        raise ValueError('operations do not cover entire partition')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--url-file', type=Path, required=True)
    p.add_argument('--payload-offset', type=int, required=True)
    p.add_argument('--metadata-size', type=int, required=True)
    p.add_argument('--metadata-hash', required=True, help='base64 SHA-256 from recorded payload properties')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--workers', type=int, default=8, choices=range(1, 17))
    p.add_argument('partitions', nargs='+')
    a = p.parse_args()
    if any(not re.fullmatch('[a-z][a-z0-9_]*', n) for n in a.partitions):
        p.error('invalid partition name')
    url = a.url_file.read_text().strip()
    if not url.startswith('https://'):
        p.error('HTTPS URL required')
    a.out.mkdir(parents=True, exist_ok=True)
    metadata = fetch(url, a.payload_offset, a.metadata_size)
    if hashlib.sha256(metadata).digest() != base64.b64decode(a.metadata_hash, validate=True):
        raise ValueError('trusted metadata hash mismatch')
    if metadata[:4] != b'CrAU':
        raise ValueError('invalid payload magic')
    version, length, signature = struct.unpack('>QQI', metadata[4:24])
    if version != 2 or length + 24 != len(metadata):
        raise ValueError('unexpected metadata version/length')
    manifest = pb.fields(metadata[24:])
    blocksize = pb.one(manifest, 3, 4096)
    if blocksize != 4096:
        raise ValueError('unsupported block size')
    parts = {pb.one(pb.fields(raw), 1).decode(): pb.fields(raw) for raw in pb.get(manifest, 13)}
    if set(a.partitions) - parts.keys():
        raise ValueError('requested partition missing from manifest')
    data_start = a.payload_offset + len(metadata) + signature
    (a.out/'metadata.bin').write_bytes(metadata)
    reports = {}
    for name in a.partitions:
        part = parts[name]
        info = pb.fields(pb.one(part, 7))
        size, expected = pb.one(info, 1), pb.one(info, 2)
        if not isinstance(size, int) or size <= 0 or not isinstance(expected, bytes) or len(expected) != 32:
            raise ValueError('missing partition size/hash')
        ops = operations(part, blocksize, size)
        output = a.out/(name+'.img')
        if output.exists():
            if not output.is_file() or output.is_symlink() or output.stat().st_size != size or digest(output) != expected:
                raise ValueError(f'{output}: existing final image does not match')
        else:
            cache = a.out/(name+'-chunks')
            cache.mkdir(exist_ok=True)
            temporary = a.out/(name+'.partial')
            if temporary.is_symlink():
                raise ValueError('partial file must not be a symlink')
            with temporary.open('wb') as f:
                f.truncate(size)
                def extract(item):
                    index, (kind, offset, length, checksum, extents) = item
                    if kind == 6:
                        return
                    chunk = cache/checksum.hex()
                    raw = chunk.read_bytes() if chunk.is_file() else b''
                    if len(raw) != length or hashlib.sha256(raw).digest() != checksum:
                        raw = fetch(url, data_start+offset, length)
                        if hashlib.sha256(raw).digest() != checksum:
                            raise ValueError('operation checksum mismatch')
                        chunk.write_bytes(raw)
                    data = raw if kind == 0 else bz2.decompress(raw) if kind == 1 else lzma.decompress(raw)
                    if len(data) != sum(n for _, n in extents):
                        raise ValueError('decompressed extent size mismatch')
                    cursor = 0
                    for start, count in extents:
                        written = 0
                        while written < count:
                            n = os.pwrite(f.fileno(), data[cursor+written:cursor+count], start+written)
                            if n <= 0:
                                raise OSError('short pwrite')
                            written += n
                        cursor += count
                with ThreadPoolExecutor(a.workers) as executor:
                    for completed, _ in enumerate(executor.map(extract, enumerate(ops)), 1):
                        if completed % 100 == 0 or completed == len(ops):
                            print(f'{name}: {completed}/{len(ops)} operations', flush=True)
                os.fsync(f.fileno())
            if digest(temporary) != expected:
                raise ValueError('full partition checksum mismatch')
            temporary.rename(output)
        reports[name] = {'size': size, 'sha256': expected.hex(), 'verified': True}
        (a.out/'manifest.json').write_text(json.dumps({'url': url, 'metadata_sha256': hashlib.sha256(metadata).hexdigest(),
            'payload_offset': a.payload_offset, 'data_start': data_start, 'partitions': reports}, indent=2)+'\n')
        print(f'{name}: full partition SHA-256 verified', flush=True)


if __name__ == '__main__':
    main()
