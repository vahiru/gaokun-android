#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location('touch', Path(__file__).with_name('analyze-touch-events.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def event(t, code, value='00000000', typ='EV_ABS'):
    return f'[ {t:.6f}] /dev/input/event7: {typ} {code} {value}\n'


def syn(t):
    return event(t, 'SYN_REPORT', typ='EV_SYN')


class TouchTests(unittest.TestCase):
    def test_empty_is_not_failure(self):
        r=m.analyze([])
        self.assertEqual(r['status'],'no_events')
        self.assertFalse(r['device_failure_inferred'])
        self.assertEqual(r['contacts'],[])

    def test_sparse_coordinates_and_release(self):
        r=m.analyze([event(1,'ABS_MT_TRACKING_ID','00000017'),
            event(1,'ABS_MT_POSITION_X','00000064'),event(1,'ABS_MT_POSITION_Y','000000c8'),syn(1),
            event(1.02,'ABS_MT_POSITION_X','00000078'),syn(1.02),syn(1.05),
            event(1.07,'ABS_MT_TRACKING_ID','ffffffff'),syn(1.07)])
        c=r['contacts'][0]
        self.assertEqual(c['frames'],3)
        self.assertEqual(c['first_xy'],[100,200]);self.assertEqual(c['last_xy'],[120,200])
        self.assertEqual((c['dx'],c['dy']),(20,0))
        self.assertAlmostEqual(c['duration_s'],.07)
        self.assertAlmostEqual(c['max_frame_interval_s'],.03)
        self.assertEqual(c['end_reason'],'released')

    def test_slots_contact_reuse_and_bounds(self):
        r=m.analyze([event(1,'ABS_MT_TRACKING_ID','1'),event(1,'ABS_MT_POSITION_X','10'),
            event(1,'ABS_MT_POSITION_Y','20'),event(1,'ABS_MT_SLOT','1'),
            event(1,'ABS_MT_TRACKING_ID','2'),event(1,'ABS_MT_POSITION_X','640'),
            event(1,'ABS_MT_POSITION_Y','30'),syn(1),
            event(2,'ABS_MT_SLOT','0'),event(2,'ABS_MT_TRACKING_ID','ffffffff'),syn(2),
            event(3,'ABS_MT_TRACKING_ID','3'),event(3,'ABS_MT_POSITION_Y','40'),syn(3)])
        self.assertEqual(r['max_simultaneous_contacts'],2)
        self.assertEqual(len(r['contacts']),3)
        self.assertEqual(r['contacts'][2]['first_xy'],[16,64])
        self.assertEqual(r['contacts'][1]['out_of_bounds_frames'],3)
        self.assertEqual(r['contacts'][2]['end_reason'],'capture_ended_active')

    def test_numeric_codes_negative_coordinates_and_missing_axis(self):
        r=m.analyze(['[ 1.0] 0003 0039 00000001\n','[ 1.0] 0003 0035 ffffffff\n',
                     '[ 1.0] 0000 0000 00000000\n'],width=20,height=30)
        self.assertEqual(r['contacts'][0]['first_xy'],None)
        self.assertEqual(r['out_of_bounds'][0]['xy'],[-1,None])

    def test_dropped_and_unterminated_report(self):
        r=m.analyze([event(1,'ABS_MT_TRACKING_ID','1'),syn(1),
            event(2,'SYN_DROPPED',typ='EV_SYN'),event(2,'ABS_MT_POSITION_X','20'),syn(2),
            event(3,'ABS_MT_TRACKING_ID','2')])
        self.assertEqual(r['contacts'][0]['end_reason'],'syn_dropped')
        self.assertEqual(r['contacts'][1]['frames'],0)
        self.assertEqual(len(r['warnings']),1)


if __name__ == '__main__':
    unittest.main()
