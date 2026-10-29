#!/usr/bin/env python3
"""Replace only DRM composer in an existing ext4 APEX, retaining labels and keys.

Run on the Linux build host. Does not touch devices or relax SELinux checks.
The input APEX and replacement binary must already have trusted provenance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('apex', 'composer', 'key_dir', 'host_tools', 'out'):
        parser.add_argument('--' + name.replace('_', '-'), type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    args.out = args.out.resolve()
    args.host_tools = args.host_tools.resolve()
    keys = args.key_dir.resolve()
    blob = args.composer.read_bytes()
    if blob[:5] != b'\x7fELF\x02' or blob[18:20] != b'\xb7\x00':
        raise ValueError('expected arm64 ELF')
    logs = []

    def run(argv, allowed=(0,)):
        result = subprocess.run([str(x) for x in argv], capture_output=True, text=True)
        logs.append({'argv': [str(x) for x in argv], 'returncode': result.returncode,
                     'output': result.stdout + result.stderr})
        (args.out / 'commands.json').write_text(json.dumps(logs, indent=2) + '\n')
        if result.returncode not in allowed:
            raise RuntimeError(logs[-1])
        return result.stdout + result.stderr

    avb = args.host_tools / 'avbtool'
    signer = args.host_tools / 'apksigner'
    align = args.host_tools / 'zipalign'
    debugfs = args.host_tools / 'debugfs'
    payload = args.out / 'payload.img'
    with zipfile.ZipFile(args.apex) as archive:
        if archive.testzip() is not None:
            raise ValueError('corrupt input APEX')
        payload.write_bytes(archive.read('apex_payload.img'))
        original_key = archive.read('apex_pubkey')
    extracted_key = args.out / 'signing.avbpubkey'
    key = keys / 'com.android.hardware.pem'
    run([avb, 'extract_public_key', '--key', key, '--output', extracted_key])
    if extracted_key.read_bytes() != original_key:
        raise ValueError('AVB signing key does not match input APEX')
    run([avb, 'verify_image', '--image', payload, '--key', key])
    info = run([avb, 'info_image', '--image', payload])
    salt = re.search(r'^\s+Salt:\s+([a-f0-9]+)$', info, re.M)
    if not salt or 'SHA256_RSA4096' not in info or "Prop: apex.key -> 'com.android.hardware'" not in info:
        raise ValueError('unexpected input APEX AVB parameters')
    if not re.search(r'^\s+Partition Name:\s*$', info, re.M):
        raise ValueError('unexpected partition name')
    run([avb, 'erase_footer', '--image', payload])
    remote = '/bin/hw/android.hardware.composer.hwc3-service.drm'

    def q(value):
        return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'

    def debug(command, write=False):
        return run([debugfs, *(['-w'] if write else []), '-R', command, payload])

    stat_before = debug('stat ' + q(remote))
    mode = re.search(r'Mode:\s+([0-7]+)', stat_before)
    owners = re.search(r'User:\s+(\d+)\s+Group:\s+(\d+)', stat_before)
    if not mode or not owners or 'Type: regular' not in stat_before:
        raise ValueError('missing original composer inode')
    attrs = {}
    for name in re.findall(r'^\s+([\w.-]+) \(\d+\)', debug('ea_list ' + q(remote)), re.M):
        path = args.out / ('xattr-' + name)
        debug(f'ea_get -f {q(path)} {q(remote)} {q(name)}')
        attrs[name] = path
    if attrs.get('security.selinux') is None:
        raise ValueError('missing executable SELinux label')
    original_binary = args.out / 'original-composer'
    debug(f'dump {q(remote)} {q(original_binary)}')
    local = args.out / 'composer'
    shutil.copyfile(args.composer, local)
    debug('rm ' + q(remote), True)
    debug(f'write {q(local)} {q(remote)}', True)
    for field, value in (('mode', '010' + mode[1].zfill(4)), ('uid', owners[1]), ('gid', owners[2])):
        debug(f'set_inode_field {q(remote)} {field} {value}', True)
    for field in ('atime', 'mtime', 'ctime', 'crtime'):
        value = re.search(r'^\s*' + field + r': (0x[0-9a-f]+):', stat_before, re.M)[1]
        debug(f'set_inode_field {q(remote)} {field} {value}', True)
    for name, path in attrs.items():
        debug(f'ea_set -f {q(path)} {q(remote)} {q(name)}', True)
    readback = args.out / 'readback-composer'
    debug(f'dump {q(remote)} {q(readback)}')
    if readback.read_bytes() != blob:
        raise ValueError('composer data readback mismatch')
    stat_after = debug('stat ' + q(remote))
    if re.search(r'Mode:\s+([0-7]+)', stat_after)[1] != mode[1]:
        raise ValueError('composer mode changed')
    if re.search(r'User:\s+(\d+)\s+Group:\s+(\d+)', stat_after).groups() != owners.groups():
        raise ValueError('composer owners changed')
    for name, path in attrs.items():
        check = args.out / ('readback-' + name)
        debug(f'ea_get -f {q(check)} {q(remote)} {q(name)}')
        if check.read_bytes() != path.read_bytes():
            raise ValueError('composer xattr changed: ' + name)
    run(['/usr/sbin/e2fsck', '-f', '-n', payload])
    run([avb, 'add_hashtree_footer', '--image', payload,
         '--partition_name', '', '--algorithm', 'SHA256_RSA4096', '--key', key,
         '--hash_algorithm', 'sha256', '--salt', salt[1], '--do_not_generate_fec',
         '--prop', 'apex.key:com.android.hardware'])
    run([avb, 'verify_image', '--image', payload, '--key', key])
    unsigned = args.out / 'unsigned.apex'
    with zipfile.ZipFile(args.apex) as old, zipfile.ZipFile(unsigned, 'w') as new:
        for entry in old.infolist():
            new.writestr(entry, payload.read_bytes() if entry.filename == 'apex_payload.img'
                         else old.read(entry.filename))
    aligned = args.out / 'aligned.apex'
    run([align, '-f', '4096', unsigned, aligned])
    output = args.out / 'composer.apex'
    run([signer, 'sign', '--v1-signing-enabled', 'false', '--v4-signing-enabled', 'false',
         '--key', keys / 'com.android.hardware.pk8',
         '--cert', keys / 'com.android.hardware.x509.pem', '--out', output, aligned])
    run([signer, 'verify', '--verbose', output])
    run([align, '-c', '4096', output])
    for path in (args.apex, output):
        cert = run([signer, 'verify', '--print-certs', path])
        digests = set(re.findall(r'certificate SHA-256 digest: ([0-9a-f]{64})', cert))
        if len(digests) != 1:
            raise ValueError('expected one APK signing certificate')
        digest = digests.pop()
        if path == args.apex:
            original_cert = digest
        elif digest != original_cert:
            raise ValueError('APK signer differs from original')
    with zipfile.ZipFile(args.apex) as old, zipfile.ZipFile(output) as new:
        if old.namelist() != new.namelist():
            raise ValueError('APEX member set changed')
        for name in old.namelist():
            if name != 'apex_payload.img' and old.read(name) != new.read(name):
                raise ValueError('unexpected APEX member change: ' + name)
    report = {'input_sha256': sha(args.apex), 'composer_sha256': sha(args.composer),
              'output_sha256': sha(output), 'original_composer_sha256': sha(original_binary),
              'apk_certificate_sha256': original_cert, 'avb_key_unchanged': True,
              'composer_mode_uid_gid_xattrs_preserved': True, 'installed': False}
    (args.out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
