#!/usr/bin/env python3
"""Verify and stage the ignored fingerprint firmware.

Offline only. Does not build, sync, flash, or contact a device.
The TA comes either from QcTreeExtOem8280.cab of the Windows driver release
(--ta) or from the private handoff ZIP (--handoff, used when --ta is absent).
The Goodix configuration template comes from gfspi.dll of the same release
(uup-drivers-sc8280xp 200.0.10.0, gfspi.cab).
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TA_HASH = "b081543c7b6ae4360969007836747601e8c48ec48a8335a5479e45ec0e34355a"
# Sensor type 14 template (chips 0x2507/0x2508/0x2510), 224 bytes at
# 0x1800b85a0 + 14 * 0xe0 in gfspi.dll 1.1.143.622. The kernel patches it from
# the unit's OTP and accepts only this template.
GOODIX_CFG_VA = 0x1800B85A0 + 14 * 0xE0
GOODIX_CFG_LEN = 0xE0
GOODIX_CFG_HASH = "273537378e759cc19b829c6c5a3b5226da603b55f96be6bcdf9a2e66c6c26725"


def atomic_copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=target.name + ".", dir=target.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(source)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, target)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def goodix_template(dll):
    """Map the template's VA through the PE section table."""
    if dll[:2] != b"MZ":
        raise ValueError("gfspi.dll is not a PE image")
    pe = struct.unpack_from("<I", dll, 0x3C)[0]
    if dll[pe:pe + 4] != b"PE\0\0":
        raise ValueError("gfspi.dll is not a PE image")
    sections = struct.unpack_from("<H", dll, pe + 6)[0]
    optional = struct.unpack_from("<H", dll, pe + 20)[0]
    image_base = struct.unpack_from("<Q", dll, pe + 24 + 24)[0]
    entry = pe + 24 + optional
    for _ in range(sections):
        vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", dll, entry + 8)
        start = image_base + vaddr
        if start <= GOODIX_CFG_VA and GOODIX_CFG_VA + GOODIX_CFG_LEN <= start + min(vsize, rsize):
            off = raw + GOODIX_CFG_VA - start
            cfg = dll[off:off + GOODIX_CFG_LEN]
            if hashlib.sha256(cfg).hexdigest() != GOODIX_CFG_HASH:
                raise ValueError("unexpected gfspi.dll revision (Goodix template)")
            return cfg
        entry += 40
    raise ValueError("Goodix template not backed by file data")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ta", type=Path,
                        help="fingerpr.mbn from the Windows driver release (QcTreeExtOem8280.cab)")
    parser.add_argument("--handoff", type=Path,
                        default=ROOT / "gaokun3-fingerprint-driver-handoff-20261101.zip",
                        help="private handoff ZIP; read only when --ta is not given")
    parser.add_argument("--gfspi", type=Path, default=ROOT / "gfspi.dll",
                        help="gfspi.dll from the Windows driver release (gfspi.cab)")
    parser.add_argument("--device-tree", type=Path, default=ROOT / "device/huawei/gaokun3")
    args = parser.parse_args()
    if args.ta:
        ta = args.ta.read_bytes()
    else:
        with zipfile.ZipFile(args.handoff) as z:
            # Read known members only; never extract archive-controlled paths.
            for line in z.read("SHA256SUMS").decode().splitlines():
                digest, name = line.split(None, 1)
                data = z.read(name.lstrip("*"))
                if hashlib.sha256(data).hexdigest() != digest:
                    raise ValueError("handoff checksum mismatch: " + name)
            ta = z.read("firmware/fingerpr.mbn")
    if hashlib.sha256(ta).hexdigest() != TA_HASH:
        raise ValueError("unexpected TA revision")
    goodix_cfg = goodix_template(args.gfspi.read_bytes())
    # Budget for a staged copy AND atomic-write temporary files, leaving 2 GiB.
    args.device_tree.mkdir(parents=True, exist_ok=True)
    need = 2 * (len(ta) + len(goodix_cfg)) + 2 * 1024**3
    if shutil.disk_usage(args.device_tree).free < need:
        raise OSError("insufficient free space for staging plus 2 GiB reserve")
    dest = args.device_tree / "firmware/fingerpr.mbn"
    atomic_copy(ta, dest)
    print("Verified/staged ignored TA:", dest)
    dest = args.device_tree / "firmware/gaokun3-goodix-cfg.bin"
    atomic_copy(goodix_cfg, dest)
    print("Verified/staged ignored Goodix template:", dest)
    print("Free space after staging:", shutil.disk_usage(args.device_tree).free, "bytes")


if __name__ == "__main__":
    main()
