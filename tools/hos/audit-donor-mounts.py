#!/usr/bin/env python3
"""Inspect every overlay target/lower directory in the actual EROFS images.

Does not mount or modify images. dump.erofs can return zero for a missing path,
so require positive directory information in its output as well as exit status.
This checks paths, not SELinux authorization or runtime mount success.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def locate(path):
    for prefix, partition in (('/mnt/vendor/mi_ext', 'mi_ext'),
                              ('/mnt/vendor/mi_product', 'mi_product'),
                              ('/system_ext', 'system_ext'), ('/product', 'product')):
        if path == prefix or path.startswith(prefix + '/'):
            return partition, path[len(prefix):] or '/'
    if path.startswith('/system/'):
        return 'system', path
    raise ValueError(f'unsupported Android path: {path}')


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('system', 'system-ext', 'product', 'mi-ext', 'mi-product', 'fstab', 'dump-erofs', 'out'):
        p.add_argument('--'+name, type=Path, required=True)
    a = p.parse_args()
    images = {name: getattr(a, name).resolve() for name in ('system', 'system_ext', 'product', 'mi_ext', 'mi_product')}
    if any(not image.is_file() for image in images.values()):
        p.error('all input images must be regular files')
    a.out.mkdir(parents=True, exist_ok=False)
    entries = [s.split() for s in a.fstab.read_text().splitlines() if s.startswith('overlay ')]
    checks, overlays = {}, []
    for row in entries:
        lowerdirs = next(x[9:] for x in row[3].split(',') if x.startswith('lowerdir=')).split(':')
        paths = [row[1]] + lowerdirs
        for raw in paths:
            path = raw.rstrip('/')
            if path in checks:
                continue
            partition, inner = locate(path)
            result = subprocess.run([str(a.dump_erofs), '--path='+inner, str(images[partition])], capture_output=True, text=True)
            output = result.stdout + result.stderr
            exists = result.returncode == 0 and re.search(r'^Size:.*\bdirectory\s*$', output, re.M) is not None and '<E>' not in output
            checks[path] = {'partition': partition, 'inner': inner, 'directory_exists': exists, 'output': output}
        overlays.append({'target': row[1], 'lowerdirs': lowerdirs,
                         'all_directories_exist': all(checks[x.rstrip('/')]['directory_exists'] for x in paths)})
    missing = [path for path, value in checks.items() if not value['directory_exists']]
    report = {'inputs': {name: {'path': str(path), 'sha256': sha(path)} for name, path in images.items()},
              'fstab_sha256': sha(a.fstab), 'overlays': overlays, 'paths': checks, 'missing': missing,
              'path_check_passed': bool(overlays) and not missing, 'ready_to_boot': False,
              'not_verified': ['SELinux labels/authorization', 'runtime mounts', 'init built-in overlays and rc actions']}
    (a.out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'{len(overlays)} overlays; {len(checks)} unique paths; {len(missing)} missing/non-directory paths')
    for path in missing:
        print(path)
    return 0 if report['path_check_passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
