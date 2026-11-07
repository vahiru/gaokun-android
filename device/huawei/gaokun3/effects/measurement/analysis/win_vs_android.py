#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""win_vs_android.py -- 把 Windows 的数字域曲线和 Android Histen 的数字域曲线放在同一口径下
逐频段比较，并直接解出 Histen 的 8 个有效 EQ 槽该写多少。

为什么这两条曲线可以相减
------------------------
两侧都是「输入 wav -> 输出 wav」的**数字域**传递函数：

  Windows : noise-flat.wav -> lb-noise-win.wav
            （WASAPI loopback 抓渲染端点，含 Qualcomm Aqstic + 华为 Histen 全部 APO，
              不含房间、不含麦克风）
  Android : eqscan 的 base 曲线
            （in.wav 扫频 -> 引擎输出，只含华为 Histen，不含 SpeakerChain、不含扬声器）

两侧都不含扬声器，所以差值就是「处理链」的差。
⚠ **口径上仍差一个 SpeakerChain**：Android 实际输出 = SpeakerChain(Histen(...))，
  HPF@150 会把 120–150 Hz 再砍一刀，所以槽 1 的"该砍多少"有一部分已经被 HPF 做了。

⚠ **整体增益 vs 形状必须分开**：
  EQ 槽只能改形状，给不了整体增益。Windows 那 +12 dB 的整体优势要靠 PA / makeup 补。
  所以脚本同时给出「形状差」和「绝对差」两张表 —— 前者决定 EQ，后者决定 PA/makeup。

输入
----
  --win  : windows/digital_response.py --csv 出来的 CSV（abs_gain_dB / rel_1kHz_dB）
  --and  : eqscan_report.py 出来的 eqscan_curves.csv
           （base_db_rel_input + sK_VVV 列，sK_VVV 是该槽设成 VVV 时相对基线的变化）
  --base : eqscan 的 base_eq.txt（基线槽值 + 中心频率）

用法
----
  python3 analysis/win_vs_android.py --win data/win-digital-response.csv \
        --and data/eqscan-curves.csv --base data/base_eq.txt --hpf 150
  （路径相对 measurement/；原稿里的 tools/、analysis/audio/m3/、artifacts/ 是作者工作区的布局）
"""

import argparse
import math
import sys

import numpy as np
from scipy.optimize import minimize, lsq_linear

WIN_REF_HZ = 1000.0
GRID_LO, GRID_HI = 90.0, 16000.0
GRID_OCT_DIV = 6

# 由 main() 填。eval_preset() 用它来把「残差最大」的格点还原成频率。
# （残差最大的格点不一定落在 RESID_FREQS 的 19 个mark 上 —— 中高频 RMS 的
#  真正驱动点常常在两个 mark 之间，只看 mark 会漏掉。见 --worst。）
GRID_REF = None
WORST_N = 0
EQ_UNIT_DB = 0.0801      # 实测中位斜率，仅用于打印换算参考
EQ_MIN, EQ_MAX = 0, 200  # 实测可用范围（≥204 被 Init 拒，返回 -145）


def load_csv_rows(path):
    with open(path, "r", encoding="utf-8") as h:
        hdr = h.readline().strip().split(",")
        out = []
        for line in h:
            p = line.strip().split(",")
            if len(p) != len(hdr):
                continue
            try:
                out.append(dict(zip(hdr, [float(v) for v in p])))
            except ValueError:
                continue
    return out


def log_interp(f_grid, f_src, y_src):
    f_src = np.asarray(f_src, dtype=float)
    y_src = np.asarray(y_src, dtype=float)
    m = np.isfinite(y_src) & (f_src > 0)
    return np.interp(np.log10(f_grid), np.log10(f_src[m]), y_src[m])


def oct_grid(div, lo, hi):
    r = 2.0 ** (1.0 / div)
    g, f = [], lo
    while f <= hi:
        g.append(f)
        f *= r
    return np.array(g)


def lr4_hpf_db(f_grid, fc, sr=48000.0):
    """LR4 高通的幅频响应（dB），与 speaker_chain.h 的实现一致：
    两级**相同** Q=1/sqrt(2) 的二阶 Butterworth 高通级联 ⇒ 传函平方。

    直接在单位圆上求值，不做时域滤波 —— 精确且瞬时。
    `speaker_chain.h` 用的是 RBJ cookbook 系数，这里按同一组公式重算，
    避免和 C 代码的 a0 归一化约定对不上。"""
    w = 2.0 * math.pi * np.asarray(f_grid, dtype=float) / sr
    z = np.exp(-1j * w)
    w0 = 2.0 * math.pi * fc / sr
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * (1.0 / math.sqrt(2.0)))
    b = np.array([(1 + cw) / 2.0, -(1 + cw), (1 + cw) / 2.0])
    a = np.array([1 + alpha, -2 * cw, 1 - alpha])
    b = b / a[0]
    a = a / a[0]
    sec = (b[0] + b[1] * z + b[2] * z ** 2) / (a[0] + a[1] * z + a[2] * z ** 2)
    return 20.0 * np.log10(np.abs(sec ** 2) + 1e-30)


def parse_preset(s):
    """把 "1=120 7=100 2=50" 解析成 {slot: value}。"""
    out = {}
    for tok in s.replace(",", " ").split():
        if "=" not in tok:
            raise SystemExit("预设格式应为 slot=value：%r" % tok)
        k, v = tok.split("=", 1)
        out[int(k)] = int(v)
    return out


def bell_db(f_grid, fc, gain_db, q, sr=48000.0):
    """RBJ peaking EQ（bell）的幅频响应（dB）。

    为什么需要它：Histen 的 8 个 EQ 槽中心是 120/220/560/1100/2200/3700/4600/14000，
    **1.6–2.0 kHz 落在槽6(1100) 与槽3(2200) 之间**，而这两个槽都很宽 ⇒
    抬槽3 会同时抬高 2200 与 3000 Hz（3000 本来就超 Windows 目标 +5.4 dB）⇒
    实测把槽3 从 97 抬到 200，中高频 RMS 反而从 3.33 **恶化**到 4.64。
    ⇒ 要真正补 1.6–2.0 kHz 且不碰 3 kHz，只能加一个**中心在 ~1.7 kHz 的 bell**。
    这个函数就是为了给那个 bell 做参数设计（SpeakerChain 侧实现待加）。"""
    f = np.asarray(f_grid, dtype=float)
    if gain_db == 0.0:
        return np.zeros_like(f)
    w = 2.0 * math.pi * f / sr
    z = np.exp(-1j * w)
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * fc / sr
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * q)
    b = np.array([1 + alpha * A, -2 * cw, 1 - alpha * A])
    a = np.array([1 + alpha / A, -2 * cw, 1 - alpha / A])
    b = b / a[0]
    a = a / a[0]
    return 20.0 * np.log10(np.abs((b[0] + b[1] * z + b[2] * z ** 2)
                                  / (a[0] + a[1] * z + a[2] * z ** 2)) + 1e-30)


RESID_FREQS = (90, 113, 143, 180, 227, 300, 400, 587, 800, 1100, 1600,
               2000, 2300, 3000, 3700, 4600, 6000, 10000, 14000)

# ★ 2026-09-25：低频必须单独看。
# 用户拿《渡口》前几声鼓来听，说「腔体共鸣处理得不够好，就是超重低音那部分」。
# 一算就明白：默认 RESID_FREQS 从 90 Hz 起，而鼓腔在 60–110 Hz ——
# **判据的下限之外**，整段失明。要看低音就得把 mark 往下降到 40。
LOW_RESID_FREQS = (40, 50, 60, 70, 80, 90, 100, 113, 143, 180, 227, 260)


def set_resid_freqs(spec):
    """把 --resid-freqs 的逗号列表换成模块级 RESID_FREQS。"""
    global RESID_FREQS
    if not spec:
        return
    vals = []
    for tok in spec.replace(";", ",").split(","):
        tok = tok.strip()
        if tok:
            vals.append(float(tok))
    if len(vals) < 3:
        raise SystemExit("--resid-freqs 至少要 3 个频率")
    RESID_FREQS = tuple(vals)


def resid_table(grid, tgt, pred, low_guard, label):
    """逐频段打印 目标 / 预测 / 残差。+ = 预测比目标高。

    这是**下发设备前的最后一道闸**：求解器的标量代价会把"某一段被推过头、
    另一段还差着"平均掉，只有把逐段残差摊开看，才能提前发现
    「过砍（→干瘪）」或「过抬（→浑浊 / 齿音）」。"""
    print("  预测残差 @ %s" % label)
    print("    freq    目标  预测  残差   标记")
    for f_mark in RESID_FREQS:
        j = int(np.argmin(np.abs(grid - f_mark)))
        rv = pred[j] - tgt[j]
        tag = ""
        if grid[j] < low_guard and rv > 0.5:
            tag = "⚠ 低频超调"
        elif abs(rv) > 4.0:
            tag = "⚠ 偏差大"
        print("    %-7d %6.2f %6.2f %6.2f   %s" % (f_mark, tgt[j], pred[j], rv, tag))


def eval_preset(A, base, G, tgt, keys, base_val, vals, label,
                mask, lo_sel, hi_sel, penalty, headroom=6.74, w_drive=1.0,
                under_penalty=1.5, bell=None):
    """给定一套槽值，算它对目标的偏差。

    `base` 与 `tgt` 由调用方按**目标域**给：
      shape 域：base = a_rel + hpf，        tgt = w_rel
      abs   域：base = a_rel + hpf + a_ref，tgt = w_abs
    残差 = base + G + A@x − tgt，与域无关。

    ★ 判据是**不对称**的（2026-09-24 教训一）：
      中高频按平方误差（补不够就是错），低频**超调**重罚（= 浑浊）、**欠补**轻罚（= 干瘪）。
      只看全频 RMS 会把「低频超调 +15 dB」和「中频差 15 dB」当成一样的错，
      前者用户立刻听得出来（低音浑），后者只是不够亮。

    ★★ 2026-09-24 教训二（用户：「预设A整体更干净」）：
      上面这一条还不够。求解器解出的目标在 1.1/2.2 kHz 各抬 +9.5/+11.2 dB，配上 makeup，
      **总量增益**远超限幅器余量 ⇒ 限幅器持续压缩 + softclip ⇒ 听感「不干净」。
      所以再加一项 **drive 代价**：`max(0, G + EQ峰增益 − headroom)`。

      headroom 是实测出来的，不是猜的：
        outPeak(make=4) = 0.410、outPeak(make=10) = 0.817（+6 dB ⇒ 增益 ×1.993，线性）
        ceiling = −1 dBFS = 0.891  ⇒  余量 = 20·log10(0.891/0.410) = 6.74 dB
      ⇒ 相对 makeup=+4 这个工作点，**整条链最多还能再抬 6.74 dB**，
      否则限幅器开始工作。EQ 峰值增益与 makeup 共同消耗这一份预算。

    ★★★ 2026-09-24 教训三（只用单向低频守护会翻到另一个极端）：
      低频若**只罚超调**，求解器会一路把 90/180 Hz 砍低 10 dB ⇒ 从「浑浊」变成「干瘪」。
      所以低频必须是**双向**的，只是权重不对称：超调 : 欠补 ≈ 4 : 1。"""
    x = np.array([float(vals.get(k, base_val[k])) - float(base_val[k]) for k in keys])
    _b = np.zeros_like(base) if bell is None else bell
    resid = base + G + A @ x - tgt
    rm = resid[mask]
    rms = float(np.sqrt((rm ** 2).mean()))
    ov = float(np.max(np.maximum(0.0, resid[lo_sel])))
    un = float(np.max(np.maximum(0.0, -resid[lo_sel])))
    hr = float(np.sqrt((resid[hi_sel] ** 2).mean()))
    drive = G + float((A @ x + _b)[mask].max())   # 全链最多抬了多少 dB（含 bell）
    over = max(0.0, drive - headroom)
    cost = penalty * ov ** 2 + under_penalty * un ** 2 + hr ** 2 + w_drive * over ** 2
    print("  %-34s 中高频RMS %5.2f  超调 %5.2f  欠补 %5.2f  增益 %5.2f/%.1f  "
          "代价 %7.2f%s"
          % (label, hr, ov, un, drive, headroom, cost,
             "  ⚠压限幅器" if over > 0.05 else ""))
    if WORST_N and GRID_REF is not None:
        idx = np.where(hi_sel)[0]
        order = idx[np.argsort(-np.abs(resid[idx]))][:WORST_N]
        print("      ★ 中高频 RMS 的最大贡献点（只看 19 个 mark 会漏掉这些）：")
        for j in sorted(order, key=lambda t: float(GRID_REF[t])):
            print("         %6.0f Hz  残差 %+6.2f   (目标 %+6.2f / 预测 %+6.2f)"
                  % (GRID_REF[j], resid[j], tgt[j], tgt[j] + resid[j]))
        # 逐点贡献 → 谁在吃 RMS
        part = resid[idx] ** 2
        tot = float(part.sum())
        if tot > 1e-9:
            sh = np.argsort(-part)[:max(WORST_N, 6)]
            print("         前 6 大格点占全中高频平方误差的 %.0f%%"
                  "（全带 %d 个格点）"
                  % (100.0 * float(part[sh].sum()) / tot, len(idx)))
    return cost, ov, un, hr, resid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--win", required=True)
    ap.add_argument("--and", dest="and_csv", required=True)
    ap.add_argument("--base", required=True)
    ap.add_argument("--png", help="同时输出三张子图（英文标签）")
    ap.add_argument("--hpf", type=float, default=150.0,
                    help="SpeakerChain 的 LR4 高通频率（Hz），0 = 不施加。默认 150")
    ap.add_argument("--bell", action="append", default=[], metavar="FREQ:GAIN:Q",
                    help="额外加入一个 RBJ peaking EQ（设计用），例如 1700:3.5:1.2。"
                         "可重复；也可给负增益做挖谷，例如 3000:-5:1.2")
    ap.add_argument("--bell-scan", default="",
                    help="逗号分隔的 bell 候选（每条 FREQ:GAIN:Q），跑一轮参数寻优。"
                         "例：\"1600:2:1.5,1700:3.5:1.2,1800:5:1.0\"")
    ap.add_argument("--eval", action="append", default=[], metavar='"1=120 7=100 …"',
                    help="评价给定预设（可重复）。用来客观比较听测预设")
    ap.add_argument("--show-resid", action="store_true",
                    help="对每个 --eval 预设也打印逐频段残差表（对比「差在哪个频段」用）")
    ap.add_argument("--worst", type=int, default=0, metavar="N",
                    help="额外打印每个预设「中高频 RMS 最大」的 N 个格点（连续网格，"
                         "不是 19 个 mark）。用来定位 RMS 到底被谁吃掉 —— 默认 0=关")
    ap.add_argument("--low-guard", type=float, default=250.0,
                    help="低于此频率算「低频」：默认按不对称权重罚（超调重、欠补轻）。"
                         "默认 250 Hz。⚠ 2026-09-25 起若要把低频做成**双向对称**"
                         "（Windows 在那段既有凹陷也有峰，见 --grid-lo），把 "
                         "--low-penalty 与 --low-under-penalty 设成同值")
    ap.add_argument("--grid-lo", type=float, default=90.0, metavar="HZ",
                    help="评估网格的下限。默认 90 Hz —— ⚠ 但 90 Hz 以下才是「超重低音/"
                         "鼓腔」所在（渡口前几声鼓 ~60–110 Hz），默认值会让整段失明。"
                         "要看低音就设 --grid-lo 40")
    ap.add_argument("--resid-freqs", default="",
                    help="残差表的频率 mark（逗号分隔）。默认 90,113,…,14000。"
                         "看低音改用 \"40,50,60,70,80,90,100,113,143,180,227,260,"
                         "300,587,1100,1600,3000,6000,14000\"")
    ap.add_argument("--low-lift", default="", metavar='"1=… 7=…"',
                    help="★ 打印「本解相对这个参考预设的净电平变化」40–300 Hz 表。"
                         "这个量才是振膜行程 / 限幅器会不会被吃掉的真指标 —— "
                         "HPF 拐点一动，判据里的「增益」列看不见这份代价。"
                         "需同时给 --low-lift-hpf（参考预设当时的 HPF 拐点）")
    ap.add_argument("--low-lift-hpf", type=float, default=150.0,
                    help="--low-lift 参考预设所使用的 HPF 拐点（Hz）。默认 150")
    ap.add_argument("--target", choices=("shape", "abs"), default="shape",
                    help="EQ 的目标域。shape = 只补形状（1 kHz 锚定，整体增益交给 PA）★默认；"
                         "abs = 连整体增益一起补（2026-09-24 被否掉的旧做法，会压限幅器）")
    ap.add_argument("--hpf-scan", default="120,150,180,200,250",
                    help="第五节扫描的 HPF 候选（逗号分隔）。默认 120,150,180,200,250")
    ap.add_argument("--g-scan", default="0,1,2,3,4,5",
                    help="第五节扫描的平增益 G 候选（逗号分隔）。默认 0..5")
    ap.add_argument("--eval-hpf", type=float, default=None,
                    help="第六节评分时假定的 HPF（默认 = --hpf，即现场口径）")
    ap.add_argument("--low-penalty", type=float, default=6.0,
                    help="低频**超调**的惩罚权重（浊）。越大越不允许低音比目标厚。默认 6")
    ap.add_argument("--low-under-penalty", type=float, default=1.5,
                    help="低频**欠补**的惩罚权重（瘪）。只罚超调会一路砍到干瘪 —— "
                         "实测过：只看超调时解会把 90/180 Hz 砍低 10 dB。默认 1.5（约为超调权重的 1/4）")
    ap.add_argument("--headroom", type=float, default=6.74,
                    help="总量增益预算（dB，相对 makeup=+4 工作点）。实测：outPeak=0.410@make4、"
                         "0.817@make10，ceiling=−1 dBFS=0.891 ⇒ 20log10(0.891/0.410)=6.74。默认 6.74")
    ap.add_argument("--drive-penalty", type=float, default=1.0,
                    help="超出 headroom 的惩罚权重（平方）。0 = 关掉这一项。默认 1")
    ap.add_argument("--eval-g", type=float, default=0.0,
                    help="评价预设时假定的额外平增益 dB（相对 makeup=+4）。默认 0 = 现场口径")
    a = ap.parse_args()

    # --grid-lo 要在建网格（以及由它派生的 mask）之前生效。
    # 下面的 mask / band_mask 读的是模块级 GRID_LO，所以改写全局即可。
    globals()["GRID_LO"] = float(a.grid_lo)
    set_resid_freqs(a.resid_freqs)

    # ---- 基线槽值与中心频率 ----
    base_val, base_fc, scene = {}, {}, ""
    with open(a.base, "r", encoding="utf-8") as h:
        for line in h:
            line = line.strip()
            if line.startswith("# scene"):
                scene = line.lstrip("# ").strip()
                continue
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if len(p) >= 4:
                base_val[int(p[0])] = int(p[2])
                base_fc[int(p[0])] = float(p[3])
    live = [k for k in sorted(base_fc) if k not in (0, 9, 10)]

    # ---- Windows 曲线 ----
    wr = load_csv_rows(a.win)
    wf = np.array([r["freq_hz"] for r in wr])
    w_abs = np.array([r["abs_gain_dB"] for r in wr])
    w_ref = float(np.interp(WIN_REF_HZ, wf, w_abs))
    w_rel = w_abs - w_ref

    # ---- Android / Histen 曲线 + 各槽归一化形状 ----
    ar = load_csv_rows(a.and_csv)
    af = np.array([r["freq_hz"] for r in ar])
    a_abs = np.array([r["base_db_rel_input"] for r in ar])
    a_ref = float(np.interp(WIN_REF_HZ, af, a_abs))
    a_rel = a_abs - a_ref

    shapes = {}
    for k in live:
        c0, c192 = "s%02d_000" % k, "s%02d_192" % k
        if c0 not in ar[0] or c192 not in ar[0]:
            print("[warn] 槽 %d 缺 s%02d_000/s%02d_192，跳过" % (k, k, k))
            continue
        y0 = np.array([r[c0] for r in ar])
        y1 = np.array([r[c192] for r in ar])
        shapes[k] = (y1 - y0) / 192.0      # dB per unit

    grid = oct_grid(GRID_OCT_DIV, GRID_LO, GRID_HI)
    globals()["GRID_REF"] = grid          # 供 eval_preset() 的 --worst 用
    globals()["WORST_N"] = a.worst
    w_g = log_interp(grid, wf, w_rel)

    # ---- 可选：SpeakerChain 里额外加的一个 bell ----
    # 用来补 1.6–2.0 kHz 那个 8 槽填不满的缺口（见 bell_db 的说明）。
    bell_g = np.zeros_like(grid)
    bell_txt = "off"
    for _spec in a.bell:
        _p = _spec.replace(":", " ").replace(",", " ").split()
        if len(_p) != 3:
            raise SystemExit("--bell 格式应为 FREQ:GAIN:Q，例如 --bell 1700:3.5:1.2")
        _bf, _bg, _bq = float(_p[0]), float(_p[1]), float(_p[2])
        bell_g = bell_g + bell_db(grid, _bf, _bg, _bq)
        _t = "%.0f Hz %+.1f dB Q=%.2f" % (_bf, _bg, _bq)
        bell_txt = _t if bell_txt == "off" else bell_txt + " + " + _t

    # ★ 口径修正：Android 最终输出是 SpeakerChain(Histen(..))，HPF 挂在 Histen **之后**。
    #   Windows 那条曲线里已经含了它自己的低频保护，所以比较时必须把我们的 HPF 也算进来，
    #   否则会误判成"低频已经很齐、不用动"。
    a_raw = log_interp(grid, af, a_rel)        # Histen 输出（不含 SpeakerChain）
    hf_db = lr4_hpf_db(grid, a.hpf) if a.hpf else np.zeros_like(grid)
    a_g = a_raw + hf_db + bell_g
    a_ref_eff = a_ref + (float(lr4_hpf_db(np.array([WIN_REF_HZ]), a.hpf)[0]) if a.hpf else 0.0)
    gain_gap = w_ref - a_ref_eff
    need_shape = w_g - a_g                    # 形状差（决定 EQ）
    need_abs = need_shape + gain_gap          # 绝对差（决定 PA / makeup）
    band = [(f, w_g[i], a_g[i], need_shape[i], need_abs[i])
            for i, f in enumerate(grid) if GRID_LO <= f <= 15500]
    mask = (grid >= GRID_LO) & (grid <= 15500)

    def table(rows, cols, unit):
        print("  %-9s %10s %10s %10s   %s" % ("freq_Hz", cols[0], cols[1], cols[2], unit))
        print("  " + "-" * 66)
        for f, wr_, ar_, ns, na in rows:
            v1, v2, v3 = (wr_ + w_ref, ar_ + a_ref, na) if unit == "绝对域" else (wr_, ar_, ns)
            print("  %-9.0f %10.2f %10.2f %10.2f" % (f, v1, v2, v3))

    print("=" * 74)
    print("一、整体增益（1 kHz，数字域）")
    print("  Windows loopback  : %+7.2f dB" % w_ref)
    print("  Android Histen    : %+7.2f dB   (scene %s + 基线 EQ)" % (a_ref, scene))
    print("  SpeakerChain HPF  : %+7.2f dB @ %.0f Hz (LR4, 挂在 Histen 之后)" % (a_ref_eff - a_ref, a.hpf))
    print("  ----------------")
    print("  整体增益差        : %+7.2f dB   <- 与声学外录的 11.6 dB 互相印证" % gain_gap)

    print("=" * 74)
    print("二、形状差（各自相对自己的 1 kHz）—— 这一张决定 EQ 槽")
    table(band, ("Win rel", "And rel", "需要补"), "形状域")
    print("  " + "-" * 66)
    hi = sorted(band, key=lambda r: -r[3])[:4]
    lo = sorted(band, key=lambda r: r[3])[:4]
    print("  Windows 更高的频段：" + "，".join("%.0f Hz %+.1f" % (r[0], r[3]) for r in hi))
    print("  Android  更高的频段：" + "，".join("%.0f Hz %+.1f" % (r[0], r[3]) for r in lo))

    print("=" * 74)
    print("三、绝对差（+ = Android 实际更轻）—— 这一张决定 PA / makeup")
    table(band, ("Win abs", "And abs", "Android 缺"), "绝对域")
    print("  " + "-" * 66)
    worst = sorted(band, key=lambda r: -r[4])[:6]
    print("  缺得最多：" + "，".join("%.0f Hz %+.1f dB" % (r[0], r[4]) for r in worst))
    ok = [r for r in band if abs(r[4]) <= 2.0]
    print("  已在 ±2 dB 内的频段：%d/%d" % (len(ok), len(band)))

    # ---- 求解槽值 ----
    # 两种目标，意义完全不同：
    #   形状域 need_shape = w_rel - a_rel
    #       假设"整体增益另外用 PA/makeup 补"，EQ 只负责把形状掰过来。
    #       问题是 need_shape 在低频是 −12~−13（要砍），在 1–2 kHz 是 +6，呈"上下摆动"；
    #       8 个槽的削减量不够 ⇒ 4 个槽撞到下界 0，残差 RMS 3.2 dB。
    #   绝对域 need_abs = need_shape + 整体增益差
    #       直接问"每个频段还缺多少 dB"，让 EQ 一次补齐、PA/makeup 不动或只微调。
    #       因为绝对缺口处处 ≥ −1 dB（低频已对齐、其余都偏轻），解是**近似非负**的，
    #       条件数好得多，也不会逼着去砍本来就刚好的低频。
    keys = [k for k in live if k in shapes]
    A = np.column_stack([log_interp(grid, af, shapes[k]) for k in keys])
    lb = np.array([-float(base_val[k]) for k in keys])
    ub = np.array([EQ_MAX - float(base_val[k]) for k in keys])
    band_mask = np.array([GRID_LO <= f <= 15500 for f in grid])

    # ---- 旧指标对照：不加守护、全频段等权重最小二乘 ----
    # 这一段**故意保留**：它就是 2026-09-24 得出"hpf=100 / G=+5.5"的那套算法，
    # 而用户实测反馈"低音更浑浊"。留着是为了让下次不再踩同一个坑。
    print("=" * 74)
    print("四、对照：旧指标（全频段等权重最小二乘，**不推荐**）")
    print("   它看不见低频超调的听感代价 ⇒ 会挑出「低音很厚但浑」的解。")
    x_old = lsq_linear(A, need_abs, bounds=(lb, ub), lsmr_tol="auto").x
    x_old = np.clip(x_old, lb, ub)
    n_hit = sum(1 for i in range(len(keys))
                if abs(x_old[i] - ub[i]) < 0.5 or abs(x_old[i] - lb[i]) < 0.5)
    print("   旧解：%s" % " ".join("%d=%d" % (k, int(round(base_val[k] + x_old[i])))
                                  for i, k in enumerate(keys)))
    print("   其中 %d/8 个槽撞到限值（0 或 200）⇒ 说明它是在「够不着」的情况下硬凑。" % n_hit)

    # ---- ★★★ 带「低频守护」的求解：平增益 G + EQ ----
    #
    # 【教训一】为什么必须加守护（2026-09-24）：
    #   最初用「全频段等权重的残差 RMS」选参数，选出 hpf=100 / G=+5.5，结果用户听到的是
    #   **「人声饱满了，但低音更浑浊」**。原因是这个指标**看不见低频超调的听感代价**：
    #     · LR4 150→100 在 100 Hz 加 +9.6 dB、120 Hz +7.3（HPF 拐点下移的必然结果）
    #     · makeup +6 dB 又是全频平抬
    #     · 于是 100–150 Hz 净增 ~+11…+15 dB
    #   而 Android 的 143 Hz 本来就比 Windows 高约 6 dB（形状域）⇒ 净超 Windows 十几 dB。
    #   100–150 Hz 正是「浑浊 / 箱声 / 轰」的频段 ⇒ 听感立刻变糊。
    #   ⇒ 指标必须**不对称**：中高频按平方误差（补不够就是错），
    #     低频**只惩罚超调**（补不够可以接受，超了就是浑浊）。
    #
    # 【教训二】目标域必须是「形状」不是「绝对」（2026-09-24，用户：「预设A整体更干净」）：
    #   abs 域（旧做法）让 8 个 EQ 槽去补 `need_abs`，而 need_abs = need_shape + 12.27 dB，
    #   等于**逼着 EQ 承担整体增益**。8 个 bell 给不出平坦的 +12 dB，只会把 +11…+12 dB
    #   硬塞进它覆盖得到的几个频段（槽3 @2.2 kHz 顶格、槽6 @1.1 kHz 也顶格），结果：
    #     · 全链最大增益 12.6 dB，而实测限幅器余量只有 6.74 dB ⇒ 必然持续压限幅器
    #     · 听感 = 「不干净」——这正是用户听到的第二个问题
    #   ⇒ EQ 只该补**形状**（1 kHz 处天然锚定），整体增益交给 **PA Volume**
    #     （PA 在 DAC 之后、纯线性，不消耗限幅器余量、不改音色）。
    #   实测依据：outPeak(make=4)=0.410、outPeak(make=10)=0.817（+6 dB ⇒ ×1.993，线性）
    #             ceiling=−1 dBFS=0.891 ⇒ 余量 = 20·log10(0.891/0.410) = 6.74 dB
    lo_sel = mask & (grid < a.low_guard)
    hi_sel = mask & (grid >= a.low_guard)
    tgt_abs = w_g + w_ref
    USE_SHAPE = (a.target == "shape")

    def guarded(hpf, G):
        """给定 HPF 与平增益 G，求 EQ 槽。
        返回 (x, 低频最大超调, 中高频 RMS, 全链最大增益, 残差向量)

        `base` 与 `tgt` 都是**域无关**的：换域只换这两个向量，代价函数不变。
          shape 域：base = a_rel + hpf，        tgt = w_rel     （1 kHz 处天然锚定）
          abs   域：base = a_rel + hpf + a_ref，tgt = w_abs
        """
        ag = a_raw + (lr4_hpf_db(grid, hpf) if hpf else 0.0) + bell_g
        base = ag if USE_SHAPE else ag + a_ref
        tgt = w_g if USE_SHAPE else tgt_abs

        def cost(x):
            r = base + G + A @ x - tgt
            c = float(np.sum(r[hi_sel] ** 2))
            c += a.low_penalty * float(np.sum(np.maximum(0.0, r[lo_sel]) ** 2))
            c += a.low_under_penalty * float(np.sum(np.minimum(0.0, r[lo_sel]) ** 2))
            # drive 项：EQ 峰增益 + bell 峰增益 + G 超出限幅器余量的部分，平方惩罚
            drive = G + float((A @ x + bell_g)[mask].max())
            c += a.drive_penalty * max(0.0, drive - a.headroom) ** 2
            return c

        r = minimize(cost, np.zeros(len(keys)), method="L-BFGS-B",
                     bounds=list(zip(lb, ub)), options={"maxiter": 4000})
        x = np.clip(r.x, lb, ub)
        resid = base + G + A @ x - tgt
        ov = float(np.max(np.maximum(0.0, resid[lo_sel]))) if lo_sel.any() else 0.0
        un = float(np.max(np.maximum(0.0, -resid[lo_sel]))) if lo_sel.any() else 0.0
        hr = float(np.sqrt((resid[hi_sel] ** 2).mean())) if hi_sel.any() else 0.0
        drive = G + float((A @ x + bell_g)[mask].max())
        return x, ov, un, hr, drive, base, tgt, resid

    print("=" * 74)
    print("五、★ 扫描 HPF × 平增益 G（目标域=%s；低频超调罚 %.1f / 欠补罚 %.1f；增益预算 %.2f dB）"
          % ("形状 shape" if USE_SHAPE else "绝对 abs",
             a.low_penalty, a.low_under_penalty, a.headroom))
    print("  HPF   G(dB)  低频超调  低频欠补  中高频RMS  增益   代价   槽1   槽7")
    print("  " + "-" * 74)
    cand = []
    hpf_list = [float(v) for v in a.hpf_scan.replace(",", " ").split()]
    g_list = [float(v) for v in a.g_scan.replace(",", " ").split()]
    for hpf in hpf_list:
        for G in g_list:
            x_, ov, un, hr, dv, base_, tgt_, resid_ = guarded(hpf, G)
            c = a.low_penalty * ov ** 2 + a.low_under_penalty * un ** 2 + hr ** 2 \
                + a.drive_penalty * max(0.0, dv - a.headroom) ** 2
            cand.append((c, hpf, G, ov, un, hr, dv, x_, base_, tgt_))
            if G in (0, 2, 4) or len(g_list) <= 2:
                print("  %-5d %-7.1f %8.2f %9.2f %10.2f %6.2f %7.2f  %4d %5d"
                      % (hpf, G, ov, un, hr, dv, c,
                         int(round(base_val[1] + x_[keys.index(1)])),
                         int(round(base_val[7] + x_[keys.index(7)]))))
    cand.sort(key=lambda t: t[0])
    cbest, hpf_best, gbest, ov_best, un_best, hr_best, dv_best, xb, base_best, tgt_best = cand[0]
    pred = base_best + gbest + A @ xb
    print("  " + "-" * 74)
    print("  ★ 最优：HPF = %d Hz，G = %+.1f dB" % (hpf_best, gbest))
    print("     低频最大超调 %+.2f dB（正 = 比目标厚 → 浑浊）" % ov_best)
    print("     低频最大欠补 %+.2f dB（正 = 比目标薄 → 干瘪）" % un_best)
    print("     中高频残差 RMS %.2f dB" % hr_best)
    print("     全链最大增益 %.2f dB / 预算 %.2f dB %s"
          % (dv_best, a.headroom,
             "（限幅器不介入）" if dv_best <= a.headroom + 0.05 else "（⚠ 超了，会压限幅器）"))
    print("     总代价 %.2f" % cbest)
    x = xb

    print("  " + "-" * 62)
    print("  ⚠ 对比：不加守护的旧指标会选 HPF=100 / G=+5.5 —— 正是「低音变浑」的那组。")
    print("     HPF 越低 → 100–150 Hz 进来越多 → 低频越浑；守护把这个代价算进去了。")

    preset = []
    print("  " + "-" * 68)
    print("  槽  中心频率     基线    需调(dB)   新值   换算(dB)  备注")
    for i, k in enumerate(keys):
        newv = max(EQ_MIN, min(EQ_MAX, int(round(base_val[k] + xb[i]))))
        note2 = ""
        if abs(xb[i] - ub[i]) < 0.5:
            note2 = "已到顶"
        elif abs(xb[i] - lb[i]) < 0.5:
            note2 = "已到底"
        print("  %-3d %8.0f Hz %7d %10.2f %7d %9.2f  %s"
              % (k, base_fc[k], base_val[k], xb[i], newv, (newv - base_val[k]) * EQ_UNIT_DB, note2))
        preset.append("%d=%d" % (k, newv))

    # 该解对应的 EQ 形状（用于评分）。
    # ★ 第六节用**现场 HPF**（--eval-hpf，默认 = --hpf）重建 base/tgt：
    #   用户听到的是「当前部署的 hpf + makeup=4」，用最优解自己的 HPF 去评分会串口径。
    ehpf = a.hpf if a.eval_hpf is None else a.eval_hpf
    ag_ev = a_raw + (lr4_hpf_db(grid, ehpf) if ehpf else 0.0) + bell_g
    base_ev = ag_ev if USE_SHAPE else (ag_ev + a_ref)
    tgt_ev = w_g if USE_SHAPE else tgt_abs

    if a.eval:
        print("=" * 74)
        print("六、听测预设客观评分（★ 现场口径：HPF=%.0f Hz、makeup %+g dB，只改 EQ）"
              % (ehpf, a.eval_g))
        print("   代价 = %.0f·超调² + %.1f·欠补² + 中高频RMS² + %.0f·max(0,增益−%.2f)²"
              % (a.low_penalty, a.low_under_penalty, a.drive_penalty, a.headroom))
        print("   " + "-" * 68)
        _ev = dict(mask=mask, lo_sel=lo_sel, hi_sel=hi_sel, penalty=a.low_penalty,
                   headroom=a.headroom, w_drive=a.drive_penalty,
                   under_penalty=a.low_under_penalty, bell=bell_g)

        def _score(vals, label):
            _c, _ov, _un, _hr, _res = eval_preset(
                A, base_ev, a.eval_g, tgt_ev, keys, base_val, vals, label, **_ev)
            if a.show_resid:
                # eval_preset 回的是残差向量；预测 = 残差 + 目标
                resid_table(grid, tgt_ev, _res + tgt_ev,
                            a.low_guard, label + "（HPF=%.0f Hz）" % ehpf)
            return _c

        _score({k: base_val[k] for k in keys}, "场景表基线（EQ 全清）")
        for s in a.eval:
            _score(parse_preset(s), s)
        _score(dict(zip(keys, [int(round(base_val[k] + xb[i])) for i, k in enumerate(keys)])),
               "★ 本次求解目标")
        if abs(gbest - a.eval_g) > 0.05:
            print("   " + "-" * 68)
            print("   下面这行把上式求解目标换到它自己的 G=%+.1f dB 工作点（供对照，非现场口径）：" % gbest)
            eval_preset(A, base_ev, gbest, tgt_ev, keys, base_val,
                        dict(zip(keys, [int(round(base_val[k] + xb[i])) for i, k in enumerate(keys)])),
                        "★ 求解目标 @ G=%+.1f" % gbest, **_ev)
        print("  ⚠ 「代价」才是选择判据；RMS 只看全频，会被低频超调和压限幅器骗过去。")
        print("  ⚠ 「增益」列 = G + EQ 峰增益。超过 %.2f dB 就是压限幅器 = 听感「不干净」。"
              % a.headroom)

    print("=" * 74)
    print("七、可直接粘贴（设备端）")
    # ---- 预测残差：部署前先看它到底把哪几段推到了哪里 ----
    x_sol = np.array([float(int(round(base_val[k] + xb[i]))) - float(base_val[k])
                      for i, k in enumerate(keys)])
    print("  域=%s，HPF=%d，G=%+.1f" % ("shape" if USE_SHAPE else "abs", hpf_best, gbest))
    resid_table(grid, tgt_best, base_best + gbest + A @ x_sol, a.low_guard, "★ 本次求解目标")
    # 维护者注（2026-09-27，PR #8 审阅）：下面原来打印的是作者工作区的 tools/_tmp/histen-ab.sh
    # （不在本仓），换成本仓 effect 的旋钮（gaokun_effect.cpp / histen_chain.h）。
    # 另外三处：①基线数据是哪个场景就先切到哪个场景（部署默认是场景 0，EQ 槽的中心频率随场景变）；
    # ②HPF 低于部署值 150 Hz 时先看 §九 并做 THD；③PA 的建议值不超过内核上限 23（patches/kernel/audio/0001）。
    def knob(name, val):
        print('     adb shell su -c "setprop persist.vendor.gaokun3.histen.%s %s"' % (name, val))
    scene_no = scene.split()[1] if scene.startswith("scene ") and len(scene.split()) > 1 else ""
    if scene_no:
        print('  0) 基线是 %s —— 预设只对这个场景成立（部署默认场景 0，EQ 槽中心频率不同）：' % scene)
        knob("scene", scene_no)
    print('  1) HPF：')
    knob("hpf", "%d" % hpf_best)
    if hpf_best < 150:
        print('     ⚠ 低于部署默认 150 Hz。先加 --low-lift "<部署时的槽值>" --low-lift-hpf 150 看 §九：')
        print('       60 Hz 以下净抬升 >8 dB 就必须先做一次麦克风近场 THD 测量再下发。')
        print('       注意本脚本在 --grid-lo（默认 90 Hz）以下不计分、也没有失真项，')
        print('       所以把 --hpf-scan 往低放，它会一直选更低的 HPF —— 那不是最优，是看不见。')
    print('  2) EQ 槽（eq.N 直接写无符号值，引擎在 ≥204 时 Init 失败，effect 已钳到 0..203）：')
    for kv in preset:
        k, v = kv.split("=")
        knob("eq.%s" % k, v)
    make_rec = 4.0 + gbest
    if abs(gbest) > 0.05:
        if make_rec <= 4.0 + a.headroom:
            print('  3) makeup（当前部署 +4 dB，本解 %+.1f dB，在 %.1f dB 预算内）：' % (gbest, a.headroom))
            knob("makeup", "%.0f" % make_rec)
        else:
            print('  3) ⚠ 本解想要 makeup %+.1f dB，但预算只有 %.1f dB ⇒ 会被限幅器压。'
                  '建议 makeup 停在 +%.0f。' % (gbest, a.headroom, 4.0 + a.headroom))
    else:
        print('  3) makeup 保持 +4 dB 不动（本解不需要额外平增益）')
    print()
    print('  ★ PA Volume：本仓 audio-route.sh 写 21（+6 dB），内核上限 23（+9 dB，patches/kernel/audio/0001，')
    print('     = 器件允许的一半；理由：没有出厂校准、两颗 WSA 的 SoundWire Alert 没人服务、同设置隔几分钟漂 7.5 dB）。')
    print('     PA 在限幅器【之后】：它不吃上面的 %.1f dB 预算，也意味着 −1 dBFS 天花板管不到最终声压，' % a.headroom)
    print('     低频冲程跟着一起涨。要试就在 21..23 之间、先看 /sys/class/hwmon/*/temp1_input：')
    print('       adb shell su -c \'tinymix -D 0 "SpkrLeft PA Volume" 23; tinymix -D 0 "SpkrRight PA Volume" 23\'')
    print()
    print('  回到部署默认（场景 0、hpf 150、makeup +4、EQ 用场景表）：把上面设过的属性清空，例如')
    print("""     adb shell su -c 'setprop persist.vendor.gaokun3.histen.eq.1 ""'   # 空值 = 不覆盖，用场景表""")
    print('  作者的「预设 A」（用户验证过「干净」，hpf 150 + makeup 4，场景 3）：')
    print('     eq: 1=120 7=100 2=50 6=120 3=140 4=140 5=120 8=140')

    if a.png:
        # 用**现场 HPF** 重建曲线，这样图里就是最终方案的样子。
        # 「Android 绝对」恒为 a_rel + a_ref + hpf —— 与目标域无关，物理量只有一个。
        a_abs_ev = ag_ev + a_ref
        band_best = [(f, w_g[i] + w_ref, a_abs_ev[i],
                      w_g[i] - ag_ev[i], (w_g[i] + w_ref) - a_abs_ev[i])
                     for i, f in enumerate(grid) if GRID_LO <= f <= 15500]
        make_plot(a.png, grid, band_best, w_ref, a_ref, pred, tgt_ev,
                  base_fc, mask, a.low_guard)
        print("\n图已写出 %s" % a.png)

    # ---- §八 bell 参数设计 ----
    # ⚠ 放在最后：它会临时改写闭包里的 bell_g（guarded() 读的是那个变量），
    #   所以前面所有输出已经算完再跑，不会串口径。
    if a.bell_scan:
        print("=" * 74)
        print("八、★ bell 参数设计 —— 补 1.6–2.0 kHz（8 槽做不到的那段）")
        print("   固定 HPF=%d Hz、额外平增益 G=0（= 现场 makeup +4 口径），只让 EQ 重新求解"
              % a.hpf)
        print("   bell              中高频RMS  超调  增益   代价     1600     2000     3000")
        print("  " + "-" * 80)
        cands = [("off", np.zeros_like(grid))]
        for tok in a.bell_scan.replace(",", ";").split(";"):
            tok = tok.strip()
            if not tok:
                continue
            p = tok.replace(":", " ").split()
            if len(p) != 3:
                print("  [warn] 跳过无法解析的 bell 候选：%r" % tok)
                continue
            cands.append(("%.0fHz %+.1f Q%.2f" % (float(p[0]), float(p[1]), float(p[2])),
                          bell_db(grid, float(p[0]), float(p[1]), float(p[2]))))
        best = None
        for label, bg in cands:
            bell_g = bg
            x_, ov_, un_, hr_, dv_, base_, tgt_, _ = guarded(a.hpf, 0.0)
            resid_ = base_ + A @ x_ - tgt_
            cc = (a.low_penalty * ov_ ** 2 + a.low_under_penalty * un_ ** 2 + hr_ ** 2
                  + a.drive_penalty * max(0.0, dv_ - a.headroom) ** 2)

            def _r(fq, _rr=resid_):
                return _rr[int(np.argmin(np.abs(grid - fq)))]

            print("  %-18s %9.2f %5.2f %6.2f %7.2f %8.2f %8.2f %8.2f"
                  % (label, hr_, ov_, dv_, cc, _r(1600), _r(2000), _r(3000)))
            if best is None or cc < best[0]:
                best = (cc, label, x_, ov_, un_, hr_, dv_)
        print("  " + "-" * 80)
        _cb, _lab, _xb, _ov, _un, _hr, _dv = best
        print("  ★ 最优 bell = %s" % _lab)
        print("     中高频 RMS %.2f（不加 bell 时 %.2f ⇒ 改善 %.2f dB）"
              % (_hr, hr_best, hr_best - _hr))
        print("     低频超调 %.2f  全链增益 %.2f / 预算 %.2f  代价 %.2f"
              % (_ov, _dv, a.headroom, _cb))
        _pre = " ".join("%d=%d" % (k, max(EQ_MIN, min(EQ_MAX, int(round(base_val[k] + _xb[i])))))
                        for i, k in enumerate(keys))
        print("     配套 EQ：%s" % _pre)
        print("     ⚠ 设备上**还没有**这个 bell**：SpeakerChain 目前只有 hpf/makeup/limit/")
        print("       ceiling/release 五个旋钮。要落地需给 speaker_chain.h 加一段 biquad +")
        print("       3 个 setprop（bell.freq / bell.gain / bell.q），然后重编 effect 模块。")

    # ---- §九 净低频抬升（振膜行程 / 限幅器代价）----
    # ⚠ 为什么单独做这一节：
    #   判据里的「增益」列 = G + max(EQ 形状)，它是在**归一化形状**上取的峰，
    #   **完全看不见 HPF 拐点下移带来的低频电平抬升**。
    #   而 HPF 150→90 这种改动，在 60 Hz 上是十几 dB 的量级 —— 那是振膜行程，
    #   是真会把限幅器打起来、把「腔体共鸣」糊掉的东西。
    #   所以任何动过 HPF 的方案，下发前都必须看这张表。
    if a.low_lift:
        print("=" * 74)
        print("九、★ 净低频抬升 —— 本解 vs 参考预设（%s @ HPF=%.0f Hz）"
              % (a.low_lift, a.low_lift_hpf))
        print("   正 = 本解在该频率更响。低频段这一列就是**振膜行程的增量**。")
        ref = parse_preset(a.low_lift)
        lf = np.array([40, 45, 50, 55, 60, 70, 80, 90, 100, 113, 143, 180,
                       227, 260, 300, 400, 600, 1000], dtype=float)

        def chain_at(eqmap, hpf, freqs):
            g = log_interp(freqs, af, a_rel).copy()
            for k in live:
                v = eqmap.get(k, base_val[k])
                g = g + log_interp(freqs, af, shapes[k]) * (v - base_val[k])
            if hpf:
                g = g + lr4_hpf_db(freqs, hpf)
            return g

        _c_ref = chain_at(ref, a.low_lift_hpf, lf)
        _c_new = chain_at({k: max(EQ_MIN, min(EQ_MAX, int(round(base_val[k] + x_sol[i]))))
                           for i, k in enumerate(keys)}, hpf_best, lf)
        print("   %7s %13s %13s %12s" % ("freq", "参考", "本解", "净抬升"))
        print("   " + "-" * 50)
        for i, fq in enumerate(lf):
            d = _c_new[i] - _c_ref[i]
            flag = ""
            if fq <= 60 and d > 8.0:
                flag = "  ⚠⚠ 行程风险"
            elif fq <= 110 and d > 5.0:
                flag = "  ⚠ 低频抬升"
            print("   %7.0f %13.2f %13.2f %+11.2f%s" % (fq, _c_ref[i], _c_new[i], d, flag))
        print("   ⚠ 60 Hz 以下抬升 >8 dB ⇒ 下发前先量一次（麦克风近场 + 看 THD），")
        print("     LR4 是 24 dB/oct，拐点从 150 降到 90 时 45 Hz 会多 21 dB。")
    return 0


def make_plot(path, grid, band, wref_abs, aref_abs, pred_abs, tgt_abs,
              base_fc, mask, low_guard):
    """三张子图。标签一律用英文 —— 容器里不一定有中文字体，中文会变豆腐块。

    band 的每个元组 = (频率, Windows 绝对, Android 绝对, 形状差, 绝对缺口)"""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    f = np.array([r[0] for r in band])
    plt.rcParams.update({"font.size": 9, "axes.grid": True, "grid.alpha": 0.3})
    fig, ax = plt.subplots(3, 1, figsize=(11, 12), sharex=True)

    ax[0].semilogx(f, np.array([r[1] for r in band]), "r-o", ms=2.5,
                   label="Windows loopback (digital)")
    ax[0].semilogx(f, np.array([r[2] for r in band]), "b-o", ms=2.5,
                   label="Android Histen + SpeakerChain (digital)")
    ax[0].axvspan(85, low_guard, color="orange", alpha=0.10, lw=0)
    ax[0].set_ylabel("absolute gain vs input [dB]")
    ax[0].set_title("Absolute digital response  (1 kHz: Win %+.1f, And %+.1f); "
                    "shaded = low band, overshoot forbidden" % (wref_abs, aref_abs))
    ax[0].legend(loc="lower right")

    ax[1].semilogx(f, np.array([r[1] for r in band]) - wref_abs, "r-o", ms=2.5,
                   label="Windows rel 1 kHz")
    ax[1].semilogx(f, np.array([r[2] for r in band]) - aref_abs, "b-o", ms=2.5,
                   label="Android rel 1 kHz")
    ax[1].axvspan(85, low_guard, color="orange", alpha=0.10, lw=0)
    ax[1].set_ylabel("relative to own 1 kHz [dB]")
    ax[1].set_title("Shape  (Windows is multi-band; Android is one smooth slope)")
    ax[1].legend(loc="lower right")

    ax[2].axhline(0, color="k", lw=0.8)
    ax[2].axvspan(85, low_guard, color="orange", alpha=0.10, lw=0)
    ax[2].semilogx(f, np.array([r[4] for r in band]), "k-o", ms=2.5,
                   label="deficit vs Windows (absolute)")
    fdense = grid[mask]
    resid = (pred_abs - tgt_abs)[mask]
    ax[2].semilogx(fdense, resid, "g--", lw=1.3,
                   label="residual after guarded EQ solve")
    ax[2].set_ylabel("+ = Android quieter / louder [dB]")
    ax[2].set_xlabel("frequency [Hz]")
    ax[2].set_title("Deficit vs Windows  (in the shaded low band, only overshoot counts)")
    ax[2].legend(loc="upper left")

    for a_ in ax:
        a_.set_xlim(85, 16000)
    for fc in base_fc.values():
        for a_ in ax:
            a_.axvline(fc, color="0.85", lw=0.7, zorder=0)
    fig.tight_layout()
    fig.savefig(path, dpi=110)


if __name__ == "__main__":
    sys.exit(main())
