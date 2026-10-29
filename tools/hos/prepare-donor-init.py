#!/usr/bin/env python3
"""Remove donor-only debug mounts and duplicate overlays now supplied by HOS fstab.

Inputs must be extracted from the exact images being repacked. This emits local
replacement files; it does not modify framework code or bypass service checks.
"""
import argparse
import hashlib
import json
from pathlib import Path

DEBUG_MOUNTS = {
    'mount overlay overlay /system/xbin lowerdir=/mnt/vendor/mi_ext/root/xbin:/system/xbin,override_creds=off',
    'mount none /mnt/vendor/mi_ext/root/bin/remount /system/bin/remount bind',
    'mount overlay overlay /system/xbin lowerdir=/mnt/vendor/mi_ext/root/xbin:/system/xbin',
}
BOOT_MOUNTS = {
    'mount overlay overlay product/usr lowerdir=/mnt/vendor/mi_ext/product/usr:/product/usr',
    'mount overlay overlay product/etc/precust_theme lowerdir=/mnt/vendor/mi_ext/product/etc/precust_theme:product/etc/precust_theme',
}


def prepare(system_ext, mi_ext):
    selected = [line.strip() for line in system_ext.splitlines()
                if line.strip().startswith('mount ') and '/mnt/vendor/mi_ext/root/' in line]
    if len(selected) != 4 or set(selected) != DEBUG_MOUNTS:
        raise ValueError('unexpected donor debug mounts; audit this donor before changing it')
    boot = [line.strip() for line in mi_ext.splitlines() if line.strip() and not line.lstrip().startswith('#')]
    if len(boot) != 3 or boot[0] != 'on boot' or set(boot[1:]) != BOOT_MOUNTS:
        raise ValueError('unexpected mi_ext rc actions; refusing to discard them')
    patched = ''.join('   # HOS: donor debug mount source absent; mainline has no override_creds option.\n'
                      if line.strip() in DEBUG_MOUNTS else line for line in system_ext.splitlines(keepends=True))
    return patched, '# HOS: product/usr and precust_theme overlays are mounted once by first-stage fstab.\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--system-ext-rc', type=Path, required=True)
    p.add_argument('--mi-ext-rc', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    sources = [a.system_ext_rc.read_text(), a.mi_ext_rc.read_text()]
    results = prepare(*sources)
    a.out.mkdir(parents=True, exist_ok=False)
    report = {}
    for name, original, patched in zip(('init.miui.ext.rc', 'init.miui.mi_ext.rc'), sources, results):
        (a.out/name).write_text(patched)
        report[name] = {'input_sha256': hashlib.sha256(original.encode()).hexdigest(),
                        'output_sha256': hashlib.sha256(patched.encode()).hexdigest()}
    (a.out/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
