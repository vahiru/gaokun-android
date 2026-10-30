#!/usr/bin/env python3
"""Regression: the shipped driver's TA command surface must stay at 4 commands."""
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
proc = subprocess.run([sys.executable, str(HERE / 'ta_client_surface.py')],
                      capture_output=True, text=True)
assert proc.returncode == 0, proc.stderr or proc.stdout
report = json.loads(proc.stdout)
assert report['commands'] == ['0x8020', '0x8021', '0x8029', '0x8031'], report['commands']
assert len(report['direct_wrapper_call_sites']) == 7, report['direct_wrapper_call_sites']
assert report['wrapper_data_references'] == [], report['wrapper_data_references']
assert report['unexpected_command'] is False
print('ok')
