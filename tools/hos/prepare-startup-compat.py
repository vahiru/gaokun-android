#!/usr/bin/env python3
"""Prepare explicit donor startup compatibility replacements.

Default mode makes iorapd opt-in. The optional product-props mode disables
Xiaomi's unsupported PowerMillet subsystem, including its OEM job/network
policies. This also removes its CN-only veto on the Android cached-app freezer;
existing freezer settings and donor per-app eligibility still apply. It does
not alter cgroup mounts. Persisted data may override a new product default.
"""
import argparse
from pathlib import Path


def update(text):
    """Validate the expected init sections and insert exactly one service option."""
    lines = text.splitlines(keepends=True)
    sections = []
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            continue
        if '\\' in line or '\r' in line:
            raise ValueError('unsupported continuation or CRLF input')
        if not line[0].isspace():
            tokens = stripped.split()
            if tokens[0] not in ('on', 'service', 'import'):
                raise ValueError(f'unexpected init section at line {index + 1}')
            sections.append((index, tokens, []))
        elif not sections or sections[-1][1][0] == 'import':
            raise ValueError(f'command outside section at line {index + 1}')
        else:
            sections[-1][2].append(stripped.split())
    services = [s for s in sections if s[1][:2] == ['service', 'iorapd']]
    if len(services) != 1:
        raise ValueError('expected exactly one iorapd service')
    index, header, commands = services[0]
    if header != ['service', 'iorapd', '/system_ext/bin/iorapd']:
        raise ValueError('unexpected iorapd executable or arguments')
    if [c for c in commands if c[0] == 'class'] != [['class', 'main']]:
        raise ValueError('expected iorapd class main')
    disabled = [c for c in commands if c[0] == 'disabled']
    if disabled not in ([], [['disabled']]):
        raise ValueError('ambiguous disabled options')
    for value, action in [('true', 'start'), ('false', 'stop')]:
        expected = ['on', f'property:persist.sys.stability.PrereadEnable={value}']
        matches = [s for s in sections if s[1] == expected]
        if len(matches) != 1 or matches[0][2] != [[action, 'iorapd']]:
            raise ValueError(f'expected unchanged {value} property action')
    if not disabled:
        if not lines[index].endswith('\n'):
            raise ValueError('unterminated service header')
        lines.insert(index + 1, '    disabled\n')
    return ''.join(lines)


def update_product_props(text):
    """Disable the known donor boolean without changing other power policies."""
    key = 'persist.sys.powmillet.enable'
    lines = text.splitlines(keepends=True)
    positions = []
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith('#') or '=' not in stripped:
            continue
        name, value = stripped.split('=', 1)
        if name.strip() == key:
            if value.strip() not in ('true', 'false'):
                raise ValueError('unexpected PowerMillet boolean value')
            positions.append(index)
    if len(positions) != 1:
        raise ValueError('expected exactly one donor PowerMillet property')
    index = positions[0]
    ending = '\r\n' if lines[index].endswith('\r\n') else '\n' if lines[index].endswith('\n') else ''
    lines[index] = key + '=false' + ending
    return ''.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--kind', choices=('init', 'product-props'), default='init',
                        help='explicitly select init RC or PowerMillet product-property replacement')
    args = parser.parse_args()
    transform = update if args.kind == 'init' else update_product_props
    result = transform(args.input.read_bytes().decode('utf-8'))
    # Exclusive creation protects original donor inputs and previous output.
    with args.out.open('x', encoding='utf-8', newline='') as output:
        output.write(result)


if __name__ == '__main__':
    main()
