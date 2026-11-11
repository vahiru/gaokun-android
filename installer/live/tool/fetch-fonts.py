#!/usr/bin/env python3
# 取安装器界面打包进应用的两个字体（Material Design 3：拉丁用 Roboto，中文用 Noto Sans CJK SC）。
#
#   python3 tool/fetch-fonts.py              # 从连着的设备（adb）取 Android 系统自带的那两份
#   python3 tool/fetch-fonts.py --from DIR   # 已经取下来的 NotoSansCJK-Regular.ttc / Roboto-Regular.ttf
#   → assets/fonts/NotoSansSC-VF.otf、assets/fonts/Roboto-VF.ttf（不入库，见 .gitignore；许可证见 assets/fonts/README.md）
#
# ★ 为什么打包进应用、不再靠系统字体回退：2026-09-25 真机上中文全是方块 —— fontconfig 查得到
#   文泉驿，Flutter 却没回退过去。字体跟着应用走，Mac 上的离线出图、测试、设备三处渲染就是同一份。
# ★ 为什么从 Android 取：本仓的 ROM 里本来就带着这两份（开源：Noto CJK 是 SIL OFL 1.1，Roboto 是 Apache 2.0），
#   版本由我们自己的构建钉住；上游是 github.com/notofonts/noto-cjk 与 github.com/googlefonts/roboto-flex。
#   换网之后 GitHub 只有几十 KB/s（2026-09-25），USB 从设备取 38 MB/s。
# ★ Android 的 NotoSansCJK-Regular.ttc 是 5 个字形版本的集合（JP/KR/SC/TC/HK，都是可变字重），Flutter 只读
#   第 0 个 = 日文字形。这里把 SC 那个拆成独立的 OTF（纯标准库：表是共享的，重写表目录、拷表数据即可）。
# ★ 输出的 sha256 钉死：换了来源（ROM 升级换了字体）会在这里报出来，而不是静默换了字形。
import hashlib, os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "fonts")
WANT = {  # 输出文件 → sha256（2026-09-25，取自 v0.6.3 候选版 1790206017 的 /system/fonts）
    "NotoSansSC-VF.otf": "96adc7ed34e633d84e81d9e69e91f12b76ff3ff75949a4c8f1545bf632e27d10",
    "Roboto-VF.ttf": "9e157d5ff70ae52cc8c527b36e89d9b3c69c6c814da79fa6237a0bf0895abc7c",
}

def sfnt_tables(d, off):
    numT = struct.unpack(">H", d[off + 4:off + 6])[0]
    return d[off:off + 4], [struct.unpack(">4sIII", d[off + 12 + 16 * i:off + 28 + 16 * i]) for i in range(numT)]

def name_of(d, tables, nid=4):
    for t, _, o, l in tables:
        if t == b"name":
            _, cnt, so = struct.unpack(">HHH", d[o:o + 6])
            for i in range(cnt):
                pid, _, lid, n, ln, no = struct.unpack(">6H", d[o + 6 + 12 * i:o + 18 + 12 * i])
                if pid == 3 and lid == 0x409 and n == nid:
                    return d[o + so + no:o + so + no + ln].decode("utf-16-be")
    return None

def extract_face(d, want_family):
    tag, _, n = struct.unpack(">4sII", d[:12])
    assert tag == b"ttcf", "不是 TTC"
    for off in struct.unpack(">%dI" % n, d[12:12 + 4 * n]):
        ver, tabs = sfnt_tables(d, off)
        if name_of(d, tabs, 1) == want_family:
            break
    else:
        sys.exit("!! TTC 里没有 %s" % want_family)
    num = len(tabs)
    es = max(k for k in range(16) if (1 << k) <= num)
    head = ver + struct.pack(">HHHH", num, (1 << es) * 16, es, num * 16 - (1 << es) * 16)
    pos = 12 + 16 * num
    recs, blobs = b"", b""
    for t, cs, o, l in sorted(tabs):
        recs += struct.pack(">4sIII", t, cs, pos + len(blobs), l)
        blobs += d[o:o + l] + b"\0" * (-l % 4)
    return head + recs + blobs

def fvar(d):
    _, tabs = sfnt_tables(d, 0)
    for t, _, o, l in tabs:
        if t == b"fvar":
            _, ao, _, ac, asz = struct.unpack(">IHHHH", d[o:o + 12])   # version, axesArrayOffset, reserved, axisCount, axisSize
            return [(d[o + ao + asz * i:o + ao + asz * i + 4].decode(),
                     *(struct.unpack(">i", d[o + ao + asz * i + 4 + 4 * k:o + ao + asz * i + 8 + 4 * k])[0] / 65536 for k in range(3)))
                    for i in range(ac)]
    return []

src = sys.argv[2] if len(sys.argv) > 2 and sys.argv[1] == "--from" else None
tmp = None
if not src:
    tmp = tempfile.mkdtemp(); src = tmp
    for f in ("NotoSansCJK-Regular.ttc", "Roboto-Regular.ttf"):
        subprocess.run(["adb", "pull", "/system/fonts/" + f, os.path.join(tmp, f)], check=True, stdout=subprocess.DEVNULL)
os.makedirs(OUT, exist_ok=True)
outs = {
    "NotoSansSC-VF.otf": extract_face(open(os.path.join(src, "NotoSansCJK-Regular.ttc"), "rb").read(), "Noto Sans CJK SC"),
    "Roboto-VF.ttf": open(os.path.join(src, "Roboto-Regular.ttf"), "rb").read(),
}
bad = False
for name, data in outs.items():
    h = hashlib.sha256(data).hexdigest()
    if WANT[name] and h != WANT[name]:
        print("!! %s 的 sha256 变了：%s（期望 %s）—— 来源换了字体？确认之后改 WANT" % (name, h, WANT[name])); bad = True; continue
    open(os.path.join(OUT, name), "wb").write(data)
    axes = ", ".join("%s %g..%g（默认 %g）" % (a, lo, hi, dflt) for a, lo, dflt, hi in fvar(data))
    print("→ assets/fonts/%s  %d 字节  sha256 %s  %s" % (name, len(data), h, axes))
sys.exit(1 if bad else 0)
