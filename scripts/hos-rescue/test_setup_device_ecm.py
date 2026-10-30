import contextlib
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

spec=importlib.util.spec_from_file_location('ecm',Path(__file__).with_name('setup-device-ecm.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


class Fake(m.System):
    def __init__(self):
        self.ident=(0,'gaokun3-rescue','/dev/nvme0n1p3',m.FLAG)
        self.events=[];self.g=[];self.snap=dict(m.EXPECTED);self.record=None
        self.bound='';self.legacy='';self.role_value='host';self.managed='no'
        self.managers={'NetworkManager.service':'active','systemd-networkd.service':'inactive'}
        self.ips=set();self.route_set=set();self.add_count=0
        self.descriptor={'role':'device','state':'configured',
                         'current_speed':'high-speed','maximum_speed':'super-speed-plus'}
    def identity(self):return self.ident
    @contextlib.contextmanager
    def lock(self):yield
    def boot(self):return 'boot-test'
    def manager(self,name):return self.managers[name]
    def prepare(self):self.events.append('prepare')
    def names(self):return self.g
    def snapshot(self):return dict(self.snap)
    def read_marker(self):return self.record
    def save_marker(self,s):self.record={'schema':1,'boot':self.boot(),'snapshot':dict(s)}
    def discovery_rule(self):self.events.append('rule')
    def create(self):self.events.append('create');self.g=[m.NAME]
    def device_role(self):
        if self.role_value!='device':self.events.append('device');self.role_value='device'
    def binding(self):return self.bound
    def udc_function(self):return self.legacy
    def descriptor_evidence(self):return dict(self.descriptor)
    def bind(self):
        if not self.bound:self.events.append('bind');self.bound=m.UDC
        elif self.bound!=m.UDC:raise m.Refuse('wrong binding')
    def interface(self):return 'enx02484f531201'
    def addresses(self,i):return set(self.ips)
    def routes(self,i):return set(self.route_set)
    def verify(self,i):self.events.append('verify')
    def run(self,*args,check=True):
        self.events.append(args)
        if args[:3]==('nmcli','-g','GENERAL.NM-MANAGED'):
            return SimpleNamespace(returncode=0,stdout=self.managed+'\n',stderr='')
        if args[:3]==('nmcli','device','set'):
            self.managed='no'
        elif args[:3]==('ip','addr','add'):
            self.add_count+=1;self.ips.add(('inet',m.ADDRESS,16))
        elif args[:3]==('ip','link','set'):
            pass
        else:raise AssertionError('unexpected command '+repr(args))
        return SimpleNamespace(returncode=0,stdout='',stderr='')
    def owned(self):
        self.g=[m.NAME];self.save_marker(self.snap)


class SafetyTests(unittest.TestCase):
    def test_configfs_mac_accepts_only_exact_supported_suffixes(self):
        for mac in (b'02:48:4f:53:12:01', b'02:48:4F:53:12:01'):
            for suffix in (b'', b'\n', b'\0', b'\0\n'):
                with self.subTest(mac=mac,suffix=suffix):
                    self.assertEqual(m.parse_configfs_mac(mac+suffix),m.MAC)

    def test_configfs_mac_rejects_embedded_nul_garbage_and_bad_lengths(self):
        mac=b'02:48:4f:53:12:01'
        invalid=[b'',mac[:-1],mac+b'0',b'2:48:4f:53:12:01',b'02:48:4g:53:12:01',
                 b'02-48-4f-53-12-01',b'02:48:4f:53:12',b'02:48:4f:53:12:01:00',
                 mac[:5]+b'\0'+mac[5:],mac+b'\0garbage',mac+b'garbage\0',
                 mac+b'\0\0',mac+b'\n\n',mac+b'\n\0',mac+b'\r\n',
                 b' '+mac,mac+b' ',mac+b'\t',mac+b'\0 \n',b'\xff'+mac,mac.decode()]
        for value in invalid:
            with self.subTest(value=value),self.assertRaises(m.Refuse):
                m.parse_configfs_mac(value)

    def test_real_snapshot_normalizes_configfs_mac_without_adopting_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);g=root/m.NAME
            (g/'strings/0x409').mkdir(parents=True)
            (g/'configs/c.1/strings').mkdir(parents=True)
            (g/'functions/ecm.usb0').mkdir(parents=True)
            for name,value in {'idVendor':'0x1d6b','idProduct':'0x0104',
                               'bcdUSB':'0x0000','bcdDevice':'0x0702',
                               'bDeviceClass':'0x00','bDeviceSubClass':'0x00',
                               'bDeviceProtocol':'0x00'}.items():
                (g/name).write_text(value+'\n')
            for name in ('serialnumber','manufacturer','product'):
                (g/'strings/0x409'/name).write_text(m.EXPECTED[name]+'\n')
            (g/'functions/ecm.usb0/dev_addr').write_bytes(m.MAC.upper().encode()+b'\0\n')
            (g/'functions/ecm.usb0/host_addr').write_bytes(m.HOST_MAC.encode()+b'\0')
            (g/'configs/c.1/ecm.usb0').symlink_to(g/'functions/ecm.usb0')
            (g/'configs/c.1/MaxPower').write_text('2\n')
            (g/'configs/c.1/bmAttributes').write_text('0x80\n')
            runtime=m.System();runtime.gadgets=root
            snapshot=runtime.snapshot()
            self.assertEqual(snapshot['dev_addr'],m.MAC)
            self.assertEqual(snapshot['host_addr'],m.HOST_MAC)
            self.assertTrue(all(snapshot[k]==v for k,v in m.EXPECTED.items()))
            with self.assertRaises(m.Refuse):
                m.check_owned(snapshot,None,'boot-test')
            marker={'schema':1,'boot':'boot-test','snapshot':dict(snapshot)}
            (g/'bcdUSB').write_text('0x0210\n')
            enumerated=runtime.snapshot()
            evidence={'role':'device','state':'configured',
                      'current_speed':'high-speed','maximum_speed':'super-speed'}
            m.check_owned(enumerated,marker,'boot-test',m.UDC,evidence)
            self.assertEqual(marker['snapshot']['bcdUSB'],'0x0000')
            with self.assertRaises(m.Refuse):
                m.check_owned(enumerated,None,'boot-test',m.UDC,evidence)
            (g/'configs/c.1/MaxPower').write_text('500\n')
            with self.assertRaises(m.Refuse):
                m.check_owned(runtime.snapshot(),marker,'boot-test',m.UDC,evidence)

    def test_identity_guards_precede_all_actions(self):
        for ident in [(1000,'gaokun3-rescue','/dev/nvme0n1p3',m.FLAG),
                      (0,'android','/dev/nvme0n1p3',m.FLAG),
                      (0,'gaokun3-rescue','/dev/nvme0n1p2',m.FLAG),
                      (0,'gaokun3-rescue','/dev/nvme0n1p3',''),
                      (0,'gaokun3-rescue','/dev/nvme0n1p3',m.FLAG+' '+m.FLAG),
                      (0,'gaokun3-rescue','/dev/nvme0n1p3',m.FLAG+' gaokun3.hos_rescue_device=0')]:
            f=Fake();f.ident=ident
            with self.subTest(ident=ident),self.assertRaises(m.Refuse):m.setup(f)
            self.assertEqual(f.events,[])

    def test_foreign_configfs_and_legacy_gadgets_do_not_change_role(self):
        for names,legacy in [(['adb'],''),([m.NAME,'adb'],''),([], 'g_ether')]:
            f=Fake();f.g=names;f.legacy=legacy
            with self.assertRaises(m.Refuse):m.setup(f)
            self.assertEqual(f.events,['prepare'])
            self.assertEqual(f.role_value,'host')

    def test_mismatched_or_partial_owned_gadget_refused(self):
        for fault in ('marker','boot','mac','link','udc','extra-default'):
            f=Fake();f.owned()
            if fault=='marker':f.record=None
            elif fault=='boot':f.record['boot']='old-boot'
            elif fault=='mac':f.snap['dev_addr']='02:00:00:00:00:01'
            elif fault=='link':f.snap['link_target']='functions/rndis.usb0'
            elif fault=='udc':f.bound='foreign-controller'
            else:f.snap['MaxPower']='500'
            with self.subTest(fault=fault),self.assertRaises(m.Refuse):m.setup(f)
            self.assertEqual(f.events,['prepare'])

    def test_repeated_success_preserves_binding_addresses_routes(self):
        f=Fake();f.ips={('inet','192.0.2.5',24)};f.route_set={'existing-route'}
        m.setup(f);m.setup(f)
        self.assertEqual(f.events.count('create'),1)
        self.assertEqual(f.events.count('bind'),1)
        self.assertEqual(f.events.count('device'),1)
        self.assertEqual(f.add_count,1)
        self.assertIn(('inet','192.0.2.5',24),f.ips)
        self.assertEqual(f.route_set,{'existing-route'})

    def test_existing_correct_binding_never_rebound(self):
        f=Fake();f.owned();f.bound=m.UDC;f.legacy='configfs-gadget';f.role_value='device'
        m.setup(f)
        self.assertNotIn('bind',f.events);self.assertNotIn('device',f.events)

    def test_kernel_bcdusb_enumeration_change_reuses_without_rewriting_marker(self):
        f=Fake();f.snap['bcdUSB']='0x0000';f.owned()
        original_record=json.loads(json.dumps(f.record))
        f.snap['bcdUSB']='0x0210';f.bound=m.UDC;f.role_value='device'
        f.ips.add(('inet',m.ADDRESS,16));f.route_set.add('existing-route')
        m.setup(f);m.setup(f)
        self.assertEqual(f.record,original_record)
        self.assertNotIn('bind',f.events);self.assertNotIn('device',f.events)
        self.assertEqual(f.add_count,0)
        self.assertEqual(f.route_set,{'existing-route'})

    def test_bcdusb_exception_requires_exact_controller_speed_and_only_one_difference(self):
        cases=[('current_speed','super-speed'),('current_speed','unknown'),
               ('maximum_speed','high-speed'),('state','not attached'),('role','host')]
        for key,value in cases:
            f=Fake();f.snap['bcdUSB']='0x0000';f.owned();f.snap['bcdUSB']='0x0210'
            f.bound=m.UDC;f.descriptor[key]=value
            with self.subTest(key=key,value=value),self.assertRaises(m.Refuse):m.setup(f)
            self.assertEqual(f.events,['prepare'])
        for fault in ('unbound','wrong-boot','other-field','wrong-before','wrong-after','no-marker'):
            f=Fake();f.snap['bcdUSB']='0x0000';f.owned();f.snap['bcdUSB']='0x0210';f.bound=m.UDC
            if fault=='unbound':f.bound=''
            elif fault=='wrong-boot':f.record['boot']='old-boot'
            elif fault=='other-field':f.snap['bcdDevice']='0x9999'
            elif fault=='wrong-before':f.record['snapshot']['bcdUSB']='0x0200'
            elif fault=='wrong-after':f.snap['bcdUSB']='0x0320'
            else:f.record=None
            with self.subTest(fault=fault),self.assertRaises(m.Refuse):m.setup(f)
            self.assertEqual(f.events,['prepare'])

    def test_nm_only_changes_owned_empty_interface(self):
        f=Fake();f.managed='yes';m.setup(f)
        self.assertIn(('nmcli','device','set',f.interface(),'managed','no'),f.events)
        self.assertEqual(f.managed,'no')
        for existing in ('address','route'):
            f=Fake();f.managed='yes'
            if existing=='address':f.ips.add(('inet','192.0.2.7',24))
            else:f.route_set.add('existing')
            with self.assertRaises(m.Refuse):m.setup(f)
            self.assertNotIn(('nmcli','device','set',f.interface(),'managed','no'),f.events)
            self.assertEqual(f.managed,'yes')

    def test_inactive_nm_is_not_started_or_reconfigured(self):
        f=Fake();f.managers['NetworkManager.service']='inactive';m.setup(f)
        self.assertFalse(any(isinstance(e,tuple) and e[0]=='nmcli' for e in f.events))

    def test_networkd_refused_without_stopping_it(self):
        f=Fake();f.managers['systemd-networkd.service']='active'
        with self.assertRaises(m.Refuse):m.setup(f)
        self.assertEqual(f.events,[])

    def test_conflicting_prefix_not_replaced(self):
        f=Fake();f.ips.add(('inet',m.ADDRESS,24))
        with self.assertRaises(m.Refuse):m.setup(f)
        self.assertEqual(f.add_count,0)
        self.assertEqual(f.ips,{('inet',m.ADDRESS,24)})

    def test_network_failure_keeps_bound_device_no_rollback(self):
        f=Fake();run=f.run
        def failed(*args,**kwargs):
            if args[:3]==('ip','addr','add'):
                raise m.Refuse('injected address failure')
            return run(*args,**kwargs)
        f.run=failed
        with self.assertRaises(m.Refuse):m.setup(f)
        self.assertEqual(f.bound,m.UDC)
        self.assertEqual(f.role_value,'device')
        self.assertIsNotNone(f.record)
        self.assertNotIn('verify',f.events)


if __name__=='__main__':unittest.main()
