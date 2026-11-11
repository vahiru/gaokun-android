#!/usr/bin/env python3
# 取安装器界面打包进应用的两个字体（Material Design 3：拉丁用 Roboto，中文用 Noto Sans SC，即思源黑体简体中文）。
# 字体在 gk3_common（installer/common）里声明，各个安装器应用共用这一份。
#
#   python3 tool/fetch-fonts.py              # 在 installer/common/ 下；从 google/fonts 的固定提交下载
#   python3 tool/fetch-fonts.py --from DIR   # 已经下好的 NotoSansSC[wght].ttf / Roboto[wdth,wght].ttf
#   → assets/fonts/NotoSansSC-VF.ttf、assets/fonts/Roboto-VF.ttf（相对 installer/common/，与从哪里运行无关；
#     不入库，见 .gitignore；许可证见 assets/fonts/README.md）
#
# ★ 为什么打包进应用、不再靠系统字体回退：2026-09-25 真机上中文全是方块 —— fontconfig 查得到
#   文泉驿，Flutter 却没回退过去。字体跟着应用走，离线出图、测试、设备三处渲染就是同一份。
# ★ 来源：Google Fonts 的发布仓库 github.com/google/fonts，钉在一个提交上；两份都是可变字体，SIL OFL 1.1。
#   原先从设备的 /system/fonts 取（要连着平板，还要从 NotoSansCJK 合集里拆出简中那一份）；界面跑在 LiveCD
#   的 Debian 上，用哪一份字体与 Android 无关，2026-11-11 改为直接下载。
# ★ sha256 钉死：换了提交或文件被改，会在这里报出来，而不是静默换了字形。
import hashlib
import os
import struct
import sys
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "fonts")
COMMIT = "5bb28733cec313b63fbd415b574b5ae35c8aa8b8"   # google/fonts main，2026-11-08
FILES = {  # 输出文件 → (仓库里的路径, sha256)
    "NotoSansSC-VF.ttf": ("ofl/notosanssc/NotoSansSC[wght].ttf",
                          "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da"),
    "Roboto-VF.ttf": ("ofl/roboto/Roboto[wdth,wght].ttf",
                      "d7598e12c5dbef095ff8272cfc55da0250bd07fbdecbac8a530b9b277872a134"),
}


def fvar(d):
    """可变字体的轴：[(标签, 最小, 默认, 最大)]"""
    num = struct.unpack(">H", d[4:6])[0]
    for i in range(num):
        t, _, o, _ = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
        if t == b"fvar":
            _, ao, _, ac, asz = struct.unpack(">IHHHH", d[o:o + 12])   # version, axesArrayOffset, reserved, axisCount, axisSize
            return [(d[o + ao + asz * k:o + ao + asz * k + 4].decode(),
                     *(struct.unpack(">i", d[o + ao + asz * k + 4 + 4 * j:o + ao + asz * k + 8 + 4 * j])[0] / 65536
                       for j in range(3)))
                    for k in range(ac)]
    return []


def fetch(path):
    url = "https://raw.githubusercontent.com/google/fonts/%s/%s" % (COMMIT, urllib.parse.quote(path, safe="/,"))
    with urllib.request.urlopen(url, timeout=600) as r:
        return r.read()


src = sys.argv[2] if len(sys.argv) > 2 and sys.argv[1] == "--from" else None
os.makedirs(OUT, exist_ok=True)
bad = False
for name, (path, want) in FILES.items():
    if src:
        f = os.path.join(src, os.path.basename(path))
        if not os.path.isfile(f):
            print("!! %s not found in %s" % (os.path.basename(path), src))
            bad = True
            continue
        data = open(f, "rb").read()
    else:
        data = fetch(path)
    h = hashlib.sha256(data).hexdigest()
    if h != want:
        print("!! %s: sha256 %s, expected %s - did the source change? Check it, then update FILES" % (name, h, want))
        bad = True
        continue
    open(os.path.join(OUT, name), "wb").write(data)
    axes = ", ".join("%s %g..%g (default %g)" % (a, lo, hi, dflt) for a, lo, dflt, hi in fvar(data))
    print("-> assets/fonts/%s  %d bytes  sha256 %s  %s" % (name, len(data), h, axes))
sys.exit(1 if bad else 0)
