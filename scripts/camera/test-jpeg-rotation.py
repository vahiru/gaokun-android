#!/usr/bin/env python3
"""照片方向链路的离线自检（无依赖，纯 Python 3）。

为什么要写这个
--------------

整条链路里有【三个】互相独立的「90 度」，任何一个方向反了，用户看到的都是
「照片逆时针转了 90 度」这同一种症状 —— 而单看照片分辨不出是哪一个反了：

    ① 设备树 rotation                  逆时针，传感器物理安装角
    ② libcamera properties::Rotation   逆时针，继承 ①
    ③ ANDROID_SENSOR_ORIENTATION       顺时针，= 360 - ②′
       ②′ = ② 扣掉 libcamera 用翻转消掉的角，再从横屏参照系换到竖屏自然方向
    ④ ANDROID_JPEG_ORIENTATION         顺时针，应用按当前设备姿态算出来的
    ⑤ libyuv RotationMode              顺时针，与 ④ 同向

HAL 对 ④ 的合同只有一条，也是本脚本 B 部分唯一断言的东西：
**把传感器读出顺时针旋转 ④ 度**。设备姿态怎么折算成 ④ 是应用的事，HAL 不该
再猜一次 —— 猜错就是双重校正，而且猜错了从照片上一样看不出来。

用法::

    python3 scripts/camera/test-jpeg-rotation.py
    python3 scripts/camera/test-jpeg-rotation.py --verbose

退出码 0 = 全部通过。
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CAM_DIR = os.path.normpath(
    os.path.join(HERE, "..", "..", "device", "huawei", "gaokun3", "camera"))

FAILURES = []
CHECKS = 0


def check(cond, label, detail=""):
    global CHECKS
    CHECKS += 1
    if cond:
        print("  [PASS] %s" % label)
        if detail:
            print("         %s" % detail)
    else:
        print("  [FAIL] %s" % label)
        if detail:
            print("         %s" % detail)
        FAILURES.append(label)
    return cond


def read(name):
    with open(os.path.join(CAM_DIR, name), encoding="utf-8") as f:
        return f.read()


def strip_comments(text):
    """去掉注释 —— 免得断言被注释里的说明文字骗过去。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


# ─────────────────────────── A. 源码级断言 ───────────────────────────


def part_a():
    print("\n=== A. 源码级断言（解析 HAL 的 .cpp）===")

    session = read("Session.cpp")
    device = read("Device.cpp")
    header = read("Session.h")
    code = strip_comments(session)
    dev_code = strip_comments(device)

    # ── A1. ANDROID_JPEG_ORIENTATION 真的被读了 ──
    check("ANDROID_JPEG_ORIENTATION" in code,
          "A1 请求里的 ANDROID_JPEG_ORIENTATION 被消费",
          "老代码一个字都没读它，JPEG 永远停在传感器原始朝向")
    check(re.search(r"requestEntryInt\(\s*pend\.settings\s*,\s*ANDROID_JPEG_ORIENTATION",
                    code) is not None,
          "A1b 取值来源是【本帧的请求设置】，不是静态元数据")
    check("ANDROID_JPEG_QUALITY" in code,
          "A1c ANDROID_JPEG_QUALITY 也读进来了（顺手修掉质量恒 90）")

    # ── A2. libyuv 映射表必须是恒等映射（顺时针 → 顺时针） ──
    mapper = re.search(r"toLibyuvRotation\s*\(int32_t\s+\w+\)(.*?)\n\}", code, re.S)
    if not check(mapper is not None, "A2 找到 toLibyuvRotation() 的定义"):
        return
    body = mapper.group(1)
    table = {int(a): int(b)
             for a, b in re.findall(
                 r"case\s+(\d+)\s*:\s*return\s+libyuv::kRotate(\d+)\s*;", body)}
    check(table == {0: 0, 90: 90, 180: 180, 270: 270},
          "A2b 映射表是恒等的：Android 角 == libyuv 角",
          "实测表 = %s" % sorted(table.items()))
    check("360" not in body,
          "A2c 映射表里没有 360-x 之类的反向换算",
          "libyuv 的 kRotate90 本身就是顺时针，取反会转反 180 度")
    default_branch = re.search(r"default\s*:(.*?)(?=\n\s*case\s+\d+\s*:|\Z)", body, re.S)
    check(default_branch is not None and "kRotate0" in default_branch.group(1),
          "A2d 非法角度（非 0/90/180/270）落到 kRotate0",
          "Android 只允许这四个值，静默接受别的值会得到随机方向")

    # ── A3. 90/270 的宽高互换 ──
    compact = re.sub(r"\s+", "", code)
    check("swapDims?dstH:dstW" in compact and "constint32_toutW" in compact,
          "A3 90/270 时交换输出宽高：outW=dstH, outH=dstW",
          "BLOB 流只声明字节数，JPEG 实际尺寸换了是允许的")

    # ── A4. ARGBRotate 的目标 stride 必须用旋转后的宽度 ──
    rot_call = re.search(r"libyuv::ARGBRotate\((.*?)\)\s*!=\s*0", code, re.S)
    if check(rot_call is not None, "A4 找到 ARGBRotate 的调用"):
        args = [a.strip() for a in re.sub(r"\s+", "", rot_call.group(1)).split(",")]
        check(len(args) == 7 and args[3] == "outW*4",
              "A4b 目标 stride 用【旋转后】的宽度 outW*4",
              "实测第 4 个实参 = %r；沿用 dstW*4 会让每一行错位" % (args[3],))
        check(len(args) == 7 and args[4] == "dstW" and args[5] == "dstH",
              "A4c 源宽高仍是传感器坐标系的 (dstW, dstH)")
        check(len(args) == 7 and args[6] == "rotMode",
              "A4d 旋转模式来自请求解析出的 rotMode")
    else:
        check(False, "A4b/c/d 无法校验（没找到调用）")

    # ── A5. 实参顺序：方向必须真的传进编码函数 ──
    # 第一个实参是【源缓冲】：修前是 rgb，远端加入静态照片降噪后变成 rgbStill。
    # 名字不是合同（缓冲换谁进来都行）—— 合同是后两个实参必须是质量与方向。
    call = re.search(r"deliverJpeg\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,(.*?)\)", code, re.S)
    if check(call is not None, "A5 找到 deliverJpeg 的调用点"):
        args = [a.strip() for a in re.sub(r"\s+", " ", call.group(1)).split(",")]
        check(len(args) == 6 and args[4] == "jpegQuality" and args[5] == "jpegOrientation",
              "A5b 调用点依次传入 (quality, jpegOrientation)",
              "实测实参 = %s" % args)
    sig = re.search(r"bool\s+Session::deliverJpeg\((.*?)\)\s*\{", code, re.S)
    if check(sig is not None, "A6 找到实现签名"):
        sig_args = [a.strip() for a in re.sub(r"\s+", " ", sig.group(1)).split(",")]
        check(len(sig_args) == 7 and sig_args[6].endswith("jpegOrientation"),
              "A6b 实现的形参里有 jpegOrientation",
              "实测 = %s" % sig_args)
    hdr_sig = re.search(r"bool\s+deliverJpeg\((.*?)\)\s*;", header, re.S)
    check(hdr_sig is not None and "jpegOrientation" in hdr_sig.group(1),
          "A6c 头文件里的声明同步了")

    # ── A7. Device.cpp 的逆时针 → 顺时针换算 ──
    check(re.search(r"facts_\.orientation\s*=\s*\(360\s*-\s*natural\)\s*%\s*360\s*;",
                    dev_code) is not None,
          "A7 ANDROID_SENSOR_ORIENTATION = (360 - 竖屏自然方向下的逆时针角) % 360",
          "(360 - x) 这一步和上游 camera_device.cpp 相同；x 先按 A7d、A7e 换算")
    check(re.search(r"natural\s*=\s*\(ccw\s*-\s*undone\s*\+\s*\(facts_\.frontFacing\s*\?\s*90\s*:\s*270\)",
                    dev_code) is not None,
          "A7d 横屏 → 竖屏：前摄加 90、后摄加 270，并扣掉 libcamera 翻转消掉的角",
          "设备树按横屏使用的方向写，Android 的自然方向是竖屏（#23、#42）")
    check(re.search(r"probe->validate\(\)\s*;\s*if\s*\(\s*probe->orientation\s*==\s*"
                    r"libcamera::Orientation::Rotate0\s*\)\s*undone\s*=\s*ccw\s*;",
                    dev_code) is not None,
          "A7e libcamera 用翻转凑出了 Rotate0，才把设备树的角度算作已消掉",
          "C 部分按这条规则建模；不 validate() 的话 orientation 永远是默认的 Rotate0")
    check("__system_property_get" not in dev_code
          and "debug.gaokun3.camera.orientation" not in dev_code,
          "A7f 不再读调试属性 debug.gaokun3.camera.orientation.*",
          "按旧换算设的值（后 270、前 90）会把新换算带歪")
    check(re.search(r"facts_\.orientation\s*=\s*rot\s*\?\s*\*\s*rot\s*:\s*0\s*;",
                    dev_code) is None,
          "A7b 老的直接抄数值（rot ? *rot : 0）已经删掉",
          "那个写法在 90/270 上差 180 度")
    check(re.search(r"ccw\s*%\s*90\s*!=\s*0", dev_code) is not None,
          "A7c 非 90 倍数的安装角被拦下并告警",
          "Android 只认 0/90/180/270")


# ─────────────────────── B. 行为模型（顺时针定义） ───────────────────────


def rotate_cw(img, deg):
    """按【顺时针】旋转 deg 度（deg ∈ {0,90,180,270}）。

    这是「顺时针」的数学定义，不依赖任何第三方库的命名：
        90 度：  out[i][j] = in[H-1-j][i]
        180 度： out[i][j] = in[H-1-i][W-1-j]
        270 度： out[i][j] = in[j][W-1-i]
    """
    deg %= 360
    if deg == 0:
        return [row[:] for row in img]
    h, w = len(img), len(img[0])
    if deg == 90:
        return [[img[h - 1 - j][i] for j in range(h)] for i in range(w)]
    if deg == 180:
        return [[img[h - 1 - i][w - 1 - j] for j in range(w)] for i in range(h)]
    if deg == 270:
        return [[img[j][w - 1 - i] for j in range(h)] for i in range(w)]
    raise ValueError(deg)


def libyuv_rotate(img, mode_deg):
    """按 libyuv 的【口头契约】旋转。

    include/libyuv/rotate.h 原文::

        kRotate90  = 90,   // Rotate 90 degrees clockwise.
        kRotate270 = 270,  // Rotate 270 degrees clockwise.
        kRotateClockwise = 90, kRotateCounterClockwise = 270,   // 历史别名

    所以就是 rotate_cw。上游哪天改了方向，这里和 A2 的断言会一起红掉。
    """
    return rotate_cw(img, mode_deg)


def part_b(verbose=False):
    print("\n=== B. 行为模型：场景 → 传感器读出 → HAL 旋转 → JPEG ===")
    print("""
    三个量的关系（很容易混，这里一次说清）：
      · dt_rotation  竖屏自然方向下、扣掉 libcamera 翻转之后的安装角（Device.cpp 的
                     natural），【逆时针】，= 把画面转正所需的逆时针角；
                     它【在数值上】恰好等于"照片看起来顺时针歪了多少"
                     （设备树的值怎么换到这里见 C 部分）
      · apparent_cw  照片看起来顺时针歪了多少      = dt_rotation
      · sensor_orient ANDROID_SENSOR_ORIENTATION，【顺时针】校正角
                     = (360 - apparent_cw) % 360  = (360 - dt_rotation) % 360
    校正角与"歪了多少"是【互补】关系 —— 这正是最容易写反的地方。
""")

    # 一张非对称的 3x4 场景图，每格一个唯一标签。
    # 非对称是关键：对称图案会让「转反 180 度」也蒙混过关。
    upright = [[10 * (r + 1) + (c + 1) for c in range(4)] for r in range(3)]
    if verbose:
        print("    正立场景(3 行 x 4 列):")
        for row in upright:
            print("      %s" % row)

    print("\n  -- B1/B2 设备自然朝向下：安装角 → 元数据 → 照片 --")
    for dt_rotation in (0, 90, 180, 270):
        # 传感器读出：没做任何校正时，照片看起来顺时针歪了 dt_rotation 度。
        apparent_cw = dt_rotation
        raw = rotate_cw(upright, apparent_cw)

        # ① HAL 把逆时针的安装角换算成顺时针的校正角（这才是 Android 要的）
        sensor_orientation = (360 - dt_rotation) % 360
        check(sensor_orientation == (360 - apparent_cw) % 360,
              "B1 安装角=%3d 度(逆时针) -> 上报 SENSOR_ORIENTATION=%3d 度(顺时针)"
              % (dt_rotation, sensor_orientation))

        # ② 设备自然朝向下，应用算出的 JPEG 旋转角就等于 SENSOR_ORIENTATION
        requested = sensor_orientation
        # ③ HAL 按 libyuv 顺时针语义旋转（A2 已断言映射恒等）
        out = libyuv_rotate(raw, requested)

        check(out == upright,
              "B2 安装角=%3d 度: 修复后 JPEG == 正立场景" % dt_rotation)

        # 回归：老代码完全不动像素
        if dt_rotation == 0:
            check(raw == upright, "B3 安装角=  0 度: 不旋转本来就对（老代码无辜）")
        else:
            check(raw != upright,
                  "B3 安装角=%3d 度: 修复前 JPEG != 正立场景（用户看到的歪）"
                  % dt_rotation)
        if dt_rotation == 270:
            # 需要补 90 度顺时针而一点没补 ⇒ 看起来是 270 度顺时针
            # = 90 度逆时针，逐字对上用户的描述。
            check(raw == rotate_cw(upright, 270),
                  "B3b 安装角=270 度: 修复前看起来顺时针歪 270 度",
                  "270 度顺时针 == 90 度逆时针，这就是用户报的症状")

        # 宽高：校正角为 90/270 时必须换过来
        h, w = len(raw), len(raw[0])
        swapped = sensor_orientation in (90, 270)
        dst_w, dst_h = w, h                      # 流声明 = 传感器坐标系尺寸
        out_w = dst_h if swapped else dst_w      # HAL 的 outW
        out_h = dst_w if swapped else dst_h      # HAL 的 outH
        expect = (h, w) if swapped else (w, h)
        check((out_w, out_h) == expect,
              "B4 安装角=%3d 度: 输出 %dx%d（流声明 %dx%d）"
              % (dt_rotation, out_w, out_h, dst_w, dst_h))

    # ── B5. HAL 对请求角的合同：忠实按【顺时针】转，不自己再猜设备姿态 ──
    print("\n  -- B5 HAL 的合同：JPEG == 传感器读出顺时针转 请求角 --")
    for dt_rotation in (0, 90, 180, 270):
        raw = rotate_cw(upright, dt_rotation)
        correct_angle = (360 - dt_rotation) % 360
        for requested in (0, 90, 180, 270):
            out = libyuv_rotate(raw, requested)
            faithful = out == rotate_cw(raw, requested)
            # 只有请求角正好是校正角时才该正立；任何别的组合都必须【不正立】，
            # 否则说明发生了双重校正。
            should_be_upright = (requested == correct_angle)
            check(faithful and (out == upright) == should_be_upright,
                  "B5 安装角=%3d 请求=%3d: 忠实旋转，且仅角度正确时正立"
                  % (dt_rotation, requested),
                  "" if should_be_upright
                  else "此时本就不该正立（HAL 不替应用猜设备姿态）")

    # ── B6. 一次旋转 != 两次旋转（防止"转像素 + 又写 EXIF 角度"的经典错） ──
    print("\n  -- B6 不得双重校正 --")
    for dt_rotation in (90, 270):
        raw = rotate_cw(upright, dt_rotation)
        angle = (360 - dt_rotation) % 360
        once = libyuv_rotate(raw, angle)
        twice = rotate_cw(once, angle)
        check(once == upright and twice != upright,
              "B6 安装角=%3d: 只转一次正好，转两次就歪" % dt_rotation,
              "所以 EXIF 里不能再写这个角，否则看图器会再转一遍")


# ─────────────── C. 设备树 → SENSOR_ORIENTATION（按本机已确认的事实建模） ───────────────


def undone_by_flips(dt, can_flip):
    """libcamera 用传感器翻转消掉的角（computeTransform，会话要 Rotate0）。

    0 不用消；180 = 水平 + 垂直翻转，传感器两个方向都能翻才消得掉；
    90 / 270 要转置，翻转做不到，libcamera 就不翻转。
    """
    return dt if dt == 0 or (dt == 180 and can_flip) else 0


def hal_orientation(dt, front, undone):
    """Device.cpp 的式子：横屏的设备树角 → 竖屏自然方向 → 顺时针。"""
    natural = (dt - undone + (90 if front else 270) + 360) % 360
    return (360 - natural) % 360


def shown_cw(undone, sensor_orientation):
    """原始读出最终被顺时针转了多少度才显示（翻转 = 转 180 度）。"""
    return (undone + sensor_orientation) % 360


def part_c():
    print("\n=== C. 设备树 → SENSOR_ORIENTATION（按本机已确认的事实建模） ===")
    # 真机上两颗摄像头都要把原始读出顺时针转 270 度才是正的：
    # 设调试属性后 270 / 前 90 的旧 HAL 上报后 90 / 前 270，后摄还被翻转，
    # 取景和照片都是正的（#23、#42）。
    truth = 270
    rear = dict(dt=180, front=False, can_flip=True)     # ov13b10
    front = dict(dt=0, front=True, can_flip=False)      # hi846

    for name, cam, want in (("后摄", rear, 90), ("前摄", front, 270)):
        u = undone_by_flips(cam["dt"], cam["can_flip"])
        so = hal_orientation(cam["dt"], cam["front"], u)
        check(so == want, "C1 %s 设备树 %d -> SENSOR_ORIENTATION %d" % (name, cam["dt"], so),
              "应为 %d" % want)
        check(shown_cw(u, so) == truth, "C2 %s 显示出来是正的" % name)
        # 旧 HAL 直接 360 - 设备树：差 90 度（#23）
        old = (360 - cam["dt"]) % 360
        check(shown_cw(u, old) != truth, "C3 %s 旧式子 %d 是歪的" % (name, old))

    # 推算（没在真机上见过）：设备树后摄写 270（0049 的前身）就不再翻转，
    # 旧式子上报 90，画面上下颠倒；后摄要和现在等价，设备树得写 90。
    u = undone_by_flips(270, True)
    check(u == 0 and (shown_cw(u, (360 - 270) % 360) - truth) % 360 == 180,
          "C4 推算：设备树后摄 270 + 旧式子 = 上下颠倒")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true", help="打印场景矩阵")
    args = ap.parse_args()

    print("照片方向链路自检 —— 目录 %s" % CAM_DIR)
    part_a()
    part_b(args.verbose)
    part_c()

    print("\n=== 汇总 ===")
    print("  检查项 %d，失败 %d" % (CHECKS, len(FAILURES)))
    for f in FAILURES:
        print("  [X] %s" % f)
    if FAILURES:
        return 1
    print("  全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
