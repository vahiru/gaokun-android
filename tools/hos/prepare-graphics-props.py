#!/usr/bin/env python3
"""Generate conservative HWUI/blur property replacements for the first donor boot.

GLES/ANGLE remains the graphics path; this does not enable unsupported Vulkan
HWUI or claim that applications' own Vulkan/Flutter paths have been validated.
"""
import argparse
import hashlib
import json
from pathlib import Path


def update(text, changes, required):
    lines = text.splitlines()
    positions = {}
    for i, line in enumerate(lines):
        if line.lstrip().startswith('#') or '=' not in line:
            continue
        key, value = line.split('=', 1)
        key = key.strip()
        if key in changes:
            if key in positions:
                raise ValueError(f'duplicate graphics property: {key}')
            positions[key] = i
    if set(required) - positions.keys():
        raise ValueError('missing expected donor graphics properties')
    for key, value in changes.items():
        if key in positions:
            lines[positions[key]] = f'{key}={value}'
        else:
            lines.append(f'{key}={value}')
    return '\n'.join(lines) + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--system-ext', type=Path, required=True)
    p.add_argument('--product', type=Path, required=True)
    p.add_argument('--system', type=Path,
                   help='also disable donor forced Wi-Fi Display on Wi-Fi-Direct-less gaokun3')
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    changes = {
        'system_ext': {'ro.surface_flinger.supports_background_blur': '0',
                       'persist.sys.gaokun3.touch_mode': 'game'},
        'product': {'debug.hwui.renderer': 'skiagl',
                    # Match gaokun3's native 2560x1600 tablet configuration.
                    # MIUI reads its own density property as well as SurfaceFlinger.
                    'ro.sf.lcd_density': '240',
                    'persist.miui.density_v2': '240',
                    'persist.sys.enhance_vkpipelinecache.enable': 'false',
                    'persist.sys.background_blur_supported': 'false',
                    'persist.sys.background_blur_status_default': 'false',
                    # gaokun3 has neither AF_VSOCK nor Xiaomi's /dev/iorap_dev.
                    # Keep local tracing; disable only the unavailable relay and
                    # the donor's optional kernel-dependent preread accelerator.
                    'traced.relay_producer_port': '',
                    'persist.sys.stability.PrereadEnable': 'false'},
    }
    required = {'system_ext': ['ro.surface_flinger.supports_background_blur'],
                'product': ['persist.sys.enhance_vkpipelinecache.enable']}
    if a.system:
        changes['system'] = {'persist.debug.wfd.enable': '0'}
        required['system'] = ['persist.debug.wfd.enable']
    output, report = {}, {}
    for name, values in changes.items():
        original = getattr(a, name).read_text()
        output[name] = update(original, values, required[name])
        report[name] = {'input_sha256': hashlib.sha256(original.encode()).hexdigest(),
                        'output_sha256': hashlib.sha256(output[name].encode()).hexdigest(), 'changes': values}
    a.out.mkdir(parents=True, exist_ok=False)
    for name, value in output.items():
        (a.out/(name+'.build.prop')).write_text(value)
    (a.out/'manifest.json').write_text(json.dumps({'partitions': report, 'ready_to_boot': False,
        'remaining': ['Verify no later property/rc overrides in all final partitions',
                      'Verify fresh isolated data and runtime properties/rendering on hardware']}, indent=2)+'\n')


if __name__ == '__main__':
    main()
