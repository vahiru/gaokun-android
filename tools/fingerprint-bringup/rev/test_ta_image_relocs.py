#!/usr/bin/env python3
"""Assert the pinned TA image's loader inventory stays unchanged."""
import hashlib
import unittest

from replay_preprocess import HERE, TA_PIN
from ta_image_relocs import inventory


class TaImageRelocsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        blob=(HERE/'cabs/qctree/fingerpr.mbn').read_bytes()
        assert hashlib.sha256(blob).hexdigest()==TA_PIN
        cls.report=inventory(blob)

    def test_counts(self):
        self.assertEqual(self.report['relocation_counts'],
                         {'RELATIVE':373,'JUMP_SLOT':36,'GLOB_DAT':1,'ABS64':4})
        self.assertEqual(len(self.report['imports']),40)
        # One symbol is bound through both GLOB_DAT and JUMP_SLOT.
        self.assertEqual(len(self.report['imports']['__funcs_on_exit']),2)

    def test_segments(self):
        segs=self.report['load_segments']
        self.assertEqual([(s['vaddr'],s['filesz']) for s in segs],
                         [(0,0x224d60),(0x225000,0xf8),(0x226000,0x13a020),
                          (0x361000,0x868),(0x362000,0x1ae47)])

    def test_known_got_slots(self):
        rel={r['slot']:r['addend'] for r in self.report['relative_relocations']}
        self.assertEqual(rel[0x361160],0x226138)
        self.assertEqual(rel[0x361170],0x2261a8)
        self.assertEqual(rel[0x361178],0x279648)

    def test_import_names(self):
        names=set(self.report['imports'])
        for name in ('qsee_realloc','qsee_malloc','qsee_free','qsee_log','qsee_sfs_open',
                     'qsee_sfs_read','qsee_sfs_write','qsee_spi_open','qsee_spi_read',
                     'qsee_spi_write','qsee_spi_close','GPAppLib_appInit',
                     'GPAppLib_appShutdown','cmnlib_init','app_heap_base'):
            self.assertIn(name,names)


if __name__=='__main__':
    unittest.main()
