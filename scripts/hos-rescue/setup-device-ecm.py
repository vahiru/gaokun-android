#!/usr/bin/python3
"""Flag-only rescue ECM setup. No unbind, address flush, route deletion or host writes."""
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

FLAG = 'gaokun3.hos_rescue_device=1'
NAME = 'hos_transfer'
UDC = 'a600000.usb'
MAC = '02:48:4f:53:12:01'
HOST_MAC = '02:48:4f:53:12:02'
ADDRESS = '169.254.77.1'
EXPECTED = {'idVendor': '0x1d6b', 'idProduct': '0x0104',
            'serialnumber': 'gaokun3-hos-transfer', 'manufacturer': 'gaokun3',
            'product': 'HOS rescue transfer', 'dev_addr': MAC, 'host_addr': HOST_MAC,
            'functions': ['ecm.usb0'], 'configs': ['c.1'], 'languages': ['0x409'],
            'links': ['ecm.usb0'], 'link_target': 'functions/ecm.usb0'}


class Refuse(RuntimeError):
    pass


def parse_configfs_mac(raw):
    """Accept a MAC plus at most one terminal NUL and then one LF.

    Some configfs ECM attributes include the terminating C-string byte in
    their read length. Do not strip arbitrary whitespace or embedded NULs.
    """
    if not isinstance(raw, bytes):
        raise Refuse('configfs MAC must be raw bytes')
    match = re.fullmatch(rb'([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\x00?\n?', raw)
    if match is None:
        raise Refuse('invalid configfs MAC syntax or suffix')
    return match.group(1).decode('ascii').lower()


def check_identity(uid, hostname, root, cmdline):
    tokens = cmdline.split()
    flags = [t for t in tokens if t == 'gaokun3.hos_rescue_device' or
             t.startswith('gaokun3.hos_rescue_device=')]
    if uid != 0 or hostname != 'gaokun3-rescue' or root != '/dev/nvme0n1p3' or flags != [FLAG]:
        raise Refuse('requires root, gaokun3-rescue, root p3 and exactly one device flag')


def check_owned(snapshot, marker, boot, binding='', descriptor_evidence=None):
    if any(snapshot.get(k) != v for k, v in EXPECTED.items()):
        raise Refuse('owned gadget has unexpected configuration')
    if marker == {'schema': 1, 'boot': boot, 'snapshot': snapshot}:
        return
    # composite_setup(USB_DT_DEVICE) rewrites cdev.desc.bcdUSB, which is
    # exactly what configfs exposes. Only accept the proven USB2-on-SS case,
    # not arbitrary descriptor drift or a forged replacement marker.
    if (isinstance(marker, dict) and set(marker) == {'schema','boot','snapshot'} and
            marker['schema'] == 1 and marker['boot'] == boot and
            isinstance(marker['snapshot'], dict) and binding == UDC and
            isinstance(descriptor_evidence, dict) and
            descriptor_evidence.get('role') == 'device' and
            descriptor_evidence.get('state') == 'configured' and
            descriptor_evidence.get('maximum_speed') in ('super-speed','super-speed-plus') and
            descriptor_evidence.get('current_speed') in ('full-speed','high-speed') and
            marker['snapshot'].get('bcdUSB') == '0x0000' and
            snapshot.get('bcdUSB') == '0x0210'):
        before_enumeration = dict(snapshot, bcdUSB='0x0000')
        if before_enumeration == marker['snapshot']:
            return
    raise Refuse('missing/mismatched current-boot ownership record; do not adopt or rebuild')


class System:
    gadgets = Path('/sys/kernel/config/usb_gadget')
    role = Path('/sys/class/usb_role/a600000.usb-role-switch/role')
    marker = Path('/run/gaokun3-hos-rescue-ecm.json')
    rule = Path('/run/udev/rules.d/90-gaokun3-hos-rescue-ecm.rules')

    def run(self, *args, check=True):
        result = subprocess.run(args, text=True, capture_output=True, timeout=8,
                                env=dict(os.environ, LC_ALL='C'))
        if check and result.returncode:
            raise Refuse('command failed: '+repr(args)+': '+result.stderr.strip())
        return result

    def identity(self):
        return (os.geteuid(), socket.gethostname(),
                self.run('findmnt', '-nro', 'SOURCE', '/').stdout.strip(),
                Path('/proc/cmdline').read_text())

    def boot(self):
        return Path('/proc/sys/kernel/random/boot_id').read_text().strip()

    @contextlib.contextmanager
    def lock(self):
        fd = os.open('/run/gaokun3-hos-rescue-ecm.lock',
                     os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            yield
        finally:
            os.close(fd)

    def manager(self, name):
        r = self.run('systemctl', 'show', name, '--property=ActiveState', '--value', check=False)
        if r.returncode or r.stdout.strip() not in ('active', 'inactive', 'failed'):
            raise Refuse('cannot establish stable manager state: '+name)
        return r.stdout.strip()

    def prepare(self):
        self.run('modprobe', 'libcomposite')
        r = self.run('findmnt', '-nro', 'FSTYPE', '--mountpoint', '/sys/kernel/config', check=False)
        if r.returncode:
            self.run('mount', '-t', 'configfs', 'none', '/sys/kernel/config')
        elif r.stdout.strip() != 'configfs':
            raise Refuse('/sys/kernel/config is mounted with an unexpected filesystem')

    def names(self):
        return sorted(p.name for p in self.gadgets.iterdir())

    def snapshot(self):
        g = self.gadgets / NAME
        def read(p):
            return p.read_text().strip()
        result = {n: read(g/n) for n in ('idVendor','idProduct','bcdUSB','bcdDevice',
                                         'bDeviceClass','bDeviceSubClass','bDeviceProtocol')}
        result.update({n: read(g/'strings/0x409'/n) for n in ('serialnumber','manufacturer','product')})
        result.update({n: parse_configfs_mac((g/'functions/ecm.usb0'/n).read_bytes())
                       for n in ('dev_addr','host_addr')})
        result['functions'] = sorted(p.name for p in (g/'functions').iterdir())
        result['configs'] = sorted(p.name for p in (g/'configs').iterdir())
        result['languages'] = sorted(p.name for p in (g/'strings').iterdir())
        result['config_languages'] = sorted(p.name for p in (g/'configs/c.1/strings').iterdir())
        result['links'] = sorted(p.name for p in (g/'configs/c.1').iterdir() if p.is_symlink())
        result['link_target'] = str((g/'configs/c.1/ecm.usb0').resolve().relative_to(g.resolve()))
        for n in ('MaxPower','bmAttributes'):
            result[n] = read(g/'configs/c.1'/n)
        return result

    def read_marker(self):
        if not self.marker.exists() or self.marker.is_symlink():
            return None
        s = self.marker.stat()
        if s.st_uid != 0 or s.st_mode & 0o077:
            raise Refuse('ownership record has unsafe owner/mode')
        return json.loads(self.marker.read_text())

    def save_marker(self, snapshot):
        value = {'schema': 1, 'boot': self.boot(), 'snapshot': snapshot}
        fd = os.open(self.marker, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as f:
            json.dump(value, f, sort_keys=True); f.flush(); os.fsync(f.fileno())

    def discovery_rule(self):
        # New netdev ADD event gets a per-MAC runtime rule before NM can claim
        # it. Never trigger all devices or modify persistent Wi-Fi profiles.
        text = f'SUBSYSTEM=="net", ATTR{{address}}=="{MAC}", ENV{{NM_UNMANAGED}}="1"\n'
        self.rule.parent.mkdir(parents=True, exist_ok=True)
        if self.rule.exists():
            if self.rule.is_symlink() or self.rule.read_text() != text:
                raise Refuse('conflicting runtime udev rule')
            return
        else:
            fd = os.open(self.rule, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'w') as f:
                f.write(text)
        self.run('udevadm', 'control', '--reload-rules')

    def create(self):
        if self.marker.exists():
            raise Refuse('stale ownership record without gadget; inspect before retry')
        g = self.gadgets / NAME
        g.mkdir()
        for name in ('idVendor','idProduct'):
            (g/name).write_text(EXPECTED[name])
        (g/'strings/0x409').mkdir()
        for name in ('serialnumber','manufacturer','product'):
            (g/'strings/0x409'/name).write_text(EXPECTED[name])
        (g/'configs/c.1').mkdir()
        (g/'functions/ecm.usb0').mkdir()
        for name in ('dev_addr','host_addr'):
            (g/'functions/ecm.usb0'/name).write_text(EXPECTED[name])
        (g/'configs/c.1/ecm.usb0').symlink_to(g/'functions/ecm.usb0')

    def binding(self):
        return (self.gadgets/NAME/'UDC').read_text().strip()

    def udc_function(self):
        path = Path('/sys/class/udc')/UDC/'function'
        return path.read_text().strip() if path.exists() else ''

    def descriptor_evidence(self):
        root = Path('/sys/class/udc')/UDC
        value = {name: (root/name).read_text().strip()
                 for name in ('state','current_speed','maximum_speed')}
        value['role'] = self.role.read_text().strip()
        return value

    def device_role(self):
        deadline = time.monotonic()+12
        while not self.role.exists() and time.monotonic()<deadline:
            time.sleep(.2)
        role = self.role.read_text().strip()
        if role not in ('device','host','none'):
            raise Refuse('unexpected USB role '+repr(role))
        if role != 'device':
            self.role.write_text('device')
        while time.monotonic()<deadline:
            if self.role.read_text().strip() == 'device' and (Path('/sys/class/udc')/UDC).exists():
                return
            time.sleep(.2)
        raise Refuse('device role/UDC not ready; no unbind attempted')

    def bind(self):
        current = self.binding()
        if current == UDC:
            return
        if current:
            raise Refuse('unexpected UDC binding')
        (self.gadgets/NAME/'UDC').write_text(UDC)
        if self.binding() != UDC:
            raise Refuse('UDC readback failed')

    def interface(self):
        deadline = time.monotonic()+8
        while time.monotonic()<deadline:
            matches = [p.name for p in Path('/sys/class/net').iterdir()
                       if (p/'address').read_text().strip().lower() == MAC]
            if len(matches)>1:
                raise Refuse('ambiguous ECM device MAC')
            if len(matches)==1:
                name=matches[0]
                declared=(self.gadgets/NAME/'functions/ecm.usb0/ifname').read_text().strip()
                if declared != name or not re.fullmatch(r'[A-Za-z0-9_.:-]{1,15}',name):
                    raise Refuse('ECM ifname/MAC ownership mismatch')
                return name
            time.sleep(.2)
        raise Refuse('ECM network interface not found')

    def addresses(self, interface):
        records=json.loads(self.run('ip','-j','addr','show','dev',interface).stdout)
        return {(a['family'],a['local'],a['prefixlen']) for r in records for a in r['addr_info']}

    def routes(self, interface):
        records=json.loads(self.run('ip','-j','route','show','table','all','dev',interface).stdout)
        return {json.dumps({k:v for k,v in r.items() if k not in ('expires','cache','flags')},sort_keys=True)
                for r in records}

    def unmanaged(self, interface, active):
        if self.manager('NetworkManager.service') != active:
            raise Refuse('NetworkManager state changed during setup')
        if active != 'active':
            return
        managed=self.run('nmcli','-g','GENERAL.NM-MANAGED','device','show',interface).stdout.strip().lower()
        if managed == 'no':
            return
        if managed != 'yes':
            raise Refuse('cannot determine ECM NetworkManager ownership')
        if self.addresses(interface) or self.routes(interface):
            raise Refuse('managed ECM already has addresses/routes; refusing disruptive takeover')
        self.run('nmcli','device','set',interface,'managed','no')
        if self.run('nmcli','-g','GENERAL.NM-MANAGED','device','show',interface).stdout.strip().lower() != 'no':
            raise Refuse('per-interface unmanaged readback failed')

    def network(self, interface):
        before=self.addresses(interface); routes=self.routes(interface)
        if any(f=='inet' and a==ADDRESS and prefix!=16 for f,a,prefix in before):
            raise Refuse('desired IPv4 address exists with a conflicting prefix')
        if ('inet',ADDRESS,16) not in before:
            self.run('ip','addr','add',ADDRESS+'/16','dev',interface)
        self.run('ip','link','set','dev',interface,'up')
        after=self.addresses(interface)
        if ('inet',ADDRESS,16) not in after or not before <= after or not routes <= self.routes(interface):
            raise Refuse('address/route preservation verification failed')

    def verify(self, interface):
        if self.role.read_text().strip() != 'device' or self.binding() != UDC:
            raise Refuse('USB role/binding changed during network setup')
        if self.interface() != interface:
            raise Refuse('ECM interface changed')
        state=(Path('/sys/class/udc')/UDC/'state').read_text().strip()
        print(f'ECM configured: {interface} {ADDRESS}/16 role=device UDC={UDC} state={state}; peer connectivity still requires host check')


def setup(system):
    check_identity(*system.identity())
    with system.lock():
        if system.manager('systemd-networkd.service') == 'active':
            raise Refuse('active networkd rules have not been reviewed; refusing to compete')
        nm=system.manager('NetworkManager.service')
        system.prepare()
        names=system.names()
        if names not in ([],[NAME]):
            raise Refuse('foreign gadget present; role unchanged')
        if names:
            snapshot, marker, binding = system.snapshot(),system.read_marker(),system.binding()
            if binding not in ('',UDC):
                raise Refuse('owned gadget bound to unexpected UDC; role unchanged')
            evidence = None
            if (isinstance(marker, dict) and isinstance(marker.get('snapshot'), dict) and
                    snapshot.get('bcdUSB') != marker['snapshot'].get('bcdUSB')):
                evidence = system.descriptor_evidence()
            check_owned(snapshot, marker, system.boot(), binding, evidence)
        if (not names or not system.binding()) and system.udc_function():
            raise Refuse('UDC already used by another gadget driver; role unchanged')
        system.discovery_rule()
        if not names:
            system.create()
            snapshot=system.snapshot()
            if any(snapshot.get(k)!=v for k,v in EXPECTED.items()):
                raise Refuse('new gadget does not match expected identity')
            system.save_marker(snapshot)
        system.device_role()
        system.bind()
        interface=system.interface()
        system.unmanaged(interface,nm)
        system.network(interface)
        system.verify(interface)


if __name__=='__main__':
    if len(sys.argv) != 1:
        raise SystemExit('No arguments supported; this is only for the flagged rescue boot service')
    try:
        setup(System())
    except (Refuse,OSError,ValueError,subprocess.SubprocessError) as exc:
        raise SystemExit('REFUSED/FAILED (no automatic unbind or host rollback): '+str(exc))
