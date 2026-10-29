"""Donor mount isolation and image format/capacity guards, without proprietary data."""
import importlib.util
from pathlib import Path
import tempfile
import unittest


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


ramdisk = module('prepare-ramdisk')
superimage = module('make-baseline-super')


class DonorTests(unittest.TestCase):
    def setUp(self):
        source = Path(__file__).resolve().parents[2] / 'device/huawei/gaokun3/fstab.gaokun3'
        self.fstab = ramdisk.isolated_fstab(source.read_bytes(), 'erofs')
        self.overlay = b'overlay /system/app overlay ro,lowerdir=/mnt/vendor/mi_ext/system/app/:/product/pangu/system/app/:/system/app check,nofail\n'

    def test_mount_order_isolation_and_first_stage_filter(self):
        donor = b'/dev/block/by-name/userdata /data f2fs rw wait,formattable\n' + self.overlay
        output = ramdisk.donor_extensions(self.fstab, donor)
        self.assertTrue(output.startswith(self.fstab))
        rows = [line.split() for line in output.decode().splitlines() if line and not line.startswith('#')]
        first = [row for row in rows if 'first_stage_mount' in row[4].split(',')]
        self.assertEqual([row[0] for row in first[-4:]], ['mi_ext', 'mi_product', '/mnt/vendor/mi_ext', 'overlay'])
        self.assertEqual(first[-2][1:4], ['/mi_ext', 'erofs', 'ro,bind'])
        self.assertEqual(first[-1][3], self.overlay.decode().split()[3])
        self.assertNotIn(b'/dev/block/by-name/userdata ', output)
        self.assertNotIn(b'avb=', output)

    def test_reject_unsafe_or_ambiguous_overlays(self):
        for bad in (self.overlay * 2, self.overlay.replace(b'/system/app', b'/data/app'),
                    self.overlay.replace(b'ro,lowerdir=', b'rw,lowerdir='),
                    self.overlay.replace(b'/product/pangu/', b'/product/../'),
                    self.overlay.replace(b'check,nofail', b'wait,formattable'), b'# no overlay\n'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                ramdisk.donor_extensions(self.fstab, bad)
        with self.assertRaises(ValueError):
            ramdisk.donor_extensions(ramdisk.donor_extensions(self.fstab, self.overlay), self.overlay)
        with self.assertRaises(ValueError):
            ramdisk.donor_extensions(self.fstab.replace(b'erofs', b'ext4'), self.overlay)

    def test_six_partition_formats_completeness_and_capacity(self):
        with tempfile.TemporaryDirectory() as tmp:
            images = {}
            for name in ('system', 'system_ext', 'product', 'vendor', 'mi_ext', 'mi_product'):
                data = bytearray(4096)
                if name == 'vendor':
                    data[1080:1082] = b'\x53\xef'
                else:
                    data[1024:1028] = b'\xe2\xe1\xf5\xe0'
                images[name] = Path(tmp) / name
                images[name].write_bytes(data)
            superimage.validate_images(images, True)
            with self.assertRaises(ValueError):
                superimage.validate_images(images, False)
            with self.assertRaises(ValueError):
                superimage.validate_images({k: v for k, v in images.items() if k != 'mi_product'}, True)
            with images['product'].open('r+b') as f:
                f.truncate(superimage.SIZE)
            with self.assertRaises(ValueError):
                superimage.validate_images(images, True)
            images['product'].write_bytes(bytes(4096))
            with self.assertRaises(ValueError):
                superimage.validate_images(images, True)


if __name__ == '__main__':
    unittest.main()
