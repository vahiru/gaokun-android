import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('graphics', Path(__file__).with_name('prepare-graphics-props.py'))
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class GraphicsTests(unittest.TestCase):
    def test_keeps_unrelated_properties_and_rejects_ambiguous_input(self):
        source = '# comment\nro.product.device=gaokun3\ndebug.hwui.renderer=skiavk\n'
        changes = {'debug.hwui.renderer': 'skiagl', 'persist.sys.background_blur_supported': 'false'}
        result = mod.update(source, changes, ['debug.hwui.renderer'])
        self.assertTrue(result.startswith('# comment\nro.product.device=gaokun3\n'))
        self.assertIn('debug.hwui.renderer=skiagl\n', result)
        self.assertEqual(mod.update(result, changes, ['debug.hwui.renderer']), result)
        for text in ('# empty\n', source+'debug.hwui.renderer=other\n'):
            with self.assertRaises(ValueError):
                mod.update(text, changes, ['debug.hwui.renderer'])


if __name__ == '__main__':
    unittest.main()
