#!/usr/bin/env python3
"""Offline SSC JSON validator/generator. Default is validation only.
Never overwrite an output directory; never resolve conflicting values by order.
This validates tooling semantics, not SEE firmware compatibility.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re

NAME = re.compile(r'[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)*\Z')
REFERENCE = {'type': 'grp', 'ver': '0', 'data': ''}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON key: {key}')
        result[key] = value
    return result


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(',', ':'), allow_nan=False)


def select(config, platform, soc):
    if not isinstance(config, dict):
        raise ValueError('config must be an object')
    for key, wanted in [('hw_platform', platform), ('soc_id', soc)]:
        if key not in config:
            continue
        choices = config[key]
        if not isinstance(choices, list) or not all(isinstance(x, str) for x in choices):
            raise ValueError(f'config.{key} must be an array of strings')
        if wanted not in choices:
            return False
    return True


def flatten(document):
    result = {}

    def group(name, node):
        if not isinstance(name, str) or not NAME.fullmatch(name) or len(name) > 255:
            raise ValueError(f'unsafe output group name: {name!r}')
        if not isinstance(node, dict):
            raise ValueError(f'group {name} is not an object')
        if name in result:
            raise ValueError(f'overlapping nested/top-level group: {name}')
        values = {}
        result[name] = values
        for key, value in node.items():
            if key.startswith('.'):
                child = key[1:]
                if not child or child in values or child in node:
                    raise ValueError(f'ambiguous nested group reference: {name}{key}')
                group(name + key, value)
                values[child] = dict(REFERENCE)
            else:
                values[key] = value
    for name, node in document.items():
        if name != 'config':
            group(name, node)
    return result


def collect(indir, platform, soc, merge_disjoint=False):
    paths = sorted(indir.glob('*.json'))
    if not paths:
        raise ValueError('no JSON inputs')
    groups, sources, inputs, excluded, merged = {}, {}, {}, [], []
    for path in paths:
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'input must be a regular non-symlink file: {path}')
        blob = path.read_bytes()
        inputs[path.name] = hashlib.sha256(blob).hexdigest()
        document = json.loads(blob, object_pairs_hook=unique_object,
                              parse_constant=lambda x: (_ for _ in ()).throw(ValueError(f'invalid constant {x}')))
        if not isinstance(document, dict):
            raise ValueError(f'input root is not an object: {path.name}')
        if not select(document.get('config', {}), platform, soc):
            excluded.append(path.name)
            continue
        for name, values in flatten(document).items():
            if name in groups:
                if not merge_disjoint:
                    raise ValueError(f'duplicate output group {name}: {sources[name]} and {path.name}; no files written')
                # Never merge inside a typed item or choose a version winner.
                for key, value in values.items():
                    if key in groups[name] and canonical(groups[name][key]) != canonical(value):
                        raise ValueError(f'conflicting value/type/version at {name}.{key}: {sources[name]} vs {path.name}')
                merged.append({'group': name, 'existing_sources': list(sources[name]),
                               'additional_source': path.name,
                               'added_fields': sorted(set(values) - set(groups[name]))})
                groups[name].update(values)
                sources[name].append(path.name)
            else:
                groups[name] = dict(values)
                sources[name] = [path.name]
    payloads = {name: (canonical({name: values}) + '\n').encode()
                for name, values in sorted(groups.items())}
    report = {
        'schema': 1, 'status': 'offline semantic validation only; SEE compatibility unproven',
        'filter': {'hw_platform': platform, 'soc_id': soc},
        'merge_disjoint_explicit': merge_disjoint, 'inputs_sha256': inputs,
        'excluded': excluded, 'merged_groups': merged,
        'file_count': len(payloads), 'total_bytes': sum(map(len, payloads.values())),
        'files': {name: {'sha256': hashlib.sha256(blob).hexdigest(), 'bytes': len(blob),
                         'sources': sources[name]} for name, blob in payloads.items()},
    }
    return payloads, report


def write_new_directory(output, payloads):
    # Entire collection is validated and serialized before creating anything.
    # Refuse symlinks, old output, empty existing directories and partial reruns.
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    directory = os.open(output, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for name, blob in payloads.items():
            if not NAME.fullmatch(name):
                raise ValueError(f'unsafe filename: {name}')
            fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                         0o600, dir_fd=directory)
            with os.fdopen(fd, 'wb') as file:
                file.write(blob)
                file.flush()
                os.fsync(file.fileno())
        os.fsync(directory)
    finally:
        os.close(directory)
    # I/O failure leaves an explicit partial directory, never an overwrite/retry.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--platform', required=True)
    parser.add_argument('--soc-id', required=True)
    parser.add_argument('--merge-disjoint', action='store_true')
    parser.add_argument('--output', type=Path, help='optional NEW output directory; no existing directory accepted')
    args = parser.parse_args()
    payloads, report = collect(args.input, args.platform, args.soc_id, args.merge_disjoint)
    if args.output is not None:
        write_new_directory(args.output, payloads)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
