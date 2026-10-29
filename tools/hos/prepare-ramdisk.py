#!/usr/bin/env python3
"""Prepare a local gzip/newc HOS debug ramdisk; never flashes or edits a boot image.

Preserves original cpio metadata, symlinks and file data without extracting to APFS.
References: init/first_stage_init.cpp:504-522, init/selinux.cpp:207-222;
device/huawei/gaokun3/fstab.gaokun3; init.gaokun3.rc:130-133.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat


def read_newc(raw):
    entries, names, pos = [], set(), 0
    while True:
        header = raw[pos:pos + 110]
        if len(header) != 110 or header[:6] != b"070701":
            raise ValueError(f"expected newc header at {pos}")
        values = [int(header[i:i + 8], 16) for i in range(6, 110, 8)]
        size, namesize = values[6], values[11]
        pos += 110
        namebytes = raw[pos:pos + namesize]
        if namesize < 1 or len(namebytes) != namesize or namebytes[-1:] != b"\0":
            raise ValueError("truncated/unterminated cpio name")
        name = namebytes[:-1].decode()
        pos = (pos + namesize + 3) & ~3
        data = raw[pos:pos + size]
        if len(data) != size:
            raise ValueError(f"truncated file: {name}")
        pos = (pos + size + 3) & ~3
        if name == "TRAILER!!!":
            if any(raw[pos:]):
                raise ValueError("extra archive/data after trailer is unsupported")
            return entries
        canonical = str(PurePosixPath(name))
        if name.startswith("/") or ".." in PurePosixPath(name).parts or canonical in names:
            raise ValueError(f"unsafe/duplicate cpio path: {name}")
        names.add(canonical)
        entries.append((name, values, data))


def write_newc(entries):
    out = bytearray()
    for name, original, data in entries + [("TRAILER!!!", [0] * 13, b"")]:
        values = original.copy()
        encoded = name.encode() + b"\0"
        values[6], values[11], values[12] = len(data), len(encoded), 0
        out += b"070701" + b"".join(f"{v:08x}".encode() for v in values) + encoded
        out += b"\0" * (-len(out) % 4)
        out += data
        out += b"\0" * (-len(out) % 4)
    out += b"\0" * (-len(out) % 512)
    return bytes(out)


def isolated_fstab(data, system_fs):
    lines, seen = [], set()
    physical = {"/metadata": "metadata_hos", "/data": "userdata_hos", "/misc": "misc_hos"}
    logical = {"/system", "/system_ext", "/product", "/vendor"}
    for line in data.decode().splitlines():
        cols = line.split()
        if not cols or line.lstrip().startswith("#"):
            lines.append(line)
            continue
        if len(cols) != 5:
            raise ValueError(f"unexpected fstab entry: {line}")
        mount = cols[1]
        if mount in physical:
            cols[0] = "/dev/block/by-name/" + physical[mount]
            seen.add(mount)
        elif mount in logical:
            if "logical" not in cols[4].split(",") or "slotselect" not in cols[4].split(","):
                raise ValueError(f"missing logical/slotselect: {line}")
            if mount != "/vendor":
                cols[2] = system_fs
                cols[3] = "ro" if system_fs == "erofs" else "ro,barrier=1"
            seen.add(mount)
        lines.append("  ".join(cols))
    if seen != set(physical) | logical:
        raise ValueError(f"missing expected mounts: {(set(physical) | logical) - seen}")
    return ("\n".join(lines) + "\n").encode()


def donor_extensions(fstab, donor):
    """Import only read-only donor overlays, never its physical disk entries.

    Keep extension partitions for HyperOS init's hardcoded property/rc paths.
    First-stage fstab filtering requires first_stage_mount even on overlays.
    """
    existing = [s.split() for s in fstab.decode().splitlines()
                if s.strip() and not s.lstrip().startswith('#')]
    if any(c[0] in ('mi_ext', 'mi_product', 'overlay') for c in existing):
        raise ValueError('extension mounts already present')
    if any(c[2] != 'erofs' for c in existing if c[1] in ('/system', '/system_ext', '/product')):
        raise ValueError('donor extensions require EROFS system partitions')
    overlays, seen = [], set()
    for line in donor.decode().splitlines():
        cols = line.split()
        if not cols or cols[0] != 'overlay':
            continue
        if len(cols) != 5 or cols[2] != 'overlay':
            raise ValueError(f'invalid donor overlay: {line}')
        target = cols[1]
        def safe(path):
            return path.startswith(('/system/', '/system_ext/', '/product/')) and '..' not in PurePosixPath(path).parts and str(PurePosixPath(path)) == path
        if not safe(target) or target in seen:
            raise ValueError(f'unsafe/duplicate overlay target: {target}')
        options = cols[3].split(',')
        if len(options) != 2 or options[0] != 'ro' or not options[1].startswith('lowerdir='):
            raise ValueError('only read-only lowerdir overlays are supported')
        lowers = options[1][9:].split(':')
        for lower in lowers:
            path = lower.rstrip('/')
            translated = path.removeprefix('/mnt/vendor/mi_ext').removeprefix('/mnt/vendor/mi_product')
            if not safe(translated):
                raise ValueError(f'unsafe overlay lowerdir: {lower}')
        if lowers[-1].rstrip('/') != target or len(lowers) < 2:
            raise ValueError('overlay must retain its original target as lowest layer')
        if set(cols[4].split(',')) - {'check', 'nofail', 'first_stage_mount'}:
            raise ValueError('unsupported donor overlay fs_mgr flags')
        cols[4] = 'first_stage_mount,nofail'
        overlays.append('  '.join(cols))
        seen.add(target)
    if not overlays:
        raise ValueError('donor fstab contains no overlays')
    mounts = [f'{name}  /mnt/vendor/{name}  erofs  ro  wait,logical,slotselect,first_stage_mount'
              for name in ('mi_ext', 'mi_product')]
    # FeatureParser and other framework clients use /mi_ext, whereas init uses
    # /mnt/vendor/mi_ext. The donor system root already contains /mi_ext.
    mounts.append('/mnt/vendor/mi_ext  /mi_ext  erofs  ro,bind  first_stage_mount')
    return fstab + ('\n# HyperOS extension partitions and donor overlay order\n' +
                    '\n'.join(mounts + overlays) + '\n').encode()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--input", type=Path, required=True, help="original gzip or uncompressed newc ramdisk")
    p.add_argument("--out", type=Path, required=True, help="new output directory")
    p.add_argument("--system-fs", choices=("ext4", "erofs"), required=True,
                   help="ext4 for original baseline, erofs for donor system side; vendor remains ext4")
    p.add_argument('--donor-fstab', type=Path, help='add mi_ext/mi_product and validated donor overlays')
    p.add_argument('--root-adb', action='store_true',
                   help='experimental debug ramdisk: start adbd as root without adbroot_service')
    a = p.parse_args()
    source = a.input.read_bytes()
    entries = read_newc(gzip.decompress(source) if source[:2] == b"\x1f\x8b" else source)
    lookup = {str(PurePosixPath(n)): (n, v, d) for n, v, d in entries}
    for name in ("init", "fstab.gaokun3"):
        if name not in lookup or not stat.S_ISREG(lookup[name][1][1]):
            p.error(f"missing regular file: {name}")
    if lookup["fstab.gaokun3"][1][4] != 1:
        p.error("hardlinked fstab is unsupported")
    for name in ("force_debuggable", "adb_debug.prop"):
        if name in lookup:
            p.error(f"input already contains {name}; use an unmodified baseline")
    fstab = isolated_fstab(lookup["fstab.gaokun3"][2], a.system_fs)
    if a.donor_fstab:
        fstab = donor_extensions(fstab, a.donor_fstab.read_bytes())
    result = [(n, v, fstab if str(PurePosixPath(n)) == "fstab.gaokun3" else d) for n, v, d in entries]
    # AOSP debug properties, plus a marker unaffected by crDroid's property overrides.
    props = b"ro.adb.secure=0\nro.debuggable=1\nro.force.debuggable=1\ndebug.gk3.dbgramdisk=1\nro.llk.enable=false\n"
    if a.root_adb:
        props += b"ro.secure=0\n"
    inode = max(v[0] for _, v, _ in entries) + 1
    for i, (name, data) in enumerate((("force_debuggable", b""), ("adb_debug.prop", props))):
        result.append((name, [inode + i, stat.S_IFREG | 0o644, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0], data))
    archive = write_newc(result)
    # Parse again before publishing; verify all metadata, allowing refreshed sizes/checksum.
    def normalized(items):
        output = []
        for name, original, data in items:
            values = original.copy()
            values[6], values[11], values[12] = len(data), len(name.encode()) + 1, 0
            output.append((name, values, data))
        return output

    if read_newc(archive) != normalized(result):
        raise ValueError("cpio round-trip verification failed")
    a.out.mkdir(parents=True, exist_ok=False)
    compressed = gzip.compress(archive, mtime=0)
    (a.out / "ramdisk.cpio.gz").write_bytes(compressed)
    (a.out / "fstab.gaokun3").write_bytes(fstab)
    (a.out / "cmdline.required.txt").write_text("androidboot.force_super_partition=super_hos\n")
    report = {"input": str(a.input.resolve()), "input_sha256": hashlib.sha256(source).hexdigest(),
              "root_adb_requested": a.root_adb,
              "ramdisk_sha256": hashlib.sha256(compressed).hexdigest(), "system_fs": a.system_fs,
              "changed": ["fstab.gaokun3", "force_debuggable", "adb_debug.prop"],
              "unchanged_entries": len(entries) - 1, "ready_to_boot": False,
              "remaining_requirements": ["Copy output fstab byte-for-byte to experimental vendor/etc/fstab.gaokun3",
                  "Create and verify super_hos, metadata_hos, userdata_hos, misc_hos in rescue environment",
                  "Set force_super_partition=super_hos and align slot suffix with experimental LP metadata",
                  "Isolate boot HAL ESP writes and disable experimental update_engine",
                  "Verify donor root mountpoint directories, EROFS support and mi_ext/mi_product integration",
                  "Package separate boot/ESP entry and arrange attended boot with early failure logging",
                  "Verify orange unlock state and debug ramdisk loading on device; adb root is not guaranteed"]}
    if a.donor_fstab:
        report['donor_fstab'] = {'path': str(a.donor_fstab.resolve()),
                                'sha256': hashlib.sha256(a.donor_fstab.read_bytes()).hexdigest()}
        report['required_logical_partitions'] = ['system_a', 'system_ext_a', 'product_a', 'vendor_a', 'mi_ext_a', 'mi_product_a']
        report['remaining_requirements'].append('Verify overlay lower directories, mountpoints and labels against full donor images; overlay mount errors are not fatal in first-stage init')
    (a.out / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Prepared {a.out}/ramdisk.cpio.gz; deployment requirements are in manifest.json")


if __name__ == "__main__":
    main()
