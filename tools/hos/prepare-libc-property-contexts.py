#!/usr/bin/env python3
"""Label four donor libc diagnostic probes without changing values or policy."""
import argparse
import hashlib
import json
from pathlib import Path
import re

PROPERTIES = ('persist.track.malloc.enable', 'ro.sanitize',
              'persist.resleak.enable.nativeheap', 'persist.fd.monitor.enable')
CONTEXT = 'u:object_r:libc_debug_prop:s0'
PARTITIONS = ('plat', 'system_ext', 'product', 'vendor', 'odm')


def entries(text):
    for line in text.splitlines():
        words = line.split('#', 1)[0].split()
        if not words:
            continue
        if len(words) < 2 or (len(words) > 2 and words[2] not in ('exact', 'prefix')):
            raise ValueError('unrecognized property context: ' + line)
        yield words


def generate(contexts, policy):
    if set(contexts) != set(PARTITIONS) or contexts['plat'] is None:
        raise ValueError('account for all five partitions; use None only for absent files')
    # Reuse the existing read-only diagnostic property contract, not a new allow.
    compact = re.sub(r'\s+', ' ', policy)
    if '(allow domain libc_debug_prop (file (read getattr map open)))' not in compact:
        raise ValueError('review platform policy: expected existing libc_debug_prop read rule')
    parsed = {p: list(entries(t or '')) for p, t in contexts.items()}
    anchor = ['libc.debug.malloc.options', CONTEXT, 'exact', 'string']
    if parsed['plat'].count(anchor) != 1:
        raise ValueError('expected one platform libc malloc diagnostic mapping')
    missing = []
    for name in PROPERTIES:
        exact = []
        for part, rows in parsed.items():
            for row in rows:
                key = row[0]
                if key == name:
                    exact.append((part, row))
                elif key != '*' and (len(row) == 2 or row[2] == 'prefix') and name.startswith(key):
                    if row[1] != 'u:object_r:default_prop:s0':
                        raise ValueError(f'conflicting prefix for {name}: {part} {row}')
        wanted = ['plat', [name, CONTEXT, 'exact', 'string']]
        if exact:
            if len(exact) != 1 or list(exact[0]) != wanted:
                raise ValueError(f'conflicting or duplicate mapping for {name}: {exact}')
        else:
            missing.append(name)
    original = contexts['plat']
    if not missing:
        return original, []
    suffix = '\n# HOS donor libc startup diagnostic probes (exact, read-only policy unchanged)\n'
    return original + ('' if original.endswith('\n') else '\n') + suffix + ''.join(
        f'{name} {CONTEXT} exact string\n' for name in missing), missing


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--context', action='append', default=[], metavar='PARTITION=PATH')
    p.add_argument('--absent', action='append', default=[], choices=PARTITIONS,
                   help='explicitly record a partition with no property_contexts file')
    p.add_argument('--plat-policy', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args()
    contexts, inputs = {}, {}
    for part in a.absent:
        if part in contexts:
            p.error('duplicate partition: ' + part)
        contexts[part] = None
    for value in a.context:
        part, sep, path = value.partition('=')
        if not sep or part not in PARTITIONS or part in contexts:
            p.error('invalid or duplicate context input: ' + value)
        data = Path(path).read_bytes()
        contexts[part] = data.decode()
        inputs[part] = {'path': str(Path(path).resolve()), 'sha256': hashlib.sha256(data).hexdigest()}
    policy = a.plat_policy.read_bytes()
    result, added = generate(contexts, policy.decode())
    a.out.mkdir(parents=True, exist_ok=False)
    (a.out / 'plat_property_contexts').write_text(result)
    (a.out / 'manifest.json').write_text(json.dumps({
        'inputs': inputs, 'absent': a.absent,
        'plat_policy_sha256': hashlib.sha256(policy).hexdigest(),
        'output_sha256': hashlib.sha256(result.encode()).hexdigest(),
        'added_exact_properties': added, 'target_context': CONTEXT,
        'property_values_changed': False, 'policy_rules_changed': False,
        'requires_reboot_and_runtime_validation': True,
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
