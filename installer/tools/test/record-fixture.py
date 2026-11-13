#!/usr/bin/env python3
# 录一次后端调用，写成图形安装器 FixtureBackend 能回放的格式。
#
#   record-fixture.py <输出文件> [--sub 旧=新 …] -- <命令…>
#
# 格式（一行一条，按【到达顺序】）：
#   O <stdout 的一行>
#   E <stderr 的一行>
#   D <毫秒>        与上一行之间的停顿（>50 ms 才记，最长记 1500 ms —— 回放要快）
#   X <退出码>
#
# ★ 为什么 stdout/stderr 要按到达顺序交错记：进度（stderr 上的 PROGRESS）与结果
#   （stdout 上的记录）之间的先后，正是界面要处理的东西。分开录就丢了。
# ★ --sub 把 loop 设备名换成真机上会出现的名字（/dev/loop3 → /dev/nvme0n1），
#   让界面看到的路径和真机一样。替换规则由调用方写进 index.txt 的注释里 ——
#   fixture 必须说清楚自己哪里不是原样。
import os, selectors, subprocess, sys, time

args = sys.argv[1:]
out = args.pop(0)
subs = []
while args and args[0] == "--sub":
    a, b = args[1].split("=", 1); subs.append((a, b)); args = args[2:]
assert args and args[0] == "--", "用法：record-fixture.py <输出> [--sub 旧=新 …] -- <命令…>"
cmd = args[1:]

def fix(line):
    for a, b in subs:
        line = line.replace(a, b)
    return line

p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
sel = selectors.DefaultSelector()
bufs = {}
for f, tag in ((p.stdout, "O"), (p.stderr, "E")):
    os.set_blocking(f.fileno(), False)
    sel.register(f, selectors.EVENT_READ, tag); bufs[tag] = b""
rows, last = [], time.monotonic()
def emit(tag, raw):
    global last
    now = time.monotonic(); gap = int((now - last) * 1000); last = now
    if gap > 50:
        rows.append("D %d" % min(gap, 1500))
    rows.append("%s %s" % (tag, fix(raw.decode("utf-8", "replace"))))
open_streams = 2
while open_streams:
    for key, _ in sel.select():
        chunk = os.read(key.fileobj.fileno(), 65536)
        tag = key.data
        if not chunk:
            sel.unregister(key.fileobj); open_streams -= 1
            if bufs[tag]:
                emit(tag, bufs[tag]); bufs[tag] = b""
            continue
        bufs[tag] += chunk
        while b"\n" in bufs[tag]:
            line, bufs[tag] = bufs[tag].split(b"\n", 1)
            emit(tag, line)
rc = p.wait()
rows.append("X %d" % rc)
with open(out, "w") as f:
    f.write("\n".join(rows) + "\n")
print("  录好 %s（%d 行，退出码 %d）" % (os.path.basename(out), len(rows), rc))
