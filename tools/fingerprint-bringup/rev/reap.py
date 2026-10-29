#!/usr/bin/env python3
"""Extract decompiled code from `rea decompile --format json` output."""
import json, sys

def emit(r):
    if isinstance(r, dict):
        c = r.get("code")
        if isinstance(c, str) and c.strip():
            print(c)
            return
        for v in r.values():
            emit(v)
    elif isinstance(r, list):
        for it in r:
            emit(it)

d = json.load(sys.stdin)
rr = d.get("raw_result", d)
for o in rr.get("observations", []) or []:
    emit(o.get("result"))
