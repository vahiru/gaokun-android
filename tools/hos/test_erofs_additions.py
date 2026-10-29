import importlib.util
import os
from pathlib import Path
import stat
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('erofs', Path(__file__).with_name('repack-erofs.py'))
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class AdditionTests(unittest.TestCase):
    @unittest.skipUnless(hasattr(os, 'listxattr'), 'Linux xattr APIs required by EROFS repacker')
    def test_metadata_and_escape_guards(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)/'image'
            root.mkdir()
            source = Path(tmp)/'new.xml'
            source.write_text('<features/>\n')
            template = root/'donor.xml'
            template.write_text('donor')
            template.chmod(0o640)
            os.setxattr(template, 'user.hos_test', b'label fixture')
            mod.add_file(root, 'gaokun3.xml', source, 'donor.xml')
            added = root/'gaokun3.xml'
            self.assertEqual(added.read_bytes(), source.read_bytes())
            self.assertEqual(stat.S_IMODE(added.stat().st_mode), 0o640)
            self.assertEqual(os.getxattr(added, 'user.hos_test'), b'label fixture')
            self.assertEqual((added.stat().st_uid, added.stat().st_gid), (template.stat().st_uid, template.stat().st_gid))
            (root/'outside').symlink_to(Path(tmp), target_is_directory=True)
            (root/'template-link').symlink_to(template)
            for name, metadata in [('gaokun3.xml', 'donor.xml'), ('../escape', 'donor.xml'),
                                   ('outside/escape', 'donor.xml'), ('new', 'template-link')]:
                with self.subTest(name=name), self.assertRaises(ValueError):
                    mod.add_file(root, name, source, metadata)


if __name__ == '__main__':
    unittest.main()
