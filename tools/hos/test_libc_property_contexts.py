#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('libc_contexts', Path(__file__).with_name('prepare-libc-property-contexts.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class ContextTests(unittest.TestCase):
    def setUp(self):
        self.contexts = dict.fromkeys(m.PARTITIONS)
        self.contexts['plat'] = ('* u:object_r:default_prop:s0\n'
                                'libc.debug.malloc.options u:object_r:libc_debug_prop:s0 exact string\n'
                                'ro.sanitize.address u:object_r:build_prop:s0 exact bool\n')
        self.policy = '(allow domain libc_debug_prop (file (read getattr map open)))'

    def test_exact_only_and_idempotent(self):
        result, added = m.generate(self.contexts, self.policy)
        self.assertEqual(added, list(m.PROPERTIES))
        self.assertTrue(result.startswith(self.contexts['plat']))
        for name in m.PROPERTIES:
            self.assertEqual(result.count(f'{name} {m.CONTEXT} exact string\n'), 1)
        self.contexts['plat'] = result
        self.assertEqual(m.generate(self.contexts, self.policy), (result, []))

    def test_conflicts_across_partitions(self):
        for value in ('ro.sanitize u:object_r:build_prop:s0 exact string\n',
                      'persist.track. u:object_r:some_prop:s0 prefix string\n',
                      f'ro.sanitize {m.CONTEXT} exact string\n'):
            with self.subTest(value=value):
                self.contexts['vendor'] = value
                with self.assertRaises(ValueError):
                    m.generate(self.contexts, self.policy)

    def test_duplicate_existing_mapping(self):
        self.contexts['plat'] += (f'ro.sanitize {m.CONTEXT} exact string\n') * 2
        with self.assertRaises(ValueError):
            m.generate(self.contexts, self.policy)

    def test_all_partitions_and_existing_policy_required(self):
        with self.assertRaises(ValueError):
            m.generate(self.contexts, '')
        del self.contexts['odm']
        with self.assertRaises(ValueError):
            m.generate(self.contexts, self.policy)

    def test_malformed_context(self):
        self.contexts['product'] = 'invalid\n'
        with self.assertRaises(ValueError):
            m.generate(self.contexts, self.policy)


if __name__ == '__main__':
    unittest.main()
