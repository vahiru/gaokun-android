#!/usr/bin/env python3
"""把 himax 驱动导出的 40×60 电容网格画成能一眼看懂的图。

数据来源（内核 #23 起，patches/kernel/input/0007）：
    /sys/kernel/debug/himax-hx83121a/frame_raw   面板【产出】的（仅去基线）
    /sys/kernel/debug/himax-hx83121a/frame       流水线【判定】的（CMF/边缘增强/IIR 之后）
各 4800 字节 = 40 行 × 60 列 × s16 小端。

用法：
    adb shell 'cat /sys/kernel/debug/himax-hx83121a/frame_raw' > raw.bin
    python3 scripts/touch/grid.py raw.bin
    python3 scripts/touch/grid.py raw.bin out.bin      # 并排比较

★ 为什么要并排比较：两张图的差别就是【算法做了什么】。
  raw 干净而 out 脏 ⇒ 是 CMF/边缘增强/IIR 引入的；两张都脏 ⇒ 是信号问题。

★ 空载读 raw 是回答"固件到底做不做逐像素基线跟踪"的唯一办法：
  没有手指时每个格子都应该在 0 附近。如果某些格子长期偏在几百，
  那就是驱动侧那个编译期常量基线（HX_BASELINE 0x7ffe）补不回来的漂移。
"""
import struct, sys

ROWS, COLS = 40, 60
MACRO_DEFAULT = 280          # hx_algo_init_defaults() 的 macro_threshold（patches/kernel/input/0019 前是 800）
RAMP = ' .:-=+*#%@'

def load(path):
    d = open(path, 'rb').read()
    want = ROWS * COLS * 2
    if len(d) < want:
        sys.exit(f"{path}: 只有 {len(d)} 字节，要 {want} —— 是不是没读完整？")
    v = struct.unpack('<%dh' % (ROWS * COLS), d[:want])
    return [list(v[r*COLS:(r+1)*COLS]) for r in range(ROWS)]

def render(g, thr):
    hi = max(max(r) for r in g) or 1
    out = []
    for row in g:
        line = ''
        for v in row:
            if v >= thr:
                line += '@'                       # 过阈 = 会被 BFS 收进连通域
            elif v <= 0:
                line += ' '
            else:
                line += RAMP[min(len(RAMP) - 2, v * (len(RAMP) - 2) // max(1, thr))]
            line += ''
        out.append(line)
    return out, hi

def stats(g, thr):
    flat = [v for row in g for v in row]
    over = sum(1 for v in flat if v >= thr)
    pos  = sum(1 for v in flat if v > 0)
    flat_sorted = sorted(flat)
    n = len(flat_sorted)
    return dict(min=flat_sorted[0], p50=flat_sorted[n//2], p99=flat_sorted[int(n*0.99)],
                max=flat_sorted[-1], over=over, pos=pos)

def main():
    paths = sys.argv[1:]
    thr = MACRO_DEFAULT
    if not paths:
        sys.exit(__doc__)
    grids = [load(p) for p in paths]
    rends = [render(g, thr)[0] for g in grids]
    print(f"阈值 macro_threshold = {thr}（'@' = 过阈，会被 BFS 收进连通域）")
    print("列 0 在左；每格约 4.4 × 4.2 mm\n")
    heads = [f"{p}" for p in paths]
    print('   ' + '   '.join(h.ljust(COLS) for h in heads))
    for r in range(ROWS):
        print(f"{r:2d} " + '   '.join(x[r] for x in rends))
    print()
    for p, g in zip(paths, grids):
        s = stats(g, thr)
        print(f"{p}: 最小 {s['min']}  中位 {s['p50']}  p99 {s['p99']}  最大 {s['max']}"
              f"   >0 的格子 {s['pos']}/2400   ≥{thr} 的格子 {s['over']}")
    if len(grids) == 2:
        diff = [[grids[1][r][c] - grids[0][r][c] for c in range(COLS)] for r in range(ROWS)]
        fl = sorted(v for row in diff for v in row)
        print(f"\n两图之差（第二个 − 第一个）：最小 {fl[0]}  中位 {fl[len(fl)//2]}  最大 {fl[-1]}")

if __name__ == '__main__':
    main()
