import struct
import unittest
from sc_image_frame import request_frames, validate_reply, ENCODED_BYTES

class ImageFrameTests(unittest.TestCase):
    def reply(self):
        body = b'\x02' + (ENCODED_BYTES + 1).to_bytes(2, 'big') + b'\x04' + bytes(range(256)) * (ENCODED_BYTES // 256) + bytes(range(ENCODED_BYTES % 256))
        crc = 0
        for x in body[1:]: crc ^= x
        return bytes(12) + body + bytes([crc])

    def test_requests(self):
        w, r = request_frames()
        self.assertEqual(w, bytes.fromhex('2180000009000000000000000200058b34003d40c7'))
        self.assertEqual(r, struct.pack('<III', 0x8020, 0, 15685))

    def test_valid_encoded_only(self):
        result = validate_reply(self.reply())
        self.assertEqual(result['encoded_bytes'], 15680)
        self.assertFalse(result['decoded_image_verified'])

    def test_invalid(self):
        raw = self.reply()
        for bad in [raw[:-1], raw + b'\0', bytes(len(raw)), b'\xa5' * len(raw), raw[:-1] + bytes([raw[-1] ^ 1])]:
            with self.assertRaises(ValueError): validate_reply(bad)
        for index in [13, 14, 15]:
            bad = bytearray(raw); bad[index] ^= 1
            with self.assertRaises(ValueError): validate_reply(bad)

if __name__ == '__main__': unittest.main()
