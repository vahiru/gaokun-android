#!/usr/bin/env python3
"""把 meson 文件里的反斜杠续行拼成单行（meson_to_hermetic 的 lark 语法不认 \\）。

**仅 A16（crDroid 16.0，mesa 25.3 / 26.0.3 + 旧工具 meson_to_hermetic）。**
A17（crDroid 17.0，mesa 26.1.0-devel）的 external/mesa3d 已删掉 meson_to_hermetic，
改用 meson2hermetic（`meson convert`）；A17 的管线见 docs/dev/mesa-a17.md 与
scripts/mesa-a17/regen.sh。本文件只作 A16 历史留档，在 A17 树上运行会直接退出。

语义等价变换；只处理行尾为 `\\` 的行。在 external/mesa3d 下运行，幂等。
"""
import os
import sys

if not os.path.isdir('meson_to_hermetic'):
    sys.exit('本脚本只适用于带 meson_to_hermetic 的 A16 mesa 树；'
             'A17（mesa 26.1）请用 scripts/mesa-a17/regen.sh（见 docs/dev/mesa-a17.md）')

changed = 0
for root, _dirs, files in os.walk('.'):
    if '/meson_to_hermetic' in root or '/.git' in root:
        continue
    for fn in files:
        if fn not in ('meson.build', 'meson.options', 'meson_options.txt'):
            continue
        p = os.path.join(root, fn)
        try:
            src = open(p, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        if '\\\n' not in src:
            continue
        out_lines = []
        buf = ''
        for line in src.split('\n'):
            if line.rstrip().endswith('\\'):
                buf += line.rstrip()[:-1].rstrip() + ' '
            else:
                out_lines.append(buf + line)
                buf = ''
        if buf:
            out_lines.append(buf)
        open(p, 'w').write('\n'.join(out_lines))
        changed += 1
print('已拼平 %d 个 meson 文件的续行' % changed)
