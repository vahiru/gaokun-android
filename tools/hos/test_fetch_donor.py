import hashlib
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('fetch_donor', Path(__file__).with_name('fetch-donor.py'))
fetcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetcher)


def vint(n):
    out = bytearray()
    while n > 127:
        out.append((n & 127) | 128)
        n >>= 7
    return bytes(out) + bytes([n])


def field(n, value):
    return vint(n * 8 + 2) + vint(len(value)) + value if isinstance(value, bytes) else vint(n * 8) + vint(value)


class FetchTests(unittest.TestCase):
    def test_reject_partial_overlapping_and_delta_operations(self):
        def op(start, blocks, kind=0):
            return field(1, kind) + field(3, 4096) + field(8, hashlib.sha256(b'x').digest()) + field(6, field(1, start) + field(2, blocks))
        self.assertEqual(len(fetcher.operations([(8, op(0, 1)), (8, op(1, 1))], 4096, 8192)), 2)
        for parts in ([(8, op(1, 1))], [(8, op(0, 2)), (8, op(1, 1))],
                      [(8, op(0, 3))], [(8, op(0, 2, 4))]):
            with self.assertRaises(ValueError):
                fetcher.operations(parts, 4096, 8192)

    def test_range_must_match_and_must_be_complete(self):
        class Response:
            status = 206
            headers = {'Content-Range': 'bytes 10-12/100'}
            data = b'abc'
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def read(self, size): return self.data
        response = Response()
        with patch.object(fetcher.urllib.request, 'urlopen', return_value=response), patch.object(fetcher.time, 'sleep'):
            self.assertEqual(fetcher.fetch('https://example.test', 10, 3), b'abc')
            for status, content_range, data in ((200, 'bytes 10-12/100', b'abc'),
                    (206, 'bytes 11-13/100', b'abc'), (206, 'bytes 10-12/100', b'ab')):
                response.status, response.headers, response.data = status, {'Content-Range': content_range}, data
                with self.assertRaises(RuntimeError):
                    fetcher.fetch('https://example.test', 10, 3)


if __name__ == '__main__':
    unittest.main()
