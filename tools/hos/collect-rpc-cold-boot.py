#!/usr/bin/env python3
"""Read-only capture of early isolated-HyperOS RPC and optional sensor logs; never reboots or flashes."""
import argparse
import json
from pathlib import Path
import subprocess
import time

GUARD = 'grep -qw androidboot.force_super_partition=super_hos /proc/cmdline || exit 90'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', default='gaokun3')
    p.add_argument('--out', type=Path, required=True, help='new evidence directory')
    p.add_argument('--seconds', type=int, default=120)
    p.add_argument('--wait-seconds', type=int, default=180)
    p.add_argument('--expected-build', required=True)
    p.add_argument('--daemon-sha256', required=True)
    p.add_argument('--sensor-hal-sha256', help='also capture sensor HAL logs and pin its binary')
    a = p.parse_args()
    if not 5 <= a.seconds <= 300 or not 1 <= a.wait_seconds <= 600:
        p.error('capture must be5..300s and connection wait1..600s')
    if any(len(h) != 64 or any(x not in '0123456789abcdef' for x in h)
           for h in [a.daemon_sha256] + ([a.sensor_hal_sha256] if a.sensor_hal_sha256 else [])):
        p.error('expected lowercase SHA256')
    a.out.mkdir(parents=True, exist_ok=False)
    adb = ['adb', '-s', a.serial]
    report = {'serial': a.serial, 'requested_seconds': a.seconds,
              'expected_build': a.expected_build, 'expected_daemon_sha256': a.daemon_sha256,
              'capture_complete': False, 'identity_verified': False,
              'expected_sensor_hal_sha256': a.sensor_hal_sha256}
    process = None
    start = None
    try:
        subprocess.run(adb + ['wait-for-device'], check=True, timeout=a.wait_seconds,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        # Guard runs in the same shell immediately before logcat. No -T/-t:
        # buffered boot events must be included, even before boot_completed.
        filters = 'hexagonrpcd-denied:V'
        hal_path = '/vendor/bin/hw/android.hardware.sensors-service.gaokun3'
        if a.sensor_hal_sha256:
            filters += ' android.hardware.sensors-service.gaokun3:V'
        command = GUARD + '; exec logcat -b all -v threadtime -s ' + filters
        with (a.out / 'events.txt').open('wb') as events, (a.out / 'logcat-stderr.txt').open('wb') as errors:
            start = time.monotonic()
            process = subprocess.Popen(adb + ['shell', command], stdout=events, stderr=errors)
            identity = subprocess.run(adb + ['shell', GUARD +
                '; cat /proc/sys/kernel/random/boot_id; getprop ro.boot.hos_build; '
                'getenforce; sha256sum /vendor/bin/hexagonrpcd' +
                ('; sha256sum ' + hal_path if a.sensor_hal_sha256 else '')],
                capture_output=True, timeout=20)
            (a.out / 'identity.txt').write_bytes(identity.stdout + identity.stderr)
            values = identity.stdout.decode(errors='replace').splitlines()
            report['identity_verified'] = (identity.returncode == 0 and len(values) == (5 if a.sensor_hal_sha256 else 4)
                and values[1].strip() == a.expected_build and values[2].strip() == 'Enforcing'
                and values[3].split() == [a.daemon_sha256, '/vendor/bin/hexagonrpcd']
                and (not a.sensor_hal_sha256 or values[4].split() == [a.sensor_hal_sha256, hal_path]))
            if values:
                report['boot_id'] = values[0].strip()
            if not report['identity_verified']:
                raise RuntimeError('isolated boot/build/SELinux/daemon identity was not verified')
            remaining = max(0, a.seconds - (time.monotonic() - start))
            try:
                report['logcat_exit'] = process.wait(timeout=remaining)
                raise RuntimeError('logcat exited before the full capture window')
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
                report['capture_complete'] = True
        ending = subprocess.run(adb + ['shell', GUARD + '; cat /proc/sys/kernel/random/boot_id'],
                                capture_output=True, timeout=10)
        report['same_boot_at_end'] = (ending.returncode == 0 and
            ending.stdout.decode(errors='replace').strip() == report.get('boot_id'))
        if not report['same_boot_at_end']:
            report['capture_complete'] = False
            raise RuntimeError('cannot verify the same isolated boot at end')
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
        if start is not None:
            report['elapsed_seconds'] = round(time.monotonic() - start, 3)
        (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['capture_complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
