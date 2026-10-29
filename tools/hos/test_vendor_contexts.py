"""Regression coverage for enforcing access to renamed physical partitions."""
import importlib.util
import io
from pathlib import Path
import stat
import struct
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('prepare_vendor', Path(__file__).with_name('prepare-vendor.py'))
vendor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vendor)


class IsolatedContextsTest(unittest.TestCase):
    def test_recents_overlay_container_rejects_bad_inputs(self):
        def apk(entries):
            target = io.BytesIO()
            with zipfile.ZipFile(target, 'w') as archive:
                for name, data in entries:
                    archive.writestr(name, data)
            return target.getvalue()
        manifest = struct.pack('<HHI', 3, 8, 12) + b'\0' * 4
        resources = struct.pack('<HHI', 2, 12, 16) + b'\0' * 8
        valid = [('AndroidManifest.xml', manifest), ('resources.arsc', resources)]
        vendor.validate_recents_overlay(apk(valid))
        for entries in (valid[:1], [('AndroidManifest.xml', b'<manifest/>'), valid[1]],
                        [valid[0], ('resources.arsc', resources[:-1])],
                        valid + [('classes.dex', b'code')],
                        valid + [('lib/arm64-v8a/test.so', b'code')]):
            with self.subTest(entries=entries), self.assertRaises(ValueError):
                vendor.validate_recents_overlay(apk(entries))
        with self.assertRaises(ValueError):
            vendor.validate_recents_overlay(b'not an APK')

    def test_recents_overlay_metadata_is_inherited_and_not_executable(self):
        info = 'Inode: 12 Type: regular Mode: 0644\nUser: 0 Group: 0\n'
        label = b'u:object_r:vendor_overlay_file:s0\0'
        self.assertEqual(vendor.overlay_template_label(info, label),
                         'u:object_r:vendor_overlay_file:s0')
        for invalid in (info.replace('0644', '0755'), info.replace('Group: 0', 'Group: 2000'),
                        info.replace('regular', 'directory')):
            with self.assertRaises(ValueError):
                vendor.overlay_template_label(invalid, label)
        with self.assertRaises(ValueError):
            vendor.overlay_template_label(info, b'')
        self.assertEqual(vendor.new_file_metadata(vendor.RECENTS_OVERLAY_PATH),
                         (stat.S_IFREG | 0o644, 0, 0))
        self.assertEqual(vendor.new_file_metadata('/etc/media_profiles.xml'),
                         (stat.S_IFREG | 0o644, 0, 0))
        self.assertEqual(vendor.new_file_metadata('/bin/tinymix'),
                         (stat.S_IFREG | 0o755, 0, 2000))

    def test_optional_pmf_preserves_radio_settings_and_refuses_network_blocks(self):
        baseline = 'disable_scan_offload=1\nwowlan_triggers=disconnect gtk_rekey_failure\nap_scan=1\n'
        result = vendor.optional_pmf_config(baseline)
        self.assertTrue(result.startswith(baseline))
        self.assertIn('\npmf=1\n', result)
        for text in (result, baseline + ' pmf = 0\n', baseline + 'network={\n}\n'):
            with self.assertRaises(ValueError):
                vendor.optional_pmf_config(text)

    def setUp(self):
        self.original = ''.join(f'/dev/block/by-name/{name} -b u:object_r:{name}_block_device:s0\n'
                                for name in ('misc', 'metadata', 'super', 'userdata'))

    def test_preserves_originals_and_copies_all_four_block_labels(self):
        result = vendor.isolated_contexts(self.original)
        entries = {e[0]: e[1:] for line in result.splitlines()
                   if (e := line.split()) and not e[0].startswith('#')}
        self.assertTrue(result.startswith(self.original))
        for name in ('misc', 'metadata', 'super', 'userdata'):
            original = '/dev/block/by-name/' + name
            self.assertEqual(entries[original + '_hos'], entries[original])

    def test_rejects_missing_duplicate_or_unqualified_original(self):
        for text in (self.original.replace('/misc ', '/other '),
                     self.original + '/dev/block/by-name/misc -b u:object_r:misc_block_device:s0\n',
                     self.original.replace('/misc -b ', '/misc ')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                vendor.isolated_contexts(text)

    def test_rejects_already_patched_contexts(self):
        with self.assertRaises(ValueError):
            vendor.isolated_contexts(vendor.isolated_contexts(self.original))

    def test_media_profile_rejects_observed_empty_camera_abort(self):
        with self.assertRaises(ValueError):
            vendor.validate_media_profiles(b'<MediaSettings/>')
        candidate = Path(__file__).parent / 'config/media_profiles_bootstrap.xml'
        vendor.validate_media_profiles(candidate.read_bytes())
        with self.assertRaises(ValueError):
            vendor.validate_media_profiles(candidate.read_bytes().replace(b'frameRate="15"', b'frameRate="0"'))

    def test_donor_app_abi_does_not_advertise_missing_native_architecture(self):
        props = ('ro.vendor.product.cpu.abilist=arm64-v8a,armeabi-v7a,armeabi\n'
                 'ro.vendor.product.cpu.abilist32=armeabi-v7a,armeabi\n'
                 'ro.vendor.product.cpu.abilist64=arm64-v8a\n'
                 'ro.zygote=zygote64_32\n#ro.zygote?=zygote32\nother=keep\n')
        result = vendor.arm64_app_properties(props)
        self.assertIn('ro.vendor.product.cpu.abilist=arm64-v8a\n', result)
        self.assertIn('ro.vendor.product.cpu.abilist32=\n', result)
        self.assertIn('ro.zygote=zygote64\n', result)
        self.assertTrue(result.endswith('#ro.zygote?=zygote32\nother=keep\n'))
        self.assertEqual(vendor.arm64_app_properties(result), result)
        with self.assertRaises(ValueError):
            vendor.arm64_app_properties(props + 'ro.zygote=zygote64\n')
        odm = props.replace('ro.vendor.', 'ro.odm.').replace('ro.zygote=zygote64_32\n', '')
        result = vendor.arm64_app_properties(odm, 'odm')
        self.assertIn('ro.odm.product.cpu.abilist32=\n', result)
        self.assertIn('ro.odm.product.cpu.abilist=arm64-v8a\n', result)
        self.assertNotIn('\nro.zygote=', result)


if __name__ == '__main__':
    unittest.main()
