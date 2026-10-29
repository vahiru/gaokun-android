#!/usr/bin/env python3
"""Wrap an already isolated ramdisk with the static, UUID-bound early logger.

No device access. The original init is preserved as init.hos.real, and all other
archive entries retain their original metadata and data. Requires new outputs.
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import stat
import struct

spec = importlib.util.spec_from_file_location('ramdisk', Path(__file__).with_name('prepare-ramdisk.py'))
ramdisk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ramdisk)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('input', 'logger', 'plan', 'out'):
        p.add_argument('--' + name, required=True, type=Path)
    a = p.parse_args()
    plan = json.loads(a.plan.read_text())
    misc = [v for v in plan['new_partitions'] if v['name'] == 'misc_hos']
    if len(misc) != 1 or misc[0]['size'] != 4 * 1024 * 1024:
        p.error('plan must contain one 4 MiB misc_hos')
    binary = a.logger.read_bytes()
    if binary[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', binary, 18)[0] != 183:
        p.error('logger must be little-endian arm64 ELF64')
    offset = struct.unpack_from('<Q', binary, 32)[0]
    entry_size, count = struct.unpack_from('<HH', binary, 54)
    if entry_size < 56 or offset + count * entry_size > len(binary):
        p.error('invalid program headers')
    if any(struct.unpack_from('<I', binary, offset + i * entry_size)[0] in (2, 3) for i in range(count)):
        p.error('logger must be static, with no PT_DYNAMIC or PT_INTERP')
    if misc[0]['guid'].encode() not in binary:
        p.error('logger UUID does not match plan')
    source = a.input.read_bytes()
    entries = ramdisk.read_newc(gzip.decompress(source))
    lookup = {str(PurePosixPath(n)): (n, v, d) for n, v, d in entries}
    original = lookup.get('init')
    if not original or not stat.S_ISREG(original[1][1]) or original[1][4] != 1:
        p.error('expected a single-link regular original init')
    if 'init.hos.real' in lookup or 'hos-proc' in lookup or 'hos-sys' in lookup or 'hos-dev' in lookup:
        p.error('input already contains reserved wrapper paths')
    fstab = lookup['fstab.gaokun3'][2]
    for name in ('misc_hos', 'userdata_hos', 'metadata_hos'):
        if ('/dev/block/by-name/' + name).encode() not in fstab:
            p.error('input fstab is not isolated')
    result = [('init.hos.real' if str(PurePosixPath(n)) == 'init' else n, v, d) for n, v, d in entries]
    inode = max(v[0] for _, v, _ in entries) + 1
    result.append(('init', [inode, stat.S_IFREG | 0o750, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0], binary))
    compressed = gzip.compress(ramdisk.write_newc(result), mtime=0)
    verified = ramdisk.read_newc(gzip.decompress(compressed))
    by_name = {str(PurePosixPath(n)): (v, d) for n, v, d in verified}
    for name, (_, values, data) in lookup.items():
        new_name = 'init.hos.real' if name == 'init' else name
        actual_v, actual_d = by_name[new_name]
        expected_v = values.copy()
        expected_v[6], expected_v[11], expected_v[12] = len(data), len(new_name.encode()) + 1, 0
        if actual_v != expected_v or actual_d != data:
            raise ValueError('archive round-trip mismatch: ' + name)
    a.out.mkdir(parents=True, exist_ok=False)
    (a.out/'ramdisk.cpio.gz').write_bytes(compressed)
    (a.out/'fstab.gaokun3').write_bytes(fstab)
    (a.out/'cmdline.required.txt').write_text('androidboot.force_super_partition=super_hos\nandroidboot.hos_earlylog=misc-ring-v1\n')
    sha = lambda blob: hashlib.sha256(blob).hexdigest()
    manifest = {'input_sha256': sha(source), 'logger_sha256': sha(binary), 'misc_hos_guid': misc[0]['guid'],
                'ramdisk_sha256': sha(compressed), 'original_init_sha256': sha(original[2]),
                'preserved_original_entries': len(entries), 'logger_duration_seconds': 180,
                'log_partition_size': misc[0]['size'], 'reserved_prefix_bytes': 65536,
                'requires_early_log_vendor_policy': True, 'ready_to_boot': False,
                'remaining': ['Install exact isolated partition GUIDs and matching vendor CIL',
                              'Validate actual PID1 handoff, detached mounts and enforcing capture on device',
                              'Approve and perform attended baseline boot']}
    (a.out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print('Prepared UUID-bound logging ramdisk:', a.out)


if __name__ == '__main__':
    main()
