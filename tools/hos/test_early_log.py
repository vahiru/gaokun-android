"""CRC recovery, ring order, bounded writes and command-line rejection tests."""
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

spec = importlib.util.spec_from_file_location('early_reader', Path(__file__).with_name('read-early-log.py'))
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


def header(generation, total):
    values = [b'GK3HLOG1', 1, 64, reader.SIZE-reader.DATA, total, generation, 0, 0, 0, 0]
    block = struct.pack(reader.FORMAT, *values)
    values[6] = zlib.crc32(block)
    return struct.pack(reader.FORMAT, *values)


class DecoderTest(unittest.TestCase):
    def test_recovers_latest_valid_header_and_falls_back_on_corruption(self):
        raw = bytearray(reader.SIZE)
        raw[reader.DATA:reader.DATA+6] = b'abcdef'
        raw[reader.HEADER+512:reader.HEADER+576] = header(1, 3)
        raw[reader.HEADER:reader.HEADER+64] = header(2, 6)
        info, data = reader.decode(raw)
        self.assertEqual(data, b'abcdef')
        self.assertEqual(info['generation'], 2)
        raw[reader.HEADER+40] ^= 1
        self.assertEqual(reader.decode(raw)[1], b'abc')

    def test_rejects_uninitialized_wrong_size_and_bad_bounds(self):
        for raw in (bytes(reader.SIZE), b'short'):
            with self.assertRaises(ValueError):
                reader.decode(raw)
        raw = bytearray(reader.SIZE)
        h = bytearray(header(1, 3))
        struct.pack_into('<Q', h, 16, reader.SIZE)
        struct.pack_into('<I', h, 40, 0)
        struct.pack_into('<I', h, 40, zlib.crc32(h))
        raw[reader.HEADER+512:reader.HEADER+576] = h
        with self.assertRaises(ValueError):
            reader.decode(raw)


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'), 'native Linux compiler required')
class NativeWriterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='hos-log-test-')
        cls.binary = Path(cls.build.name)/'logger'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11',
                        '-DHOS_MISC_UUID="00000000-0000-0000-0000-000000000001"',
                        str(Path(__file__).with_name('early-log.c')), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def test_ring_wrap_preserves_reserved_prefix_and_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            image, source = Path(tmp)/'misc', Path(tmp)/'source'
            prefix = b'BCB-PRESERVE-' * 6000
            image.write_bytes(prefix[:65536] + bytes(reader.SIZE-65536))
            payload = bytes(range(251)) * 24000
            source.write_bytes(payload)
            subprocess.run([str(self.binary), '--capture-test', str(image), str(source)], check=True)
            raw = image.read_bytes()
            self.assertEqual(len(raw), reader.SIZE)
            self.assertEqual(raw[:65536], prefix[:65536])
            info, log = reader.decode(raw)
            self.assertTrue(info['wrapped'])
            self.assertEqual(info['total'], len(payload))
            self.assertEqual(log, payload[-(reader.SIZE-reader.DATA):])

    def test_wrong_size_and_same_file_are_rejected_without_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp)/'misc'
            for size in (128, reader.SIZE):
                image.write_bytes(b'Z'*size)
                r = subprocess.run([str(self.binary), '--capture-test', str(image), str(image)])
                self.assertNotEqual(r.returncode, 0)
                self.assertEqual(image.read_bytes(), b'Z'*size)

    def test_missing_wrong_or_duplicate_boot_parameters_are_rejected(self):
        good = 'androidboot.force_super_partition=super_hos androidboot.hos_earlylog=misc-ring-v1'
        with tempfile.TemporaryDirectory() as tmp:
            cmdline = Path(tmp)/'cmdline'
            for text, expected in ((good, 0), ('', 1), (good.replace('super_hos', 'super'), 1),
                                   (good.replace('misc-ring-v1', 'other'), 1),
                                   (good+' androidboot.force_super_partition=super_hos', 1),
                                   (good+' androidboot.hos_earlylog=misc-ring-v1', 1)):
                cmdline.write_text(text)
                r = subprocess.run([str(self.binary), '--check-cmdline', str(cmdline)])
                self.assertEqual(r.returncode, expected, text)


if __name__ == '__main__':
    unittest.main()
