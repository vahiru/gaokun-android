#!/usr/bin/env python3
"""解码 cat /dev/input/eventN 录下的原始 evdev 流，把触摸手感量化。

为什么不用 getevent：`timeout N getevent > file` 会因块缓冲丢光输出。
录二进制、离线解码是既定方法。

aarch64 的 struct input_event = timeval(16) + type(2) + code(2) + value(4) = 24 字节。
"""
import struct, sys, math

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2f, 0x35, 0x36, 0x39

# 单位 -> 毫米：X 轴 1600 单位横跨 167 mm，Y 轴 2560 单位横跨 267 mm（两者都是 0.1043 mm/单位）
MM_PER_UNIT = 0.1043

def decode(path):
    data = open(path, 'rb').read()
    n = len(data) // 24
    slot, slots = 0, {}          # slot -> dict(id, x, y)
    strokes = {}                 # tracking_id -> list[(t, x, y)]
    order = []
    frames = 0
    for i in range(n):
        sec, usec, typ, code, val = struct.unpack_from('<qqHHi', data, i*24)
        t = sec + usec/1e6
        if typ == EV_SYN:
            frames += 1
            for s in slots.values():
                if s['id'] is not None and s['x'] is not None and s['y'] is not None:
                    strokes[s['id']].append((t, s['x'], s['y']))
            continue
        if typ != EV_ABS:
            continue
        if code == ABS_MT_SLOT:
            slot = val
        else:
            s = slots.setdefault(slot, {'id': None, 'x': None, 'y': None})
            if code == ABS_MT_TRACKING_ID:
                if val == -1:
                    s['id'] = None
                else:
                    s['id'] = val
                    if val not in strokes:
                        strokes[val] = []
                        order.append(val)
            elif code == ABS_MT_POSITION_X: s['x'] = val
            elif code == ABS_MT_POSITION_Y: s['y'] = val
    return frames, [(tid, strokes[tid]) for tid in order if strokes.get(tid)]

def report(path):
    frames, strokes = decode(path)
    print(f"{path}: {frames} 个 SYN 帧, {len(strokes)} 条轨迹")
    if not strokes:
        print("  （没有任何触摸事件）")
        return
    for tid, pts in strokes:
        if len(pts) < 2:
            print(f"  id={tid:<6} 只有 {len(pts)} 帧 —— 点按或碎片")
            continue
        dur = pts[-1][0] - pts[0][0]
        dist = sum(math.hypot(pts[k+1][1]-pts[k][1], pts[k+1][2]-pts[k][2])
                   for k in range(len(pts)-1)) * MM_PER_UNIT
        # 逐帧速度
        vs = []
        for k in range(len(pts)-1):
            dt = pts[k+1][0] - pts[k][0]
            if dt <= 0: continue
            d = math.hypot(pts[k+1][1]-pts[k][1], pts[k+1][2]-pts[k][2]) * MM_PER_UNIT
            vs.append(d/dt/1000.0)          # m/s
        # 帧间隔：> 2 倍中位数算一次“断帧”
        gaps = [pts[k+1][0]-pts[k][0] for k in range(len(pts)-1)]
        med = sorted(gaps)[len(gaps)//2] if gaps else 0
        stalls = sum(1 for g in gaps if med > 0 and g > 2.5*med)
        # 静止帧：位移为 0（fuzz 死区的直接指纹）
        frozen = sum(1 for k in range(len(pts)-1)
                     if pts[k+1][1] == pts[k][1] and pts[k+1][2] == pts[k][2])
        print(f"  id={tid:<6} {len(pts):>4} 帧 {dur*1000:>6.0f} ms "
              f"路径 {dist:>6.1f} mm  峰值 {max(vs) if vs else 0:>4.2f} m/s "
              f"中位帧距 {med*1000:>4.1f} ms  断帧 {stalls}  零位移帧 {frozen}"
              f" ({100*frozen/max(1,len(pts)-1):.0f}%)")
    allv = []
    for _, pts in strokes:
        for k in range(len(pts)-1):
            dt = pts[k+1][0]-pts[k][0]
            if dt > 0:
                allv.append(math.hypot(pts[k+1][1]-pts[k][1], pts[k+1][2]-pts[k][2])*MM_PER_UNIT/dt/1000)
    if allv:
        allv.sort()
        print(f"  >>> 全部帧速度：中位 {allv[len(allv)//2]:.2f} m/s  "
              f"p95 {allv[int(len(allv)*0.95)]:.2f} m/s  峰值 {allv[-1]:.2f} m/s  "
              f"超过 1.00 m/s 的帧 {100*sum(1 for v in allv if v>1.0)/len(allv):.0f}%")

def gap_report(path):
    """★ 判断"一条滑动有没有被驱动切开"最可靠的指标。

    不要用"短轨迹的比例" —— 那只在用户【连续滑动】时才有意义，
    正常使用里点按本身就是短轨迹，会得出假的回归结论（2026-09-14 我栽过一次）。

    真正的判据是**相邻轨迹之间的间隔**：手指若真的抬起再按下，
    跨过间隔的隐含速度应当接近 0；若是一条滑动被切开，手指在"隐形"期间
    仍在移动，隐含速度就等于它当时的滑行速度。
    #114 实测：修复前 35% 的间隔短于 100 ms、隐含速度中位 **1.01 m/s**
    （正好是那条限速线）；修复后最小间隔 124 ms、隐含速度 0.08 m/s。
    """
    _, strokes = decode(path)
    S = [(t, p) for t, p in strokes if p]
    S.sort(key=lambda kp: kp[1][0][0])
    g = []
    for i in range(len(S) - 1):
        a, b = S[i][1], S[i + 1][1]
        dt = b[0][0] - a[-1][0]
        dd = math.hypot(b[0][1] - a[-1][1], b[0][2] - a[-1][2]) * MM_PER_UNIT
        g.append((dt * 1000, dd))
    if not g:
        print("  轨迹不足，无法做间隔分析")
        return
    overlap = sum(1 for x, _ in g if x <= 0)
    g = [(x, d) for x, d in g if x > 0]          # ≤0 ms = 两条轨迹在时间上重叠 = 多指，不是碎片
    if overlap:
        print(f"  时间上重叠的轨迹对（多指同时在屏）：{overlap} 对 —— 不计入下面的间隔统计")
    if not g:
        print("  没有先后相接的轨迹，无法做间隔分析")
        return
    ts = sorted(x for x, _ in g)
    print(f"  相邻轨迹间隔 ms：最小 {ts[0]:.0f}  中位 {ts[len(ts)//2]:.0f}  最大 {ts[-1]:.0f}")
    for thr in (25, 50, 100, 200):
        n = sum(1 for x, _ in g if x < thr)
        print(f"    < {thr:>3} ms：{n:>3} 个 ({100*n/len(g):>3.0f}%)")
    close = [(x, d) for x, d in g if x < 100 and x > 0]
    if close:
        sp = sorted(d / (x / 1000) / 1000 for x, d in close)
        v = sp[len(sp)//2]
        verdict = "⚠️ 像是被切开的滑动" if v > 0.3 else "✓ 像是真实的抬手"
        print(f"    间隔 <100 ms 时跨间隔的隐含速度中位 {v:.2f} m/s  {verdict}")
    else:
        print("    没有短于 100 ms 的间隔 ⇒ 每次轨迹边界都是真实抬手 ✓")


if __name__ == '__main__':
    for p in sys.argv[1:]:
        report(p)
        gap_report(p)
