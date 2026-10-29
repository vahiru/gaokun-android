import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import registry_schema as r

class SchemaTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.inputs=self.root/'inputs';self.inputs.mkdir()
    def put(self,name,obj):
        (self.inputs/name).write_text(json.dumps(obj))
    def collect(self,merge=False):
        return r.collect(self.inputs,'QRD','449',merge)
    def test_filter_and_unchanged_nested_values(self):
        cal={'type':'flt','ver':'2','data':'1.0000'}
        self.put('a.json',{'config':{'hw_platform':['QRD'],'soc_id':['449']},'sensor':{'owner':'driver','.fac_cal':{'scale':cal}}})
        self.put('b.json',{'config':{'soc_id':['']},'unselected':{}})
        payload,report=self.collect()
        self.assertEqual(json.loads(payload['sensor.fac_cal']),{'sensor.fac_cal':{'scale':cal}})
        self.assertEqual(json.loads(payload['sensor'])['sensor']['fac_cal'],r.REFERENCE)
        self.assertEqual(report['excluded'],['b.json'])
    def test_default_duplicates_rejected_before_output(self):
        self.put('a.json',{'g':{'a':1}});self.put('b.json',{'g':{'b':2}})
        with self.assertRaisesRegex(ValueError,'duplicate output'):self.collect()
        self.assertFalse((self.root/'output').exists())
    def test_explicit_disjoint_merge_preserves_fields(self):
        self.put('a.json',{'g':{'owner':'x','param':{'ver':'0','data':'0.06'}}})
        self.put('b.json',{'g':{'owner':'x','enabled':{'ver':'0','data':'1'}}})
        payload,report=self.collect(True)
        self.assertEqual(set(json.loads(payload['g'])['g']),{'owner','param','enabled'})
        self.assertEqual(report['merged_groups'][0]['added_fields'],['enabled'])
    def test_conflicting_value_type_or_version_never_wins(self):
        for other in [{'ver':'1','data':'1'}, {'ver':'0','data':'2'}, {'ver':'0','data':1}]:
            self.put('a.json',{'g':{'item':{'ver':'0','data':'1'}}});self.put('b.json',{'g':{'item':other}})
            with self.assertRaisesRegex(ValueError,'conflicting'):self.collect(True)
    def test_duplicate_json_key_and_nonfinite_rejected(self):
        for text in ['{"g":{},"g":{}}','{"g":{"x":NaN}}']:
            (self.inputs/'a.json').write_text(text)
            with self.assertRaises(ValueError):self.collect()
    def test_unsafe_group_and_nested_overlap_rejected(self):
        for obj in [{'../escape':{}},{'a':{'.b':{},'b':1}},{'a':{'.b':{}},'a.b':{}}]:
            self.put('a.json',obj)
            with self.assertRaises(ValueError):self.collect()
    def test_fresh_output_only_and_no_tail_overwrite(self):
        self.put('a.json',{'g':{'long_value':'abcdefgh'}});payload,_=self.collect()
        out=self.root/'output';r.write_new_directory(out,payload);before=(out/'g').read_bytes()
        with self.assertRaises(FileExistsError):r.write_new_directory(out,{'g':b'{}'})
        self.assertEqual((out/'g').read_bytes(),before)
        alias=self.root/'link';alias.symlink_to(out)
        with self.assertRaises(FileExistsError):r.write_new_directory(alias,payload)
    def test_io_failure_leaves_partial_output_and_refuses_retry(self):
        self.put('a.json',{'g':{}});payload,_=self.collect();out=self.root/'output'
        with patch.object(r.os,'fsync',side_effect=OSError('disk error')):
            with self.assertRaises(OSError):r.write_new_directory(out,payload)
        self.assertTrue(out.exists())
        with self.assertRaises(FileExistsError):r.write_new_directory(out,payload)
    def test_wrong_filter_shape_rejected(self):
        self.put('a.json',{'config':{'soc_id':'449'},'g':{}})
        with self.assertRaises(ValueError):self.collect()

if __name__=='__main__':unittest.main()
