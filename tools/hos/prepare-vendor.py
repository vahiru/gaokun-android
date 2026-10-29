#!/usr/bin/env python3
"""Patch a COPY of a raw ext4 vendor image, preserving unchanged files and xattrs.

Run in tools/hos/Dockerfile. Produces an experimental image, not a flash command.
debugfs errors are also detected by readback; e2fsck validates the result.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import xml.etree.ElementTree as ET
import zipfile

RECENTS_OVERLAY_PATH = '/overlay/HosRecentsOverlay.apk'


HAL_TARGETS = {
    'android.hardware.usb-service.gaokun3': '/bin/hw/android.hardware.usb-service.gaokun3',
    'gaokun3-usbrole.sh': '/bin/gaokun3-usbrole.sh',
    'usbrole.rc': '/etc/init/usbrole.rc',
}
THERMAL_HAL_PATH = '/bin/hw/android.hardware.thermal-service.gaokun3'
HEXAGONRPCD_PATH = '/bin/hexagonrpcd'


def validate_arm64_executable(blob):
    """Validate ELF64 LE AArch64 executable header and load segment bounds."""
    if len(blob) < 64 or blob[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('expected ELF64 little-endian executable')
    kind, machine, version, entry, phoff, _, _, ehsize, phsize, phnum = struct.unpack_from('<HHIQQQIHHH', blob, 16)
    if (kind not in (2, 3) or machine != 183 or version != 1 or ehsize != 64 or
            phsize != 56 or not phnum or phnum == 65535 or phoff < 64 or
            phoff + phnum * phsize > len(blob)):
        raise ValueError('invalid AArch64 executable/program header')
    executable_entry = False
    for i in range(phnum):
        typ, flags, offset, vaddr, _, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', blob, phoff + i * phsize)
        if offset + filesz > len(blob):
            raise ValueError('ELF segment extends beyond input')
        if typ == 1:
            if filesz > memsz or (align > 1 and (align & (align - 1) or offset % align != vaddr % align)):
                raise ValueError('invalid ELF load segment')
            executable_entry |= bool(flags & 1 and entry and vaddr <= entry < vaddr + filesz)
    if not executable_entry:
        raise ValueError('ELF has no file-backed executable entry point')


def optional_hal_replacements(thermal=None, usb_dir=None, hexagonrpcd=None):
    """Validate the entire optional group before returning any replacements."""
    inputs = []
    if thermal is not None:
        inputs.append((thermal, THERMAL_HAL_PATH, True))
    if hexagonrpcd is not None:
        inputs.append((hexagonrpcd, HEXAGONRPCD_PATH, True))
    if usb_dir is not None:
        if not usb_dir.is_dir() or usb_dir.is_symlink():
            raise ValueError('USB group must be a real directory')
        inputs.extend((usb_dir / name, remote, name.endswith('.gaokun3'))
                      for name, remote in HAL_TARGETS.items())
    replacements, hashes = [], {}
    for source, remote, elf in inputs:
        if source.is_symlink() or not source.is_file():
            raise ValueError('missing/nonregular HAL group input: ' + str(source))
        blob = source.read_bytes()
        if elf:
            validate_arm64_executable(blob)
        else:
            text = blob.decode('utf-8')
            if not text.strip() or '\0' in text:
                raise ValueError('empty/invalid USB group text: ' + str(source))
        replacements.append((remote, blob, None)) # Existing inode metadata/xattrs only.
        hashes[str(source.resolve())] = hashlib.sha256(blob).hexdigest()
    return replacements, hashes


def validate_recents_overlay(content):
    """Check APK container integrity and compiled manifest/resource chunks.

    Signing, target package and the single resource value must also be verified
    by the APK producer with apksigner/aapt2; this does not replace that review.
    """
    try:
        with zipfile.ZipFile(io.BytesIO(content)) as archive:
            names = archive.namelist()
            if len(names) != len(set(names)) or archive.testzip() is not None:
                raise ValueError('duplicate or corrupt recents overlay ZIP entries')
            if any(re.fullmatch(r'classes\d*\.dex', n) or n.startswith('lib/') for n in names):
                raise ValueError('recents overlay must not contain executable code')
            for name, expected_type, minimum_header in (
                    ('AndroidManifest.xml', 0x0003, 8), ('resources.arsc', 0x0002, 12)):
                data = archive.read(name)
                if len(data) < minimum_header:
                    raise ValueError('empty/truncated overlay resource: ' + name)
                chunk_type, header_size, total_size = struct.unpack_from('<HHI', data)
                if (chunk_type != expected_type or header_size < minimum_header or
                        header_size >= total_size or total_size != len(data)):
                    raise ValueError('invalid compiled overlay resource: ' + name)
    except (zipfile.BadZipFile, KeyError, RuntimeError) as error:
        raise ValueError('invalid recents overlay APK') from error


def overlay_template_label(info, label):
    """Require the known existing overlay's root-owned, non-executable inode."""
    mode = re.search(r'Mode:\s+([0-7]+)', info)
    owners = re.search(r'User:\s+(\d+)\s+Group:\s+(\d+)', info)
    if (not mode or not owners or 'Type: regular' not in info or
            (int(mode[1], 8), int(owners[1]), int(owners[2])) != (0o644, 0, 0)):
        raise ValueError('expected root:root 0644 Wi-Fi overlay template')
    if not re.fullmatch(rb'u:object_r:[a-zA-Z0-9_]+:s0\x00?', label):
        raise ValueError('invalid Wi-Fi overlay SELinux label')
    return label.rstrip(b'\0').decode('ascii')


def new_file_metadata(remote):
    if remote.startswith('/etc/') or remote == RECENTS_OVERLAY_PATH:
        return stat.S_IFREG | 0o644, 0, 0
    return stat.S_IFREG | 0o755, 0, 2000

TINYMIX_CIL = '''
;; HOS: narrow vendor tinymix execution label; no system executable dependency.
(type gaokun3_tinymix_exec)
(roletype object_r gaokun3_tinymix_exec)
(typeattributeset file_type (gaokun3_tinymix_exec))
(typeattributeset vendor_file_type (gaokun3_tinymix_exec))
(typeattributeset exec_type (gaokun3_tinymix_exec))
(allow gaokun3_audioroute gaokun3_tinymix_exec (file (ioctl read getattr lock map execute open watch watch_reads execute_no_trans)))
'''

EARLYLOG_CIL = '''
;; HOS bounded pre-init logger keeps only pre-opened tmpfs device descriptors.
;; No access to generic block_device; writer UUID/size guards precede every boot.
(allow kernel tmpfs (chr_file (read)))
(allow kernel tmpfs (blk_file (write)))
(allow kernel self (capability2 (syslog)))
(allow kernel self (system (syslog_read)))
'''

POWERKEEPER_THERMAL_READ_CIL = '''
;; HOS: PowerKeeper enumerates /sys/class/thermal before reading sensor data.
;; system_app is shared by privileged apps; allow thermal inspection only.
;; No writes to cooling states, trip points, governor or thermal policy.
(allow system_app sysfs_thermal (dir (getattr open read search)))
(allow system_app sysfs_thermal (file (getattr open read)))
(allow system_app sysfs_thermal (lnk_file (getattr read)))
'''


def isolated_contexts(contexts):
    """Keep exact by-name block labels after renaming physical partitions.

    ueventd uses the alias in its best-match lookup (init/devices.cpp:452).
    Derive labels from the supplied vendor, rather than guessing new types.
    """
    additions = []
    for name in ('misc', 'metadata', 'super', 'userdata'):
        original = '/dev/block/by-name/' + name
        target = original + '_hos'
        entries = [line.split() for line in contexts.splitlines()
                   if line.strip() and not line.lstrip().startswith('#')]
        if any(e[0] == target for e in entries):
            raise ValueError('isolated block context already exists: ' + target)
        matches = [e for e in entries if e[0] == original]
        if len(matches) != 1 or len(matches[0]) != 3 or matches[0][1] != '-b':
            raise ValueError('expected one exact original block context: ' + original)
        additions.append(f'{target}  -b {matches[0][2]}')
    return contexts.rstrip() + '\n# HOS isolated physical block devices\n' + '\n'.join(additions) + '\n'


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def validate_media_profiles(content):
    root = ET.fromstring(content)
    if root.tag != 'MediaSettings':
        raise ValueError('expected MediaSettings root')
    profiles = root.findall('./CamcorderProfiles/EncoderProfile')
    if not profiles:
        raise ValueError('donor MediaProfiles aborts with no camera profiles')
    for camera in root.findall('CamcorderProfiles'):
        if int(camera.attrib['cameraId']) < 0:
            raise ValueError('negative camera ID')
    for profile in profiles:
        for tag, fields in (('Video', ('bitRate', 'width', 'height', 'frameRate')),
                            ('Audio', ('bitRate', 'sampleRate', 'channels'))):
            codec = profile.find(tag)
            if codec is None or not codec.get('codec'):
                raise ValueError('missing media codec')
            if any(int(codec.attrib[field]) <= 0 for field in fields):
                raise ValueError('nonpositive media profile capability')


def arm64_app_properties(props, partition='vendor'):
    # Match the verified piano vendor ABI contract. Its multiArch SDK APKs only
    # contain arm64 libraries and cannot be scanned while 32-bit apps are advertised.
    if partition not in ('vendor', 'odm'):
        raise ValueError('unsupported ABI property source')
    prefix = 'ro.' + partition + '.product.cpu.'
    changes = {prefix + 'abilist': 'arm64-v8a',
               prefix + 'abilist32': '', prefix + 'abilist64': 'arm64-v8a'}
    if partition == 'vendor':
        changes['ro.zygote'] = 'zygote64'
    for name, value in changes.items():
        pattern = r'^' + re.escape(name) + r'=[^\n]*$'
        if len(re.findall(pattern, props, re.M)) != 1:
            raise ValueError('expected one ABI property: ' + name)
        props = re.sub(pattern, name + '=' + value, props, flags=re.M)
    return props


def optional_pmf_config(text):
    # Donor framework requests ieee80211w=3 (use global default). The baseline
    # default of 0 lets SAE association omit PMF and the AP rejects it with 31.
    # Optional PMF retains compatibility with non-PMF WPA2 networks.
    if re.search(r'^\s*(pmf\s*=|network\s*=)', text, re.M):
        raise ValueError('expected baseline global config without PMF or networks')
    return text.rstrip() + '\n\n# HOS framework default PMF policy\npmf=1\n'


def all_codec2_components_properties(props):
    # Piano system defaults to mode 1; its original vendor overrides that to 4.
    # Mode 1 hides non-AVC hardware codecs and ranks hardware AVC last.
    # Require the reviewed gaokun3 baseline, including its working BLOB poolmask.
    name = 'debug.stagefright.ccodec'
    if re.search(r'^\s*' + re.escape(name) + r'\s*=', props, re.M):
        raise ValueError('unexpected existing C2 component selection; review base image')
    poolmask = re.findall(r'^\s*debug\.stagefright\.c2-poolmask\s*=([^\n]*)$',
                          props, re.M)
    if poolmask != ['0xfc0000']:
        raise ValueError('expected one original C2 poolmask=0xfc0000')
    return props.rstrip() + '\n\n# HOS expose all declared Codec2 components at normal ranks\n' + name + '=4\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'fstab', 'boot-hal', 'tinymix', 'out'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--early-log-policy', action='store_true',
                   help='experimental bounded pre-init misc_hos logger permissions')
    p.add_argument('--media-profiles', type=Path,
                   help='experimental profiles for donor piano and generic vendor paths; recording still needs validation')
    p.add_argument('--composer-apex', type=Path,
                   help='rebuilt DRM composer APEX; requires original manifest and AVB public key')
    p.add_argument('--64-bit-apps', dest='arm64_apps', action='store_true',
                   help='match piano donor arm64-only app ABI and zygote; leaves standalone vendor binaries intact')
    p.add_argument('--wifi-optional-pmf', action='store_true',
                   help='enable optional global PMF for donor framework default network policy')
    p.add_argument('--codec-map-overlay', action='store_true',
                   help='permit init to bind the reviewed Codec2 compatibility library at boot')
    p.add_argument('--all-codec2-components', action='store_true',
                   help='override donor mode 1 with Codec2 mode 4; preserve original 0xfc0000 poolmask')
    p.add_argument('--powerkeeper-thermal-read', action='store_true',
                   help='allow system_app read-only thermal enumeration; no temperature-control writes')
    p.add_argument('--codec2-components', type=Path,
                   help='reviewed arm64 V4L2 component library with pooled ByteBuffer output support')
    p.add_argument('--recents-overlay', type=Path,
                   help='reviewed static framework RRO; add /overlay/HosRecentsOverlay.apk')
    p.add_argument('--thermal-hal', type=Path, help='reviewed ARM64 thermal HAL; replace existing inode')
    p.add_argument('--usb-device-only-dir', type=Path,
                   help='complete USB HAL/script/rc group; does not set persistent device_only policy')
    p.add_argument('--hexagonrpcd', type=Path,
                   help='reviewed ARM64 daemon; replace /bin/hexagonrpcd only, preserve rc and libraries')
    a = p.parse_args()
    hal_replacements, hal_inputs = optional_hal_replacements(a.thermal_hal, a.usb_device_only_dir, a.hexagonrpcd)
    recents_overlay = None
    if a.recents_overlay:
        if not a.recents_overlay.is_file() or not stat.S_ISREG(a.recents_overlay.stat().st_mode):
            p.error('recents overlay must be a regular APK input')
        recents_overlay = a.recents_overlay.read_bytes()
        validate_recents_overlay(recents_overlay)
    media_profiles = None
    if a.media_profiles:
        media_profiles = a.media_profiles.read_bytes()
        validate_media_profiles(media_profiles)
    for f in (a.image, a.fstab, a.boot_hal, a.tinymix):
        if not f.is_file() or not stat.S_ISREG(f.stat().st_mode):
            p.error(f'not a regular input file: {f}')
    for f in (a.boot_hal, a.tinymix):
        blob = f.read_bytes()
        if blob[:5] != b'\x7fELF\x02' or struct.unpack_from('<H', blob, 18)[0] != 183:
            p.error(f'expected arm64 ELF: {f}')
    if a.codec2_components:
        blob = a.codec2_components.read_bytes()
        if len(blob) < 64 or blob[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', blob, 18)[0] != 183:
            p.error('expected little-endian arm64 Codec2 ELF')
    hal = a.boot_hal.read_bytes()
    for marker in (b'refusing non-isolated misc', b'isolated experiment: OTA mutation disabled', b'super_hos'):
        if marker not in hal:
            p.error('boot HAL lacks isolation marker; use the isolated module')
    fstab = a.fstab.read_text()
    for mount, label in (('/misc', 'misc_hos'), ('/data', 'userdata_hos'), ('/metadata', 'metadata_hos')):
        entries = [l.split() for l in fstab.splitlines() if l.strip() and not l.lstrip().startswith('#')]
        matches = [e for e in entries if len(e) == 5 and e[1] == mount]
        if len(matches) != 1 or matches[0][0] != '/dev/block/by-name/' + label:
            p.error(f'fstab is not isolated for {mount}')
    a.out.mkdir(parents=True, exist_ok=False)
    a.out = a.out.resolve()
    image = a.out / 'vendor.img'
    shutil.copyfile(a.image, image)
    logs = []

    def run(argv, allowed=(0,)):
        r = subprocess.run([str(v) for v in argv], capture_output=True)
        logs.append({'argv': [str(v) for v in argv], 'returncode': r.returncode,
                     'output': (r.stdout + r.stderr).decode(errors='replace')})
        (a.out / 'commands.json').write_text(json.dumps(logs, indent=2) + '\n')
        if r.returncode not in allowed:
            raise RuntimeError(logs[-1])
        return (r.stdout + r.stderr).decode(errors='replace')

    def q(value):
        return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'

    def debug(command, write=False):
        return run(['debugfs', *(['-w'] if write else []), '-R', command, image])

    def dump(remote, name):
        local = a.out / name
        debug(f'dump {q(remote)} {q(local)}')
        if not local.is_file():
            raise ValueError(f'debugfs did not extract {remote}')
        return local

    run(['e2fsck', '-f', '-n', image])
    with image.open('r+b') as f:
        f.truncate(max(image.stat().st_size, 384 * 1024 * 1024))
    run(['resize2fs', image])
    original_props = dump('/build.prop', 'original-build.prop').read_text()
    if re.search(r'^media\.c2\.hal\.selection=', original_props, re.M):
        raise ValueError('unexpected existing C2 property; review base image')
    props = original_props.rstrip() + '\n\n# HOS standalone vendor\nmedia.c2.hal.selection=aidl\n'
    if a.arm64_apps:
        props = arm64_app_properties(props)
    if a.all_codec2_components:
        props = all_codec2_components_properties(props)
    audio = dump('/bin/audio-route.sh', 'original-audio-route.sh').read_text()
    old = 'M=/system/bin/tinymix\n[ -x $M ] || M=/vendor/bin/tinymix'
    if audio.count(old) != 1:
        raise ValueError('audio-route baseline changed')
    audio = audio.replace(old, 'M=/vendor/bin/tinymix')
    policy = dump('/etc/selinux/vendor_sepolicy.cil', 'original-vendor_sepolicy.cil').read_text()
    if 'gaokun3_tinymix_exec' in policy:
        raise ValueError('tinymix policy already exists')
    contexts = dump('/etc/selinux/vendor_file_contexts', 'original-vendor_file_contexts').read_text()
    contexts = isolated_contexts(contexts)
    boot_rc = '''# Isolated HOS HAL: no GK3 triggers or shutdown exec.
service vendor.boot-gaokun3 /vendor/bin/hw/android.hardware.boot-service.gaokun3
    class early_hal
    user root
    group root
    capabilities
'''
    replacements = [('/build.prop', props.encode(), None),
                    ('/etc/fstab.gaokun3', a.fstab.read_bytes(), None),
                    ('/bin/audio-route.sh', audio.encode(), None),
                    ('/etc/init/android.hardware.boot-service.gaokun3.rc', boot_rc.encode(), None),
                    ('/bin/hw/android.hardware.boot-service.gaokun3', a.boot_hal.read_bytes(), None),
                    ('/etc/selinux/vendor_sepolicy.cil',
                     (policy + TINYMIX_CIL + (EARLYLOG_CIL if a.early_log_policy else '') +
                      (POWERKEEPER_THERMAL_READ_CIL if a.powerkeeper_thermal_read else '') +
                      ('\n; HOS Codec2 compatibility library bind mount by init\n'
                       '(allow init system_lib_file (file (mounton)))\n'
                       if a.codec_map_overlay else '')).encode(), None),
                    ('/etc/selinux/vendor_file_contexts', (contexts.rstrip() + '\n/(vendor|system/vendor)/bin/tinymix  u:object_r:gaokun3_tinymix_exec:s0\n').encode(), None),
                    ('/bin/tinymix', a.tinymix.read_bytes(), 'u:object_r:gaokun3_tinymix_exec:s0')]
    # Preflight every optional target before replacing any inode, including group members.
    for remote, _, _ in hal_replacements:
        info = debug('stat ' + q(remote))
        if 'Type: regular' not in info:
            raise ValueError('missing/nonregular HAL replacement target: ' + remote)
        if 'security.selinux' not in debug('ea_list ' + q(remote)):
            raise ValueError('missing HAL target SELinux xattr: ' + remote)
    replacements.extend(hal_replacements)
    if a.wifi_optional_pmf:
        wifi = dump('/etc/wifi/wpa_supplicant.conf', 'original-wpa_supplicant.conf').read_text()
        replacements.append(('/etc/wifi/wpa_supplicant.conf', optional_pmf_config(wifi).encode(), None))
    if recents_overlay is not None:
        if 'Type: directory' not in debug('stat /overlay'):
            raise ValueError('missing existing vendor /overlay directory')
        target_info = debug('stat ' + q(RECENTS_OVERLAY_PATH))
        if 'File not found' not in target_info:
            raise ValueError('recents overlay target already exists or cannot be inspected')
        template = '/overlay/Gaokun3WifiOverlay.apk'
        template_info = debug('stat ' + q(template))
        label_file = a.out / 'recents-template-selinux'
        debug(f'ea_get -f {q(label_file)} {q(template)} security.selinux')
        if not label_file.is_file():
            raise ValueError('missing Wi-Fi overlay template SELinux label')
        label = overlay_template_label(template_info, label_file.read_bytes())
        replacements.append((RECENTS_OVERLAY_PATH, recents_overlay, label))
    if a.codec2_components:
        remote = '/lib64/libv4l2_codec2_components.so'
        original = dump(remote, 'original-libv4l2_codec2_components.so')
        if sha(original) != '0b36f7b13b5accd0b1475895d91a6483559f8dc294622fc6b19d791e1901acbe':
            raise ValueError('unexpected V4L2 component baseline; review before replacing')
        replacements.append((remote, a.codec2_components.read_bytes(), None))
    if a.arm64_apps:
        # init selects product > odm > vendor > system. The baseline keeps ODM
        # properties inside vendor, so vendor-only changes would still expose ARM32.
        odm = dump('/odm/etc/build.prop', 'original-odm-build.prop').read_text()
        replacements.append(('/odm/etc/build.prop', arm64_app_properties(odm, 'odm').encode(), None))
    if media_profiles is not None:
        for suffix in ('vendor', 'piano'):
            remote = f'/etc/media_profiles_{suffix}.xml'
            if 'File not found' not in debug('stat ' + q(remote)):
                raise ValueError('refusing to overwrite existing media profiles: ' + remote)
            replacements.append((remote, media_profiles, 'u:object_r:vendor_configs_file:s0'))
    if a.composer_apex:
        remote = '/apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex'
        original = dump(remote, 'original-composer.apex')
        content = a.composer_apex.read_bytes()
        with zipfile.ZipFile(original) as old, zipfile.ZipFile(io.BytesIO(content)) as new:
            if new.testzip() is not None:
                raise ValueError('corrupt composer APEX ZIP')
            for name in ('apex_manifest.pb', 'apex_pubkey'):
                if old.read(name) != new.read(name):
                    raise ValueError('composer APEX identity/key changed: ' + name)
            if not new.getinfo('apex_payload.img').file_size:
                raise ValueError('empty composer payload')
        replacements.append((remote, content, None))
    manifest = {'inputs': {str(f.resolve()): sha(f) for f in (a.image, a.fstab, a.boot_hal, a.tinymix)},
                'early_log_policy': a.early_log_policy,
                'arm64_only_apps': a.arm64_apps,
                'wifi_optional_pmf': a.wifi_optional_pmf,
                'codec_map_overlay': a.codec_map_overlay,
                'all_codec2_components': a.all_codec2_components,
                'powerkeeper_thermal_read': a.powerkeeper_thermal_read,
                'recents_overlay': ({'source': str(a.recents_overlay.resolve()),
                                     'sha256': sha(a.recents_overlay),
                                     'template': '/overlay/Gaokun3WifiOverlay.apk'}
                                    if a.recents_overlay else None),
                'codec2_components': ({'source': str(a.codec2_components.resolve()),
                                       'sha256': sha(a.codec2_components)}
                                      if a.codec2_components else None),
                'replacements': [], 'removed': [], 'ready_to_boot': False,
                'remaining': ['Compile patched vendor CIL with repaired donor debug policy',
                              'Repack donor system_ext with repaired debug CIL and disable llkd',
                              'Disable system update_engine/update_verifier activation paths as appropriate',
                              'Complete mi_ext/mi_product, RRO, root mountpoints and super_hos packaging',
                              'Prepare real isolated partitions and early logs, then attended validation']}
    manifest['inputs'].update(hal_inputs)
    manifest['optional_hal_targets'] = [remote for remote, _, _ in hal_replacements]
    for i, (remote, content, new_label) in enumerate(replacements):
        attrs = {}
        label = new_label
        if label is None:
            info = debug('stat ' + q(remote))
            mode = re.search(r'Mode:\s+([0-7]+)', info)
            owners = re.search(r'User:\s+(\d+)\s+Group:\s+(\d+)', info)
            if not mode or not owners or 'Type: regular' not in info:
                raise ValueError(f'unsupported original inode: {remote}')
            metadata = (int(mode[1], 8) | stat.S_IFREG, int(owners[1]), int(owners[2]))
            names = re.findall(r'^\s+([\w.-]+) \(\d+\) =', debug('ea_list ' + q(remote)), re.M)
            for name in names:
                dest = a.out / f'xattr-{i}-{name}'
                debug(f'ea_get -f {q(dest)} {q(remote)} {q(name)}')
                if not dest.is_file():
                    raise ValueError('failed xattr extraction')
                attrs[name] = dest
            if 'security.selinux' not in attrs:
                raise ValueError(f'missing SELinux xattr: {remote}')
            debug('rm ' + q(remote), True)
        else:
            # New configuration is root-owned 0644; tinymix remains an executable.
            metadata = new_file_metadata(remote)
            dest = a.out / f'xattr-{i}-security.selinux'
            dest.write_bytes(label.encode() + b'\0')
            attrs['security.selinux'] = dest
        local = a.out / f'replacement-{i}'
        local.write_bytes(content)
        debug(f'write {q(local)} {q(remote)}', True)
        for key, value in zip(('mode', 'uid', 'gid'), metadata):
            encoded = f'0{value:o}' if key == 'mode' else str(value)
            debug(f'set_inode_field {q(remote)} {key} {encoded}', True)
        for key in ('atime', 'ctime', 'mtime', 'crtime'):
            debug(f'set_inode_field {q(remote)} {key} 1230768000', True)
        for name, source in attrs.items():
            debug(f'ea_set -f {q(source)} {q(remote)} {q(name)}', True)
        readback = dump(remote, f'readback-{i}')
        if readback.read_bytes() != content:
            raise ValueError(f'content readback mismatch: {remote}')
        info = debug('stat ' + q(remote))
        mode = re.search(r'Mode:\s+([0-7]+)', info)
        owners = re.search(r'User:\s+(\d+)\s+Group:\s+(\d+)', info)
        if not mode or not owners or (int(mode[1], 8) | stat.S_IFREG, int(owners[1]), int(owners[2])) != metadata:
            raise ValueError(f'metadata readback mismatch: {remote}')
        for name, source in attrs.items():
            dest = a.out / f'readback-xattr-{i}-{name}'
            debug(f'ea_get -f {q(dest)} {q(remote)} {q(name)}')
            if dest.read_bytes() != source.read_bytes():
                raise ValueError(f'xattr readback mismatch: {remote}:{name}')
        manifest['replacements'].append({'path': remote, 'sha256': sha(local), 'mode': oct(metadata[0]),
                                         'uid': metadata[1], 'gid': metadata[2],
                                         'xattrs': {n: f.read_bytes().hex() for n, f in attrs.items()}})
    # Export the exact final CIL and contexts for offline policy compilation.
    for remote in ('vendor_sepolicy.cil', 'vendor_file_contexts'):
        dump('/etc/selinux/' + remote, remote)
    # Platform hash markers alone do not include vendor CIL changes. Leaving this cache
    # would let the baseline init silently load the original policy without our new type.
    for name in ('precompiled_sepolicy',
                 'precompiled_sepolicy.plat_sepolicy_and_mapping.sha256',
                 'precompiled_sepolicy.system_ext_sepolicy_and_mapping.sha256',
                 'precompiled_sepolicy.product_sepolicy_and_mapping.sha256'):
        remote = '/etc/selinux/' + name
        debug('rm ' + q(remote), True)
        if 'File not found' not in debug('stat ' + q(remote)):
            raise ValueError('precompiled cache removal failed: ' + remote)
        manifest['removed'].append(remote)
    run(['e2fsck', '-f', '-n', image])
    manifest.update(image_sha256=sha(image), image_size=image.stat().st_size)
    (a.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (a.out / 'commands.json').write_text(json.dumps(logs, indent=2) + '\n')
    print(f'Prepared {image}; content, metadata, xattrs and e2fsck verified')


if __name__ == '__main__':
    main()
