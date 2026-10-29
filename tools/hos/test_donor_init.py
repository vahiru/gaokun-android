import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('donor_init', Path(__file__).with_name('prepare-donor-init.py'))
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class DonorInitTests(unittest.TestCase):
    def test_preserves_unrelated_actions_and_rejects_extra_mounts(self):
        debug = sorted(mod.DEBUG_MOUNTS)
        debug.append(next(s for s in debug if s.endswith(':/system/xbin')))
        original = 'on post-fs\n    start example\n' + '\n'.join('    '+s for s in debug) + '\n    setprop example 1\n'
        boot = 'on boot\n' + '\n'.join('    '+s for s in sorted(mod.BOOT_MOUNTS)) + '\n'
        patched, other = mod.prepare(original, boot)
        self.assertIn('    start example\n', patched)
        self.assertIn('    setprop example 1\n', patched)
        self.assertNotIn('    mount ', patched)
        self.assertTrue(other.startswith('#'))
        for wrong_system, wrong_mi in ((original, boot+'    start must_preserve\n'),
                (original.replace('/system/xbin', '/system/other'), boot)):
            with self.assertRaises(ValueError):
                mod.prepare(wrong_system, wrong_mi)


if __name__ == '__main__':
    unittest.main()
