#!/usr/bin/env python3
"""Replace one regular file in EROFS on Linux; verify full tree metadata/content.

Use a privileged local tools container so security.selinux/capability xattrs can
be preserved. Only image files are used; no block-device input is accepted.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import tempfile


def relative_path(name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or str(path) in ('.', '') or str(path) != name:
        raise ValueError('expected a canonical safe relative path')
    return path


def add_file(root, name, source, metadata_from):
    """Add a regular file with explicit metadata inherited from an image file."""
    relative_path(name)
    relative_path(metadata_from)
    root = root.resolve()
    target, template = root/name, root/metadata_from
    if target.exists() or target.is_symlink():
        raise ValueError('addition target already exists')
    if not target.parent.is_dir() or not target.parent.resolve().is_relative_to(root):
        raise ValueError('addition parent must exist inside the extracted image')
    if template.is_symlink() or not template.is_file() or not template.resolve().is_relative_to(root):
        raise ValueError('metadata template must be a regular file inside image')
    if not source.is_file():
        raise ValueError('addition source must be a regular file')
    info = template.stat()
    with target.open('xb') as f:
        f.write(source.read_bytes())
    os.chown(target, info.st_uid, info.st_gid)
    os.chmod(target, stat.S_IMODE(info.st_mode))
    for attr in os.listxattr(template):
        os.setxattr(target, attr, os.getxattr(template, attr))
    os.utime(target, ns=(info.st_atime_ns, info.st_mtime_ns))


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def inventory(root):
    result = {}
    for base, dirs, files in os.walk(root, followlinks=False):
        for path in ([root] if Path(base) == root else []) + [Path(base)/name for name in dirs + files]:
            info = path.lstat()
            item = {'mode': info.st_mode, 'uid': info.st_uid, 'gid': info.st_gid,
                    'xattrs': {n: os.getxattr(path, n, follow_symlinks=False).hex()
                              for n in sorted(os.listxattr(path, follow_symlinks=False))}}
            if stat.S_ISREG(info.st_mode):
                item['sha256'] = sha(path)
            elif stat.S_ISLNK(info.st_mode):
                item['target'] = os.readlink(path)
            elif not stat.S_ISDIR(info.st_mode):
                item['rdev'] = info.st_rdev
            result[str(path.relative_to(root))] = item
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--replace', help='relative path inside EROFS')
    p.add_argument('--with-file', type=Path)
    p.add_argument('--plan', type=Path, help='JSON mapping relative target paths to replacement files')
    p.add_argument('--add-plan', type=Path, help='JSON mapping new relative paths to {source, metadata_from}')
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    if a.plan:
        if a.replace or a.with_file:
            p.error('plan and single replacement options are mutually exclusive')
        replacements = {name: Path(file) for name, file in json.loads(a.plan.read_text()).items()}
    elif a.replace and a.with_file:
        replacements = {a.replace: a.with_file}
    elif not a.replace and not a.with_file and a.add_plan:
        replacements = {}
    else:
        p.error('provide --plan, both --replace and --with-file, or --add-plan')
    additions = json.loads(a.add_plan.read_text()) if a.add_plan else {}
    for name in set(replacements) | set(additions):
        relative_path(name)
    for item in additions.values():
        if set(item) != {'source', 'metadata_from'}:
            p.error('each addition requires exactly source and metadata_from')
        relative_path(item['metadata_from'])
        if not Path(item['source']).is_file():
            p.error('addition source must be a regular file')
    if set(replacements) & set(additions):
        p.error('replacement and addition targets overlap')
    if not (replacements or additions) or not a.image.is_file() or any(not f.is_file() for f in replacements.values()):
        p.error('only regular input files are supported')
    a.out.mkdir(parents=True, exist_ok=False)
    a.out = a.out.resolve()
    logs = []

    def run(argv):
        r = subprocess.run([str(v) for v in argv], capture_output=True)
        logs.append({'argv': [str(v) for v in argv], 'returncode': r.returncode,
                     'output': (r.stdout + r.stderr).decode(errors='replace')})
        (a.out/'commands.json').write_text(json.dumps(logs, indent=2)+'\n')
        if r.returncode:
            raise RuntimeError(logs[-1])

    with tempfile.TemporaryDirectory(prefix='hos-erofs-') as tmp:
        root, check = Path(tmp)/'root', Path(tmp)/'check'
        run(['fsck.erofs', '--extract='+str(root), '--xattrs', '--preserve', a.image.resolve()])
        before = inventory(root)
        if any('security.selinux' not in i['xattrs'] for i in before.values() if not stat.S_ISLNK(i['mode'])):
            raise ValueError('missing extracted SELinux xattrs; refusing lossy repack')
        for name, source in replacements.items():
            target = root / name
            if target.is_symlink() or not target.is_file() or not target.resolve().is_relative_to(root):
                raise ValueError('replacement target must be an existing regular file inside root')
            info = target.stat()
            target.write_bytes(source.read_bytes())
            os.utime(target, ns=(info.st_atime_ns, info.st_mtime_ns))
        for name, item in additions.items():
            add_file(root, name, Path(item['source']), item['metadata_from'])
        expected = inventory(root)
        changes = [name for name in set(before) | set(expected) if before.get(name) != expected.get(name)]
        if set(changes) != set(replacements) | set(additions):
            raise ValueError(f'unexpected changed files: {changes}')
        image = a.out/'image.erofs'
        run(['mkfs.erofs', '-b4096', '-zlz4hc,12', '-T1230768000', '--mkfs-time', '--preserve-mtime', image, root])
        run(['fsck.erofs', '--extract='+str(check), '--xattrs', '--preserve', image])
        actual = inventory(check)
        if actual != expected:
            diff = sorted(k for k in set(actual)|set(expected) if actual.get(k) != expected.get(k))
            (a.out/'mismatches.json').write_text(json.dumps(diff, indent=2)+'\n')
            raise ValueError(f'EROFS round-trip changed {len(diff)} entries: {diff[:8]}')
        (a.out/'tree.json').write_text(json.dumps(actual, indent=2)+'\n')
        report = {'input': str(a.image.resolve()), 'input_sha256': sha(a.image),
                  'replacements': {name: {'source': str(f), 'sha256': sha(f)} for name, f in replacements.items()},
                  'additions': {name: dict(item, sha256=sha(Path(item['source']))) for name, item in additions.items()},
                  'output_sha256': sha(image), 'output_size': image.stat().st_size,
                  'verified_entries': len(actual), 'changes': changes,
                  'verified': ['file data', 'symlinks', 'uid/gid', 'mode', 'all extracted xattrs'],
                  'ready_to_boot': False}
        (a.out/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
        print(f'Prepared {image}; all {len(actual)} entries match expected content/metadata')


if __name__ == '__main__':
    main()
