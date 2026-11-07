import unittest
from functools import reduce
from operator import xor
from sc_info_frames import describe, parse


def response(payload):
    frame = bytes((3, 0, len(payload) + 1, 4)) + payload
    return bytes(12) + frame + bytes((reduce(xor, frame[1:], 0),))


class InfoTests(unittest.TestCase):
    def test_fixed_queries(self):
        for name, expected in [('dimension0', '02 00 03 88 14 01 9e'),
                               ('dimension1', '02 00 03 88 15 01 9f'),
                               ('device_status', '02 00 03 88 20 02 a9')]:
            self.assertEqual(describe(name)['mcu_request_hex'], expected)

    def test_dimension_bounds(self):
        for value in (1, 80, 255):
            self.assertEqual(parse('dimension0', response(bytes([value])))['value_little_endian'], value)
        with self.assertRaisesRegex(ValueError, 'zero dimension'):
            parse('dimension1', response(b'\0'))

    def test_status_is_little_endian(self):
        self.assertTrue(parse('device_status', response(b'\xa5\x5a'))['windows_ready_value_matches'])
        self.assertFalse(parse('device_status', response(b'\x5a\xa5'))['windows_ready_value_matches'])

    def test_corruption_short_and_unwritten(self):
        good = response(b'\x50')
        bad = bytearray(good)
        bad[-1] ^= 1
        for data in (good[:-1], good + b'\0', bytes(len(good)), b'\xa5' * len(good), bad):
            with self.assertRaises(ValueError):
                parse('dimension0', data)

    def test_status_and_declared_length(self):
        for offset, value in ((13, 1), (14, 1), (15, 0x15)):
            data = bytearray(response(b'\x50'))
            data[offset] = value
            with self.assertRaises(ValueError):
                parse('dimension0', data)


if __name__ == '__main__':
    unittest.main()
