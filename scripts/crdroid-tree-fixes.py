#!/usr/bin/env python3
"""对 crDroid 源码树的本地修补（幂等，可反复跑）。

在构建机上执行：
    python3 <repo>/scripts/crdroid-tree-fixes.py ~/crdroid

—— 修补 1：关掉 SPOOF_SAFETYNET ——

crDroid 在 system/core/init/property_service.cpp 里加了 SetSafetyNetProps()，
在【解析 kernel cmdline 之前】硬写一整张属性表来伪装"已锁定、已验证、user 版"，
好让 Play Integrity 通过。源码注释原话：

    // Report a valid verified boot chain to make Google SafetyNet integrity
    // checks pass. This needs to be done before parsing the kernel cmdline as
    // these properties are read-only and will be set to invalid values with
    // androidboot cmdline arguments.

被它强制的值（2026-08-19 实机 getprop 逐条确认）：
    ro.boot.verifiedbootstate = green      （cmdline 写的是 orange）
    ro.boot.flash.locked      = 1          （cmdline 写的是 0）
    ro.boot.veritymode        = enforcing  （cmdline 写的是 disabled）
    ro.debuggable = 0    ro.adb.secure = 1    ro.secure = 1
    ro.build.type = user   ro.build.tags = release-keys
    ro.crypto.state = encrypted            ro.secureboot.lockstate = locked

对本项目这是致命的：
  * ro.debuggable=0            → adb root / adb remount 全部不可用，
                                 而 M3 部署 turnip 完全依赖 overlayfs remount
  * verifiedbootstate != orange → adb remount 的前提不成立（Stage 5 的运维基础）
  * ro.adb.secure=1            → adb 要授权（可用 PRODUCT_ADB_KEYS 绕开，但治标）
  * 这些值把 WITH_ADB_INSECURE、PRODUCT_SYSTEM_EXT_PROPERTIES、cmdline
    统统盖掉 —— 排查时极具迷惑性，因为产物里的 build.prop 明明是对的。

上游只在 eng 变体里关它（Android.bp 的 product_variables.eng），
但 eng 会关掉 dexpreopt，首次开机全靠 JIT —— 本机跑 swangle 软渲染，
慢到不可接受。所以直接把默认值改成 0。

我们本来就不追求 Play Integrity（这是台开发机），关掉没有副作用。
"""
import io, re, sys, pathlib

def patch_spoof_safetynet(tree: pathlib.Path) -> str:
    p = tree / "system/core/init/Android.bp"
    if not p.exists():
        return f"跳过（找不到 {p}）"
    s = io.open(p, encoding="utf-8").read()
    n = s.count('"-DSPOOF_SAFETYNET=1"')
    if n == 0:
        return "已是 0（幂等，无需改动）" if '"-DSPOOF_SAFETYNET=0"' in s else "⚠️ 找不到 SPOOF_SAFETYNET，上游可能改了写法"
    s = s.replace('"-DSPOOF_SAFETYNET=1"', '"-DSPOOF_SAFETYNET=0"')
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    return f"已把 {n} 处 -DSPOOF_SAFETYNET=1 改为 =0"

def patch_hexagonfs_cr(tree: pathlib.Path) -> str:
    """—— 修补 2：给 hexagonfs 的路径解析加 CR 截断 ——

    SLPI 的 DSP 固件是在 Windows 上编译的，它通过 FastRPC 请求文件时，
    路径的【每一段都带一个尾随 CR】。hexagonrpcd 上游没有处理这件事，
    于是 DSP 读不到传感器注册表 —— 症状是传感器一个都出不来。

    ★ 补丁位置很关键：打在 copy_segment_and_advance() 里，那是【通用】分段
      解析函数，清理后的 segment 才分派给各后端（hexagonfs.c 的 openat 循环）。
      所以物理目录后端同样受益 —— 这就是为什么【不需要】贡献者指南里那 6 个
      带 CR 的 socinfo symlink（本仓移走它们后加速度计照样正常，实测确认）。
      而这一条正是 Android 侧能用普通 PRODUCT_COPY_FILES 的前提：
      构建系统造不出带控制字符的文件名。

    完整背景见 docs/stage4-findings.md #37。
    """
    p = tree / "external/hexagonrpc/hexagonrpcd/hexagonfs.c"
    if not p.exists():
        return f"跳过（找不到 {p} —— local manifest 同步过了吗）"
    s = io.open(p, encoding="utf-8").read()
    if "segment[--segment_len] = 0;" in s:
        return "已打过（幂等，无需改动）"
    anchor = "segment[segment_len] = 0;"
    if anchor not in s:
        return "⚠️ 找不到锚点，上游可能改了 copy_segment_and_advance()"
    # ⚠️ 生成 C 代码时【一律用 chr()】，不写反斜杠转义：这段代码本身经过多层
    #    引号传递，\n / \r 之类会被中间层 collapse 掉（本仓踩过两次）。
    NL, TAB, BS = chr(10), chr(9), chr(92)
    patch = (NL + TAB + "/* DSP 固件在 Windows 上编译，请求的路径每段都带尾随 CR。" + NL
             + TAB + " * 这里是通用分段解析，清理后才分派给各后端，物理目录后端同样受益。 */" + NL
             + TAB + "if (segment_len > 0 && segment[segment_len - 1] == " + chr(39) + BS + "r" + chr(39) + ")" + NL
             + TAB + TAB + "segment[--segment_len] = 0;")
    s = s.replace(anchor, anchor + patch, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    return "已加上 CR 截断"


def patch_v4l2_input_size(tree: pathlib.Path) -> str:
    """—— 修补 3：v4l2_codec2 给压缩输入队列传了非法分辨率 ——

    症状：任何视频用 c2.v4l2.*.decoder 都 "start 失败"，内核打
    `qcom-venus: HW can't support this load`。

    ★ 根因链（内核插桩实测，见 docs/stage4-findings.md）：
      1. V4L2Decoder::setupInputFormat() 对【压缩输入队列】做 S_FMT 时传
         `ui::Size()`，而 ui::Size 的默认值是 **-1 x -1**
         （frameworks/native/libs/ui/include/ui/Size.h:32-33），
         不是想当然的 0 x 0。
      2. buildV4L2Format() 把它赋给 __u32 -> 0xFFFFFFFF。
      3. venus 的 vdec_try_fmt_common() clamp 到 [128, 8192] -> **8192**，
         而 vdec_s_fmt() 就用这个值填 inst->width/height。
      4. 同一个函数末尾立刻 streamon -> decide_core() 按 8192x8192@30fps
         算出 1.573 GHz 负载 > max_freq 1.332 GHz -> -EINVAL。
      实测插桩输出：`8192x8192 fps=30 ... inst=1572864000 max_freq=1332000000`，
      而视频其实只有 640x360。

    ★ 这是 v4l2_codec2 的**可移植性 bug**：ChromeOS 的驱动不看压缩队列的
      分辨率，所以上游一直没暴露；venus 看，于是一撞就死。

    修法：传设备自报的最小分辨率 —— 与同文件 setupMinimalOutputFormat()
    对输出队列的做法完全一致。真实分辨率随后由 source-change 事件带来。
    """
    p = tree / "external/v4l2_codec2/v4l2/V4L2Decoder.cpp"
    if not p.exists():
        return f"跳过（找不到 {p}）"
    s = io.open(p, encoding="utf-8").read()
    if "inputMinRes" in s:
        return "已打过（幂等，无需改动）"
    anchor = ("    auto format = mInputQueue->setFormat(inputPixelFormat, ui::Size(), "
              "inputBufferSize, 0);")
    if anchor not in s:
        return "⚠️ 找不到锚点，上游可能改了 setupInputFormat()"
    NL = chr(10)
    new = (
        "    // venus 会拿【压缩输入队列】S_FMT 的分辨率去算硬件负载，而 ui::Size()" + NL
        + "    // 默认是 -1 x -1，转成 __u32 就是 0xFFFFFFFF，被 clamp 到 8192x8192，" + NL
        + "    // decide_core() 于是一律判 HW overload。传设备自报的最小分辨率。" + NL
        + "    ui::Size inputMinRes, inputMaxRes;" + NL
        + "    mDevice->getSupportedResolution(inputPixelFormat, &inputMinRes, &inputMaxRes);" + NL
        + "    if (inputMinRes.isEmpty()) inputMinRes.set(128, 128);" + NL
        + "    auto format = mInputQueue->setFormat(inputPixelFormat, inputMinRes, "
          "inputBufferSize, 0);"
    )
    s = s.replace(anchor, new, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    return "已改为传最小分辨率（原来传的是 ui::Size() = -1 x -1）"


def patch_gapps_conflicts(tree: pathlib.Path) -> str:
    """—— 修补 4：MindTheGapps 与 crDroid 树的三处冲突 ——

    (a) **Google 的开机向导必须去掉**（`SetupWizard`，arm64-vendor.mk）。
        它在初始化阶段强制连 Google 服务器，**中国大陆网络下会卡在欢迎页
        过不去，设备无法完成初始化**。crDroid 自带的 LineageSetupWizard
        不依赖 Google 服务，保留它。
        ★ 安全性判据：MindTheGapps 的 `GmsSetupWizardOverlay` 目标包是
          **org.lineageos.setupwizard**（不是 Google 那个），内容只有
          dynamic_color / partner_experiment 这类外观 bool ——
          **不会把流程交给 Google 向导**，删掉不会断链。

    (b) **`libjni_latinimegoogle` 与树内 LatinIME 模块名撞车**
        （arm64-vendor.mk + arm64/Android.bp）。两边都是
        `cc_prebuilt_library_shared` 且都写了 `prefer: true`，而 soong 的
        prefer 只解决「prebuilt 覆盖源码」，两个 prebuilt 它管不了。
        MindTheGapps 用了 soong namespace，所以 soong 阶段不报错，到 kati 才炸：
            base_rules.mk:320: error: vendor/gapps/arm64:
            MODULE.TARGET.SHARED_LIBRARIES.libjni_latinimegoogle
            already defined by packages/inputmethods/LatinIME/java
        ⚠️ 这个错误发生在【生成模块定义】时，不是安装时 ——
        **光从 PRODUCT_PACKAGES 里拿掉不够，必须删掉模块本身。**

    (c) **`google.xml` 与 crDroid 的 addons 装到同一路径**
        （common-vendor.mk）。`vendor/addons/config.mk:32` 无条件把自己那份
        拷到 `product/etc/sysconfig/google.xml`，于是：
            Makefile:148: error: overriding commands for target
            `.../product/etc/sysconfig/google.xml'
        ★ 留 crDroid 那份 —— **实测它是超集**：60 条声明 vs GApps 的 39 条，
          GApps 独有的只有 3 行，其中唯一的实条目是
          `<allow-in-power-save package="com.google.ambient.streaming" />`，
          而 MindTheGapps 压根不带那个应用。（是按内容比对定的，不是按文件大小。）
        这个是【安装路径】冲突而非模块名冲突，所以从 PRODUCT_PACKAGES
        里拿掉就够了。

    ⚠️★ **为什么不在设备树里用 `$(filter-out ...)`：那是无效的。**
      实测 `get_build_var PRODUCT_PACKAGES` —— 写了 filter-out 之后
      SetupWizard 仍在 1005 个条目里。现代 AOSP 从**继承图**重新推导
      PRODUCT_PACKAGES，产品 makefile 里的直接赋值会被丢弃。

    ★ 冲突是**一次性全找出来的**，不是一个个撞：36 个 gapps 模块名与全树
      逐个比对（只有 b），13 个 gapps etc 文件名与全树 PRODUCT_COPY_FILES
      逐个比对（只有 c）。

    ⚠️ repo sync 会还原 vendor/gapps，所以每次构建前都要跑本脚本（幂等）。
    """
    NL = chr(10)
    BS = chr(92)
    root = tree / "vendor/gapps"
    if not (root / "arm64/arm64-vendor.mk").exists():
        return "跳过（没同步 vendor/gapps —— 不装 GApps 就不需要这一步）"

    msgs = []

    def drop_from_mk(mk: pathlib.Path, names):
        """从 PRODUCT_PACKAGES 列表里删掉这些名字，并修好续行反斜杠。"""
        if not mk.exists():
            return []
        lines = io.open(mk, encoding="utf-8").read().split(NL)
        gone = []
        for name in names:
            for i, ln in enumerate(lines):
                if ln.strip().rstrip(BS).strip() == name:
                    had_cont = ln.rstrip().endswith(BS)
                    del lines[i]
                    # 删的是列表最后一项时，上一行的续行反斜杠要去掉
                    if not had_cont and i > 0 and lines[i - 1].rstrip().endswith(BS):
                        lines[i - 1] = lines[i - 1].rstrip().rstrip(BS).rstrip()
                    gone.append(name)
                    break
        if gone:
            io.open(mk, "w", encoding="utf-8", newline="").write(NL.join(lines))
        return gone

    g1 = drop_from_mk(root / "arm64/arm64-vendor.mk",
                      ["SetupWizard", "libjni_latinimegoogle"])
    g2 = drop_from_mk(root / "common/common-vendor.mk", ["google.xml"])
    gone = g1 + g2
    msgs.append("从包列表删掉 " + "/".join(gone) if gone else "包列表已是干净的")

    # ★★ 删掉模块定义【本身】——只从 PRODUCT_PACKAGES 里拿掉是不够的。
    #   实测：把 google.xml 从 common-vendor.mk 的包列表里删掉之后，
    #   构建仍然报同一个 "overriding commands for target ... google.xml"，
    #   因为 soong 会为它 namespace 里的模块照样生成安装规则
    #   （错误出自 out/soong/installs-lineage_gaokun3.mk）。
    #   ⇒ 凡是【模块名撞车】或【安装路径撞车】，都必须删模块块。
    def drop_module(bp: pathlib.Path, name: str):
        if not bp.exists():
            return False
        lines = io.open(bp, encoding="utf-8").read().split(NL)
        target = None
        needle = 'name: "%s",' % name
        for i, ln in enumerate(lines):
            if ln.strip() == needle:
                target = i
                break
        if target is None:
            return False
        start = target
        while start > 0 and not lines[start].rstrip().endswith("{"):
            start -= 1
        end = target
        while end < len(lines) and lines[end].rstrip() != "}":
            end += 1
        while end + 1 < len(lines) and lines[end + 1].strip() == "":
            end += 1
        del lines[start:end + 1]
        io.open(bp, "w", encoding="utf-8", newline="").write(NL.join(lines))
        return True

    dropped = []
    for rel, name in (("arm64/Android.bp", "libjni_latinimegoogle"),
                      ("common/Android.bp", "google.xml")):
        if drop_module(root / rel, name):
            dropped.append(name)
    msgs.append("从 Android.bp 删掉模块 " + "/".join(dropped)
                if dropped else "Android.bp 已是干净的")

    return " · ".join(msgs)


def main():
    tree = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path.home() / "crdroid").expanduser()
    if not (tree / "build/envsetup.sh").exists():
        print(f"✗ {tree} 看起来不是 Android 源码树"); sys.exit(1)
    print(f"树: {tree}")
    print("  [1] SPOOF_SAFETYNET: " + patch_spoof_safetynet(tree))
    print("  [2] hexagonfs CR 截断: " + patch_hexagonfs_cr(tree))
    print("  [3] v4l2_codec2 输入分辨率: " + patch_v4l2_input_size(tree))
    print("  [4] GApps 冲突: " + patch_gapps_conflicts(tree))

if __name__ == "__main__":
    main()
