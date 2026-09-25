#!/usr/bin/env python3
"""从华为的 sws_config.xml 生成 histen_scenes.h（扬声器场景参数表）。

用法：
    python3 gen_histen_scenes.py <sws_config.xml> [输出路径]

`sws_config.xml` 是设备的音频调音配置（内含全部 SWS_SPK_* 场景的 p3 / ext 参数块）。
它不在版本库里，需要从你自己的设备或厂商固件包中取得。换固件版本后重跑本脚本，
并把生成的 histen_scenes.h 一起提交。

输出刻意保持**确定性**（同样的输入 ⇒ 逐字节相同的输出），这样"仓库里的表
是否真的由这份配置生成"可以用 diff 验证，而不是只能信任作者。

★ 少一个场景就报错退出，不要静默降级：场景数是编译期常量
  （HISTEN_SCENE_COUNT），少一个会让索引整体错位，运行时选中错误的调音参数。
"""
import re
import sys

SCENES = [
    "SWS_SPK_LANDSCAPE_ONE",
    "SWS_SPK_LANDSCAPE_ONE_MOVIE",
    "SWS_SPK_LANDSCAPE_ONE_GAME",
    "SWS_SPK_LANDSCAPE_TWO",
    "SWS_SPK_LANDSCAPE_TWO_MOVIE",
    "SWS_SPK_LANDSCAPE_TWO_GAME",
    "SWS_SPK_LANDSCAPE_THREE",
    "SWS_SPK_LANDSCAPE_THREE_MOVIE",
    "SWS_SPK_LANDSCAPE_THREE_GAME",
    "SWS_SPK_PORTRAIT_ONE",
    "SWS_SPK_PORTRAIT_ONE_MOVIE",
    "SWS_SPK_PORTRAIT_ONE_GAME",
    "SWS_SPK_PORTRAIT_TWO",
    "SWS_SPK_PORTRAIT_TWO_MOVIE",
    "SWS_SPK_PORTRAIT_TWO_GAME",
]

P3_LEN = 255
EXT_LEN = 16


def extract(xml, name):
    m = re.search(r"<" + name + r'\s+PARAM_VALUE="\{([^}]*)\}"', xml)
    if not m:
        return None
    return [int(v.strip(), 16) for v in m.group(1).split(",")]


def read_text(path):
    """读配置文件；失败时给一句人能看懂的说明，而不是抛 traceback。

    最常见的两种失败是路径写错和把 XML 文件放错了目录，直接回显路径
    比 Python 的 FileNotFoundError 更有用。
    """
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "ignore")
    except OSError as e:
        print("✗ 读不到 %s：%s" % (path, e.strerror or e))
        print("  （sws_config.xml 不在版本库里，需自行从设备或厂商固件包取得）")
        return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else "histen_scenes.h"

    xml = read_text(src)
    if xml is None:
        return 1

    scenes = []
    for name in SCENES:
        p3 = extract(xml, name)
        ext = extract(xml, name + "_EXT")
        if not p3 or not ext:
            print("✗ %s: 配置里没有这个场景 —— 拒绝生成（场景数会错位）" % name)
            return 1
        if len(p3) != P3_LEN or len(ext) != EXT_LEN:
            print("✗ %s: 长度不对 (p3=%d 应为 %d, ext=%d 应为 %d)"
                  % (name, len(p3), P3_LEN, len(ext), EXT_LEN))
            return 1
        scenes.append((name, p3, ext))

    with open(dst, "w", newline="\n") as f:
        f.write("/* 从 sws_config.xml 自动生成 —— 不要手改。\n")
        f.write(" * 重新生成：python3 gen_histen_scenes.py <sws_config.xml> histen_scenes.h */\n")
        f.write("#pragma once\n")
        f.write("typedef struct { const char *name; const unsigned short *p3; "
                "const unsigned short *ext; } HistenScene;\n\n")
        for name, p3, ext in scenes:
            cname = name.replace("SWS_", "").lower()
            f.write("static const unsigned short %s_p3[%d] = {\n" % (cname, P3_LEN))
            for i in range(0, P3_LEN, 8):
                f.write("    " + ", ".join("0x%x" % v for v in p3[i:i + 8]) + ",\n")
            f.write("};\n")
            f.write("static const unsigned short %s_ext[%d] = { %s };\n\n"
                    % (cname, EXT_LEN, ", ".join("0x%x" % v for v in ext)))
        f.write("static const HistenScene histen_scenes[] = {\n")
        for name, p3, ext in scenes:
            cname = name.replace("SWS_", "").lower()
            f.write('    { "%s", %s_p3, %s_ext },\n' % (name, cname, cname))
        f.write("};\n")
        f.write("#define HISTEN_SCENE_COUNT %d\n" % len(scenes))

    print("✓ %s：%d 个场景 → %s" % (src, len(scenes), dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
