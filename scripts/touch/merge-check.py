#!/usr/bin/env python3
"""交互断触的指纹：两根手指被驱动并进了同一条轨迹。

hx_track_contacts() 里，没匹配上的轨迹不立刻释放，而是保留 track_lost_frames 帧、照常按"按着"上报
（位置停在最后一帧，himax-spi-core.c himax_report_tracked_state()）。这期间在预测位置
track_dist2_max 以内（出厂 420 单位 ≈ 44 mm；patches/kernel/input/0014 起默认 115 单位 ≈ 12 mm）冒出来的新触点会被接到这条旧轨迹上 ——
对系统来说没有新的"按下"，旧手指"滑"了过去：新的那一下点击丢了。

指纹：同一条轨迹里位置【一动不动】停 ≥1 帧，下一帧【瞬移】≥ JUMP_MM。
同一根手指做不到"完全静止后 8 ms 内移出几毫米"；冻结帧数 ≤ track_lost_frames 也和机制对得上。

    python3 scripts/touch/merge-check.py ts_A.bin ts_B.bin
"""
import importlib.util, math, os, sys

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("evdev_strokes", os.path.join(_here, "evdev-strokes.py"))
es = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(es)

JUMP_MM = 6.0
# 冻结只可能持续 track_lost_frames（3）帧，再留 1 帧余量。patches/kernel/input/0016/0017 起，按住不动的手指
# 一直报同一个点，长时间的"冻结"后接一次快速滑动不是并轨。
MAX_FROZEN = 4


def merges(pts):
    """返回 [(冻结帧数, 瞬移 mm, 冻结时长 ms)]"""
    # 冻结有两种样子：别的手指在动时每帧都有 SYN，冻结的槽表现为重复的点；
    # 屏上没有别的变化时 input 核心连 SYN 都不发（值没变的 ABS 被丢，空帧不投递），表现为时间空档。
    # 两种都按"冻结"算：帧周期 8.3 ms（120 Hz），空档 ≥ 12 ms 记作至少冻结 1 帧。
    out = []
    frozen = 0
    for k in range(1, len(pts)):
        dx = pts[k][1] - pts[k - 1][1]
        dy = pts[k][2] - pts[k - 1][2]
        if dx == 0 and dy == 0:
            frozen += 1
            continue
        gap_frames = round((pts[k][0] - pts[k - 1][0]) / 0.00833) - 1
        f = frozen + max(0, gap_frames)
        d = math.hypot(dx, dy) * es.MM_PER_UNIT
        if 1 <= f <= MAX_FROZEN and d >= JUMP_MM:
            out.append((f, d, (pts[k][0] - pts[k - 1 - frozen][0]) * 1000))
        frozen = 0
    return out


def report(path):
    _, strokes = es.decode(path)
    if not strokes:
        print(f"{path}: 没有触摸事件"); return
    durs = sorted((p[-1][0] - p[0][0]) * 1000 for _, p in strokes)
    t0 = min(p[0][0] for _, p in strokes); t1 = max(p[-1][0] for _, p in strokes)
    ms = [m for _, p in strokes for m in merges(p)]
    nmerged = sum(1 for _, p in strokes if merges(p))
    # 同时在屏的最多手指数
    ev = sorted([(p[0][0], 1) for _, p in strokes] + [(p[-1][0], -1) for _, p in strokes])
    cur = mx = 0
    for _, s in ev:
        cur += s; mx = max(mx, cur)
    print(f"{path}: {len(strokes)} 条轨迹 / {t1 - t0:.1f} s（{len(strokes) / max(1e-3, t1 - t0):.1f} 次/秒），同时在屏最多 {mx} 指")
    print(f"  轨迹时长 ms：中位 {durs[len(durs) // 2]:.0f}  p90 {durs[int(len(durs) * .9)]:.0f}  最长 {durs[-1]:.0f}")
    print(f"  ★ 冻结后瞬移 ≥{JUMP_MM:.0f} mm（= 两指被并成一条）：{len(ms)} 次，涉及 {nmerged} 条轨迹"
          f"（约每 {len(strokes) / max(1, len(ms)):.0f} 次点按丢 1 次）" if ms else
          f"  ★ 冻结后瞬移 ≥{JUMP_MM:.0f} mm：0 次 ✓")
    if ms:
        fz = {}
        for f, _, _ in ms: fz[f] = fz.get(f, 0) + 1
        ds = sorted(d for _, d, _ in ms)
        print(f"    冻结帧数分布 {dict(sorted(fz.items()))} · 瞬移距离 mm 中位 {ds[len(ds) // 2]:.1f}  最小 {ds[0]:.1f}  最大 {ds[-1]:.1f}")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        report(p)
