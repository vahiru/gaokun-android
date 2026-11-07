#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""扬声器响度 A/B 分析的 PC 端工具（配套 audio-loudness-sweep.sh）。

能做的两件事：
  1) --make-tone PATH   生成 440 Hz 测试音 wav，推到设备给扫描脚本用
  2) DIRECTORY          对录回的一批 wav 做 Goertzel，输出 dBFS 对比表
  3) --selftest         不需要设备，自检：造一个 -6 dBFS 的音再量回来

方法沿用 patches/kernel/audio/0001 已验证的做法：内置麦回采 440 Hz 单音，Goertzel 取该频点幅度。
寂静基准实测约 -99 dBFS、有音约 -23 dBFS，约 76 dB 测量余量。

完整流程：
    py -3 audio_goertzel.py --make-tone tone.wav
    adb push tone.wav /data/local/tmp/tone.wav
    adb push audio-loudness-sweep.sh /data/local/tmp/
    adb shell chmod 0755 /data/local/tmp/audio-loudness-sweep.sh
    adb shell /data/local/tmp/audio-loudness-sweep.sh              # dry-run
    adb shell /data/local/tmp/audio-loudness-sweep.sh --apply      # 真跑
    adb pull /data/local/tmp/loudness-sweep ./sweep-out
    py -3 audio_goertzel.py ./sweep-out

⚠️ Windows 上请用 `py -3` 或 `python`，不要直接用 `python3` ——
   后者常命中 %LOCALAPPDATA%\\Microsoft\\WindowsApps\\python3.exe 这个
   0 字节的「微软商店应用执行别名」，会静默拉起商店窗口且不产生任何输出。
"""

import argparse
import math
import os
import struct
import sys
import wave

TONE_HZ = 440.0
FS = 48000
FULL_SCALE = 32768.0


# --------------------------------------------------------------------------
# 输出：Windows 控制台编码兜底
# --------------------------------------------------------------------------
def _setup_stdio(ascii_only=False):
    """PowerShell 5.1 + 老版 conhost 的管道编码可能是 cp936，中文/制表符
    会抛 UnicodeEncodeError 导致脚本「看起来什么都没输出」。这里统一兜底。"""
    enc = "ascii" if ascii_only else "utf-8"
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding=enc, errors="replace")
        except Exception:
            pass


# --------------------------------------------------------------------------
# 测试音生成
# --------------------------------------------------------------------------
def make_tone(path, seconds=3.0, freq=TONE_HZ, fs=FS, amp=0.5, next_step=True):
    """生成 48k 16bit 立体声正弦音，带 20ms 淡入淡出，避免硬起停的额外瞬态。"""
    path = os.path.abspath(path)
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d, exist_ok=True)

    n = int(fs * seconds)
    fade = int(fs * 0.02)
    frames = bytearray()

    for i in range(n):
        a = 1.0
        if i < fade:
            a = i / fade
        elif i > n - fade:
            a = (n - i) / fade
        v = int(FULL_SCALE * amp * a * math.sin(2 * math.pi * freq * i / fs))
        v = max(-32768, min(32767, v))
        frames += struct.pack("<hh", v, v)          # 双声道：PCM 1 要求 stereo

    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes(bytes(frames))

    size = os.path.getsize(path)
    print("[OK] 测试音已生成")
    print("     路径   : %s" % path)
    print("     规格   : %.1f s / %.0f Hz / %d Hz / stereo / 16-bit" % (seconds, freq, fs))
    print("     电平   : 峰值 %.0f%% FS  (%.1f dBFS)" % (amp * 100, 20 * math.log10(amp)))
    print("     大小   : %d 字节" % size)
    if next_step:
        print("")
        print("     下一步：")
        print("       adb push \"%s\" /data/local/tmp/tone.wav" % os.path.basename(path))
    return path


# --------------------------------------------------------------------------
# 分析
# --------------------------------------------------------------------------
def read_wav(path):
    """返回 (采样率, 单声道 float 列表)。多声道取左声道。"""
    with wave.open(path, "rb") as w:
        fs = w.getframerate()
        nch = w.getnchannels()
        sw = w.getsampwidth()
        raw = w.readframes(w.getnframes())

    if sw != 2:
        raise ValueError("只支持 16-bit，实际 %d-bit" % (sw * 8))

    cnt = len(raw) // 2
    shorts = struct.unpack("<%dh" % cnt, raw)
    mono = [shorts[i] / FULL_SCALE for i in range(0, cnt, nch)]
    return fs, mono


def goertzel_dbfs(samples, fs, freq=TONE_HZ, win_s=0.1):
    """Goertzel 取 freq 频点幅度，返回 dBFS。

    窗口取文件正中，长度取整数个周期（0.1 s @ 440 Hz = 44 个周期整），
    避免非整周期截断带来的频谱泄漏。
    """
    n = int(fs * win_s)
    if len(samples) < n:
        n = len(samples)
    if n < 16:
        return None

    start = (len(samples) - n) // 2
    seg = samples[start:start + n]

    k = int(0.5 + n * freq / fs)
    w = 2.0 * math.pi * k / n
    coeff = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for x in seg:
        s0 = x + coeff * s1 - s2
        s2, s1 = s1, s0

    real = s1 - s2 * math.cos(w)
    imag = s2 * math.sin(w)
    mag = math.hypot(real, imag) * 2.0 / n

    if mag <= 0:
        return None
    return 20.0 * math.log10(mag)


def rms_dbfs(samples):
    if not samples:
        return None
    s = math.sqrt(sum(x * x for x in samples) / len(samples))
    if s <= 0:
        return None
    return 20.0 * math.log10(s)


def trim_edges(samples, fs, head_ms=300.0, tail_ms=100.0):
    """掐掉录音首尾，返回 (裁剪后样本, 掐掉的头尾字节数说明)。

    ⚠️ 为什么必须裁：
      WSA883x 的 PA 在 GLOBAL_PA_EN 拉高瞬间会「啪」一声（爆音），
      这段瞬态是宽带的、幅度可以直接打到满刻度。不裁掉的话：
        - 峰值恒为 -0.0 dBFS → 削顶检查永远误报
        - RMS 被瞬态能量拖高 → 各配置之间的差值被压缩
      440 Hz 的 Goertzel 几乎不受影响（瞬态能量不在该频点），
      但 RMS 和峰值必须裁。

      wsa883x.c 里 mute_unmute_on_trigger=true 时，digital_mute 会操作
      GLOBAL_PA_EN，所以每次起流都会带这一下 pop。
    """
    a = int(fs * head_ms / 1000.0)
    b = len(samples) - int(fs * tail_ms / 1000.0)
    if b - a < int(fs * 0.05):      # 文件太短，裁了就没了 → 不裁
        return samples, False
    return samples[a:b], True


def diagnose(fs, mono, path):
    """给出 n/a 的具体原因。返回 (结论串, 建议串)。"""
    n = len(mono)
    if n == 0:
        return ("零帧（只有 wav 头）",
                "tinycap 没录到任何数据。多半是内置麦 DAPM 路径没通，或 PCM 号不对。")

    peak = max(abs(x) for x in mono)
    if peak == 0:
        return ("全零样本（数字静音）",
                "录到了但全是 0。检查麦克风前端："
                "MultiMedia4 Mixer VA_CODEC_DMA_TX_0 是否为 1、"
                "VA DEC0/DEC1 MUX 是否为 VA_DMIC。")

    if peak < 1.0 / FULL_SCALE:      # 连 1 LSB 都不到
        return ("幅度低于 1 LSB（%.6f）" % (peak * FULL_SCALE),
                "麦克风增益为 0 或被静音。检查 VA_DEC0/DEC1 Volume。")

    if n < fs * 0.2:
        return ("时长过短（%.3f s）" % (n / fs),
                "录制窗口太小。检查脚本里 tinycap 的收尾方式（应靠 SIGINT 收尾）。")

    return ("幅度 %.5f FS 但 RMS≈0" % peak,
            "可能是直流或极窄脉冲。用 --freq 换频点试试。")


def peak_dbfs(samples):
    if not samples:
        return None
    p = max(abs(x) for x in samples)
    if p <= 0:
        return None
    return 20.0 * math.log10(p)


def analyze_dir(d, freq, head_ms=300.0, tail_ms=100.0):
    wavs = sorted(f for f in os.listdir(d) if f.lower().endswith(".wav"))
    if not wavs:
        print("[ERR] %s 里没有 .wav 文件" % d)
        return 1

    rows = []
    bad = []
    popped = []
    for f in wavs:
        p = os.path.join(d, f)
        try:
            fs, mono = read_wav(p)
        except Exception as e:
            print("  !! %s: %s" % (f, e))
            bad.append((f, "解析失败: %s" % e, ""))
            continue

        raw_peak = peak_dbfs(mono)
        cut, trimmed = trim_edges(mono, fs, head_ms, tail_ms)
        g = goertzel_dbfs(cut, fs, freq)
        r = rms_dbfs(cut)
        pk = peak_dbfs(cut)

        # 原始峰值远高于裁剪后峰值 → 首尾有瞬态（PA 上电爆音），不是削顶
        if (trimmed and raw_peak is not None and pk is not None
                and raw_peak - pk > 6.0):
            popped.append((f, raw_peak, pk))

        rows.append((f, g, r, pk, fs, len(mono) / fs))
        # 只要 Goertzel 或 RMS 拿不到值，就诊断
        if g is None or r is None:
            why, fix = diagnose(fs, cut, p)
            bad.append((f, why, fix))

    if not rows and not bad:
        print("[ERR] 没有可分析的 wav")
        return 1

    if bad:
        print("")
        print("==================================================================")
        print(" ⚠️  有 %d/%d 个文件量不出电平，录音环节有问题" % (len(bad), len(wavs)))
        print("==================================================================")
        groups = {}
        for f, why, fix in bad:
            groups.setdefault((why, fix), []).append(f)
        for (why, fix), files in groups.items():
            print("")
            print("  原因: %s" % why)
            print("  文件: %s" % ", ".join(files))
            if fix:
                print("  建议: %s" % fix)
        print("")
        print(" 排查顺序：")
        print("   1) adb shell su -c /data/local/tmp/audio-loudness-sweep.sh --preflight")
        print("   2) 看 _preflight-room.wav（安静房间应约 -30 dBFS）")
        print("   3) room 都量不出来 → 内置麦路径没通，A/B 数据全部无效")
        print("")

    good = [r for r in rows if r[1] is not None and r[2] is not None]
    if not good:
        print("[ERR] 没有任何文件量得出电平 —— 录音环节没通，别拿这批数据做判断。")
        print("")
        return 2

    base = None
    for f, g, r, pk, fs, dur in rows:
        if f.lower().startswith("baseline"):
            base = g
            break

    print("")
    print("==================================================================")
    print(" 响度对比（内置麦回采，Goertzel @ %.0f Hz）" % freq)
    print("==================================================================")
    print(" %-18s %10s %9s %9s %10s" % ("配置", "440Hz dBFS", "RMS", "峰值", "vs 基线"))
    print(" " + "-" * 62)

    for f, g, r, pk, fs, dur in good:
        name = f[:-4] if f.lower().endswith(".wav") else f
        if base is not None:
            line = " %-18s %10.1f %9.1f %9.1f %+10.1f" % (name, g, r, pk, g - base)
        else:
            line = " %-18s %10.1f %9.1f %9.1f %10s" % (name, g, r, pk, "-")
        print(line)

    print(" " + "-" * 62)
    print(" %-18s %10s %9s %9s %10s" % ("", "dBFS", "", "", "dB"))

    # 排序：按 440 Hz 电平从高到低
    ranked = sorted(good, key=lambda x: -x[1])
    if len(ranked) > 1:
        print("")
        print(" 排序（由响到轻）:")
        for i, (f, g, r, pk, fs, dur) in enumerate(ranked, 1):
            name = f[:-4] if f.lower().endswith(".wav") else f
            print("   %d. %-18s %6.1f dBFS" % (i, name, g))

    # 削顶检查（用裁剪后的峰值，避免 PA 爆音误报）
    clipped = [f for f, g, r, pk, fs, dur in rows if pk is not None and pk > -0.5]
    if clipped:
        print("")
        print(" [!] 峰值 > -0.5 dBFS，可能已削顶（结果不可信，请降低 --amp 重测）:")
        for f in clipped:
            print("     - %s" % f)

    # PA 上电爆音提示（说明为什么原始峰值是 -0.0 dBFS）
    if popped:
        print("")
        print(" [i] 检测到起流爆音（已自动掐掉首尾，以下为裁剪前后峰值）：")
        print("     %-24s %10s %10s" % ("文件", "原始峰值", "裁剪后峰值"))
        for f, rp, pk in popped:
            print("     %-24s %10.1f %10.1f" % (f, rp, pk))
        print("     这是 WSA883x 的 GLOBAL_PA_EN 上电瞬态，属正常现象，")
        print("     不是削顶，也不影响 440 Hz 的 Goertzel 读数。")

    print("")
    print(" 注：Δ 是【麦克风回采到的声压差】，不是电增益。")
    print("     WSA883x 的 COMP/DRE 会压缩峰值，实测增益通常只有请求值的 60~70%。")
    print("     （patches/kernel/audio/0001 实测：请求 +9 dB，实得 +5.7 dB）")
    print("")
    return 0


# --------------------------------------------------------------------------
# 自检
# --------------------------------------------------------------------------
def selftest():
    """不依赖设备：造一个已知电平的音，再量回来，验证分析链路精度。"""
    import tempfile
    print("== 自检 ==")
    d = tempfile.mkdtemp(prefix="goertzel-selftest-")
    p = os.path.join(d, "selftest.wav")

    want_db = -6.0
    make_tone(p, seconds=1.0, amp=10 ** (want_db / 20.0), next_step=False)
    print("")

    fs, mono = read_wav(p)
    g = goertzel_dbfs(mono, fs)
    r = rms_dbfs(mono)
    print("     期望 440Hz 电平 : %.1f dBFS" % want_db)
    print("     实测 440Hz 电平 : %.1f dBFS   (误差 %+.2f dB)" % (g, g - want_db))
    print("     实测 RMS        : %.1f dBFS   (正弦理论值 %.1f)" % (r, want_db - 3.01))

    ok = abs(g - want_db) < 0.3 and abs(r - (want_db - 3.01)) < 0.3
    print("")
    print("     ==> %s" % ("PASS：分析链路精度正常" if ok else "FAIL：请检查脚本"))

    import shutil
    shutil.rmtree(d, ignore_errors=True)
    return 0 if ok else 1


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="扬声器响度 A/B 分析（Goertzel @ 440 Hz）")
    ap.add_argument("dir", nargs="?",
                    help="含录回 wav 的目录（loudness-sweep 拉取结果）")
    ap.add_argument("--make-tone", metavar="PATH",
                    help="生成测试音 wav 而不是分析")
    ap.add_argument("--selftest", action="store_true",
                    help="自检：不需要设备，验证分析精度")
    ap.add_argument("--freq", type=float, default=TONE_HZ,
                    help="分析/生成频点，默认 440 Hz")
    ap.add_argument("--seconds", type=float, default=3.0,
                    help="测试音时长，默认 3 s")
    ap.add_argument("--amp", type=float, default=0.5,
                    help="测试音峰值幅度 0..1，默认 0.5（-6 dBFS）")
    ap.add_argument("--ascii", action="store_true",
                    help="老版 conhost 中文乱码时用，强制 ASCII 输出")
    ap.add_argument("--head-ms", type=float, default=300.0,
                    help="掐掉录音开头多少毫秒再统计（默认 300，跳过 PA 上电爆音）")
    ap.add_argument("--tail-ms", type=float, default=100.0,
                    help="掐掉录音结尾多少毫秒再统计（默认 100）")
    args = ap.parse_args()

    _setup_stdio(ascii_only=args.ascii)

    if args.selftest:
        return selftest()

    if args.make_tone:
        make_tone(args.make_tone, seconds=args.seconds,
                  freq=args.freq, amp=args.amp)
        return 0

    if not args.dir:
        ap.error("需要给出 wav 目录，或用 --make-tone 生成测试音 / --selftest 自检")

    if not os.path.isdir(args.dir):
        print("[ERR] 不是目录: %s" % args.dir)
        return 1

    return analyze_dir(args.dir, args.freq, args.head_ms, args.tail_ms)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
