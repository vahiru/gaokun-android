import unittest
from sc_version_frame import parse_mcu_response, request_description


class FrameTests(unittest.TestCase):
    @staticmethod
    def frame(payload=b'ST1.0', status=4):
        data = bytearray(69)
        length = len(payload) + 1
        data[0] = 3  # Synthetic; the real response discriminator is not established.
        data[1:3] = length.to_bytes(2, 'big')
        data[3] = status
        data[4:4 + len(payload)] = payload
        for b in data[1:length + 3]:
            data[length + 3] ^= b
        return bytes(data)

    def test_maximum_length_and_padding(self):
        data = self.frame(b'V' * 64)
        self.assertEqual(parse_mcu_response(data)['length'], 65)
        self.assertEqual(parse_mcu_response(self.frame())['payload_ascii'], 'ST1.0')

    def test_corrupt_checksum(self):
        data = bytearray(self.frame())
        data[5] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum'):
            parse_mcu_response(data)

    def test_unwritten_or_invalid_buffers(self):
        for data in (bytes(69), b'\xa5' * 69, b'\xff' * 69,
                     self.frame()[:-1], self.frame() + b'\0'):
            with self.assertRaises(ValueError):
                parse_mcu_response(data)

    def test_status_and_empty(self):
        with self.assertRaisesRegex(ValueError, 'status'):
            parse_mcu_response(self.frame(status=0x15))
        with self.assertRaisesRegex(ValueError, 'empty'):
            parse_mcu_response(self.frame(b''))

    def test_exact_fixed_requests(self):
        desc = request_description()
        self.assertEqual(desc['ta_write_request_hex'],
                         '21 80 00 00 05 00 00 00 00 00 00 00 02 00 01 30 31')
        self.assertEqual(desc['ta_read_request_hex'],
                         '20 80 00 00 00 00 00 00 45 00 00 00')
        self.assertEqual(desc['ta_read_response_size'], 81)


if __name__ == '__main__':
    unittest.main()
