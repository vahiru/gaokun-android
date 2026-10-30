#!/usr/bin/python3
"""Dedicated rescue boot: claim atomically or reserve an unclaimed reboot.
A claim and fallback compete for the same O_EXCL marker. This is not an updater
lock: the update tool must still enforce its own exclusive partition/marker rules.
No partition, BCB, EFI default or oneshot is changed by this helper.
"""
import errno
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import time
import uuid

FLAG = 'gaokun3.hos_rescue_device=1'
CLAIM = Path('/run/gaokun3-hos-rescue-session.claimed')
PROC = Path('/proc')
BLOCK_TARGETS = ('/dev/nvme0n1p7', '/dev/nvme0n1')


class RebootRequestUncertain(RuntimeError):
    pass


def identity_ok():
    source = subprocess.check_output(['findmnt', '-nro', 'SOURCE', '/'], text=True, timeout=5).strip()
    flags = [x for x in Path('/proc/cmdline').read_text().split()
             if x == 'gaokun3.hos_rescue_device' or x.startswith('gaokun3.hos_rescue_device=')]
    return os.geteuid() == 0 and socket.gethostname() == 'gaokun3-rescue' and source == '/dev/nvme0n1p3' and flags == [FLAG]


def boot_id():
    value = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    if str(uuid.UUID(value)) != value:
        raise RuntimeError('noncanonical boot ID')
    return value


def claim_present():
    # lexists() suppresses inspection errors, including PermissionError. Only
    # actual ENOENT is evidence of absence; all other failures inhibit reboot.
    try:
        os.lstat(CLAIM)
    except OSError as exc:
        if exc.errno == errno.ENOENT:
            return False
        raise
    return True


def _regular_root_private(info):
    return (stat.S_ISREG(info.st_mode) and info.st_uid == 0 and info.st_gid == 0
            and stat.S_IMODE(info.st_mode) == 0o600)


def create_marker(payload):
    """Return False if another participant won. Never remove a partial marker."""
    try:
        fd = os.open(CLAIM, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    except FileExistsError:
        return False
    try:
        os.fchown(fd, 0, 0)
        os.fchmod(fd, 0o600)
        if not _regular_root_private(os.fstat(fd)):
            raise RuntimeError('new marker ownership/mode verification failed')
        remaining = memoryview(payload)
        while remaining:
            count = os.write(fd, remaining)
            if count <= 0:
                raise OSError(errno.EIO, 'short marker write')
            remaining = remaining[count:]
        os.fsync(fd)
    finally:
        os.close(fd)
    parent = os.open(CLAIM.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        os.fsync(parent)
    finally:
        os.close(parent)
    return True


def verify_existing_claim(expected):
    before = os.lstat(CLAIM)
    if not _regular_root_private(before) or before.st_size != len(expected):
        raise RuntimeError('existing claim is not a complete root:root0600 current-boot claim')
    fd = os.open(CLAIM, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        current = os.fstat(fd)
        if (not _regular_root_private(current) or current.st_size != len(expected)
                or (before.st_dev, before.st_ino) != (current.st_dev, current.st_ino)):
            raise RuntimeError('claim changed during inspection')
        if os.read(fd, len(expected) + 1) != expected:
            raise RuntimeError('claim belongs to another boot or contains a reboot reservation')
    finally:
        os.close(fd)


def claim_session(boot):
    expected = (boot + '\n').encode('ascii')
    if not create_marker(expected):
        verify_existing_claim(expected)
    print('Rescue session claimed for current boot; automatic fallback inhibited.', flush=True)


def update_active():
    targets = set()
    for target in BLOCK_TARGETS:
        info = os.stat(target)
        if not stat.S_ISBLK(info.st_mode):
            raise RuntimeError('expected block device: ' + target)
        targets.add(info.st_rdev)
    for proc in PROC.iterdir():
        if not proc.name.isdigit():
            continue
        try:
            args = (proc/'cmdline').read_bytes().split(b'\0')
            if any(os.path.basename(x) == b'update-isolated-super.py' for x in args):
                return True
            for fd in (proc/'fd').iterdir():
                try:
                    # /proc/PID/fd magic links retain device identity even for
                    # aliases, alternate mount paths or an unlinked device node.
                    info = os.stat(fd)
                    if stat.S_ISBLK(info.st_mode) and info.st_rdev in targets:
                        return True
                except OSError as exc:
                    if exc.errno in (errno.ENOENT, errno.ESRCH):
                        continue  # descriptor/process exited while being inspected
                    raise
        except OSError as exc:
            if exc.errno in (errno.ENOENT, errno.ESRCH):
                continue
            raise
    return False


def unclaimed_fallback(boot):
    print('Unclaimed rescue fallback armed for600s; use --claim before update preparation.', flush=True)
    time.sleep(600)
    if not identity_ok() or boot_id() != boot:
        print('Fallback held: identity/boot changed.', flush=True)
        return
    if claim_present() or update_active():
        print('Fallback held: claim or active block-device/update I/O.', flush=True)
        return
    # This O_EXCL is the decision boundary, not another racy existence check.
    # A later controller cannot accept this payload as its current-boot claim.
    if not create_marker(('reboot-requested:' + boot + '\n').encode('ascii')):
        print('Fallback held: a claimant won atomic arbitration.', flush=True)
        return
    # Even if these checks fail, keep the reservation. Never allow a controller
    # to start writing after an uncertain reboot decision by clearing the marker.
    if not identity_ok() or boot_id() != boot or update_active():
        print('Fallback held after reservation; marker retained for inspection.', flush=True)
        return
    print('Unclaimed rescue reserved; requesting normal reboot without changing boot settings.', flush=True)
    try:
        subprocess.run(['systemctl', '--no-block', 'reboot'], check=True, timeout=10)
    except Exception as exc:
        # A timed-out/failed client may already have queued a shutdown. Do not
        # claim it was definitely held, and never remove the blocking intent.
        raise RebootRequestUncertain(str(exc)) from exc


def main():
    if sys.argv[1:] not in ([], ['--claim']):
        raise SystemExit('Only no arguments or --claim is supported')
    try:
        if not identity_ok():
            raise RuntimeError('not the dedicated rescue boot')
        boot = boot_id()
        if sys.argv[1:] == ['--claim']:
            claim_session(boot)
        else:
            unclaimed_fallback(boot)
    except RebootRequestUncertain as exc:
        print('Reboot request outcome uncertain; reservation retained, do not update: '+str(exc), file=sys.stderr, flush=True)
        raise SystemExit(1)
    except Exception as exc:
        print('Claim refused or fallback held; guard/inspection failed, any marker retained: '+str(exc), file=sys.stderr, flush=True)
        raise SystemExit(1)


if __name__ == '__main__':
    main()
