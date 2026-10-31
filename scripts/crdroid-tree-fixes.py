#!/usr/bin/env python3
"""对 crDroid 源码树的本地修补（幂等，可反复跑）。

在构建机上执行：
    python3 <repo>/scripts/crdroid-tree-fixes.py ~/crdroid

目标树：crDroid 17.0（crDroid 13 / Android 17 = lineage-24.0、android-17.0.0_r1、release config cp2a），
2026-10-25 起。crDroid 16.0（A16 / bp4a）不再维护，锚点只按 17.0 写，不再兼容 16.0。

★ 严格模式（A17 迁移时改的）：每一条要么生效（或已生效），要么在下面 main() 里明确写着"A17 上废弃"。
  * 结果以 ✗ / ⚠️ 开头 = 没做成，记失败、整个脚本退 1。17.0 树里【一定有】的项目缺文件、缺锚点都算这一类
    —— 以前"跳过（找不到 …）"不算失败，上游一挪文件，修补就悄悄没了。
  * 只有 [4] 的 vendor/gapps 是可选项目（不装 GApps 就不同步），没同步时打印"跳过"、不算失败。
  * "已废弃（A17）"开头 = 上游已经不需要这条（例如 [11] 的 flag 被上游删掉了），照常打印、不算失败。

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

A17（crDroid 17.0）原样还在：system/core/init/Android.bp:133、:499 两处 "-DSPOOF_SAFETYNET=1"，
eng 分支各有一对 "-USPOOF_SAFETYNET" / "-DSPOOF_SAFETYNET=0"（:163-164、:529-530）；
property_service.cpp:1383-1385 照旧按它调 SetSafetyNetProps()。
"""
import io, os, re, sys, pathlib, shutil, subprocess, tempfile

def patch_spoof_safetynet(tree: pathlib.Path) -> str:
    p = tree / "system/core/init/Android.bp"
    if not p.exists():
        return f"✗ 找不到 {p}"
    s = io.open(p, encoding="utf-8").read()
    n = s.count('"-DSPOOF_SAFETYNET=1"')
    if n == 0:
        # eng 分支本来就成对写着 -USPOOF_SAFETYNET / -DSPOOF_SAFETYNET=0；只有比 -U 多出来的 =0 才是我们改的。
        # 光看"有没有 =0"会把"上游整个删掉了 =1"误判成已改过。
        n0, nu = s.count('"-DSPOOF_SAFETYNET=0"'), s.count('"-USPOOF_SAFETYNET"')
        return ("已是 0（幂等，无需改动）" if n0 > nu
                else "⚠️ 找不到 -DSPOOF_SAFETYNET=1（也不是我们改过的样子），上游可能改了写法")
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

    external/hexagonrpc 来自本仓 manifests/local_manifest_gaokun3.xml（钉 435ed1c），A17 树上照样同步；
    构建机 17.0 树上的副本（2026-10-25 核过）锚点原样在。没有这个项目 = 传感器全没 ⇒ 缺了算失败。
    """
    p = tree / "external/hexagonrpc/hexagonrpcd/hexagonfs.c"
    if not p.exists():
        return f"✗ 找不到 {p} —— local manifest 同步过了吗"
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

    A17：锚点在 V4L2Decoder.cpp:205，原样未变（V4L2Decoder.cpp 本身比 16.0 有改动，这一行没动）。
    """
    p = tree / "external/v4l2_codec2/v4l2/V4L2Decoder.cpp"
    if not p.exists():
        return f"✗ 找不到 {p}"
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


def patch_minigbm_p010_scanout(tree: pathlib.Path) -> str:
    """—— 修补 29：minigbm 只给 SC7280 开了 P010 的显示用途 ——

    症状：10-bit 视频放到 SurfaceView 时黑屏。minigbm 报 `Unsupported combination`
    （P010 + HW_COMPOSER，usage 0x400900），v4l2_codec2 拿不到输出块，也不报错给播放器。

    根因：msm.c 里 P010 的 BO_USE_SCANOUT 包在 `#ifdef SC_7280` 里，本机是 sc8280xp。

    ⚠️★ 必须和 patches/0130（turnip 的 P010）一起生效。只放开这里、GPU 又采样不了 P010 时，
    10-bit 视频一进 SurfaceFlinger，RenderEngine 就 abort
    （`Failed to create a valid texture ... format:54`），整个界面重启。
    这台机器竖屏面板横屏用，视频要 GPU 旋转，所以 P010 一定会走到这里。
    """
    p = tree / "external/minigbm/msm.c"
    if not p.exists():
        return f"✗ 找不到 {p}"
    s = io.open(p, encoding="utf-8").read()
    NL = chr(10)
    body = (
        "\tdrv_modify_combination(drv, DRM_FORMAT_P010, &LINEAR_METADATA," + NL
        + "\t\t\t       BO_USE_SCANOUT | BO_USE_HW_VIDEO_ENCODER);" + NL
    )
    guarded = "#ifdef SC_7280" + NL + body + "#endif" + NL
    if s.count(guarded) == 1:
        io.open(p, "w", encoding="utf-8").write(s.replace(guarded, body))
        return "已修补"
    if body in s:
        return "已打过（幂等，无需改动）"
    return "⚠️ 锚点不唯一或找不到，上游可能改了 msm_init()"


def patch_v4l2_10bit_profiles(tree: pathlib.Path) -> str:
    """—— 修补 30：v4l2_codec2 没有声明 10-bit profile ——

    症状：MediaCodecList 里硬解组件没有 HEVC Main10 和 VP9 Profile 2。ExoPlayer 记
    NoSupport 但还会硬试，mpv（FFmpeg）按能力表选解码器，直接走软解。

    根因：iris gen1 的解码器没有 profile 控件（iris_hfi_gen1.c 的 inst_fw_cap_sm8250_dec
    只有 PIPE、STAGE），V4L2Device::getSupportedProfiles() 的 VIDIOC_QUERYCTRL 必然失败，
    走这里的默认列表，而默认列表只有 8-bit。

    ⚠️ 改 components/DecodeInterface.cpp 里那份同样的列表没有用：它只在 profiles
    为空时才用，而这里的默认列表保证了 profiles 不为空。
    """
    p = tree / "external/v4l2_codec2/v4l2/V4L2Device.cpp"
    if not p.exists():
        return f"✗ 找不到 {p}"
    s = io.open(p, encoding="utf-8").read()
    if "C2Config::PROFILE_HEVC_MAIN_10,\n            };" in s:
        return "已打过（幂等，无需改动）"
    NL = chr(10)
    vp9 = "            profiles = {C2Config::PROFILE_VP9_0};" + NL
    hevc = "                    C2Config::PROFILE_HEVC_MAIN," + NL + "            };" + NL
    if s.count(vp9) != 1 or s.count(hevc) != 1:
        return "⚠️ 锚点不唯一或找不到，上游可能改了 getSupportedProfiles()"
    s = s.replace(vp9, "            profiles = {C2Config::PROFILE_VP9_0, C2Config::PROFILE_VP9_2};" + NL)
    s = s.replace(hevc, "                    C2Config::PROFILE_HEVC_MAIN," + NL
                  + "                    C2Config::PROFILE_HEVC_MAIN_10," + NL + "            };" + NL)
    io.open(p, "w", encoding="utf-8").write(s)
    return "已修补"


def patch_v4l2_device_scan_range(tree: pathlib.Path) -> str:
    """—— 修补 7：v4l2_codec2 只扫 /dev/video0..9，开了 camss 之后就找不到 Venus 了 ——

    症状：硬件视频编解码**静默消失**（组件名还在 MediaCodecList 里，因为那是
    media_codecs_c2.xml 驱动的，但底下一个设备都没匹配上），一切回落软解。

    ★ 根因：`V4L2Device::getDeviceInfosForType()` 里写死
        for (int i = 0; i < 10; ++i)        // v4l2/V4L2Device.cpp:2567（A17；16.0 是 :2565）
      只探 /dev/video0..video9。而 Stage 6 M22 把 camss 编进内核之后，
      **camss 一家就占了 32 个 video 节点**（msm_vfeN_videoM，实测 video0-31），
      Venus 被挤到 **video32 / video33**（实测 `cat /sys/class/video4linux/*/name`）。
      于是扫描范围内全是 camss 的 capture 节点，而解码器要的是
      V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE —— camss 不提供 ⇒ deviceInfos 为空。

    ⚠️★ 编号还**不是稳定的**：谁先 probe 谁拿低号。所以"把 Venus 钉在 video0/1"
      这种做法靠不住，正解就是把扫描范围放宽。

    修法：上界 10 -> 64。多出来的 open() 全部立即失败、只在首次调用时发生一次
    （结果有 sDeviceInfosCache 缓存），代价可以忽略。
    """
    p = tree / "external/v4l2_codec2/v4l2/V4L2Device.cpp"
    if not p.exists():
        return f"✗ 找不到 {p}"
    s = io.open(p, encoding="utf-8").read()
    if "kMaxVideoDeviceIndex" in s:
        return "已打过（幂等，无需改动）"
    anchor = "    for (int i = 0; i < 10; ++i) {"
    if s.count(anchor) != 1:
        return "⚠️ 锚点不唯一或找不到，上游可能改了 getDeviceInfosForType()"
    NL = chr(10)
    new = (
        "    // 只扫 video0..9 在本机是不够的：camss 一家就占 32 个节点" + NL
        + "    // （实测 video0-31 = msm_vfeN_videoM），Venus 被挤到 video32/33。" + NL
        + "    // 编号取决于 probe 先后，不稳定，所以放宽范围而不是钉死编号。" + NL
        + "    static constexpr int kMaxVideoDeviceIndex = 64;" + NL
        + "    for (int i = 0; i < kMaxVideoDeviceIndex; ++i) {"
    )
    s = s.replace(anchor, new, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    return "扫描上界 10 -> 64（camss 占了 video0-31，Venus 在 video32/33）"


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
    ★ A17 复查（2026-10-25，构建机 17.0 树，vendor/gapps = MindTheGapps cinnamonbun 分支 bfa45ac「17_202606」之后一提交）：
      34 个模块名对全树 Android.bp / Android.mk 逐个比对，仍只撞 libjni_latinimegoogle
      （packages/inputmethods/LatinIME/java/Android.bp:81）；15 个 etc 文件名对 vendor/lineage、vendor/addons、
      device/huawei、build/make/target 的 PRODUCT_COPY_FILES 逐个比对，仍只撞 google.xml（vendor/addons/config.mk:32）。
      三处锚点都在：arm64-vendor.mk:9 libjni_latinimegoogle、:15 SetupWizard；arm64/Android.bp:98；
      common-vendor.mk:24 google.xml；common/Android.bp:192。GmsSetupWizardOverlay 的目标包仍是
      org.lineageos.setupwizard（overlay/GmsSetupWizardOverlay/AndroidManifest.xml:5）。

    ⚠️ repo sync 会还原 vendor/gapps，所以每次构建前都要跑本脚本（幂等）。
    ★ 严格判据：某个名字在列表 / Android.bp 里找不到时，看 vendor/gapps 的 git 工作区 ——
      有本地改动 = 我们以前删过（幂等）；工作区是干净的 = 上游自己没了这一项（改名或删了），
      那就要重新做一遍上面的冲突比对，报 ⚠️ 而不是当成"已是干净的"。
    """
    NL = chr(10)
    BS = chr(92)
    root = tree / "vendor/gapps"
    if not root.is_dir():
        # 全脚本唯一一处可选项目：不装 GApps 就不同步 vendor/gapps
        return "跳过（没同步 vendor/gapps —— 不装 GApps 就不需要这一步）"
    # 同步了 vendor/gapps 却没有 arm64-vendor.mk = MindTheGapps 改了布局，不是"没装"，不能当跳过
    # （下面 mk_names / bp_names 的存在性检查会报 ⚠️）
    st = subprocess.run(["git", "-C", str(root), "status", "--porcelain", "--", "arm64", "common"],
                        capture_output=True, text=True)
    if st.returncode != 0:
        return "✗ vendor/gapps 不是 git 仓库，分不出是我们改过还是上游变了"
    pristine = st.stdout.strip() == ""

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

    mk_names = (("arm64/arm64-vendor.mk", ["SetupWizard", "libjni_latinimegoogle"]),
                ("common/common-vendor.mk", ["google.xml"]))
    bp_names = (("arm64/Android.bp", "libjni_latinimegoogle"),
                ("common/Android.bp", "google.xml"))
    for rel in [r for r, _ in mk_names] + [r for r, _ in bp_names]:
        if not (root / rel).exists():
            return f"⚠️ vendor/gapps 里找不到 {rel}，MindTheGapps 可能改了布局；什么都没动"

    # 先在干净树上核对全部锚点再动手：别删了一半才发现另一半上游已经变了
    if pristine:
        for rel, names in mk_names:
            lines = io.open(root / rel, encoding="utf-8").read().split(NL)
            for name in names:
                if not any(ln.strip().rstrip(BS).strip() == name for ln in lines):
                    return f"⚠️ {rel} 的包列表里没有 {name}（vendor/gapps 是干净的 ⇒ 上游改了），要重新比对冲突；什么都没动"
        for rel, name in bp_names:
            # 与 drop_module() 同一个判据（整行等于 name: "…",），别出现"核对通过、删除却没匹配上"
            if not any(ln.strip() == 'name: "%s",' % name
                       for ln in io.open(root / rel, encoding="utf-8").read().split(NL)):
                return f"⚠️ {rel} 里没有模块 {name}（vendor/gapps 是干净的 ⇒ 上游改了），要重新比对冲突；什么都没动"

    gone = []
    for rel, names in mk_names:
        gone += drop_from_mk(root / rel, names)
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
    for rel, name in bp_names:
        if drop_module(root / rel, name):
            dropped.append(name)
    msgs.append("从 Android.bp 删掉模块 " + "/".join(dropped)
                if dropped else "Android.bp 已是干净的")

    return " · ".join(msgs)


def patch_v4l2_initial_output(tree: pathlib.Path) -> str:
    """—— 修补 5：v4l2_codec2 在拿到 SOURCE_CHANGE 之前就想建输出队列 ——

    症状（修补 3 之后暴露出来的下一层）：
        V4L2Decoder: ioctl() failed: VIDIOC_G_FMT
        V4L2Decoder: Failed to start initialy output queue
        V4L2DecodeComponent: Failed to create V4L2Decoder for H264

    ★ **venus 这边是对的。** `vdec_check_src_change()`（vdec.c）明写着：

        if (inst->subscriptions & V4L2_EVENT_SOURCE_CHANGE &&
            inst->codec_state == VENUS_DEC_STATE_INIT &&
            !inst->reconfig)
                return -EINVAL;

    客户端订阅了 SOURCE_CHANGE，就必须**等事件到了再问 CAPTURE 的格式** ——
    这正是 V4L2 stateful 解码器规范要求的顺序。而 v4l2_codec2 的
    `setupInitialOutput()` 在**喂任何码流之前**就 G_FMT，那是 ChromeOS/ARCVM
    的一个优化（预先备一个 EOS 缓冲区），在守规范的驱动上必然失败。

    ⇒ 改成**失败不致命**：真正的输出队列本来就在分辨率变更事件里建
      （changeResolution() -> startOutputQueue()，A17 同文件 :738-747）。

    ★ 安全性判据（逐个查过，不是猜的）：`mInitialEosBuffer` 的**每一处使用
      都已经有空指针判断**（A17 V4L2Decoder.cpp 的 112 / 369 / 448 / 743 行；16.0 是 112 / 375 / 454 / 749），
      也就是说"它是空"本来就是被支持的状态。setupInitialOutput() 自己里面的 :244-255 是分配它的地方，不算使用。
      那几处分别是：
        · 369 提前判 DRC —— 跳过后 mPendingDRC 保持 false，只是少一个优化；
        · 448 "没有码流就直接结束 drain" —— 是 ARCVM 特有的捷径，
          不走它就落到 `sendV4L2DecoderCmd(false)`，那才是**标准**的 V4L2 drain；
        · 112 / 743 是清理。
    A17：锚点在 V4L2Decoder.cpp:175-178，原样未变。
      并且 `startOutputQueue()` 是在**第一步** getFormatInfo() 就返回 false 的，
      此时还没碰过队列，所以不会留下半配置状态。
    """
    p = tree / "external/v4l2_codec2/v4l2/V4L2Decoder.cpp"
    if not p.exists():
        return f"✗ 找不到 {p}"
    s = io.open(p, encoding="utf-8").read()
    if "venus 在收到 SOURCE_CHANGE 之前" in s:
        return "已打过（幂等，无需改动）"
    anchor = ('    if (!setupInitialOutput()) {' + chr(10)
              + '        ALOGE("Unable to setup initial output");' + chr(10)
              + '        return false;' + chr(10)
              + '    }')
    if anchor not in s:
        return "⚠️ 找不到锚点，上游可能改了 start()"
    NL = chr(10)
    new = (
        "    // venus 在收到 SOURCE_CHANGE 之前会拒绝 CAPTURE 队列的 G_FMT" + NL
        + "    // （vdec_check_src_change() 返回 -EINVAL），这是 V4L2 stateful 规范" + NL
        + "    // 要求的顺序，不是驱动缺陷。所以这个「预先建最小输出队列」的" + NL
        + "    // ChromeOS 优化在本平台必然失败 —— 失败不致命：真正的输出队列" + NL
        + "    // 会在分辨率变更事件里建起来，且 mInitialEosBuffer 的每一处使用" + NL
        + "    // 都已有空指针判断。" + NL
        + "    if (!setupInitialOutput()) {" + NL
        + '        ALOGW("Unable to setup initial output up front; the driver wants a '
          'SOURCE_CHANGE event first. Continuing without the initial EOS buffer.");' + NL
        + "    }"
    )
    s = s.replace(anchor, new, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    return "已改为失败不致命"


def patch_disable_desktop_mode(tree: pathlib.Path) -> str:
    """—— 修补 6：关掉 crDroid 给所有设备开的桌面窗口模式 ——

    用户要求关掉：这台平板上它会把每个应用都放进 freeform 窗口，日用很干扰。

    ⚠️★ **不能只在设备树的 overlay 里写 false —— 实测那样【无效】。**
      两次构建断言都顶回来 `config_isDesktopModeSupported 还是 true`：

        device/huawei/gaokun3/overlay/...        false   (DEVICE_PACKAGE_OVERLAYS)
        vendor/lineage/overlay/common/...        true    (PRODUCT_PACKAGE_OVERLAYS)

      **胜出的是 vendor/lineage 那份。** 也就是说在这棵树里
      `PRODUCT_PACKAGE_OVERLAYS` 的优先级【高于】`DEVICE_PACKAGE_OVERLAYS`
      —— 与"设备树优先"的常见说法相反。我们别的 overlay 值之所以一直好用，
      只是因为 vendor/lineage 没碰它们（逐个比对过，只有这一个资源冲突）。

    ⇒ 所以直接改胜出的那份。repo sync 会还原它，故本脚本每次构建前都要跑。

    ★ 顺带记一条被推翻的判断：上游参考（dragon-lineage 的 Radxa Dragon
      提交 d480d02）**只**设 config_canInternalDisplayHostDesktops，那对
      Lineage 系的树是**正确且充分**的 —— 因为 Lineage 自己已经把
      config_isDesktopModeSupported 设成 true 了。只查 AOSP 默认值（false）
      而不查 ROM 自己的 overlay，就会得出"他们漏了一个"的错误结论。

    A17：crDroid 17.0 的 vendor/lineage（= android_vendor_crdroid@17.0）照旧在
    overlay/common/frameworks/base/core/res/res/values/config.xml:98 写 true。
    ⚠️ A17 的桌面模式是否还有别的开关（aconfig flag 等）能绕过这个资源，没核实 —— 上机看一眼应用是不是还进 freeform。
    """
    p = tree / "vendor/lineage/overlay/common/frameworks/base/core/res/res/values/config.xml"
    if not p.exists():
        return f"✗ 找不到 {p}"
    s = io.open(p, encoding="utf-8").read()
    old = '<bool name="config_isDesktopModeSupported">true</bool>'
    new = '<bool name="config_isDesktopModeSupported">false</bool>'
    if new in s:
        return "已是 false（幂等，无需改动）"
    if old not in s:
        return "⚠️ 找不到锚点，crDroid 可能改了写法"
    s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    return "已把 crDroid 的 config_isDesktopModeSupported 改成 false"


def apply_patch_file(tree: pathlib.Path, project: str, patch_name: str,
                     covered_by: tuple = ()) -> str:
    """把 <repo>/patches/<patch_name> 用 git apply 打进 AOSP 树的 <project>（幂等）。

    ★ 为什么要有这个助手：本仓 `patches/` 里的 **AOSP 侧**补丁（0003 glslang〔A17 上废弃，见 main() 的 [9]〕、
    0010 audio HAL、0019 v4l2_codec2；后来又有 0008 tinyalsa、0051 / 0052 / 0063 audio HAL）**一直没有任何消费者** —— 全靠人手动
    `git apply`。而本仓已经为"没有消费者的配置一定会漂"付过三次账
    （M13 的 BOARD_KERNEL_CMDLINE、M17 的上游 Venus 补丁集、以及 2026-09-12
    抢救回来的那一整批 08-24 工作）。内核那边有 kernel-apply-patches.sh，
    AOSP 这边的消费者就是本脚本。

    幂等判据用 `git apply --check -R`（反向能打上 = 已经在树里了）。
    ⚠️ 与 kernel-apply-patches.sh 不同，这里**不接受 fuzz** —— AOSP 树是
    repo sync 出来的干净树，打不上就是上游动了，应当大声报错而不是模糊匹配。

    covered_by：排在后面、改到同一段上下文的补丁（例如 0069 改了 0063 加的那几行附近）。
    它们打上之后，本补丁正反两个方向都对不上 —— 2026-10-04 重跑时 [15] 就这样报了"打不上"，
    而 0063 的 42 行其实一行不缺（#130）。这时把涉及的文件拷到临时目录、先撤掉这些后续补丁，
    再对本补丁做反向检查：过了才算"已打过"。不是放宽判据，判的仍是"本补丁确实在树里"。
    """
    repo = pathlib.Path(__file__).resolve().parent.parent
    patch = repo / "patches" / patch_name
    if not patch.exists():
        return f"⚠️ 找不到补丁 {patch}"
    proj = tree / project
    if not (proj / ".git").exists():
        # 这里打补丁的项目（v4l2_codec2 / tinyalsa_new / hardware/interfaces）在 17.0 树里都一定有 ⇒ 缺了算失败
        return f"✗ {project} 不存在或不是 git 仓库"

    def git(*args):
        return subprocess.run(["git", "-C", str(proj), *args],
                              capture_output=True, text=True)

    if git("apply", "--check", "-R", str(patch)).returncode == 0:
        return "已打过（幂等，无需改动）"
    later = [repo / "patches" / n for n in covered_by]
    # 不能先逐个反向检查 later：later 之间也可能叠着改同一段（0132 之后的 0137），前面的单独撤不掉。
    if later and _applied_under(proj, patch, later):
        return "已打过（上下文被 " + "、".join(covered_by) + " 改过，撤掉它们后反向检查通过）"
    chk = git("apply", "--check", str(patch))
    if chk.returncode != 0:
        return "✗ 打不上（既不是已应用、也不干净）：" + chk.stderr.strip().splitlines()[0] if chk.stderr.strip() else "✗ 打不上"
    r = git("apply", str(patch))
    if r.returncode != 0:
        return "✗ 应用失败：" + (r.stderr.strip().splitlines()[0] if r.stderr.strip() else "?")
    return f"已应用 {patch_name}"


def _applied_under(proj: pathlib.Path, patch: pathlib.Path, later: list) -> bool:
    """在临时目录里：拷出涉及的文件 → 依次（倒序）撤掉 later → 对 patch 做反向检查。不碰 proj 本身。"""
    paths = set()
    for p in [patch, *later]:
        r = subprocess.run(["git", "-C", str(proj), "apply", "--numstat", str(p)],
                           capture_output=True, text=True)
        paths |= {line.split("\t")[2] for line in r.stdout.splitlines() if line.count("\t") >= 2}
    with tempfile.TemporaryDirectory() as tmp:
        for rel in paths:
            src = proj / rel
            if src.exists():
                (pathlib.Path(tmp) / rel).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, pathlib.Path(tmp) / rel)

        def git_tmp(*args):
            # 临时目录不在任何仓库里，git apply 按普通补丁处理文件
            return subprocess.run(["git", "apply", *args], cwd=tmp, capture_output=True, text=True)

        for l in reversed(later):
            if git_tmp("-R", str(l)).returncode != 0:
                return False
        return git_tmp("--check", "-R", str(patch)).returncode == 0


def patch_connected_displays_flag(tree: pathlib.Path) -> str:
    """—— 修补 11：关掉 status_bar_connected_displays 这个 aconfig flag ——

    ⚠️★ **这条的立项理由从来没有被记录下来。** 它是 2026-09-12 从构建机上
    抢救回来的一处未提交改动（`build/release` 项目），当时那一轮（08-24）
    的案卷没写。本条注释是现存的全部说明。

    已知的事实（不是推测）：
      * flag 名 `com.android.systemui.shared/status_bar_connected_displays`，
        构建机把 `state: ENABLED` 改成了 `DISABLED`。
      * 本机的 USB-C 外接显示**一直不工作**（UCSI 有缺陷，`/sys/class/typec/`
        为空，见 TODO A6）。一个"连接外部显示器时改变状态栏行为"的功能，
        在没有可用外接显示的机器上打开，合理推测是有害无益的。
    ⚠️ 但**"合理推测"不是证据** —— 如果将来 A6 修好了外接显示，
    应当先把这条去掉再验一次，别让它变成又一条没人敢动的祖传配置。

    ★ A17：**上游已经把这个 flag 删了**（构建机 17.0 树：frameworks/libs/systemui 提交 ab3c44e、
      frameworks/base 提交 c6b29cd899a0，标题都是 "Flag removal: com.android.systemui.status_bar_connected_displays"；
      frameworks/libs/systemui/aconfig/*.aconfig 里已经没有它的声明）。按 Android 的惯例 "Flag removal" =
      功能转正、永久打开（这一点没逐行核实）。build/release 里残留的值文件
      （aconfig/bp4a/com.android.systemui.shared/status_bar_connected_displays_flag_values.textproto）
      aconfig 会悄悄跳过：未声明的 flag 不报错（build/make/tools/aconfig/aconfig/src/commands.rs:297-303）。
      ⇒ 先查声明：没有声明就报"已废弃（A17）"、什么都不改；外接显示（TODO A6）修好时要验状态栏在外屏上的表现。
    ★ 声明还在时（例如以后上游又加回来），值文件**不再写死 bp4a**，而是沿 TARGET_RELEASE 的继承链去找
      实际生效的那一份 —— 见 _release_aconfig_value_sets()。A17 的 cp2a 链上（cp2a → mainline_2026_04 → … →
      cp1a → bp4a → …）只有 bp4a 那一份，但只要以后 cp* 自己覆盖了这个 flag，写死 bp4a 就会悄悄失效。
    """
    pkg, flag = "com.android.systemui.shared", "status_bar_connected_displays"
    # 1) flag 还在不在：com.android.systemui.shared 包声明在 frameworks/libs/systemui/aconfig/（A17 两个 .aconfig 文件第 1 行）
    decl_dir = tree / "frameworks/libs/systemui/aconfig"
    decls = [io.open(f, encoding="utf-8").read() for f in sorted(decl_dir.glob("*.aconfig"))]
    pkg_decls = [d for d in decls if re.search(r'^package:\s*"%s"' % re.escape(pkg), d, re.M)]
    if not pkg_decls:
        return f"✗ {decl_dir} 里找不到 package {pkg} 的声明，判不出这个 flag 还在不在"
    if not any(re.search(r'^\s*name:\s*"%s"' % flag, d, re.M) for d in pkg_decls):
        return ("已废弃（A17）：上游删掉了 " + pkg + "/" + flag
                + "（功能常开），build/release 里残留的值文件 aconfig 会忽略；什么都没改")
    # 2) 声明还在：沿继承链找实际生效的值文件
    release, src = _target_release(tree)
    if not release:
        return "✗ 定不了 TARGET_RELEASE（没有环境变量，也读不到 vendor/lineage/vars/aosp_target_release）"
    try:
        sets = _release_aconfig_value_sets(tree, release)
    except ValueError as e:
        return f"✗ 解析 release config {release} 失败：{e}"
    hit = None
    for set_name, set_dir in reversed(sets):   # aconfig 的 --values 是后者覆盖前者，所以从后往前找第一份
        for f in sorted((set_dir / pkg).glob("*_flag_values.textproto")):
            s = io.open(f, encoding="utf-8").read()
            for m in re.finditer(r"flag_value\s*\{[^}]*\}", s):
                if (re.search(r'package:\s*"%s"' % re.escape(pkg), m.group(0))
                        and re.search(r'name:\s*"%s"' % flag, m.group(0))):
                    hit = (f, s, m)
                    break
            if hit:
                break
        if hit:
            break
    if not hit:
        return (f"⚠️ {release}（来自 {src}）的 {len(sets)} 个 aconfig_value_set 里都没有 {flag} 的值文件，"
                "要另加一份")
    f, s, m = hit
    block = m.group(0)
    where = f.relative_to(tree)
    if "state: DISABLED" in block:
        return f"已是 DISABLED（幂等，无需改动；{where}）"
    if "state: ENABLED" not in block:
        return f"⚠️ {where} 里既不是 ENABLED 也不是 DISABLED，上游可能改了格式"
    s = s[:m.start()] + block.replace("state: ENABLED", "state: DISABLED", 1) + s[m.end():]
    io.open(f, "w", encoding="utf-8").write(s)
    return f"ENABLED -> DISABLED（{where}，TARGET_RELEASE={release}）"


def _target_release(tree: pathlib.Path):
    """TARGET_RELEASE：lunch 过就用环境变量；否则用 brunch 的默认值
    vendor/lineage/vars/aosp_target_release（A17 = cp2a；vendor/lineage/build/envsetup.sh:47、:62 拿它拼 lunch 目标）。"""
    env = os.environ.get("TARGET_RELEASE", "").strip()
    if env:
        return env, "环境变量 TARGET_RELEASE"
    p = tree / "vendor/lineage/vars/aosp_target_release"
    if p.exists():
        m = re.search(r"^aosp_target_release=(\S+)", io.open(p, encoding="utf-8").read(), re.M)
        if m:
            return m.group(1), "vendor/lineage/vars/aosp_target_release"
    return None, None


def _release_aconfig_value_sets(tree: pathlib.Path, release: str, variant: str = "") -> list:
    """按 release config 的规则算出 RELEASE_ACONFIG_VALUE_SETS：[(value_set 名, 目录), …]，前者先生效、后者覆盖。

    照着 A17 的实现抄的（不是猜的）：
      * 参与的 release_config_map：build/release，再加 device/*/release、device/*/*/release、vendor/*/release、
        vendor/*/*/release（按路径排序）—— build/make/core/release_config.mk:53-63。
        （PRODUCT_RELEASE_CONFIG_MAPS 另加的 build/release/gms_mainline/required 等只有 name、没有 aconfig_value_sets，不影响结果。）
      * 每个 map 目录下 release_configs/<名>.textproto 是对同名 config 的一份"贡献"（A17 cp2a 有两份：
        build/release 与 vendor/lineage/release）；map 里的 aliases 把别名指到真名。
      * 一个 config 的列表 = 先按 inherits 顺序依次展开被继承的 config（有 root 就先放 root），再接上自己各份贡献的
        aconfig_value_sets；最后去重、保留第一次出现的位置 —— build/soong/cmd/release_config/release_config_lib/
        release_config.go:230-261（继承）、:283-290（接上自己的）、:389-398（去重）、:186-192（InheritConfig 拼接）。
      * 接完自己的之后，若 build flag RELEASE_BUILD_USE_VARIANT_FLAGS 为真（自己的 <map>/flag_values/<名>/ 里设的，
        或从被继承的 config 带下来的），再继承一次构建变体同名的 BUILD_VARIANT config（userdebug / user / eng）——
        release_config.go:359-386。A17 是 bp3a 打开的（build/release/flag_values/bp3a/RELEASE_BUILD_USE_VARIANT_FLAGS.textproto），
        所以 cp2a 的列表里 userdebug 排在 bp3a 与 bp4a 之间。
      * 2026-10-25 对过构建机 17.0 树 lunch 出来的 out/soong/release-config/release_config-lineage_gaokun3-cp2a.varmk：
        RELEASE_ACONFIG_VALUE_SETS 共 14 项，与本函数算出来的逐项一致（maps_list 里也只有 build/release 与 vendor/lineage/release）。
      * aconfig_value_set 名 → 目录：<map>/aconfig/<子目录>/Android.bp 里的 aconfig_value_set { name: … }，
        值文件在 <子目录>/<包名>/*_flag_values.textproto（例如 build/release/aconfig/bp4a/Android.bp）。
      * 同一个 flag 多份值：aconfig create-cache 按 --values 顺序后者覆盖前者（aconfig/src/commands.rs:285-325），
        --values 的顺序就是上面这个列表（build/soong/aconfig/aconfig_declarations.go:93-97、:148-163）。
    任何一步对不上都抛 ValueError（严格模式：宁可报错，也不悄悄改错文件）。
    """
    map_dirs = [tree / "build/release"]
    extra = []
    for pat in ("device/*/release", "device/*/*/release", "vendor/*/release", "vendor/*/*/release"):
        extra += [d for d in tree.glob(pat) if (d / "release_config_map.textproto").exists()]
    map_dirs += sorted(set(extra), key=lambda d: str(d.relative_to(tree)))
    if not (map_dirs[0] / "release_config_map.textproto").exists():
        raise ValueError("找不到 build/release/release_config_map.textproto")

    # 构建变体：lunch 过就用 TARGET_BUILD_VARIANT；否则按本机唯一用的 lineage_gaokun3-cp2a-userdebug
    variant = variant or os.environ.get("TARGET_BUILD_VARIANT", "").strip() or "userdebug"
    aliases, inherits, own_sets, set_dirs, rc_type, own_uv = {}, {}, {}, {}, {}, {}
    for d in map_dirs:
        mtxt = io.open(d / "release_config_map.textproto", encoding="utf-8").read()
        for a in re.finditer(r'aliases\s*:?\s*\{\s*name:\s*"([^"]+)"\s*target:\s*"([^"]+)"', mtxt):
            aliases[a.group(1)] = a.group(2)
        for rc in sorted((d / "release_configs").glob("*.textproto")):
            t = io.open(rc, encoding="utf-8").read()
            nm = re.search(r'^name:\s*"([^"]+)"', t, re.M)
            if not nm:
                continue
            name = nm.group(1)
            inherits.setdefault(name, []).extend(re.findall(r'^inherits:\s*"([^"]+)"', t, re.M))
            own_sets.setdefault(name, []).extend(re.findall(r'^aconfig_value_sets:\s*"([^"]+)"', t, re.M))
            ty = re.search(r'^release_config_type:\s*(\w+)', t, re.M)
            if ty:
                rc_type[name] = ty.group(1)
            uvf = d / "flag_values" / name / "RELEASE_BUILD_USE_VARIANT_FLAGS.textproto"
            if uvf.exists():
                bv = re.search(r'bool_value:\s*(true|false)', io.open(uvf, encoding="utf-8").read())
                if not bv:
                    raise ValueError(f"{uvf.relative_to(tree)} 不是 bool_value，看不懂")
                own_uv[name] = bv.group(1) == "true"
        for bp in sorted((d / "aconfig").glob("*/Android.bp")):
            for n in re.findall(r'aconfig_value_set\s*\{\s*name:\s*"([^"]+)"', io.open(bp, encoding="utf-8").read()):
                set_dirs[n] = bp.parent

    def real(name):
        seen = set()
        while name in aliases and name not in seen:
            seen.add(name)
            name = aliases[name]
        return name

    memo, busy, uv_of = {}, set(), {}

    def expand(name):
        name = real(name)
        if name in memo:
            return memo[name]
        if name not in own_sets:
            raise ValueError(f"没有 release config {name}")
        if name in busy:
            raise ValueError(f"release config 继承成环：{name}")
        busy.add(name)
        out, done, uv = [], set(), None
        for i in (["root"] if "root" in own_sets and name != "root" else []) + inherits[name]:
            if i in done:
                continue
            done.add(i)
            out += expand(i)
            if uv_of.get(real(i)) is not None:      # 后继承的覆盖先继承的（InheritConfig :193-197）
                uv = uv_of[real(i)]
        out += own_sets[name]
        if name in own_uv:
            uv = own_uv[name]
        if (uv and rc_type.get(name, "RELEASE_CONFIG") == "RELEASE_CONFIG" and name != variant
                and rc_type.get(variant) == "BUILD_VARIANT"):
            out += expand(variant)
        uv_of[name] = uv
        busy.discard(name)
        memo[name] = out
        return out

    result, seen = [], set()
    for v in expand(release):
        if v in seen:
            continue
        seen.add(v)
        if v not in set_dirs:
            raise ValueError(f"aconfig_value_set {v} 找不到定义它的 Android.bp")
        result.append((v, set_dirs[v]))
    return result


def patch_wifi_sw_pno_gate(tree: pathlib.Path) -> str:
    """—— 修补 18：软件 PNO 只看 overlay 的 config_wifiSwPnoEnabled，不再看 DeviceConfig ——

    v1.0 NET-1（2026-10-05，TODO V3）。息屏且 Wi-Fi 断开时 Android 只靠 PNO 找回网络；ath11k 没有
    sched_scan（实机 iw phy 里没有 start_sched_scan），硬件 PNO 不可用 ⇒ 要软件 PNO。
    设备树 rro/Gaokun3WifiOverlay 已把 config_wifiSwPnoEnabled 设成 true（ff4a5a4），但 1.0.0-dev.1 上
    `dumpsys wifi` 里仍看不到软件 PNO：WifiScanningServiceImpl 的 StartedState 还要第二道门 ——

        } else if (mWifiGlobals.isSwPnoEnabled()
                && mDeviceConfigFacade.isSoftwarePnoEnabled()) {

    （crDroid 17.0 / A17：scanner/WifiScanningServiceImpl.java:2695-2696（16.0 是 :2676-2677）；
      isSoftwarePnoEnabled() = DeviceConfig wifi/software_pno_enabled，DeviceConfigFacade.java:407-408、:885-886，
      默认 false。）实机 `device_config get wifi software_pno_enabled` = false，而且是**显式下发**的值
    （list 里有这一项）—— 多半是 GMS Phenotype 推的，所以改默认值没用，只能去掉这道门。
    ⇒ 把条件改成只看 mWifiGlobals.isSwPnoEnabled()（= overlay 的 config_wifiSwPnoEnabled，A17 WifiGlobals.java:589-592）。
      不碰 DeviceConfigFacade：别处没有用 isSoftwarePnoEnabled()（A17 的 service/java 里 grep 只有这一处调用），
      dumpsys 里那个值照旧如实显示。isPnoSupported()（A17 WifiServiceImpl.java:8655-8658）本来就只看 isSwPnoEnabled()，不用改。

    ⚠️ 还没在构建机上确认的一条：com.android.wifi APEX 是从源码编的（不是预编译的 Google 模块 —— 那样改源码不进镜像；
       Play 系统更新也换不掉它：包名 / 签名都不同）。
    上机判据（V9 / V13）：`dumpsys wifi` 里 PNO 失败不再是 "reason: -3 not supported"，mPnoScanMetrics 的
      numPnoScanAttempts 开始增长；息屏但醒着时让 AP 断开再恢复，2 分钟内自动回连。
    ⓘ 耗电（V13 问的"SwPnoScanState 用什么闹钟"，上游副本里查到了）：schedulePnoTimer 用
      setExactAndAllowWhileIdle(ELAPSED_REALTIME_WAKEUP, …)（A17 WifiScanningServiceImpl.java:3001、:3011）⇒ 会把机器从
      s2idle 叫醒去扫。次数有上限：默认 config_wifiSwPnoFastTimerMs=300000 × 3 次、SlowTimerMs=1200000 × 10 次
      （另有 MobilityStateTimerIterations=2；A17 ServiceWifiResources config.xml:740-761），之后不再排程 ——
      只在"息屏 + 断网"时才有，批 4 量一次待机电流。
    ⚠️ 写法：正则只容忍空白差异；找不到锚点就报 ⚠️（step() 记为失败、整个脚本退 1），不会悄悄跳过。
    """
    p = (tree / "packages/modules/Wifi/service/java/com/android/server/wifi/scanner"
              / "WifiScanningServiceImpl.java")
    if not p.exists():
        # 与 [4] 那类"树里本来就可能没有"的项目不同：Wi-Fi 模块一定在，找不到只能是上游挪了文件 ⇒ 算失败
        return f"⚠️ 找不到 {p}，上游可能挪了文件"
    s = io.open(p, encoding="utf-8").read()
    marker = "gaokun3 tree-fix [18]"
    if marker in s:
        return "已改（幂等，无需改动）"
    pat = re.compile(r"\}(\s*)else if \(mWifiGlobals\.isSwPnoEnabled\(\)\s*"
                     r"&&\s*mDeviceConfigFacade\.isSoftwarePnoEnabled\(\)\)\s*\{")
    hits = pat.findall(s)
    if len(hits) != 1:
        return f"⚠️ 锚点（isSwPnoEnabled() && isSoftwarePnoEnabled()）出现 {len(hits)} 次（应为 1），上游可能改了写法"
    # 插进 Java 的注释只用 ASCII：不赌 javac 的源码编码设置
    s = pat.sub(lambda m: ("}" + m.group(1) + "else if (mWifiGlobals.isSwPnoEnabled()) {  // " + marker
                           + ": ignore DeviceConfig wifi/software_pno_enabled (GMS pushes false);"
                           + " see gaokun-android scripts/crdroid-tree-fixes.py"),
                s, count=1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    return "已去掉软件 PNO 的 DeviceConfig 门（只看 config_wifiSwPnoEnabled）"


def patch_wifi_stable_factory_mac(tree: pathlib.Path) -> str:
    """—— 修补 19：Wi-Fi HAL 的"出厂 MAC"改成由 SoC 序列号派生的稳定本地管理地址 ——

    v1.0 NET-4（2026-10-05，构建机 crDroid 树核实）。本机 WCN6855 板子上没烧 MAC：固件每次开机给一个
    00:03:7f:12:xx:xx（只有后两字节随机，见 CLAUDE.md 的路由器静态租约），ath11k 拿它当 perm_addr。
    框架拿"出厂 MAC"的链路（行号是 crDroid 17.0 / A17 的）：ClientModeImpl.retrieveFactoryMacAddressAndStoreIfNecessary（:8351）
      → WifiNative.getStaFactoryMacAddress（:2616）→ WifiVendorHal.getStaFactoryMacAddress（:989）
      → HAL IWifiStaIface.getFactoryMacAddress → WifiIfaceUtil::getFactoryMacAddress
        （hardware/interfaces/wifi/aidl/default/wifi_iface_util.cpp:54-56）
      → InterfaceTool::GetFactoryMacAddress = ETHTOOL_GPERMADDR（frameworks/opt/net/wifi/libwifi_system_iface/
        interface_tool.cpp:144-165），读的是 netdev 的 perm_addr。
    ⇒ 开机脚本里 `ip link set wlan0 address …` 改不到这一层（只改 dev_addr，不改 perm_addr）：
       用户选"使用设备 MAC"时框架会在连接前把 MAC 设回 HAL 给的出厂值（ClientModeImpl.setCurrentMacToFactoryMac，
       :4604）。所以改在 HAL 这一层：WifiIfaceUtil::getFactoryMacAddress 先读
       /sys/devices/soc0/serial_number（qcom socinfo），对 "gaokun3-wifi-mac:<接口名>:<序列号>" 做 FNV-1a 64，
       取低 6 字节、置本地管理位、清组播位；读不到序列号就照旧返回 perm_addr。
       接口名进哈希 ⇒ wlan0 / wlan1（热点）各有一个固定地址、互不相同。
    SELinux：hal_wifi 已能读所有 sysfs 类型（system/sepolicy/private/hal_wifi.te:11 r_dir_file(hal_wifi, sysfs_type)），不用加规则。
    A17 锚点（wifi_iface_util.cpp:18 的 macros.h include、:32 的 kMacAddressLocallyAssignedMask、:54-56 的函数体）原样在；
    HAL 仍链 libbase（wifi/aidl/default/Android.bp:104）。
    配套：rro/Gaokun3WifiOverlay 把 config_wifiSaveFactoryMacToWifiConfigStore 设成 false —— 默认 true 时框架把
      【第一次】拿到的出厂 MAC 存进 WifiSettingsConfigStore 永久复用（ClientModeImpl.retrieveFactoryMacAddressAndStoreIfNecessary），
      老机器存的是当年那个固件随机地址，派生地址就永远用不上。
    ⚠️ 序列号是 32 位的，派生地址能被穷举反推出序列号 —— 它不是秘密，只是别直接把序列号写进 MAC。
    上机判据：两次重启后 `cmd wifi status` / Settings「关于」里的 Wi-Fi MAC 相同、首字节第 2 位为 1；
      某个网络选"使用设备 MAC"后 `ip link show wlan0` 的地址等于它；`cat /sys/devices/soc0/serial_number` 非空。
    """
    p = tree / "hardware/interfaces/wifi/aidl/default/wifi_iface_util.cpp"
    if not p.exists():
        return f"⚠️ 找不到 {p}，上游可能挪了文件"
    s = io.open(p, encoding="utf-8").read()
    marker = "gaokun3 tree-fix [19]"
    if marker in s:
        return "已改（幂等，无需改动）"
    inc = "#include <android-base/macros.h>\n"
    ns_anchor = ("constexpr uint8_t kMacAddressLocallyAssignedMask = 0x02;\n")
    fn = re.compile(r"(std::array<uint8_t, 6> WifiIfaceUtil::getFactoryMacAddress\(const std::string& iface_name\) \{\n)"
                    r"(\s*return iface_tool_\.lock\(\)->GetFactoryMacAddress\(iface_name\.c_str\(\)\);\n\})")
    if s.count(inc) != 1 or s.count(ns_anchor) != 1 or len(fn.findall(s)) != 1:
        return "⚠️ wifi_iface_util.cpp 的锚点（include / 掩码常量 / getFactoryMacAddress）不唯一或不在，上游可能改了写法"
    s = s.replace(inc, inc + "#include <android-base/file.h>\n#include <android-base/strings.h>\n", 1)
    helper = (
        "\n// " + marker + ": stable locally-administered \"factory\" MAC derived from the SoC serial\n"
        "// (board has no programmed MAC; firmware hands out 00:03:7f:12:xx:xx, random every boot).\n"
        "// See gaokun-android scripts/crdroid-tree-fixes.py.\n"
        "bool gaokun3StableMac(const std::string& iface_name, std::array<uint8_t, 6>* mac) {\n"
        "    std::string serial;\n"
        "    if (!::android::base::ReadFileToString(\"/sys/devices/soc0/serial_number\", &serial)) {\n"
        "        return false;\n"
        "    }\n"
        "    serial = ::android::base::Trim(serial);\n"
        "    if (serial.empty() || serial == \"0\") return false;\n"
        "    const std::string key = \"gaokun3-wifi-mac:\" + iface_name + \":\" + serial;\n"
        "    uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a 64\n"
        "    for (unsigned char c : key) {\n"
        "        h ^= c;\n"
        "        h *= 0x100000001b3ULL;\n"
        "    }\n"
        "    for (size_t i = 0; i < mac->size(); i++) {\n"
        "        (*mac)[i] = static_cast<uint8_t>(h >> (8 * i));\n"
        "    }\n"
        "    (*mac)[0] &= ~kMacAddressMulticastMask;\n"
        "    (*mac)[0] |= kMacAddressLocallyAssignedMask;\n"
        "    return true;\n"
        "}\n")
    s = s.replace(ns_anchor, ns_anchor + helper, 1)
    s = fn.sub(lambda m: (m.group(1)
                          + "    std::array<uint8_t, 6> stable_mac;  // " + marker + "\n"
                          + "    if (gaokun3StableMac(iface_name, &stable_mac)) return stable_mac;\n"
                          + m.group(2)), s, count=1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    return "HAL 出厂 MAC 改为由 soc0/serial_number 派生（读不到就退回 perm_addr）"


def patch_wifi_mac_randomization_default(tree: pathlib.Path) -> str:
    """—— 修补 20：新网络的 MAC 随机化默认值从"每次连接都换"（ALWAYS）改回 AOSP 的 AUTO（按网络固定）——

    v1.0 NET-4 / D14（用户 2026-10-04 定：持久模式 + 稳定的设备 MAC）。2026-10-05 构建机核实（16.0），10-25 按 17.0 复核：
      * 这个默认值【没有资源可改】：crDroid 带的 GrapheneOS 补丁把字段默认写死成 RANDOMIZATION_ALWAYS（=100）——
        packages/modules/Wifi 提交 09e2d601ef（17.0；16.0 是 06bc263807）"add option of always randomizing MAC addresses"：
        framework/java/android/net/wifi/WifiConfiguration.java:1934、
        service/java/com/android/server/wifi/WifiConfigurationUtil.java:284（比较基准随之改成 ALWAYS）；
        frameworks/opt/net/wifi 提交 a731178c0（17.0；16.0 是 3bf65d873）：WifiTrackerLib StandardWifiEntry.getPrivacy()
        无配置时返回 PRIVACY_RANDOMIZATION_ALWAYS（:612）。
      * ★ 17.0 的变化：**Settings 那半边 GrapheneOS 补丁没有跟过来**。packages/apps/Settings 里已经没有
        wifi_privacy_entries_ext / "每次连接都换"这一项，连接对话框用的是 AOSP 的 wifi_privacy_entries
        （WifiConfigController2.java:366；res/values/arrays.xml:1238-1241 只有"随机 MAC / 设备 MAC"两项），
        新网络停在第 0 项，保存时经 WifiPrivacyPreferenceController2.translatePrefValueToWifiConfigSetting
        （:120-123）写成 RANDOMIZATION_AUTO ⇒ 16.0 时的第四处（对话框给新网络预选"按网络随机"）**在 17.0 上不需要了**，
        已删掉；其余三处照旧要改：别的路径（App 的 addNetwork / 建议网络等）建出来的配置仍取字段默认值 ALWAYS。
    ⇒ 三处一起改回 AOSP 原值（字段默认与比较基准必须成对改，否则普通 App 的 addNetwork 会因"改了随机化设置"
       被 WifiConfigManager.java:1706-1716（A17）拒掉）：字段默认 AUTO、比较基准 AUTO、无配置时 PRIVACY_RANDOMIZED_MAC。
       ALWAYS 的框架逻辑一行没删（WifiConfigManager.shouldUseNonPersistentRandomization:558-561）。
    ⓘ AUTO 的实际行为 = 按网络固定的随机 MAC（WifiConfigManager.shouldUseNonPersistentRandomization:558-597（A17）：
       只有开放网络 + config_wifiAllowNonPersistentMacRandomizationOnOpenSsids（默认 false）或 SSID 在白名单里才换），
       地址由 keystore 里的 MacRandSecret 密钥对 SSID 做 HMAC（MacAddressUtil），与出厂 MAC 无关。
    ⚠️ 只管新加的网络。老用户已保存的网络存的是 ALWAYS，会继续每次连接都换 MAC。没做自动迁移：存储里分不出
       "用户选的 ALWAYS"和"默认的 ALWAYS"（randomizedMacLastModifiedTimeMs 不落盘，XmlUtil 里没有这个标签）。
    ⚠️ 17.0 新出的显示问题（上游的，本条不修）：Settings 只把 PRIVACY_RANDOMIZED_MAC 映射成"随机 MAC"
       （WifiPrivacyPreferenceController2.translateWifiEntryPrivacyToPrefValue:109-112），ALWAYS 的网络会被显示成
       "使用设备 MAC"，实际却每次连接都换；用户在详情里改选一次"随机 MAC"就会存成 PERSISTENT（StandardWifiEntry:630-631）。
    上机判据：新连一个 WPA2 网络 → 详情里隐私显示"使用随机 MAC"（按网络）；断开重连 / 重启两次，
      `ip link show wlan0` 的地址不变且与出厂 MAC 不同；`cmd wifi list-networks` 后 dumpsys 里该网络
      macRandomizationSetting = 3（AUTO）。
    """
    edits = [
        ("packages/modules/Wifi/framework/java/android/net/wifi/WifiConfiguration.java",
         re.compile(r"public int macRandomizationSetting = RANDOMIZATION_ALWAYS;"),
         lambda m: ("public int macRandomizationSetting = RANDOMIZATION_AUTO;"
                    "  // MARK: AOSP default (was ALWAYS)")),
        ("packages/modules/Wifi/service/java/com/android/server/wifi/WifiConfigurationUtil.java",
         re.compile(r"return newConfig\.macRandomizationSetting != WifiConfiguration\.RANDOMIZATION_ALWAYS;"),
         lambda m: ("return newConfig.macRandomizationSetting != WifiConfiguration.RANDOMIZATION_AUTO;"
                    "  // MARK: must match the field default")),
        ("frameworks/opt/net/wifi/libs/WifiTrackerLib/src/com/android/wifitrackerlib/StandardWifiEntry.java",
         re.compile(r"(\}\s*else\s*\{\s*)return PRIVACY_RANDOMIZATION_ALWAYS;(\s*\})"),
         lambda m: (m.group(1) + "return PRIVACY_RANDOMIZED_MAC;  // MARK: AOSP default (was ALWAYS)"
                    + m.group(2))),
        # 16.0 的第四处（Settings WifiConfigController2 给新网络预选"按网络随机"）17.0 上不需要了，见上面 docstring
    ]
    marker = "gaokun3 tree-fix [20]"
    todo = []
    for rel, pat, rep in edits:
        p = tree / rel
        if not p.exists():
            return f"⚠️ 找不到 {rel}，上游可能挪了文件"
        s = io.open(p, encoding="utf-8").read()
        if marker in s:
            continue
        if len(pat.findall(s)) != 1:
            return f"⚠️ {p.name} 的锚点出现 {len(pat.findall(s))} 次（应为 1），上游可能改了写法；三处都没动"
        todo.append((p, pat.sub(lambda m: rep(m).replace("MARK", marker), s, count=1)))
    if not todo:
        return "已改（幂等，无需改动）"
    # 先全部核对锚点、再统一写：避免只改了一半（字段默认与比较基准不成对会让 App 的 addNetwork 被拒）
    for p, s in todo:
        io.open(p, "w", encoding="utf-8", newline="").write(s)
    return f"MAC 随机化默认改回 AUTO（改了 {len(todo)} 个文件）"


def patch_build_fingerprint_variant_user(tree: pathlib.Path) -> str:
    """—— 修补 23：BUILD_FINGERPRINT / BUILD_THUMBPRINT / BUILD_SYSTEM_FINGERPRINT 的变体段改写成 user ——

    游戏环境如实修正（Duck Detector 体检 2026-10-10）：把自相矛盾的身份字段改一致，
    不是伪造硬件证明。同一天的运行时手段（scripts/game-mode.sh identity）已实测
    只能改属性、改不掉 android.os.Build.FINGERPRINT 常量（zygote 预加载时定死），
    那三条 Type/property 漂移在运行时消不掉，只有 ROM 级能齐。

    现状（16.0 dev.11 实机 /system/build.prop 与构建机 out/ 一致）：
      ro.build.type=user              ← soong gen_build_prop.get_build_variant()：
      ro.system.build.type=user         非 eng 一律按 user 处理（A17 build/soong/scripts/
      ro.build.flavor=gaokun3-user      gen_build_prop.py:28-32 原样；lineage_gaokun3.mk 注释、#117 §16）
      ro.build.fingerprint=…:userdebug/release-keys   ← build/make/core/config.mk 的 BUILD_FINGERPRINT
      ro.system.build.fingerprint=…:userdebug/release-keys
                                        用 $(TARGET_BUILD_VARIANT)=userdebug 拼尾部
    ⇒ 半截身份：type 一侧已是 user，fingerprint 一侧还写着 userdebug。Duck Detector
      的 SystemPropertyConsistencyUtils.buildBuildFingerprintTailSignals 拿
      Build.FINGERPRINT 的尾部去比 Build.TYPE / ro.build.type，三条全是 WARNING；
      SystemPropertiesCatalog.suspiciousFingerprintPatterns 还有 "userdebug" 一条，
      盯 ro.build.fingerprint / ro.system.build.fingerprint / ro.vendor.build.fingerprint
      / ro.bootimage.build.fingerprint / Build.FINGERPRINT。
      实测 after1（跑过运行时 identity）：dangerous_apps / root_managers 已转绿，
      system_properties 的 3 条 danger 全是这个漂移 —— 属性被 resetprop 成 :user/，
      常量还是 :userdebug/，反而多出一条 "ro.build.fingerprint <> Build.FINGERPRINT"。

    改法：config.mk 里每一处 `:$(TARGET_BUILD_VARIANT)/$(BUILD_VERSION_TAGS)` 改成 `:user/$(BUILD_VERSION_TAGS)`。
      ★ A17 是【三处】（16.0 是两处）：BUILD_FINGERPRINT（config.mk:1433）、BUILD_THUMBPRINT（:1453），
        外加 A17 新增的 BUILD_SYSTEM_FINGERPRINT（:1472）。第三处不能漏：A17 的 ro.system.build.fingerprint
        改由它来 —— gen_build_prop.py:91 读 build_system_fingerprint 文件、:194-195 对 system 分区输出
        ro.system.build.fingerprint?=BuildSystemFingerprint；system 镜像的 AVB 属性
        com.android.build.system.fingerprint 也用它（build/make/core/Makefile:4905）。
        全文件 TARGET_BUILD_VARIANT 只出现在这三行，改完 type 与各 fingerprint 尾部齐。
      注释锚点：A17 上游把 "is used used to" 的错别字修掉了（config.mk:1430），按新原文找。
    ⓘ 为什么不改 gen_build_prop 让 type 跟着变 userdebug：那会让 ro.build.type
      直接撞 SystemPropertiesCatalog 的 dangerousValues=["eng","userdebug"]，
      更糟；产品身份本来就是 type=user（crDroid 的 gen_build_prop 身份），只是
      fingerprint 没跟上。
    ⓘ OTA 不受影响：本仓 full OTA 包的 META-INF/com/android/metadata 里【没有】
      pre-build / pre-build-incremental（out/release-v0.7.1-alpha 的 zip 实测），
      CheckAbSpecificMetadata 对空 pre-build 直接跳过指纹比对
      （lineage-bootable-recovery install/install.cpp:171-176）；post-build 只比
      最后一段 tags=release-keys（:203-204），与变体段无关。in-system 走
      update_engine 的 payload_properties 也不带 pre-build。（这一段是 16.0 时核的，A17 未复核。）
    ⚠️ 锚点按 crDroid 17.0 的 build/make（lineage-24.0）写的；对不上会报 ⚠️ 并被 step() 记为失败，不会悄悄跳过。
    上机判据：`getprop ro.build.fingerprint` / `ro.system.build.fingerprint` 与新装后的 Build.FINGERPRINT 都是
      …:user/release-keys，且不再与 Build.TYPE 漂移；Duck Detector 的
      system_properties 3 条 danger / "Suspicious fingerprint pattern 'userdebug'" 消失。
    """
    p = tree / "build/make/core/config.mk"
    if not p.exists():
        return f"⚠️ 找不到 build/make/core/config.mk（{p}），上游可能挪了文件"
    s = io.open(p, encoding="utf-8").read()
    marker = "gaokun3 tree-fix [23]"
    if marker in s:
        return "已改（幂等，无需改动）"
    old = ":$(TARGET_BUILD_VARIANT)/$(BUILD_VERSION_TAGS)"
    new = ":user/$(BUILD_VERSION_TAGS)"
    want = 3   # BUILD_FINGERPRINT、BUILD_THUMBPRINT、BUILD_SYSTEM_FINGERPRINT
    if s.count(old) != want:
        return (f"⚠️ 锚点 {old} 出现 {s.count(old)} 次"
                f"（应为 {want}：BUILD_FINGERPRINT / BUILD_THUMBPRINT / BUILD_SYSTEM_FINGERPRINT），上游可能改了写法")
    # 三处必须都在各自的赋值行上，别误改到别的变量
    for var in ("BUILD_FINGERPRINT", "BUILD_THUMBPRINT", "BUILD_SYSTEM_FINGERPRINT"):
        if not re.search(r"^\s*" + var + r" := .*" + re.escape(old) + r"$", s, re.M):
            return f"⚠️ {var} 的赋值行里找不到 {old}，上游可能改了写法；什么都没动"
    # 注释锚点先核对，别改了变体段却插不进标记（下次幂等判断就瞎了）
    anchor = "# BUILD_FINGERPRINT is used to uniquely identify the combined build and product;"
    if s.count(anchor) != 1:
        return "⚠️ 找不到 BUILD_FINGERPRINT 注释锚点（或不唯一），上游可能改了写法"
    comment = ("# " + marker + ": fingerprint/thumbprint/system-fingerprint variant segment\n"
               "# forced to 'user', not $(TARGET_BUILD_VARIANT). ro.build.type already comes out\n"
               "# 'user' via soong gen_build_prop.get_build_variant() (no userdebug tier), so the\n"
               "# fingerprint tail saying userdebug is a half-done identity that drifts\n"
               "# against Build.TYPE. See gaokun-android scripts/crdroid-tree-fixes.py.\n")
    s = s.replace(old, new).replace(anchor, comment + anchor, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    return f"已把指纹/拇指纹/系统指纹的变体段改成 user（{want} 处）"


def patch_adb_root_only_with_su(tree: pathlib.Path) -> str:
    """—— 修补 24：adb_root 只在 WITH_SU=true 时装（恢复 crDroid 16.0 的行为）——

    crDroid 17.0 的 vendor/lineage/config/common.mk:244-247 对所有非 user 变体无条件
    `PRODUCT_PACKAGES += adb_root`（16.0 时它在 `ifeq ($(WITH_SU),true)` 里面，和 su 一起）。
    我们的发布构建是 userdebug 变体（-user 的 init 强制 enforcing、起不来，#117 §15），
    adbd 按变体编成 ANDROID_DEBUGGABLE=1（packages/modules/adb/Android.bp:47-55）；装了
    adbroot_service 之后，restart_service.cpp 的 root 路径不再看 ro.debuggable，
    用户在开发者选项里一开「adb root」，`adb root` 就成了。
    ⇒ 发布构建 D1「没有 adb root」（docs/v1.0-plan.md、release-checklist）被悄悄打开。
    设备树去不掉一个 PRODUCT_PACKAGES（没有 PRODUCT_PACKAGES_REMOVE），只能改这里。
    开发机照旧用 KSU 拿 root（`adb shell` 经 KSU 即 root，从来没有 `adb root`）。
    上机判据：`pm list packages`/`ls /system_ext/bin /system/bin | grep adb_root` 为空；
      开发者选项里没有「Rooted debugging / adb root」开关；`adb root` 报 adbd cannot run as root。
    """
    p = tree / "vendor/lineage/config/common.mk"
    if not p.exists():
        return f"⚠️ 找不到 vendor/lineage/config/common.mk（{p}）"
    s = io.open(p, encoding="utf-8").read()
    marker = "gaokun3 tree-fix [24]"
    if marker in s:
        return "已改（幂等，无需改动）"
    old = ("# Root\n"
           "ifneq ($(TARGET_BUILD_VARIANT),user)\n"
           "PRODUCT_PACKAGES += \\\n"
           "    adb_root\n"
           "ifeq ($(WITH_SU),true)\n"
           "PRODUCT_PACKAGES += \\\n"
           "    su\n")
    if s.count(old) != 1:
        return "⚠️ 找不到 common.mk 里「# Root」那段（或不唯一），上游可能改了写法；什么都没动"
    new = ("# Root\n"
           "# " + marker + ": adb_root only with WITH_SU=true (as in crDroid 16.0); release\n"
           "# builds are the userdebug variant and must not offer adb root.\n"
           "ifneq ($(TARGET_BUILD_VARIANT),user)\n"
           "ifeq ($(WITH_SU),true)\n"
           "PRODUCT_PACKAGES += \\\n"
           "    adb_root\n"
           "PRODUCT_PACKAGES += \\\n"
           "    su\n")
    io.open(p, "w", encoding="utf-8", newline="").write(s.replace(old, new, 1))
    return "adb_root 挪进 ifeq ($(WITH_SU),true)"


def patch_keygesture_lineagesettings_guard(tree: pathlib.Path) -> str:
    """—— 修补 25：KeyGestureController 读 LineageSettings 失败不致命 ——

    来历（2026-10-11 救砖，见 memory「lineagesettings 被卸 → system_server 循环崩」）：
    有人为"隐藏 ROM 痕迹"把 org.lineageos.lineagesettings 对 user 0 卸掉，
    KeyGestureController 读 LineageSettings.System 抛异常 ⇒ system_server 每 5 秒崩一次、整机起不来。
    当时在构建机树上直接给这两处读取包了 try/catch，**从没写进本脚本、全仓零记录**，
    却编进了之后每一版（含 rc2e）。2026-10-25 A17 迁移 reset 树时，对照 A16 树的改动普查才发现。
    A17（crDroid 17.0）frameworks/base 的 KeyGestureController.java:333-338 仍是同样没有防护的写法。
    ⓘ 这只是兜底：根本的规矩仍是"别隐藏 lineagesettings / lineageparts"（docs/gaming.md）。
       读失败时两个开关保持默认 false（不启用部分截图 / 音量上下同按静音），与没装 Lineage 设置时一致。
    """
    p = tree / "frameworks/base/services/core/java/com/android/server/input/KeyGestureController.java"
    if not p.exists():
        return f"⚠️ 找不到 KeyGestureController.java（{p}），上游可能挪了文件"
    s = io.open(p, encoding="utf-8").read()
    marker = "gaokun3 tree-fix [25]"
    if marker in s:
        return "已改（幂等，无需改动）"
    old = ("        mClickPartialScreenshot = LineageSettings.System.getIntForUser(resolver,\n"
           "                LineageSettings.System.CLICK_PARTIAL_SCREENSHOT, 0,\n"
           "                UserHandle.USER_CURRENT) == 1;\n"
           "        mVolUpAndDownMute = LineageSettings.System.getIntForUser(resolver,\n"
           "                LineageSettings.System.VOLUME_UP_AND_DOWN_MUTE, 0,\n"
           "                UserHandle.USER_CURRENT) == 1;\n")
    if s.count(old) != 1:
        return "⚠️ 找不到读 CLICK_PARTIAL_SCREENSHOT / VOLUME_UP_AND_DOWN_MUTE 的那段（或不唯一），上游可能改了写法"
    new = ("        // " + marker + ": a missing/hidden LineageSettings provider must not crash-loop system_server.\n"
           "        try {\n"
           "            mClickPartialScreenshot = LineageSettings.System.getIntForUser(resolver,\n"
           "                    LineageSettings.System.CLICK_PARTIAL_SCREENSHOT, 0,\n"
           "                    UserHandle.USER_CURRENT) == 1;\n"
           "        } catch (Exception e) {\n"
           "            android.util.Log.w(\"KeyGestureController\", \"Failed to read CLICK_PARTIAL_SCREENSHOT\", e);\n"
           "        }\n"
           "        try {\n"
           "            mVolUpAndDownMute = LineageSettings.System.getIntForUser(resolver,\n"
           "                    LineageSettings.System.VOLUME_UP_AND_DOWN_MUTE, 0,\n"
           "                    UserHandle.USER_CURRENT) == 1;\n"
           "        } catch (Exception e) {\n"
           "            android.util.Log.w(\"KeyGestureController\", \"Failed to read VOLUME_UP_AND_DOWN_MUTE\", e);\n"
           "        }\n")
    io.open(p, "w", encoding="utf-8", newline="").write(s.replace(old, new, 1))
    return "两处 LineageSettings 读取包上 try/catch"


def patch_codec2_blob_linear_map(tree: pathlib.Path) -> str:
    """—— 修补 26：BLOB（gralloc 支撑）的线性块在没有 ION 的机器上照样能 map ——

    A17 上游 frameworks/av 提交 3a5dff64c9（"Reapply codec2: add support for custom mappers"）把
    C2ConstLinearBlock::map() / C2LinearBlock::map() 改成走 _C2Block1DImpl::mapReadView / mapWriteView
    （vndk/C2Buffer.cpp）：没有 /dev/ion（C2HandleIon::IsUsed() = using_ion() = false，C2Store.cpp:116、:1446）
    时一律交给 C2DmaBufAllocator::Map，而它先 CheckHandle —— 句柄不是 C2HandleBuf 就直接 C2_BAD_VALUE
    （C2DmaBufAllocator.cpp:470-476）。A16 是直接调 allocation->map()，哪种分配都行。
    本机没有 ION，device.mk 的 debug.stagefright.c2-poolmask=0xfe0000 让默认线性分配器是 BLOB
    （C2Store.cpp:526 GetPreferredLinearAllocatorId）⇒ v4l2 解码 HAL 里 map 客户端送来的输入块报 err=22，
    patches/0019 的 HEVC CSD 合并退回"单独下发 CSD"，venus 死锁、HEVC 硬解一帧不出
    （2026-10-26 A17 首个候选版 1792975257 上 A21 iris 回归 11/15，挂的正是 4 项 HEVC）。
    修法：句柄不是 DMABUF 时退回 A16 的 allocation->map()；DMABUF 句柄照旧走上游新路径（custom mapper）。
    上机判据：out/iris-rc/rc-accept.sh 15/15；logcat 有 "HEVC: stashed … bytes of CSD"、没有 "cannot map CSD"。
    """
    p = tree / "frameworks/av/media/codec2/vndk/C2Buffer.cpp"
    if not p.exists():
        return f"⚠️ 找不到 C2Buffer.cpp（{p}）"
    s = io.open(p, encoding="utf-8").read()
    marker = "gaokun3 tree-fix [26]"
    if marker in s:
        return "已改（幂等，无需改动）"
    old = "    if (android::C2HandleIon::IsUsed()) {\n"
    if s.count(old) != 2 or "android::C2DmaBufAllocator::Map(" not in s:
        return "⚠️ C2Buffer.cpp 里 mapReadView / mapWriteView 的 IsUsed() 分支不是正好两处，上游可能改了写法；什么都没动"
    new = ("    // " + marker + ": BLOB (gralloc-backed) linear blocks are not DMABUF handles;\n"
           "    // C2DmaBufAllocator::Map rejects them with C2_BAD_VALUE on ION-less devices.\n"
           "    if (android::C2HandleIon::IsUsed() ||\n"
           "            !android::C2DmaBufAllocator::CheckHandle(mAllocation->handle())) {\n")
    io.open(p, "w", encoding="utf-8", newline="").write(s.replace(old, new))
    return "mapReadView / mapWriteView：非 DMABUF 句柄退回 allocation->map()"


def main():
    tree = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path.home() / "crdroid").expanduser()
    if not (tree / "build/envsetup.sh").exists():
        print(f"✗ {tree} 看起来不是 Android 源码树"); sys.exit(1)
    print(f"树: {tree}")
    failed = []

    def step(label: str, result: str) -> None:
        print(label + result)
        # 以 ✗ 或 ⚠️ 开头的结果都是"没做成"：补丁打不上 / 应用失败 / 仓库里缺补丁文件，以及直接改文本的那几条
        # 找不到锚点（上游改了写法）。都不能悄悄过去 —— 2026-09-28 PR #10 审查：以后 repo sync 让 [13]/[14] 打不上时，
        # 空录音 / 整块静音会悄悄回来；[2] 失效就是传感器全没，[3][5][7] 失效就是硬解悄悄回落软解。
        # 原先这里只打印、退出码照样是 0；现在与 kernel-apply-patches.sh 的"有失败就 exit 1"一致。
        # A17 起是严格模式：17.0 树里一定有的项目缺文件 / 缺锚点也返回 ✗ / ⚠️（以前是"跳过（找不到 …）"、不算失败）。
        # 还会打印"跳过"的只剩 [4] 没同步 vendor/gapps；"已废弃（A17）"是上游已经不需要这条，也不算失败。
        if result.startswith(("✗", "⚠️")):
            failed.append(label.strip())
    step("  [1] SPOOF_SAFETYNET: ", patch_spoof_safetynet(tree))
    step("  [2] hexagonfs CR 截断: ", patch_hexagonfs_cr(tree))
    step("  [3] v4l2_codec2 输入分辨率: ", patch_v4l2_input_size(tree))
    step("  [4] GApps 冲突: ", patch_gapps_conflicts(tree))
    step("  [5] v4l2_codec2 初始输出队列: ", patch_v4l2_initial_output(tree))
    step("  [6] 关闭桌面窗口模式: ", patch_disable_desktop_mode(tree))
    step("  [7] v4l2_codec2 设备扫描范围: ", patch_v4l2_device_scan_range(tree))
    step("  [8] v4l2_codec2 HEVC CSD 合并: ", apply_patch_file(
        tree, "external/v4l2_codec2",
        "0019-v4l2-codec2-merge-hevc-csd-into-first-frame.patch"))
    # [9] glslang host 端 glslangValidator（patches/0003）—— A17 上废弃，不再打：
    #   上游 external/deqp-deps/glslang 自己带了 cc_binary_host "glslangValidator"（Android.bp:263-292，
    #   提交 9442208b "Build rule for Mesa's glslangValidator"；deqp_glslang_defaults 也加了 host_supported: true，
    #   :83），模块名与 0003 加的完全相同，再打就是重名模块（而且 0003 的上下文本来也对不上了）。
    #   mesa 侧照旧用 tools: ["glslangValidator"]，名字对得上上游。patches/0003 文件留着备查，不再有消费者。
    step(" [10] tinyalsa sw_params 取自 refined hw_params: ", apply_patch_file(
        tree, "external/tinyalsa_new",
        "0008-tinyalsa-derive-sw-params-from-refined-hw-params.patch"))
    # A17 上游已删掉这个 flag ⇒ 通常打印"已废弃（A17）"；声明回来了才沿 TARGET_RELEASE 继承链去改值文件
    step(" [11] 关掉 connected-displays flag: ", patch_connected_displays_flag(tree))
    step(" [12] audio AIDL primary 接受外部设备连接: ", apply_patch_file(
        tree, "hardware/interfaces",
        "0010-audio-aidl-primary-accept-external-device-connect.patch"))
    step(" [13] audio AIDL HAL 尊重策略给出的麦克风 address: ", apply_patch_file(
        tree, "hardware/interfaces",
        "0051-audio-aidl-honour-explicit-mic-address.patch"))
    step(" [14] audio AIDL HAL 采集方向 MonoPipe 容量翻倍（消除周期性插静音）: ", apply_patch_file(
        tree, "hardware/interfaces",
        "0052-audio-aidl-monopipe-capacity.patch"))
    # 依赖 [14]（采集管道要能放 2 块），所以排在它后面。#127 §6/§7：去掉开头 170 ms 静音与常驻 2 块延迟
    step(" [15] audio AIDL HAL 采集改成数据驱动交付（去掉开头静音与常驻延迟）: ", apply_patch_file(
        tree, "hardware/interfaces",
        "0063-audio-aidl-primary-capture-data-driven.patch",
        covered_by=("0069-audio-aidl-primary-playback-paced-by-alsa.patch",)))
    # 不依赖前面几条（只动 audio/aidl/default/apex/）。v0.7.0 验收 B2：effect HAL 在 vendor APEX 里，加载不了 /vendor/lib64/soundfx 的 Histen
    # ⚠️ 这是唯一一条会【新建】文件的 AOSP 补丁（linker.config.json，未跟踪）：把 hardware/interfaces 还原成上游时
    #    `git checkout -- .` / `reset --hard` 删不掉它，要再 `git clean -f audio/aidl/default/apex/`，否则这一条报"打不上"（2026-09-29 审查）
    step(" [16] audio APEX 链接器命名空间放行 /vendor/${LIB}/soundfx（Histen 效果库）: ", apply_patch_file(
        tree, "hardware/interfaces",
        "0068-audio-aidl-apex-permit-vendor-soundfx.patch"))
    # A17：改 StreamAlsaBase.{h,cpp} 与 StreamPrimary.cpp，上下文只和 [15]（0063）在 StreamPrimary.cpp 里重叠，所以排在它后面。#130：播放由硬件定拍、不再整块丢音乐
    step(" [17] audio AIDL HAL 播放由硬件定拍、位置按真实播出上报（卡顿不再整块丢音乐）: ", apply_patch_file(
        tree, "hardware/interfaces",
        "0069-audio-aidl-primary-playback-paced-by-alsa.patch"))
    # v1.0 NET-1 / TODO V3：软件 PNO 的第二道门（DeviceConfig wifi/software_pno_enabled，GMS 下发 false）
    step(" [18] Wi-Fi 软件 PNO 不受 DeviceConfig 覆盖（息屏断网后能自己回连）: ", patch_wifi_sw_pno_gate(tree))
    # v1.0 NET-4：稳定的设备 MAC（HAL 出厂 MAC 由 soc0 序列号派生）+ 新网络默认按网络固定的随机 MAC
    step(" [19] Wi-Fi HAL 出厂 MAC 由 SoC 序列号派生（稳定的设备 MAC）: ", patch_wifi_stable_factory_mac(tree))
    step(" [20] Wi-Fi 新网络 MAC 随机化默认改回 AUTO（按网络固定）: ", patch_wifi_mac_randomization_default(tree))
    # v1.0 AV-10：耳机麦。可插拔端口不能带 address ⇒ HAL 按设备类型读 ro.vendor.audio.primary.alsa.<type>（device.mk 设）。
    # 只动 StreamPrimary.cpp 的头文件区与 getCardAndDeviceId()，与 [12]–[17] 的上下文不重叠（构建机上对着打满的树核过：
    # 正向 --check 干净，打上之后 0010/0051/0052/0069 的反向检查仍过，撤 0069 后 0063 的反向检查仍过）。
    step(" [21] audio AIDL HAL 地址里没有 CARD_/DEV_ 时按设备类型回落（耳机麦）: ", apply_patch_file(
        tree, "hardware/interfaces",
        "0077-audio-aidl-primary-alsa-fallback-by-device-type.patch"))
    # 音游延迟：播放块按 ro.vendor.audio.primary.out_latency_ms（device.mk 设 10）⇒ FastMixer 开、游戏拿到 FAST 轨道。
    # 只动 ModulePrimary.cpp（头文件区与 getNominalLatencyMs()），与 [12]–[21] 不重叠；依赖 [17]（播放直接写 ALSA）。
    step(" [22] audio AIDL HAL 播放块大小可配（10 ms，开 FastMixer）: ", apply_patch_file(
        tree, "hardware/interfaces",
        "0083-audio-aidl-primary-low-latency-playback-block.patch"))
    # 扬声器低音处理（device/huawei/gaokun3/speakerfx）：播放块写进 ALSA 之前交给 ro.vendor.audio.primary.playback_hook 的库。
    # 只动 StreamPrimary.{h,cpp}；transfer() 的上下文依赖 [17]（0069 的播放直写），所以排在 [22] 之后。
    # 在 lineage-24.0 + 0010/0051/0052/0063/0068/0069/0077/0083 上核过：正向 --check 干净、打上后反向 --check 通过。
    step(" [27] audio AIDL HAL 播放方向的厂商钩子（扬声器低音处理，覆盖 FAST 轨）: ", apply_patch_file(
        tree, "hardware/interfaces",
        "0117-audio-aidl-primary-playback-hook.patch"))
    # 字节缓冲模式的块 ID。要和 device.mk 的 poolmask（GRALLOC 位）一起生效，见补丁说明。
    step(" [28] v4l2_codec2 字节缓冲模式用 dmabuf ID: ", apply_patch_file(
        tree, "external/v4l2_codec2",
        "0128-v4l2-codec2-use-dmabuf-id-for-gralloc-blocks.patch"))
    # 10-bit 视频：[29][30] 要和 patches/0129（iris）、0130（turnip）一起生效，见各函数说明。
    step(" [29] minigbm 给 P010 开显示用途: ", patch_minigbm_p010_scanout(tree))
    step(" [30] v4l2_codec2 声明 HEVC Main10、VP9 Profile 2: ", patch_v4l2_10bit_profiles(tree))
    # 要和内核平面列表的 ABGR2101010 一起生效；旧内核上 HWC 会保持 8 位，见补丁说明。
    step(" [31] drm_hwcomposer 给 10-bit 图层请求 10-bit 帧缓冲: ", apply_patch_file(
        tree, "external/drm_hwcomposer",
        "0131-drm-hwcomposer-10-bit-client-target.patch",
        covered_by=("0132-drm-hwcomposer-switch-panels-between-srgb-and-display-p3.patch",
                    "0137-drm-hwcomposer-dim-layers-through-the-gpu.patch",
                    "0140-drm-hwcomposer-declare-hdr-on-internal-panels.patch")))
    # 要和内核 0134 的 Colorspace 属性、device.mk 的两个属性一起生效；没有 Colorspace 属性时 HWC 保持原样。
    step(" [32] drm_hwcomposer 让内置屏在 sRGB 和 Display P3 之间切换: ", apply_patch_file(
        tree, "external/drm_hwcomposer",
        "0132-drm-hwcomposer-switch-panels-between-srgb-and-display-p3.patch",
        covered_by=("0137-drm-hwcomposer-dim-layers-through-the-gpu.patch",
                    "0140-drm-hwcomposer-declare-hdr-on-internal-panels.patch")))

    step(" [33] drm_hwcomposer 让压暗的图层走 GPU（HDR 提亮）: ", apply_patch_file(
        tree, "external/drm_hwcomposer",
        "0137-drm-hwcomposer-dim-layers-through-the-gpu.patch",
        covered_by=("0140-drm-hwcomposer-declare-hdr-on-internal-panels.patch",)))

    # 声明 HDR：[34] 让框架列出 HEVC 的 HDR10 profile，[35] 让应用看到显示器支持 HDR，两条一起才有意义。
    step(" [34] v4l2_codec2 声明 HDR 静态元数据: ", apply_patch_file(
        tree, "external/v4l2_codec2",
        "0139-v4l2-codec2-pass-hdr-static-metadata.patch"))
    step(" [35] drm_hwcomposer 内置屏声明 HDR10 和 HLG: ", apply_patch_file(
        tree, "external/drm_hwcomposer",
        "0140-drm-hwcomposer-declare-hdr-on-internal-panels.patch"))
    # 游戏环境如实修正：fingerprint 尾部 userdebug→user，与 ro.build.type=user 对齐
    # （Duck Detector system_properties 3 条 danger 的根；运行时改不掉 Build.FINGERPRINT 常量）
    step(" [23] BUILD_FINGERPRINT/THUMBPRINT/SYSTEM_FINGERPRINT 变体段改写成 user（与 ro.build.type 对齐）: ",
         patch_build_fingerprint_variant_user(tree))
    step(" [24] adb_root 只在 WITH_SU=true 时装（发布构建没有 adb root）: ", patch_adb_root_only_with_su(tree))
    step(" [25] KeyGestureController 读 LineageSettings 失败不致命（10-11 救砖兜底）: ", patch_keygesture_lineagesettings_guard(tree))
    step(" [26] codec2 BLOB 线性块在没有 ION 时也能 map（HEVC CSD 合并 / 硬解）: ", patch_codec2_blob_linear_map(tree))
    if failed:
        print(f"✗ {len(failed)} 条没做成：" + "；".join(failed))
        sys.exit(1)


if __name__ == "__main__":
    main()
