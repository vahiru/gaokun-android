#!/usr/bin/env python3
"""Update only R9 super_hos from stdin in verified gaokun3 rescue; preview default.

Preserves experimental userdata/metadata/misc, all production partitions and ESP.
Checks the current full super hash before writing and the new full hash afterward.
A start marker prevents blindly retrying a partial write. No implicit reboot.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import uuid


def main():
    if not __debug__:
        raise RuntimeError('optimized Python disables safety assertions; refusing')
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image-sha256', required=True)
    p.add_argument('--old-image-sha256', default='1bfa8cdc00ec1dc617cfaf03ad42bc85a01db456086ceac5d616389d8c287755')
    p.add_argument('--apply', action='store_true')
    args = p.parse_args()
    for value in (args.image_sha256, args.old_image_sha256):
        if not re.fullmatch('[0-9a-f]{64}', value):
            p.error('expected lowercase SHA-256')
    if args.image_sha256 == args.old_image_sha256:
        p.error('new image must differ')
    assert os.geteuid() == 0
    base = Path('/home/user/hos-r6/install-bundle')
    plan_path = base / 'partition-plan.json'
    assert hashlib.sha256(plan_path.read_bytes()).hexdigest() == '0adc8eb05714e651118c9a28de8c76aecea8b3d8b024c7b5a0b26a2360a678de'
    plan = json.loads(plan_path.read_text())
    spec = importlib.util.spec_from_file_location('baseline', base / 'install-baseline.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    _, _, header, entries = mod.read_gpt(Path('/dev/nvme0n1'), 1000215216)
    assert header['guid'] == plan['disk_guid']
    original_entries = (base / 'installation-backup/primary.bin').read_bytes()[1024:]
    for item in plan['existing_partitions_preserved']:
        n = item['number']
        assert entries[n] == original_entries[(n-1)*128:n*128]
    assert set(entries) == {i['number'] for i in plan['existing_partitions_preserved'] + plan['new_partitions']}
    devices = {}
    for item in plan['new_partitions']:
        e = entries[item['number']]
        assert str(uuid.UUID(bytes_le=e[16:32])) == item['guid']
        assert e[56:128].decode('utf-16-le').split('\0')[0] == item['name']
        assert struct.unpack_from('<QQ', e, 32) == (item['start_sector'], item['end_sector'])
        node = Path('/dev/disk/by-partuuid') / item['guid']
        assert node.resolve() == Path('/dev/nvme0n1p' + str(item['number']))
        assert stat.S_ISBLK(node.stat().st_mode)
        assert int(subprocess.check_output(['blockdev', '--getsize64', str(node)])) == item['size']
        assert subprocess.run(['findmnt', '-rn', '--source', str(node)], stdout=subprocess.DEVNULL).returncode == 1
        # Refuse mapped/held experimental partitions as well as direct mounts.
        assert not list((Path('/sys/class/block') / node.resolve().name / 'holders').iterdir())
        devices[item['name']] = node
    esp = Path('/boot/efi')
    assert subprocess.check_output(['findmnt', '-nro', 'SOURCE', '--target', str(esp)], text=True).strip() == '/dev/nvme0n1p1'
    package = Path('/home/user/hos-r9-hyperos/esp-package')
    assets = json.loads((package / 'manifest.json').read_text())
    assert set(assets) == {'gaokun3-hos/r9-hyperos/ramdisk.img',
                           'loader/entries/gaokun3-hos-hyperos-r9.conf'}
    for name, item in assets.items():
        assert mod.sha(esp / name) == item['sha256']
    default_before = (esp / 'loader/loader.conf').read_bytes()
    boot_before = {name: mod.sha(Path('/dev/disk/by-partlabel') / name) for name in ('boot_a', 'boot_b')}
    state = Path('/home/user/hos-r9-update') / args.image_sha256
    assert not (state / 'stream-started.json').exists(), 'partial/repeated update must be reviewed'
    assert mod.sha(devices['super_hos']) == args.old_image_sha256, 'unexpected current super; no write'
    size = 12884901888
    assert int(subprocess.check_output(['blockdev', '--getsize64', str(devices['super_hos'])])) == size
    if not args.apply:
        print('PREVIEW PASS: current super, GPT, unmapped partitions, ESP and boot assets verified')
        return
    state.mkdir(parents=True, exist_ok=True)
    record = {'old_sha256': args.old_image_sha256, 'new_sha256': args.image_sha256, 'size': size}
    (state / 'stream-started.json').write_text(json.dumps(record) + '\n')
    os.sync()
    print('STREAM READY; updating super_hos only', flush=True)
    digest, total = hashlib.sha256(), 0
    with devices['super_hos'].open('r+b', buffering=0) as target:
        while total < size:
            block = sys.stdin.buffer.read(min(4*1024*1024, size-total))
            if not block:
                raise RuntimeError('truncated update: do not boot HyperOS')
            digest.update(block)
            remaining = memoryview(block)
            while remaining:
                written = target.write(remaining)
                if not written:
                    raise RuntimeError('short device write')
                remaining = remaining[written:]
            total += len(block)
            if total % (1024**3) == 0:
                print(f'written {total // 1024**3} GiB', flush=True)
        assert sys.stdin.buffer.read(1) == b'', 'oversized stream'
        os.fsync(target.fileno())
    assert digest.hexdigest() == args.image_sha256, 'stream hash mismatch: do not boot'
    assert mod.sha(devices['super_hos']) == args.image_sha256, 'disk readback mismatch: do not boot'
    assert (esp / 'loader/loader.conf').read_bytes() == default_before
    for name, checksum in boot_before.items():
        assert mod.sha(Path('/dev/disk/by-partlabel') / name) == checksum
    for name, item in assets.items():
        assert mod.sha(esp / name) == item['sha256']
    record.update(verified=True,userdata_preserved=True,metadata_preserved=True,misc_preserved=True,
                  esp_unchanged=True,boot_partitions_unchanged=True,boot_tested=False)
    (state / 'installed.json').write_text(json.dumps(record, indent=2) + '\n')
    os.sync()
    print('SUPER UPDATE VERIFIED; no format, ESP change, or reboot', flush=True)


if __name__ == '__main__':
    main()
