#!/usr/bin/env python3
"""Remove OTA service definitions from a COPY of the A17 raw ext4 system image."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    if not a.image.is_file():
        p.error('input must be a regular image')
    a.out.mkdir(parents=True, exist_ok=False)
    a.out = a.out.resolve()
    image = a.out/'system.img'
    shutil.copyfile(a.image, image)
    logs, changes = [], []

    def run(args):
        r = subprocess.run([str(v) for v in args], capture_output=True)
        logs.append({'argv': [str(v) for v in args], 'returncode': r.returncode,
                     'output': (r.stdout+r.stderr).decode(errors='replace')})
        (a.out/'commands.json').write_text(json.dumps(logs, indent=2)+'\n')
        r.check_returncode()
        return (r.stdout+r.stderr).decode(errors='replace')

    def debug(cmd, write=False):
        return run(['debugfs', *(['-w'] if write else []), '-R', cmd, image])

    def q(value):
        return '"'+str(value).replace('\\','\\\\').replace('"','\\"')+'"'

    run(['e2fsck', '-f', '-n', image])
    for name in ('update_engine', 'update_verifier'):
        target = '/system/etc/init/'+name+'.rc'
        info = debug('stat '+q(target))
        mode = re.search(r'Mode:\s+([0-7]+)', info)
        owners = re.search(r'User:\s+(\d+)\s+Group:\s+(\d+)', info)
        if not mode or not owners or 'Type: regular' not in info:
            raise ValueError('unexpected source inode: '+target)
        attrs = {}
        for attr in re.findall(r'^\s+([\w.-]+) \(\d+\) =', debug('ea_list '+q(target)), re.M):
            file = a.out/(name+'-'+attr)
            debug(f'ea_get -f {q(file)} {q(target)} {q(attr)}')
            if not file.is_file():
                raise ValueError('failed xattr extraction')
            attrs[attr] = file
        if 'security.selinux' not in attrs:
            raise ValueError('missing original SELinux label')
        original = a.out/(name+'-original.rc')
        debug(f'dump {q(target)} {q(original)}')
        if 'service '+name not in original.read_text():
            raise ValueError('unexpected init service definition')
        replacement = a.out/(name+'-disabled.rc')
        replacement.write_text('# HOS isolated experiment: '+name+' service and triggers intentionally omitted.\n')
        debug('rm '+q(target), True)
        debug(f'write {q(replacement)} {q(target)}', True)
        for key,value in [('mode','010'+mode[1].zfill(4)),('uid',owners[1]),('gid',owners[2])]:
            debug(f'set_inode_field {q(target)} {key} {value}', True)
        for key in ('atime','ctime','mtime','crtime'):
            debug(f'set_inode_field {q(target)} {key} 1230768000', True)
        for attr,source in attrs.items():
            debug(f'ea_set -f {q(source)} {q(target)} {q(attr)}', True)
        readback = a.out/(name+'-readback.rc')
        debug(f'dump {q(target)} {q(readback)}')
        if readback.read_bytes() != replacement.read_bytes():
            raise ValueError('content mismatch')
        after = debug('stat '+q(target))
        after_mode = re.search(r'Mode:\s+([0-7]+)', after)
        after_owners = re.search(r'User:\s+(\d+)\s+Group:\s+(\d+)', after)
        if not after_mode or not after_owners or int(after_mode[1],8)!=int(mode[1],8) or after_owners.groups()!=owners.groups():
            raise ValueError('inode metadata mismatch')
        for attr,source in attrs.items():
            copy = a.out/(name+'-readback-'+attr)
            debug(f'ea_get -f {q(copy)} {q(target)} {q(attr)}')
            if copy.read_bytes()!=source.read_bytes():
                raise ValueError('xattr mismatch')
        changes.append(target)
    run(['e2fsck','-f','-n',image])
    def sha(file):
        h=hashlib.sha256()
        with file.open('rb') as f:
            for b in iter(lambda:f.read(1024*1024),b''): h.update(b)
        return h.hexdigest()
    (a.out/'manifest.json').write_text(json.dumps({'input_sha256':sha(a.image),'output_sha256':sha(image),
                                                  'changes':changes,'ready_to_boot':False},indent=2)+'\n')
    print(f'Prepared {image}; OTA service definitions removed, xattrs/mode verified, e2fsck passed')


if __name__=='__main__':
    main()
