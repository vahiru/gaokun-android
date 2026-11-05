#!/usr/bin/env python3
"""从参考源码树里收集 GUID → 名字（带出处 文件:行号），写 guiddb.json（与本脚本同目录）。

    python3 guiddb.py [refs 目录，默认 <仓库>/refs]

扫的树（先跑 scripts/clone-refs.sh）：edk2、CLO ABL 的 QcomModulePkg、systemd src/boot、GBL。
pe.py / scan.py 用这个库给模块里出现的 GUID 起名字；查不到的就是 ??（多半是高通或华为私有）。
"""
import json
import os
import re
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "..", "..", "..", "refs")
ROOTS = ["edk2", "clo-abl-5.0/QcomModulePkg", "systemd-v257/src/boot",
         "systemd-v257/src/fundamental", "gbl"]

db = {}
hexn = r"0x[0-9a-fA-F]+"
body = (r"\{\s*(" + hexn + r")\s*,\s*(" + hexn + r")\s*,\s*(" + hexn + r")\s*,\s*\{?\s*" +
        r"\s*,\s*".join(["(" + hexn + ")"] * 8))
pat = re.compile(r"(\w+)\s*(?:=|\\?\s*)\s*" + body, re.S)
defpat = re.compile(r"#define\s+(\w+)\s*\\?\s*" + body, re.S)


def add(name, parts, src):
    a, b, c = int(parts[0], 16), int(parts[1], 16), int(parts[2], 16)
    rest = bytes(int(x, 16) for x in parts[3:11])
    try:
        u = uuid.UUID(fields=(a, b, c, rest[0], rest[1], int.from_bytes(rest[2:], "big")))
    except Exception:
        return
    db.setdefault(str(u), []).append((name, src))


for root in ROOTS:
    base = os.path.join(SRC, root)
    if not os.path.isdir(base):
        print("缺 %s（先跑 scripts/clone-refs.sh）" % base, file=sys.stderr)
        continue
    for dp, dn, fn in os.walk(base):
        if ".git" in dp:
            continue
        for f in fn:
            if not f.endswith((".dec", ".h", ".c", ".rs")):
                continue
            p = os.path.join(dp, f)
            try:
                t = open(p, errors="ignore").read()
            except OSError:
                continue
            rel = os.path.relpath(p, SRC)
            if f.endswith(".rs"):
                for m in re.finditer(r'(\w+)\s*[:=][^;\n]*?guid!?\(?\s*"([0-9a-fA-F-]{36})"', t):
                    db.setdefault(m.group(2).lower(), []).append((m.group(1), rel))
                continue
            for P in (pat, defpat):
                for m in P.finditer(t):
                    ln = t.count("\n", 0, m.start()) + 1
                    add(m.group(1), m.groups()[1:], "%s:%d" % (rel, ln))

json.dump(db, open(os.path.join(HERE, "guiddb.json"), "w"), indent=0)
print(len(db))
