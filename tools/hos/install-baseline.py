#!/usr/bin/env python3
"""Review or explicitly apply one fixed isolated baseline installation bundle.

Default is read-only preview. --rehearse-gpt modifies ONLY a regular sparse disk
image and never formats partitions. --apply requires a real block device, root,
exact old GPT hashes, new partition GUIDs and a verified manifest. Never reboots,
sets EFI variables, changes existing partitions, or overwrites an existing entry.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import uuid
import zlib


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(4*1024*1024), b''):
            h.update(b)
    return h.hexdigest()


def read_gpt(disk, sectors):
    with disk.open('rb') as f:
        primary = f.read(34*512)
        f.seek((sectors-33)*512)
        secondary = f.read(33*512)
    if primary[510:512] != b'\x55\xaa':
        raise ValueError('invalid protective MBR')
    headers = []
    for block in (primary[512:1024], secondary[-512:]):
        if block[:8] != b'EFI PART':
            raise ValueError('invalid GPT signature')
        size, crc = struct.unpack_from('<II', block, 12)
        if size < 92 or size > 512:
            raise ValueError('invalid GPT header size')
        clean = bytearray(block[:size]); clean[16:20] = bytes(4)
        if zlib.crc32(clean) != crc:
            raise ValueError('GPT header CRC mismatch')
        lba, other, first, last = struct.unpack_from('<QQQQ', block, 24)
        table_lba, count, entry_size, table_crc = struct.unpack_from('<QIII', block, 72)
        if (count, entry_size) != (128, 128):
            raise ValueError('unsupported GPT array geometry')
        headers.append(dict(lba=lba, other=other, first=first, last=last,
                            table_lba=table_lba, table_crc=table_crc,
                            guid=str(uuid.UUID(bytes_le=block[56:72]))))
    a, b = headers
    if ((a['lba'], a['other'], b['lba'], b['other']) != (1, sectors-1, sectors-1, 1)
            or a['table_lba'] != 2 or b['table_lba'] != sectors-33
            or a['first'] != b['first'] or a['last'] != b['last'] or a['guid'] != b['guid']):
        raise ValueError('inconsistent GPT headers')
    table = primary[1024:]
    if table != secondary[:-512] or zlib.crc32(table) != a['table_crc'] or a['table_crc'] != b['table_crc']:
        raise ValueError('GPT arrays differ or fail CRC')
    entries = {}
    for i in range(128):
        entry = table[i*128:(i+1)*128]
        if entry[:16] != bytes(16):
            entries[i+1] = entry
    return primary, secondary, a, entries


def validate_plan(plan, header, entries):
    if plan['disk_guid'].lower() != header['guid'] or plan['sector_size'] != 512:
        raise ValueError('disk identity mismatch')
    existing = {v['number']: v for v in plan['existing_partitions_preserved']}
    if set(existing) != set(entries):
        raise ValueError('existing partition set differs from approved input')
    intervals = []
    all_guids = set()
    for number, entry in entries.items():
        start, end = struct.unpack_from('<QQ', entry, 32)
        name = entry[56:128].decode('utf-16-le').split('\0')[0]
        guid = str(uuid.UUID(bytes_le=entry[16:32]))
        old = existing[number]
        if ((start, end, name, guid) != (old['start'], old['end'], old['name'], old['guid'])
                or str(uuid.UUID(bytes_le=entry[:16])) != old['type_guid']):
            raise ValueError('original partition differs: ' + str(number))
        intervals.append((start, end, number)); all_guids.add(guid)
    required = {'super_hos': 12*1024**3, 'metadata_hos': 32*1024**2,
                'misc_hos': 4*1024**2, 'userdata_hos': 32*1024**3}
    if len(plan['new_partitions']) != 4 or {v['name'] for v in plan['new_partitions']} != set(required):
        raise ValueError('unexpected proposed partition set')
    numbers = set(entries)
    for item in plan['new_partitions']:
        n, start, end = item['number'], item['start_sector'], item['end_sector']
        guid = str(uuid.UUID(item['guid']))
        if (n in numbers or not 1 <= n <= 128 or guid in all_guids or uuid.UUID(guid).int == 0
                or start % 2048 or (end+1) % 2048 or start < header['first'] or end > header['last']
                or (end-start+1)*512 != required[item['name']] or item['size'] != required[item['name']]):
            raise ValueError('unsafe new partition: ' + item['name'])
        numbers.add(n); all_guids.add(guid); intervals.append((start, end, n))
    intervals.sort()
    if any(a[1] >= b[0] for a, b in zip(intervals, intervals[1:])):
        raise ValueError('partition overlap')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bundle', required=True, type=Path)
    p.add_argument('--disk', required=True, type=Path)
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--apply', action='store_true')
    mode.add_argument('--rehearse-gpt', action='store_true')
    a = p.parse_args()
    root = a.bundle.resolve()
    manifest = json.loads((root/'manifest.json').read_text())
    plan = json.loads((root/'partition-plan.json').read_text())
    expected_esp = {'gaokun3-hos/r6-baseline/' + n for n in ('Image', 'gaokun3.dtb', 'ramdisk.img')}
    expected_esp.add('loader/entries/gaokun3-hos-baseline-r6.conf')
    if set(manifest['esp_files']) != expected_esp:
        p.error('unexpected ESP installation paths')
    if hashlib.sha256((root/'partition-plan.json').read_bytes()).hexdigest() != manifest['plan_sha256']:
        p.error('partition plan is not bound to bundle manifest')
    expected_assets = {'super_hos.raw.img'} | {'esp/' + n for n in expected_esp}
    if set(manifest['assets']) != expected_assets:
        p.error('unexpected bundle asset set')
    disk = a.disk.resolve()
    regular = stat.S_ISREG(disk.stat().st_mode)
    if a.rehearse_gpt and not regular:
        p.error('GPT rehearsal accepts only a regular disk image')
    if a.apply and (os.geteuid() != 0 or not stat.S_ISBLK(disk.stat().st_mode)):
        p.error('apply requires root and a block device')
    sectors = manifest['disk_sectors']
    if regular:
        size = disk.stat().st_size
    else:
        size = int(subprocess.check_output(['blockdev', '--getsize64', str(disk)]))
        if int(subprocess.check_output(['blockdev', '--getss', str(disk)])) != 512:
            p.error('expected 512-byte logical sectors')
    if size != sectors*512:
        p.error('disk size mismatch')
    primary, secondary, header, entries = read_gpt(disk, sectors)
    for blob, name in ((primary, 'primary'), (secondary, 'secondary')):
        if hashlib.sha256(blob).hexdigest() != manifest['gpt_'+name+'_sha256']:
            p.error('GPT differs from pre-install backup; refusing retry/reformat')
    validate_plan(plan, header, entries)
    # Rehearsal verifies GPT inputs only; apply/preview also bind all install assets.
    if not a.rehearse_gpt:
        for name, item in manifest['assets'].items():
            path = root/name
            if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root):
                p.error('unsafe bundle asset: ' + name)
            if path.stat().st_size != item['size'] or sha(path) != item['sha256']:
                p.error('bundle asset mismatch: ' + name)
    cmd = ['sgdisk']
    for item in plan['new_partitions']:
        n = str(item['number'])
        cmd += [f'--new={n}:{item["start_sector"]}:{item["end_sector"]}',
                f'--typecode={n}:8300', f'--change-name={n}:{item["name"]}',
                f'--partition-guid={n}:{item["guid"]}']
    cmd += [str(disk)]
    print(json.dumps({'mode': 'apply' if a.apply else 'rehearse-gpt' if a.rehearse_gpt else 'preview',
                      'gpt_command': cmd, 'new_partitions': plan['new_partitions'],
                      'formats': ['metadata_hos', 'userdata_hos'],
                      'images': ['super_hos', 'zero-initialized misc_hos'],
                      'esp_files': list(manifest['esp_files']), 'reboots': False}, indent=2), flush=True)
    if not a.apply and not a.rehearse_gpt:
        return
    if a.apply:
        esp = Path('/boot/efi')
        mounted = subprocess.check_output(['findmnt', '-nro', 'SOURCE', '--target', str(esp)], text=True).strip()
        if Path(mounted).resolve() != Path(str(disk)+'p1') or not os.path.ismount(esp):
            p.error('ESP mount does not belong to the expected disk')
        for name in manifest['esp_files']:
            if (esp/name).exists():
                p.error('refusing to overwrite an existing ESP asset: ' + name)
        backup = root/'installation-backup'
        backup.mkdir(exist_ok=False)
        (backup/'primary.bin').write_bytes(primary); (backup/'secondary.bin').write_bytes(secondary)
        subprocess.run(['sgdisk', '--backup='+str(backup/'original.gpt'), str(disk)], check=True)
    subprocess.run(cmd, check=True)
    _, _, _, after = read_gpt(disk, sectors)
    for number, old in entries.items():
        if after.get(number) != old:
            raise RuntimeError('original GPT entry changed; stop and restore from backup')
    if set(after) != set(entries) | {v['number'] for v in plan['new_partitions']}:
        raise RuntimeError('unexpected resulting partition set')
    for item in plan['new_partitions']:
        e = after[item['number']]
        if (struct.unpack_from('<QQ', e, 32) != (item['start_sector'], item['end_sector'])
                or str(uuid.UUID(bytes_le=e[16:32])) != item['guid']
                or e[56:128].decode('utf-16-le').split('\0')[0] != item['name']):
            raise RuntimeError('new GPT entry mismatch')
    subprocess.run(['sgdisk', '--verify', str(disk)], check=True)
    if a.rehearse_gpt:
        print('REHEARSAL PASS: all original GPT entries byte-identical; new entries verified')
        return
    journal = []
    def run(args):
        subprocess.run([str(v) for v in args], check=True)
        journal.append([str(v) for v in args])
        (root/'installation-journal.json').write_text(json.dumps(journal, indent=2)+'\n')
    run(['partprobe', disk]); run(['udevadm', 'settle', '--timeout=15'])
    devices = {}
    for item in plan['new_partitions']:
        node = Path('/dev/disk/by-partuuid')/item['guid']
        if (node.resolve() != Path(str(disk)+'p'+str(item['number']))
                or int(subprocess.check_output(['blockdev', '--getsize64', str(node)])) != item['size']):
            raise RuntimeError('kernel device identity/size mismatch')
        if subprocess.run(['findmnt', '-rn', '--source', str(node)], stdout=subprocess.DEVNULL).returncode != 1:
            raise RuntimeError('new partition is mounted or mount status unknown')
        devices[item['name']] = node
    for name in ('metadata_hos', 'userdata_hos'):
        run(['mkfs.ext4', '-F', '-b', '4096', '-L', name, '-E',
             'lazy_itable_init=0,lazy_journal_init=0', devices[name]])
        run(['e2fsck', '-f', '-n', devices[name]])
    run(['dd', 'if=/dev/zero', 'of='+str(devices['misc_hos']), 'bs=1M', 'count=4', 'conv=fsync', 'status=none'])
    run(['dd', 'if='+str(root/'super_hos.raw.img'), 'of='+str(devices['super_hos']),
         'bs=4M', 'conv=fsync', 'status=progress'])
    if sha(devices['super_hos']) != manifest['assets']['super_hos.raw.img']['sha256']:
        raise RuntimeError('written super hash mismatch; do not boot')
    # Publish the menu entry last, after all assets have passed readback.
    for name in sorted(manifest['esp_files'], key=lambda n: n.endswith('.conf')):
        target = esp/name
        target.parent.mkdir(parents=True, exist_ok=True)
        with (root/'esp'/name).open('rb') as source, target.open('xb') as dest:
            shutil.copyfileobj(source, dest)
            dest.flush(); os.fsync(dest.fileno())
        if sha(target) != manifest['assets']['esp/'+name]['sha256']:
            raise RuntimeError('ESP readback mismatch')
    os.sync()
    (root/'installed.json').write_text(json.dumps({'installed': True, 'boot_tested': False,
                                                'default_changed': False, 'oneshot_set': False}, indent=2)+'\n')
    print('INSTALLATION VERIFIED. No default/oneshot changes or reboot performed.')


if __name__ == '__main__':
    main()
