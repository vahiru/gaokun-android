#!/usr/bin/env python3
"""No image build: verify low-space rejection precedes all output mutations."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('super_builder', Path(__file__).with_name('make-baseline-super.py'))
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class SpacePreflightTests(unittest.TestCase):
    def fixture(self, root):
        images = {}
        for name in ('system', 'system_ext', 'product', 'vendor'):
            image = root / (name + '.img')
            data = bytearray(4096)
            data[1080:1082] = b'\x53\xef'
            image.write_bytes(data)
            images[name] = image
        return images

    def test_low_space_never_starts_tools_or_creates_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            images = self.fixture(root)
            original = {name: p.read_bytes() for name, p in images.items()}
            output = root / 'new' / 'build'
            argv = ['make-baseline-super.py', '--out', str(output)]
            for name, image in images.items():
                argv += ['--' + name.replace('_', '-'), str(image)]
            with patch.object(sys, 'argv', argv), \
                 patch.object(builder.shutil, 'disk_usage', return_value=SimpleNamespace(free=0)), \
                 patch.object(builder.subprocess, 'run') as run, \
                 contextlib.redirect_stderr(io.StringIO()) as err:
                with self.assertRaises(SystemExit) as raised:
                    builder.main()
                self.assertEqual(raised.exception.code, 2)
                self.assertIn('insufficient disk space', err.getvalue())
                run.assert_not_called()
            self.assertFalse((root / 'new').exists())
            self.assertEqual(original, {name: p.read_bytes() for name, p in images.items()})

    def test_full_raw_and_unpack_footprint_and_margin(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            images = self.fixture(root)
            expected = 12 * 1024**3 + 4 * 4096 + 2 * 1024**3
            output = root / 'not-created' / 'output'
            with patch.object(builder.shutil, 'disk_usage', return_value=SimpleNamespace(free=expected-1)) as usage:
                with self.assertRaises(ValueError):
                    builder.require_build_space(images, output)
                usage.assert_called_once_with(root.resolve())
            with patch.object(builder.shutil, 'disk_usage', return_value=SimpleNamespace(free=expected)):
                result = builder.require_build_space(images, output)
                self.assertEqual(result['required_bytes'], expected)
            self.assertFalse(output.parent.exists())


if __name__ == '__main__':
    unittest.main()
