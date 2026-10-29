#!/usr/bin/env python3
"""Check the reviewed group-name set, not any calibration payload."""
import json
from pathlib import Path
import re
import sys

here=Path(__file__).resolve().parent
provenance=json.loads((here/'name-provenance.json').read_text())
expected=sorted(list(provenance['groups'])+[provenance['extra_existing_sentinel']])
header=(Path(sys.argv[1])/'hexagonrpcd/registry_names.h').read_text()
actual=re.findall(r'^\s*"([A-Za-z0-9_.]+)",$',header,re.MULTILINE)
assert actual==expected and len(actual)==142 and len(set(actual))==142
assert 'tcs3701.rgb.config' not in actual and 'tcs3701_platform.rgb.fac_cal' not in actual
print('PASS 141 reviewed QRD/449 group names + existing sentinel; no invented RGB names')
