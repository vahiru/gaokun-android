#!/usr/bin/env python3
"""Build and unpack-verify a local super_hos baseline from original A17 images.

No flash/GPT commands. Uses only explicit image files; output is raw sparse-file
extents, not Android sparse format. Current prototype uses only logical slot a.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

SIZE = 12 * 1024**3
GROUP = SIZE - 192 * 1024**2


def validate_images(images, hyperos=False):
    expected = {'system', 'system_ext', 'product', 'vendor'}
    if hyperos:
        expected |= {'mi_ext', 'mi_product'}
    if set(images) != expected:
        raise ValueError(f'expected exactly these partitions: {sorted(expected)}')
    for name, path in images.items():
        if not path.is_file() or path.stat().st_size == 0 or path.stat().st_size % 4096:
            raise ValueError(f'{name}: expected a nonempty regular 4K-aligned raw image')
        erofs = hyperos and name != 'vendor'
        with path.open('rb') as f:
            f.seek(1024 if erofs else 1080)
            magic = f.read(4 if erofs else 2)
        if magic != (b'\xe2\xe1\xf5\xe0' if erofs else b'\x53\xef'):
            raise ValueError(f'{name}: expected raw {"EROFS" if erofs else "ext4"}')
    if sum(path.stat().st_size for path in images.values()) > GROUP:
        raise ValueError('images exceed experimental group size')


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('system', 'system-ext', 'product', 'vendor', 'out'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--hyperos', action='store_true', help='six-partition EROFS donor with ext4 vendor')
    p.add_argument('--mi-ext', type=Path)
    p.add_argument('--mi-product', type=Path)
    a = p.parse_args()
    images = {name: getattr(a, name).resolve() for name in ('system', 'system_ext', 'product', 'vendor')}
    for name in ('mi_ext', 'mi_product'):
        if getattr(a, name):
            images[name] = getattr(a, name).resolve()
    try:
        validate_images(images, a.hyperos)
    except ValueError as e:
        p.error(str(e))
    a.out.mkdir(parents=True, exist_ok=False)
    image = a.out.resolve()/'super_hos.raw.img'
    cmd = ['lpmake', '--metadata-size', '65536', '--metadata-slots', '3',
           '--super-name', 'super_hos', '--device', f'super_hos:{SIZE}',
           '--group', f'gaokun3_hos_a:{GROUP}', '--output', str(image)]
    for name, path in images.items():
        cmd += ['--partition', f'{name}_a:readonly:{path.stat().st_size}:gaokun3_hos_a',
                '--image', f'{name}_a={path}']
    result = subprocess.run(cmd, capture_output=True)
    (a.out/'lpmake.log').write_bytes(result.stdout+result.stderr)
    result.check_returncode()
    result = subprocess.run(['lpdump', str(image)], capture_output=True)
    (a.out/'lpdump.txt').write_bytes(result.stdout+result.stderr)
    result.check_returncode()
    unpacked = a.out.resolve()/'unpacked'
    unpacked.mkdir()
    result = subprocess.run(['lpunpack', str(image), str(unpacked)], capture_output=True)
    (a.out/'lpunpack.log').write_bytes(result.stdout+result.stderr)
    result.check_returncode()
    parts = {}
    for name, source in images.items():
        extracted = unpacked/(name+'_a.img')
        expected = sha(source)
        if not extracted.is_file() or extracted.stat().st_size != source.stat().st_size or sha(extracted) != expected:
            raise ValueError(f'{name}: lpunpack round-trip mismatch')
        parts[name+'_a'] = {'source': str(source), 'size': source.stat().st_size, 'sha256': expected}
    report = {'mode': 'hyperos' if a.hyperos else 'a17-baseline',
              'logical_slot': '_a', 'super_name': 'super_hos', 'size': image.stat().st_size,
              'group_size': GROUP, 'argv': cmd, 'partitions': parts, 'sha256': sha(image),
              'ready_to_boot': False,
              'remaining': ['Create and verify independent GPT partitions and backups',
                            'Use matching fstab/ramdisk, force_super_partition=super_hos and slot_suffix=_a',
                            'Verify rescue entry and arrange attended boot with early failure logs']}
    if a.hyperos:
        report['remaining'] += ['Verify all donor overlay paths, SELinux labels and kernel EROFS compression support',
                                'Complete framework HAL wait audit, device features and graphics configuration',
                                'Pass isolated A17 baseline boot before donor boot']
    (a.out/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'Prepared {image}; all {len(images)} images match after lpunpack')


if __name__ == '__main__':
    main()
