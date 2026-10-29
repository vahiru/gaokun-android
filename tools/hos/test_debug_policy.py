import importlib.util
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parent))
from cil import statements, replace_symbols

spec = importlib.util.spec_from_file_location('debug_policy', Path(__file__).with_name('prepare-debug-policy.py'))
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class PolicyTests(unittest.TestCase):
    def test_namespaces_and_neverallow_preserved_without_new_allows(self):
        user = '(typeattribute base_typeattr_1)\n(typeattributeset base_typeattr_1 (domain))\n'
        debug = '''; base_typeattr_1 in comment
(typeattribute base_typeattr_1)
(typeattributeset base_typeattr_1 (and (domain) (not (init))))
(allow llkd base_typeattr_1 (process (ptrace)))
(allow init base_typeattr_1 (file (read)))
(neverallow base_typeattr_1 self (file (write)))
'''
        patched, report = policy.repair(user, debug, ['(neverallow base_typeattr_2 self (file (read)))'])
        trees = [tree for _, _, tree in statements(patched)]
        name = report['renamed_symbols']['base_typeattr_1']
        self.assertNotIn(name, ('base_typeattr_1', 'base_typeattr_2'))
        self.assertIn(['typeattributeset', 'base_typeattr_1', ['domain']], trees)
        self.assertIn(['typeattributeset', name, ['and', ['domain'], ['not', ['init']]]], trees)
        self.assertEqual([t for t in trees if t[0] == 'allow'], [['allow', 'init', name, ['file', ['read']]]])
        self.assertEqual([t for t in trees if t[0] == 'neverallow'], [['neverallow', name, 'self', ['file', ['write']]]])

    def test_reject_unexpected_llkd_rule(self):
        user = '(typeattribute base_typeattr_1)'
        for debug in ['(typeattribute base_typeattr_2)', '(allow llkd domain (process (ptrace)))']:
            with self.assertRaises(ValueError):
                policy.repair(user, debug, [])

    def test_cil_quotes_comments_and_empty_nested_lists(self):
        text = '; ) comment\n(genfscon proc "/x;base_typeattr_1" (u object_r t ((s0) ())))\n'
        self.assertEqual(replace_symbols(text, {'base_typeattr_1': 'base_typeattr_999'}), text)
        self.assertEqual(len(statements(text)), 1)
        for invalid in ['(allow x', ')', 'atom', '("unterminated)', '()']:
            with self.assertRaises(ValueError):
                statements(invalid)


if __name__ == '__main__':
    unittest.main()
