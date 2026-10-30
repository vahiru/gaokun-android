import struct
import unittest
from decode_sc_image import statistics

class DecodedChecks(unittest.TestCase):
    def data(self, n):
        return struct.pack('<7840H', *([5000]*n + [1000,2000]*( (7840-n)//2) + [1000]*((7840-n)%2)))
    def test_boundaries(self):
        self.assertTrue(statistics(self.data(100))['basic_checks_passed'])
        self.assertFalse(statistics(self.data(101))['basic_checks_passed'])
        self.assertFalse(statistics(bytes(15680))['basic_checks_passed'])
        self.assertFalse(statistics(self.data(0))['complete_image_processing_verified'])
    def test_size(self):
        for size in (0,4096,15679,15681):
            with self.assertRaises(ValueError): statistics(bytes(size))

if __name__ == '__main__': unittest.main()
