"""Reject a destructive plan before producing any GPT mutation command."""
import copy
import importlib.util
from pathlib import Path
import struct
import unittest
import uuid

spec = importlib.util.spec_from_file_location('install', Path(__file__).with_name('install-baseline.py'))
install = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install)


class PlanTest(unittest.TestCase):
    def setUp(self):
        guid = '00000000-0000-0000-0000-000000000001'
        kind = '0fc63daf-8483-4772-8e79-3d69d8477de4'
        entry = bytearray(128)
        entry[:16] = uuid.UUID(kind).bytes_le
        entry[16:32] = uuid.UUID(guid).bytes_le
        struct.pack_into('<QQ', entry, 32, 34, 2047)
        entry[56:64] = 'misc'.encode('utf-16-le')
        self.entries = {4: bytes(entry)}
        self.header = dict(guid=guid, first=34, last=1000215182)
        original = dict(number=4, name='misc', start=34, end=2047, guid=guid, type_guid=kind)
        self.plan = dict(disk_guid=guid, sector_size=512, existing_partitions_preserved=[original], new_partitions=[])
        cursor = 2048
        for i, (name, size) in enumerate((('super_hos', 12*1024**3), ('metadata_hos', 32*1024**2),
                                         ('misc_hos', 4*1024**2), ('userdata_hos', 32*1024**3))):
            self.plan['new_partitions'].append(dict(number=7+i, name=name, start_sector=cursor,
                end_sector=cursor+size//512-1, size=size, guid=str(uuid.UUID(int=i+2))))
            cursor += size//512

    def test_valid_plan_does_not_mutate_inputs(self):
        before = copy.deepcopy(self.plan)
        install.validate_plan(self.plan, self.header, self.entries)
        self.assertEqual(self.plan, before)

    def test_overlapping_existing_or_new_partitions_are_rejected(self):
        for start in (0, 2048):
            p = copy.deepcopy(self.plan)
            item = p['new_partitions'][1]
            item['start_sector'] = start
            item['end_sector'] = start+item['size']//512-1
            with self.subTest(start=start), self.assertRaises(ValueError):
                install.validate_plan(p, self.header, self.entries)

    def test_identity_number_uuid_and_preserved_partition_mismatches_rejected(self):
        mutations = [lambda p: p.update(disk_guid=str(uuid.UUID(int=999))),
                     lambda p: p['new_partitions'][0].update(number=4),
                     lambda p: p['new_partitions'][0].update(guid=self.header['guid']),
                     lambda p: p['existing_partitions_preserved'][0].update(end=4095),
                     lambda p: p['new_partitions'][0].update(name='super'),
                     lambda p: p['new_partitions'][0].update(size=1)]
        for change in mutations:
            p = copy.deepcopy(self.plan); change(p)
            with self.subTest(plan=p), self.assertRaises(ValueError):
                install.validate_plan(p, self.header, self.entries)


if __name__ == '__main__':
    unittest.main()
