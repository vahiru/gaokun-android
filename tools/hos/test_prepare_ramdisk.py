#!/usr/bin/env python3
"""Archive preservation and isolation checks; no device or proprietary input needed."""
import importlib.util
from pathlib import Path
import stat
import unittest

spec = importlib.util.spec_from_file_location("ramdisk", Path(__file__).with_name("prepare-ramdisk.py"))
ramdisk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ramdisk)


class RamdiskTests(unittest.TestCase):
    def test_archive_preserves_metadata_and_symlinks(self):
        entries = [
            ("init", [7, stat.S_IFREG | 0o750, 123, 456, 1, 100, 3, 0, 0, 0, 0, 5, 0], b"ELF"),
            ("link", [8, stat.S_IFLNK | 0o777, 0, 0, 1, 0, 4, 0, 0, 0, 0, 5, 0], b"init"),
        ]
        self.assertEqual(ramdisk.read_newc(ramdisk.write_newc(entries)), entries)

    def test_rejects_truncation_duplicate_and_traversal(self):
        values = [1, stat.S_IFREG | 0o644, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0]
        for entries in [[("../init", values, b"")], [("init", values, b""), ("./init", values, b"")]]:
            with self.assertRaises(ValueError):
                ramdisk.read_newc(ramdisk.write_newc(entries))
        with self.assertRaises(ValueError):
            ramdisk.read_newc(ramdisk.write_newc([("init", values, b"data")])[:115])

    def test_all_physical_mounts_are_isolated_vendor_stays_ext4(self):
        original = (Path(__file__).resolve().parents[2] / "device/huawei/gaokun3/fstab.gaokun3").read_bytes()
        result = ramdisk.isolated_fstab(original, "erofs")
        entries = [line.split() for line in result.decode().splitlines() if line and not line.startswith("#")]
        by_mount = {cols[1]: cols for cols in entries}
        for mount, label in (("/misc", "misc_hos"), ("/data", "userdata_hos"), ("/metadata", "metadata_hos")):
            self.assertEqual(by_mount[mount][0], f"/dev/block/by-name/{label}")
        for mount in ("/system", "/system_ext", "/product"):
            self.assertEqual(by_mount[mount][2:4], ["erofs", "ro"])
            self.assertIn("slotselect", by_mount[mount][4])
        self.assertEqual(by_mount["/vendor"][2], "ext4")
        for cols in entries:
            self.assertNotIn(cols[0], {"/dev/block/by-name/misc", "/dev/block/by-name/userdata", "/dev/block/by-name/metadata"})

    def test_missing_mount_and_slotselect_fail(self):
        with self.assertRaises(ValueError):
            ramdisk.isolated_fstab(b"system /system ext4 ro logical\n", "erofs")
        with self.assertRaises(ValueError):
            ramdisk.isolated_fstab(b"# empty\n", "ext4")


if __name__ == "__main__":
    unittest.main()
