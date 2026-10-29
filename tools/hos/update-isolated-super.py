#!/usr/bin/env python3
"""Pinned, rescue-only super_hos replacement. Default: read-only preview.
Never formats, changes GPT/EFI/assets, or reboots. A start marker prohibits retries.
"""
import argparse
import errno
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import subprocess
import uuid

SECTORS = 1000215216
DISK_GUID = 'e6c13d1a-e678-468a-b6b6-b79f19efb0e5'
DISK = Path('/dev/nvme0n1')
TARGET = Path('/dev/nvme0n1p7')
START, END, SIZE = 814606336, 839772159, 12 * 1024**3
ESP = Path('/boot/efi')
MARKER = Path('/var/lib/gaokun-hos-super-update.started.json')
CHUNK = 4 * 1024**2

spec = importlib.util.spec_from_file_location('install_baseline', Path(__file__).with_name('install-baseline.py'))
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)


def digest(blob):
    return hashlib.sha256(blob).hexdigest()


def sha_fd(fd, size, offset=0):
    h = hashlib.sha256()
    for pos in range(0, size, CHUNK):
        data = os.pread(fd, min(CHUNK, size - pos), offset + pos)
        if len(data) != min(CHUNK, size - pos):
            raise ValueError('short protected read')
        h.update(data)
    return h.hexdigest()


def valid_hash(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise ValueError('expected lowercase SHA256')
    return value


def load_manifest(path, pin):
    valid_hash(pin)
    blob = path.read_bytes()
    if digest(blob) != pin:
        raise ValueError('manifest SHA pin mismatch')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate manifest key')
            result[key] = value
        return result
    m = json.loads(blob, object_pairs_hook=unique)
    if m['schema'] != 1 or m['disk_sectors'] != SECTORS or m['disk_guid'] != DISK_GUID:
        raise ValueError('unsupported disk identity/schema')
    t = m['target']
    if (t['number'], t['name'], t['start'], t['end'], t['size']) != (7, 'super_hos', START, END, SIZE):
        raise ValueError('target geometry not the fixed 12GiB super_hos')
    if str(uuid.UUID(t['uuid'])) != t['uuid'] or uuid.UUID(t['uuid']).int == 0:
        raise ValueError('invalid target UUID')
    for h in [m['gpt_primary_sha256'], m['gpt_secondary_sha256'], m['image_sha256'], t['old_sha256']]:
        valid_hash(h)
    if t['old_sha256'] == m['image_sha256']:
        raise ValueError('old and new images are identical')
    if not m['esp_assets'] or not any(n.startswith('loader/') for n in m['esp_assets']):
        raise ValueError('ESP assets must include loader inventory')
    for name, h in m['esp_assets'].items():
        if not name or str(PurePosixPath(name)) != name or PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts:
            raise ValueError('unsafe ESP asset path')
        valid_hash(h)
    return m


def entry_info(raw):
    return (struct.unpack_from('<QQ', raw, 32), str(uuid.UUID(bytes_le=raw[16:32])),
            raw[56:128].decode('utf-16-le').split('\0')[0])


def verify_gpt(m):
    primary, secondary, header, entries = baseline.read_gpt(DISK, SECTORS)
    if header['guid'] != DISK_GUID or digest(primary) != m['gpt_primary_sha256'] or digest(secondary) != m['gpt_secondary_sha256']:
        raise ValueError('GPT identity/hash mismatch')
    expected = {str(n): raw.hex() for n, raw in entries.items()}
    if expected != m['entries']:
        raise ValueError('GPT entries differ from reviewed bytes')
    bounds = []
    guids = set()
    for number, raw in entries.items():
        (start, end), guid, _ = entry_info(raw)
        if start < header['first'] or end > header['last'] or end < start or guid in guids:
            raise ValueError('invalid partition bounds/duplicate UUID')
        bounds.append((start, end)); guids.add(guid)
    bounds.sort()
    if any(a[1] >= b[0] for a, b in zip(bounds, bounds[1:])):
        raise ValueError('overlapping partitions')
    if entry_info(entries[7]) != ((START, END), m['target']['uuid'], 'super_hos'):
        raise ValueError('super_hos entry mismatch')
    protected = m['protected_partitions']
    if set(protected) != {'boot_a', 'boot_b'}:
        raise ValueError('must protect both boot_a and boot_b')
    numbers = set()
    for name, item in protected.items():
        number = item['number']
        fixed = {'boot_a': (5, 814344192, 814475263), 'boot_b': (6, 814475264, 814606335)}[name]
        if number != fixed[0] or number not in entries or entry_info(entries[number])[0] != fixed[1:]:
            raise ValueError('protected boot geometry changed')
        if number == 7 or number in numbers or number not in entries or entry_info(entries[number])[2] != name:
            raise ValueError('invalid protected boot partition')
        valid_hash(item['sha256']); numbers.add(number)
    return entries


def block_identity(path):
    info = path.stat()
    if not stat.S_ISBLK(info.st_mode):
        raise ValueError('not a block device: ' + str(path))
    return info.st_rdev


def verify_sysfs_dev(name, dev):
    identity = Path('/sys/class/block') / name / 'dev'
    if identity.read_text().strip() != f'{os.major(dev)}:{os.minor(dev)}':
        raise ValueError('sysfs/dev node major:minor mismatch: ' + name)


def mounted_device(dev, mountinfo):
    identity = f'{os.major(dev)}:{os.minor(dev)}'
    return any(len(line.split()) > 2 and line.split()[2] == identity for line in mountinfo.splitlines())


def reject_started():
    if MARKER.exists() or MARKER.is_symlink():
        raise ValueError('start marker exists: retry/partial write requires independent recovery review')


def verify_environment(m):
    reject_started()
    if os.geteuid() != 0:
        raise ValueError('root rescue environment required even for full preview')
    source = subprocess.check_output(['findmnt', '-nro', 'SOURCE', '--target', '/'], text=True).strip()
    if source != '/dev/nvme0n1p3':
        raise ValueError('root is not the approved rescue partition p3')
    disk_dev = block_identity(DISK)
    dev = block_identity(TARGET)
    verify_sysfs_dev(DISK.name, disk_dev)
    verify_sysfs_dev(TARGET.name, dev)
    if int(subprocess.check_output(['blockdev', '--getsize64', str(DISK)])) != SECTORS * 512:
        raise ValueError('physical disk size mismatch')
    if int(subprocess.check_output(['blockdev', '--getss', str(DISK)])) != 512:
        raise ValueError('physical sector size mismatch')
    if int(subprocess.check_output(['blockdev', '--getsize64', str(TARGET)])) != SIZE:
        raise ValueError('target block size mismatch')
    sys = Path('/sys/class/block/nvme0n1p7')
    if int((sys / 'start').read_text()) != START or int((sys / 'size').read_text()) != SIZE // 512 or int((sys / 'partition').read_text()) != 7:
        raise ValueError('sysfs partition geometry mismatch')
    if (sys.resolve().parent.name != 'nvme0n1' or list((sys / 'holders').iterdir())):
        raise ValueError('target parent mismatch or active holders')
    link = Path('/dev/disk/by-partuuid') / m['target']['uuid']
    if link.resolve(strict=True) != TARGET or block_identity(link) != dev:
        raise ValueError('PARTUUID does not resolve to p7')
    for info in Path('/proc').glob('[0-9]*/mountinfo'):
        try:
            mounted = mounted_device(dev, info.read_text())
        except OSError as error:
            if error.errno in (errno.ENOENT, errno.ESRCH) and not info.parent.exists():
                continue # Only skip confirmed process disappearance.
            raise
        if mounted:
            raise ValueError('target mounted in a process mount namespace')
    esp_source = subprocess.check_output(['findmnt', '-nro', 'SOURCE', '--target', str(ESP)], text=True).strip()
    if esp_source != '/dev/nvme0n1p1' or not os.path.ismount(ESP):
        raise ValueError('ESP not mounted from p1')
    return dev


def verify_protected(m, entries):
    fd = os.open(DISK, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        for item in m['protected_partitions'].values():
            (start, end), _, _ = entry_info(entries[item['number']])
            if sha_fd(fd, (end - start + 1) * 512, start * 512) != item['sha256']:
                raise ValueError('protected boot image changed')
    finally:
        os.close(fd)
    # The manifest inventories the entire ESP, including all loader files.
    files = {}
    for p in ESP.rglob('*'):
        if p.is_symlink():
            raise ValueError('ESP symlink not allowed')
        if p.is_file():
            files[p.relative_to(ESP).as_posix()] = baseline.sha(p)
    if files != m['esp_assets']:
        raise ValueError('ESP/loader inventory or content changed')


def read_exact(fd, count):
    data = bytearray()
    while len(data) < count:
        part = os.read(fd, count - len(data))
        if not part:
            raise ValueError('truncated stdin stream: target may be partial, do not retry')
        data.extend(part)
    return bytes(data)


def stream_replace(source_fd, target_fd, size, expected, *, sequential=False):
    """Bounded overwrite, never truncates; validates streamed bytes before success."""
    h = hashlib.sha256()
    for pos in range(0, size, CHUNK):
        count = min(CHUNK, size - pos)
        data = read_exact(source_fd, count) if sequential else os.pread(source_fd, count, pos)
        if len(data) != count:
            raise ValueError('short image stream: target is partial, do not retry')
        h.update(data)
        done = 0
        while done < len(data):
            written = os.pwrite(target_fd, data[done:], pos + done)
            if written <= 0:
                raise OSError('short/zero target write')
            done += written
    os.fsync(target_fd)
    if sequential and os.read(source_fd, 1):
        raise ValueError('stdin exceeds fixed partition size: extra bytes were not written')
    if h.hexdigest() != expected:
        raise ValueError('image mutated during stream: target needs recovery review')
    if stat.S_ISBLK(os.fstat(target_fd).st_mode):
        fcntl.ioctl(target_fd, 0x1261) # BLKFLSBUF: invalidate block cache before full readback.
    if sha_fd(target_fd, size) != expected:
        raise ValueError('whole-partition readback mismatch')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--manifest-sha256', required=True)
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument('--image', type=Path)
    source.add_argument('--stdin', action='store_true', help='stream raw image only on apply; preview never reads stdin')
    p.add_argument('--apply', action='store_true')
    a = p.parse_args()
    m = load_manifest(a.manifest, a.manifest_sha256)
    dev = verify_environment(m)
    entries = verify_gpt(m)
    verify_protected(m, entries)
    image_fd = None
    target_fd = None
    try:
        if not a.stdin:
            image_fd = os.open(a.image, os.O_RDONLY | os.O_NOFOLLOW)
            info = os.fstat(image_fd)
            if not stat.S_ISREG(info.st_mode) or info.st_size != SIZE or sha_fd(image_fd, SIZE) != m['image_sha256']:
                raise ValueError('new image type/size/hash mismatch')
        target_fd = os.open(TARGET, (os.O_RDWR | os.O_EXCL if a.apply else os.O_RDONLY) | os.O_NOFOLLOW)
        if os.fstat(target_fd).st_rdev != dev or not stat.S_ISBLK(os.fstat(target_fd).st_mode):
            raise ValueError('target identity changed at open')
        if sha_fd(target_fd, SIZE) != m['target']['old_sha256']:
            raise ValueError('old super hash differs; refuse partial/repeated update')
        verify_environment(m)
        verify_gpt(m)
        verify_protected(m, entries)
        print(json.dumps({'mode': 'apply' if a.apply else 'preview', 'target': str(TARGET),
                          'bytes': SIZE, 'new_sha256': m['image_sha256'], 'reboot': False,
                          'incoming_bytes_preverified': not a.stdin,
                          'input': 'stdin (unverified until stream/readback complete)' if a.stdin else str(a.image)}), flush=True)
        if not a.apply:
            return
        MARKER.parent.mkdir(parents=True, exist_ok=True)
        marker_source = subprocess.check_output(['findmnt', '-nro', 'SOURCE', '--target', str(MARKER.parent)], text=True).strip()
        if marker_source != '/dev/nvme0n1p3':
            raise ValueError('start marker must persist on the rescue partition')
        marker_fd = os.open(MARKER, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(marker_fd, 'w') as marker:
            json.dump({'manifest_sha256': a.manifest_sha256, 'status': 'started',
                       'old': m['target']['old_sha256'], 'new': m['image_sha256']}, marker)
            marker.flush(); os.fsync(marker.fileno())
        parent_fd = os.open(MARKER.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
        try:
            if a.stdin:
                image_fd = os.dup(0)
            stream_replace(image_fd, target_fd, SIZE, m['image_sha256'], sequential=a.stdin)
        finally:
            # Even a failed/partial stream must audit the protected regions.
            verify_gpt(m)
            verify_protected(m, entries)
        print('VERIFIED: super_hos replaced; GPT, boot_a/boot_b and entire ESP unchanged. Marker retained. No reboot.')
    finally:
        if image_fd is not None:
            os.close(image_fd)
        if target_fd is not None:
            os.close(target_fd)


if __name__ == '__main__':
    main()
