"""Optional HAL inputs fail before image copying; default path stays unchanged."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('vendor', Path(__file__).with_name('prepare-vendor.py'))
vendor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vendor)


def elf():
    data = bytearray(128)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHIQQQIHHH', data, 16, 3, 183, 1, 120, 64, 0, 0, 64, 56, 1)
    struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0, 0, 128, 128, 4096)
    return data


class HalInputsTest(unittest.TestCase):
    def test_default_does_not_add_replacements_or_inputs(self):
        self.assertEqual(vendor.optional_hal_replacements(), ([], {}))

    def test_reject_wrong_arch_endian_truncation_and_nonexecutable(self):
        vendor.validate_arm64_executable(elf())
        invalid = [b'', bytes(elf()[:63]), bytes(elf()[:-1])]
        for offset, fmt, value in [(18, '<H', 62), (5, 'B', 2), (16, '<H', 1),
                                   (54, '<H', 0), (68, '<I', 4), (24, '<Q', 0),
                                   (96, '<Q', 129), (104, '<Q', 1)]:
            data = elf()
            struct.pack_into(fmt, data, offset, value)
            invalid.append(data)
        for data in invalid:
            with self.subTest(data=data[:24]), self.assertRaises(ValueError):
                vendor.validate_arm64_executable(data)

    def test_complete_usb_group_and_hashes_preserve_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            contents = {'android.hardware.usb-service.gaokun3': bytes(elf()),
                        'gaokun3-usbrole.sh': b'#!/system/bin/sh\n# optional policy only\n',
                        'usbrole.rc': b'on property:persist.vendor.usb.device_only=1\n    stop usbrole\n'}
            for name, data in contents.items():
                (root / name).write_bytes(data)
            replacements, hashes = vendor.optional_hal_replacements(usb_dir=root)
            self.assertEqual(len(replacements), 3)
            for remote, content, label in replacements:
                name = next(n for n, target in vendor.HAL_TARGETS.items() if target == remote)
                self.assertEqual(content, contents[name])
                self.assertIsNone(label) # Invoke existing inode/xattr inheritance branch.
                self.assertEqual(hashes[str((root / name).resolve())], hashlib.sha256(content).hexdigest())
            for name in contents:
                path = root / name
                original = path.read_bytes()
                path.unlink()
                with self.assertRaises(ValueError):
                    vendor.optional_hal_replacements(usb_dir=root)
                path.write_bytes(original)

    def test_hexagonrpcd_only_replaces_existing_daemon_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'hexagonrpcd'
            source.write_bytes(elf())
            baseline = vendor.optional_hal_replacements()
            replacements, hashes = vendor.optional_hal_replacements(hexagonrpcd=source)
            self.assertEqual(baseline, ([], {}))
            self.assertEqual(replacements, [('/bin/hexagonrpcd', bytes(elf()), None)])
            self.assertEqual(hashes, {str(source.resolve()): hashlib.sha256(elf()).hexdigest()})
            # No new labels: use the existing-inode metadata/all-xattr branch.
            # No rc, shared library, policy or writable-directory replacement.
            source.write_bytes(b'not an ELF')
            with self.assertRaises(ValueError):
                vendor.optional_hal_replacements(hexagonrpcd=source)
            source.unlink()
            with self.assertRaises(ValueError):
                vendor.optional_hal_replacements(hexagonrpcd=source)

    def test_thermal_and_symlink_rejection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'thermal'
            source.write_bytes(elf())
            replacements, hashes = vendor.optional_hal_replacements(thermal=source)
            self.assertEqual(replacements, [(vendor.THERMAL_HAL_PATH, bytes(elf()), None)])
            self.assertEqual(len(hashes), 1)
            link = root / 'link'
            link.symlink_to(source)
            with self.assertRaises(ValueError):
                vendor.optional_hal_replacements(thermal=link)

    def test_sensors_maps_only_existing_hal_and_records_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'reviewed-sensors'
            source.write_bytes(elf())
            replacements, hashes = vendor.optional_hal_replacements(sensors=source)
            self.assertEqual(replacements, [
                ('/bin/hw/android.hardware.sensors-service.gaokun3', bytes(elf()), None)])
            self.assertEqual(hashes, {str(source.resolve()): hashlib.sha256(elf()).hexdigest()})
            # None selects existing inode uid/gid/mode/xattrs; no rc or VINTF
            # entry is added, replaced or assigned a fabricated label.
            self.assertEqual(vendor.optional_hal_replacements(), ([], {}))

    def test_sensors_rejects_missing_directory_symlink_and_invalid_elf(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'sensors'
            for path in (source, root):
                with self.subTest(path=path), self.assertRaises(ValueError):
                    vendor.optional_hal_replacements(sensors=path)
            source.write_bytes(elf())
            link = root / 'link'
            link.symlink_to(source)
            with self.assertRaises(ValueError):
                vendor.optional_hal_replacements(sensors=link)
            wrong_arch = elf()
            struct.pack_into('<H', wrong_arch, 18, 62)
            no_entry = elf()
            struct.pack_into('<Q', no_entry, 24, 0)
            for blob in (b'not an ELF', elf()[:-1], wrong_arch, no_entry):
                source.write_bytes(blob)
                with self.subTest(blob=blob[:24]), self.assertRaises(ValueError):
                    vendor.optional_hal_replacements(sensors=source)

    def test_sensors_does_not_replace_other_optional_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            thermal, rpc, sensors = (root / name for name in ('thermal', 'rpc', 'sensors'))
            for path in (thermal, rpc, sensors):
                path.write_bytes(elf())
            baseline, baseline_hashes = vendor.optional_hal_replacements(thermal=thermal, hexagonrpcd=rpc)
            replacements, hashes = vendor.optional_hal_replacements(
                thermal=thermal, hexagonrpcd=rpc, sensors=sensors)
            self.assertEqual(replacements, baseline + [(vendor.SENSORS_HAL_PATH, bytes(elf()), None)])
            self.assertEqual(hashes, dict(baseline_hashes,
                **{str(sensors.resolve()): hashlib.sha256(elf()).hexdigest()}))
            sensors.write_bytes(b'bad')
            with self.assertRaises(ValueError):
                vendor.optional_hal_replacements(thermal=thermal, hexagonrpcd=rpc, sensors=sensors)


if __name__ == '__main__':
    unittest.main()
