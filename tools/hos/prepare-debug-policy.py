#!/usr/bin/env python3
"""Make a separate donor policy tree with disjoint user/debug generated attributes.

Keeps original split-policy anonymous definitions, alpha-renames debug anonymous
symbols, and removes exactly one broad llkd ptrace allow. Neverallow rules remain.
Refs: refs/a17/system/sepolicy/Android.bp:272-310,320-365; private/llkd.te:22-33.
This is an experimental debug policy, not a production/enforcing certification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from cil import statements, replace_symbols, tokens

GENERATED = re.compile(r"base_typeattr_[0-9]+\Z")


def repair(user, debug, split_texts):
    user_st = statements(user)
    debug_st = statements(debug)
    # Import only definitions, never user allow/neverallow rules.
    definitions = []
    for start, end, tree in user_st:
        if tree[0] in ('typeattribute', 'typeattributeset') and isinstance(tree[1], str) and GENERATED.fullmatch(tree[1]):
            definitions.append(user[start:end])
    if not definitions:
        raise ValueError("no generated user attribute definitions found")
    symbols = {m[0] for text in [user, debug, *split_texts] for m in tokens(text) if GENERATED.fullmatch(m[0])}
    offset = max(int(name.rsplit('_', 1)[1]) for name in symbols) + 10000
    debug_symbols = {m[0] for m in tokens(debug) if GENERATED.fullmatch(m[0])}
    mapping = {name: 'base_typeattr_' + str(offset + int(name.rsplit('_', 1)[1])) for name in sorted(debug_symbols)}
    if set(mapping.values()) & symbols:
        raise ValueError("generated attribute namespace collision")
    llkd = [(s, e, t) for s, e, t in debug_st if len(t) == 4 and t[:2] == ['allow', 'llkd'] and t[3] == ['process', ['ptrace']]]
    if len(llkd) != 1:
        raise ValueError(f"expected exactly one broad llkd ptrace allow, got {len(llkd)}")
    start, end, removed = llkd[0]
    if removed[2] not in debug_symbols:
        raise ValueError("unexpected llkd target; review donor update")
    # Rename first, then import original anonymous definitions for split-file references.
    patched = replace_symbols(debug[:start] + ';; HOS: llkd broad ptrace omitted.' + debug[end:], mapping)
    patched += '\n;; HOS: original user-generated definitions used by split policy.\n' + '\n'.join(definitions) + '\n'
    final = statements(patched)
    before_neverallow = [replace_symbols(debug[s:e], mapping) for s, e, t in debug_st if t[0] == 'neverallow']
    after_neverallow = [patched[s:e] for s, e, t in final if t[0] == 'neverallow']
    if before_neverallow != after_neverallow:
        raise ValueError("neverallow preservation check failed")
    return patched, {'renamed_symbols': mapping, 'imported_definition_count': len(definitions),
                     'removed_allow': removed, 'preserved_neverallow_count': len(before_neverallow)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path, help='new directory, contains root/ and manifest.json')
    args = parser.parse_args()
    source = args.root.resolve()
    user = source / 'system/etc/selinux/plat_sepolicy.cil'
    debug = source / 'system_ext/etc/selinux/userdebug_plat_sepolicy.cil'
    if args.out.resolve().is_relative_to(source):
        parser.error('output must be outside input root')
    all_cil = sorted(source.rglob('*.cil'))
    patched, report = repair(user.read_text(), debug.read_text(), [p.read_text() for p in all_cil if p not in (user, debug)])
    args.out.mkdir(parents=True, exist_ok=False)
    shutil.copytree(source, args.out / 'root', symlinks=True)
    target = args.out / 'root' / debug.relative_to(source)
    if target.is_symlink():
        raise ValueError('debug policy must not be a symlink')
    target.write_text(patched)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    report.update(inputs=[{'path': str(p), 'sha256': sha(p)} for p in all_cil],
                  output_sha256=sha(target), changed_file=str(debug.relative_to(source)),
                  requires=['Compile both runtime and neverallow modes before use',
                            'Install patched debug CIL to experimental system_ext',
                            'Keep llkd disabled (ro.llk.enable=false); no debug llkd ptrace diagnostics',
                            'Invalidate/rebuild experimental precompiled policy and verify actual device loading'],
                  ready_to_boot=False)
    (args.out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"Prepared {target}; neverallow statements preserved: {report['preserved_neverallow_count']}")


if __name__ == '__main__':
    main()
