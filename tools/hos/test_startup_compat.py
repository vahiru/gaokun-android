import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('startup', Path(__file__).with_name('prepare-startup-compat.py'))
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

SOURCE = '''# donor options
service other /system/bin/other
    class main
service iorapd /system_ext/bin/iorapd
    class main
    user system

on property:persist.sys.stability.PrereadEnable=false
    stop iorapd
on post-fs-data
    mkdir /data/misc/iorapd/ 0750 system root
on property:persist.sys.stability.PrereadEnable=true
    start iorapd
'''


class StartupCompatTests(unittest.TestCase):
    def test_product_mode_preserves_other_power_and_cgroup_controls(self):
        source = ('# donor\n'
                  'persist.sys.powmillet.enable=true\n'
                  'persist.sys.millet.cgroup=false\n'
                  'persist.sys.gz.enable=true\n'
                  'ro.miui.region=CN\n')
        expected = source.replace('persist.sys.powmillet.enable=true',
                                  'persist.sys.powmillet.enable=false')
        self.assertEqual(mod.update_product_props(source), expected)
        self.assertEqual(mod.update_product_props(expected), expected)

    def test_product_mode_rejects_missing_duplicate_or_unexpected_flag(self):
        for source in ('', '# persist.sys.powmillet.enable=true\n',
                       'persist.sys.powmillet.enable=1\n',
                       'persist.sys.powmillet.enable=true\npersist.sys.powmillet.enable=false\n'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                mod.update_product_props(source)

    def test_only_service_option_changes_and_actions_preserved(self):
        expected = SOURCE.replace('service iorapd /system_ext/bin/iorapd\n',
                                  'service iorapd /system_ext/bin/iorapd\n    disabled\n')
        self.assertEqual(mod.update(SOURCE), expected)
        self.assertEqual(mod.update(expected), expected)

    def test_rejects_donor_drift_or_truncated_capture(self):
        variants = [
            SOURCE.replace('/system_ext/bin/iorapd', '/system/bin/iorapd'),
            SOURCE.replace('    stop iorapd', '    stop other'),
            SOURCE.replace('    start iorapd', '    restart iorapd'),
            SOURCE + 'service iorapd /system_ext/bin/iorapd\n    class main\n',
            SOURCE + 'on property:persist.sys.stability.PrereadEnable=true\n    start iorapd\n',
            SOURCE + '5175\n',
            SOURCE.replace('    user system', '    disabled\n    disabled'),
            SOURCE.replace('    user system', '    disabled true'),
            SOURCE.replace('    class main', '    class late_start'),
            SOURCE.replace('=false', '=false && property:other=1'),
        ]
        for source in variants:
            with self.subTest(source=source), self.assertRaises(ValueError):
                mod.update(source)


if __name__ == '__main__':
    unittest.main()
