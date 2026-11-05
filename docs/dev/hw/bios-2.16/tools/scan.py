#!/usr/bin/env python3
"""对 extract.py 导出的每个 .efi：列出其中出现的已知 GUID（字节扫描），并解码 depex。

    python3 scan.py <extract 输出目录>/pe [模块名...]

先跑 guiddb.py 生成 guiddb.json。结果另存 scan.json（与本脚本同目录，不入库）。
⚠️ 字节扫描只说明"这个 GUID 的 16 字节出现在映像里"，不区分提供方 / 使用方，
   结论要配合 pe.py 的反汇编（xs / dis）看调用点。
"""
import json
import os
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
B = sys.argv[1]
db = json.load(open(os.path.join(HERE, "guiddb.json")))
pats = {uuid.UUID(g).bytes_le: g for g in db}


def name(g):
    v = db.get(g)
    return v[0][0] if v else "??"


def depex(p):
    d = open(p, "rb").read()
    i, out = 0, []
    while i < len(d):
        op = d[i]
        if op in (0, 1, 2):
            g = str(uuid.UUID(bytes_le=d[i + 1:i + 17]))
            out.append({0: "BEFORE", 1: "AFTER", 2: "PUSH"}[op] + " " + name(g) + "(" + g + ")")
            i += 17
        else:
            out.append({3: "AND", 4: "OR", 5: "NOT", 6: "TRUE", 7: "FALSE", 8: "END", 9: "SOR"}.get(op, hex(op)))
            i += 1
    return out


res = {}
for f in sorted(os.listdir(B)):
    if not f.endswith(".efi"):
        continue
    d = open(os.path.join(B, f), "rb").read()
    hits = [name(g) for b, g in pats.items() if b in d]
    m = f[:-4]
    res[m] = {"guids": sorted(set(hits))}
    dp = os.path.join(B, m + ".depex")
    if os.path.exists(dp):
        res[m]["depex"] = depex(dp)
json.dump(res, open(os.path.join(HERE, "scan.json"), "w"), indent=1)
for m in (sys.argv[2:] or res):
    print("##", m, "DEPEX:", " ".join(res[m].get("depex", ["-"])))
    print("   ", ", ".join(res[m]["guids"]))
