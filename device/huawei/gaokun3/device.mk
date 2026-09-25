#
# device.mk -- Huawei MateBook E Go (gaokun3)
#
# Stage 2 原则：能不装的一律不装。缺 HAL 顶多日志报错，
# 多一个坏 HAL 就可能让 init 起不来，而本机没有串口可看。
#

LOCAL_PATH := device/huawei/gaokun3

# vendor 分区的基础件（vendor_compatibility_matrix.xml、shell_and_utilities_vendor
# 即 /vendor/bin/sh + toybox_vendor 等）随 full_base.mk → … → base_vendor.mk 一起来，
# 接线在 lineage_gaokun3.mk，那里有完整的踩坑记录。

# ----------------------------------------------------------------- fstab
# 同一份 fstab 要同时进 ramdisk（first stage mount 用）和 vendor
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/fstab.gaokun3:$(TARGET_COPY_OUT_RAMDISK)/fstab.gaokun3 \
    $(LOCAL_PATH)/fstab.gaokun3:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.gaokun3

# ------------------------------------------------------------- init 脚本
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/init.gaokun3.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.gaokun3.rc \
    $(LOCAL_PATH)/init.gaokun3.usb.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.gaokun3.usb.rc \
    $(LOCAL_PATH)/ueventd.gaokun3.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.rc

# ------------------------------------------------------------------ 属性
# adb 默认开，且不要求授权 —— Stage 2 的验收就是 adb 能连上。
# persist.adb.tcp.port=5555：首次开机就把 adb over TCP 打开，
#   免得换 ROM 后 USB 侧不通就彻底失联（本机 UCSI 拔插会丢 adb，见坑 #27）。
#   属性名实名核实：packages/modules/adb/daemon/main.cpp:272-274
#   先读 service.adb.tcp.port，回落 persist.adb.tcp.port。
PRODUCT_PROPERTY_OVERRIDES += \
    ro.adb.secure=0 \
    ro.debuggable=1 \
    persist.sys.usb.config=adb \
    persist.adb.tcp.port=5555

# 屏幕密度
# [measured] 1600x2560，物理 266x166 mm -> 对角 12.34"，约 245 dpi
# 取 240（hdpi 桶），1dp=1.5px -> 1067dp 宽，平板比例合理。
# CLAUDE.md 提示可对照 Galaxy Tab S7 FE（同款面板）的取值再调。
PRODUCT_PROPERTY_OVERRIDES += \
    ro.sf.lcd_density=240

# USB gadget 控制器
# [measured] Stage 1 实测 UDC 名就是 a600000.usb
PRODUCT_PROPERTY_OVERRIDES += \
    sys.usb.controller=a600000.usb

# ------------------------------------------------- 安全 HAL（软件实现）
# keystore2 是 critical 服务且被 init.rc 的
#   exec 4 (/system/bin/vdc keymaster earlyBootEnded)
# 同步等待 —— 它起不来整个 boot 队列就堵死（实测：keystore2 连崩 52 次，
# adbd/zygote 永远排不上队，见 docs/stage2-findings.md）。
# 本机没有可用 TEE，用 AOSP 自带的软件实现（cuttlefish 同款）：
#   keymint:    hardware/interfaces/security/keymint/aidl/default/Android.bp:183
#   gatekeeper: hardware/interfaces/gatekeeper/aidl/software/Android.bp:64
PRODUCT_PACKAGES += \
    com.android.hardware.keymint.rust_nonsecure \
    com.android.hardware.gatekeeper.nonsecure

# 软件 HAL 全家桶（hardware/interfaces 各 default 实现，模块名逐一核实）。
# system_server 的 HAL 阶梯（每级都是实测 FATAL 后确认的）：
#   BatteryService     ← IHealth（"instance default isn't available"）
#   HintManagerService ← IPower（SupportInfo.headroom NPE）
# thermal/memtrack/lights/vibrator 为预防性（cuttlefish 同款 example）。
PRODUCT_PACKAGES += \
    android.hardware.health-service.example \
    android.hardware.power-service.example \
    android.hardware.memtrack-service.example \
    android.hardware.lights-service.example \
    android.hardware.vibrator-service.example

# 音频 HAL（AIDL 示例实现，null 音频）—— audioserver 没有 HAL 会 NPE
# 崩溃循环，而 system_server 主线程在 AudioService 构造时【同步阻塞】
# 等 IAudioPolicyService，等不到就被看门狗处决 → zygote 全家轮回
# （ANR trace 实锤，见 docs/stage2-findings.md）。audioserver 必须活。
# Stage 4 换 tinyhal 真声卡时再替换。
# ⚠️ example service 是 installable:false（bp 明写"installed in apex
# com.android.hardware.audio"），直接列包名会被静默丢弃——用 APEX：
PRODUCT_PACKAGES += \
    com.android.hardware.audio

# ═══ 温控 HAL：2026-08-24 换成自研的真 HAL ═══
# 原先装的是 AOSP mock（apex com.android.hardware.thermal，里面那个
# android.hardware.thermal-service.example 是 installable:false 的，
# 所以当年只能列 APEX 名 —— 直接列 binary 名 kati 会报
# "includes non-existent modules in PRODUCT_PACKAGES" 并中止构建）。
#
# ⚠️★★ 换掉它是【有危险】的一步，务必连阈值一起改：mock 报的 skin/battery
#   SHUTDOWN 阈值只有 36.0 °C，而 ThermalManagerService.shutdownIfNeeded()
#   一到 SHUTDOWN 就 powerManager.shutdown()。本机温区【空载就 36–37 °C】——
#   只换 HAL 不改阈值 = 开机几分钟自动关机。新阈值与理由见 thermal/Thermal.cpp。
# ★ 排除 mock 的办法只能是【不装那个 APEX】：overrides: 管不到 APEX 打包件，
#   而 PRODUCT_PACKAGES 只能加不能减 —— 所以这里是把那一行整个换掉。
PRODUCT_PACKAGES += \
    android.hardware.thermal-service.gaokun3

# effects HAL 启动即退（"config file audio_effects_config.xml not found"，
# 实测）。默认配置的 prebuilt_etc 被 soong config 门控着
# （hardware/interfaces/audio/aidl/default/Android.bp:372-378）：
#
# ★ 2026-09-25：开关改为 false —— 改由我们自己那份配置接管。
#   已读该 Android.bp 原文确认：这个 soong config **只门控那一个
#   prebuilt_etc 的 enabled**，不参与任何编译期决策，所以关掉是安全的。
#   ⚠ 但它与下面那行 PRODUCT_COPY_FILES 是成对的：关掉而没装上自己的配置，
#     HAL 会因找不到配置文件启动即退（正是当初设 true 的原因）。
$(call soong_config_set_bool,hardware_interfaces_audio,use_default_audio_effects_config,false)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/effects/audio_effects_config.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_effects_config.xml

# 扬声器后处理链：自研 AIDL effect（LR4 高通 + EQ + 限幅 + 软削波）。
# 与 stock 配置的差别是纯增量（stock 的 21 库/18 effect 全保留，只多一个
# gaokun_histen 槽位与一条 music postprocess）—— 已用脚本逐项比对过。
# 详见 device/huawei/gaokun3/effects/README.md。
PRODUCT_PACKAGES += \
    libgaokunhisteneffect

# ⚠ 下面这份 libhw_histen_processing.so 是**华为专有二进制**（iMedia Audio 8.0 /
#   Histen 6.1.9，从麒麟 V10 SP1 的音频栈提取，未获再分发授权）—— 这是本仓库里
#   唯一一个【没有】按 firmware/ 、hexagonrpcd-root/ 、prebuilt-boot/ 那套
#   「整目录忽略 + 只放行 README.md」约定处理的专有 blob，属于有意偏离。
#   来源、限制与替代做法见 device/huawei/gaokun3/effects/README.md 第七节。
#   不想要它：删掉下面那行 PRODUCT_COPY_FILES 与 effects/prebuilt/ 整个目录即可，
#   其余部分照常工作 —— effect 会在引擎缺失时自动降级为逐比特直通，不会哑。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/effects/prebuilt/lib64/soundfx/libhw_histen_processing.so:$(TARGET_COPY_OUT_VENDOR)/lib64/soundfx/libhw_histen_processing.so

# 音频 policy 配置 —— example HAL 的 IModule 实例清单【完全来自】
# audio_policy_configuration.xml 解析结果（main.cpp:93-99 实名核实），
# 没有它 HAL 只注册 IConfig，audioserver 等 IModule/default 永阻塞。
# xml 抄 cuttlefish（同款 HAL），XInclude 相对路径要求全家同目录：
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    $(LOCAL_PATH)/audio/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/bluetooth_with_le_audio_policy_configuration_7_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/bluetooth_with_le_audio_policy_configuration_7_0.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml

# ------------------------------------------------------------------ 固件
# [measured] 全部来自 Stage 0 的 dmesg 固件加载路径。
# 配合 cmdline 里的 firmware_class.path=/vendor/firmware/，
# 必须保持相同的子路径结构。
# 华为那三个 .mbn 不在 linux-firmware 里，需从当前 Linux 系统
# /lib/firmware/ 下取出后放进 firmware/ 目录再打包。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/firmware/qcom/a660_sqe.fw:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/a660_sqe.fw \
    $(LOCAL_PATH)/firmware/qcom/a660_gmu.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/a660_gmu.bin \
    $(LOCAL_PATH)/firmware/qca/wcnhpbtfw21.tlv:$(TARGET_COPY_OUT_VENDOR)/firmware/qca/wcnhpbtfw21.tlv \
    $(LOCAL_PATH)/firmware/qca/wcnhpnv21g.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/qca/wcnhpnv21g.bin \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn

# ------------------------------------------------- Stage 3: 图形栈
# 全套照抄 device/linaro/dragonboard/shared/graphics/（db845c 同款），
# 属性名/包名均为树内实名，非记忆。density 用我们实测的 240（不抄他家 160）。

# gralloc：minigbm，platform=msm 是高通 UBWC 后端
# （external/minigbm/Android.bp 的 soong_config_variable("minigbm","platform")）
$(call soong_config_set,minigbm,platform,msm)
PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator-service.minigbm \
    mapper.minigbm
PRODUCT_PROPERTY_OVERRIDES += \
    ro.hardware.gralloc=minigbm

# hwcomposer：drm_hwcomposer 的 HWC3 APEX
PRODUCT_PACKAGES += \
    com.android.hardware.graphics.composer.drm_hwcomposer

# GLES/Vulkan：Phase A = swangle（ANGLE over SwiftShader，纯树内 CPU 渲染）。
# ⚠️ 树内 mesa3d 不含 freedreno（见 BoardConfig 注释），Phase B 再换。
# 模板：dragonboard/shared/graphics/swangle/device.mk（db845c 同款）。
PRODUCT_PACKAGES += \
    libEGL_angle \
    libGLESv1_CM_angle \
    libGLESv2_angle \
    vulkan.pastel
PRODUCT_PROPERTY_OVERRIDES += \
    ro.opengles.version=196608
# ⚠️ ANGLE 库在 /system/lib64/ 根（AOSP 16 默认自带，不在 egl/ 子目录），
# 加载开关是 persist.graphics.egl（Loader.cpp:67-70 实名核实），
# ro.hardware.egl 走的是 egl/libEGL_*.so 搜索路径，对 ANGLE 不生效。
PRODUCT_PROPERTY_OVERRIDES += \
    persist.graphics.egl=angle
PRODUCT_VENDOR_PROPERTIES += \
    debug.hwui.renderer=skiagl

# ⚠️ 软渲染（SwiftShader）导入不了 UBWC 压缩 buffer —— SF 崩于
# "Failed to create a valid texture"（GaneshBackendTexture 导入
# AHardwareBuffer 失败，tombstone_48 实测）。强制 minigbm 分配线性 buffer。
# 来源：dragonboard minigbm_msm/device.mk 的 TARGET_USES_SWR 分支。
# Phase B 换 freedreno 后删掉这行（GPU 认 UBWC，还能提性能）。
PRODUCT_VENDOR_PROPERTIES += \
    vendor.minigbm.debug=nocompression

# ⚠️ simpledrm 先占 card0 后被 msm 顶替，msm 的 KMS 节点是 card1；
# drm_hwcomposer 默认扫 card0 会进无头模式（"No pipelines available"）。
# 活体实测：设此属性 + 重启 hwc 后 SF 立即拿到 Primary display
# 1600x2560@120Hz。属性名从 hwc3 二进制 strings 核实。
PRODUCT_VENDOR_PROPERTIES += \
    vendor.hwc.drm.device=/dev/dri/card1

# 慢设备处方（SwiftShader CPU 渲染下时序没有余量）：
# 看门狗/ANR 超时统一 ×5。cycle-1 的 AudioService 等 audioserver 发布
# 差几十秒被 60s 看门狗击杀 → 级联轮回（ANR 实锤）。Phase B 换
# freedreno 后可降回 2 或删除。
PRODUCT_VENDOR_PROPERTIES += \
    ro.hw_timeout_multiplier=5

# 硬件 feature 声明 —— 没有它 AppWidgetService 等一票系统服务不启动，
# Launcher 直接 NPE（"AppWidgetManager...on a null object" 实测）。
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/tablet_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/tablet_core_hardware.xml

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.opengles.aep.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.opengles.aep.xml \
    frameworks/native/data/etc/android.hardware.vulkan.level-1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.level.xml

# ★ Vulkan 1.3（2026-08-24 从 1.1 提上来）。依据是 turnip 自己的代码，不是估计：
#   mesa 26.0 src/freedreno/vulkan/tu_device.cc:1190
#       props->apiVersion = tu_has_multiview(pdevice)
#           ? ((chip >= 7) ? TU_API_VERSION : VK_MAKE_VERSION(1, 3, ...))
#           : VK_MAKE_VERSION(1, 0, ...);
#   Adreno 690 是 a6xx（chip 6）⇒ 走 1.3 那一支；而 tu_has_multiview() 取的是
#   props.has_hw_multiview，freedreno_devices.py 里 a690 = [a6xx_base, a6xx_gen4]，
#   而 a6xx_base 的 has_hw_multiview = True（False 的是 A702 那一档）。
#   ⇒ turnip 在本机实报 1.3，声明 1.3 是照实写，不是往高了报。
# ⚠️ 不声明 android.software.vulkan.deqp.level —— 那要跑 dEQP 才能背书，我们没跑。
# ★ 顺带补上 compute：Vulkan 1.1+ 的核心里就有计算着色器，之前只声明了
#   level 与 version，缺 compute 在真机上是不常见的组合。
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.vulkan.compute-0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.compute.xml \
    frameworks/native/data/etc/android.hardware.vulkan.version-1_3.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.version.xml

#
# Stage 3 之后再加（每次只加一个）：
#   音频   tinyhal（refs/aospm-tinyhal），源是 LENOVO-X13s.conf / sc8280xp.conf
#   WiFi   wpa_supplicant + ath11k
#   传感器 / 相机 / 振动
#

# ─── Stage 4: WiFi（ath11k 主线 + AIDL HAL APEX + wpa_supplicant）───
# ★ wpa_cli：诊断用的控制台客户端。之前没装，于是 WPA3（issue #2）
#   最直接的那条验证路——问 supplicant 自己 sae_pwe / 支持哪些 key_mgmt
#   ——在设备上根本跑不了，而 wpa_supplicant.rc 明明开着控制接口。
#   模块名已核：external/wpa_supplicant_8/wpa_supplicant/Android.bp:1272
#   是 cc_binary + proprietary:true ⇒ 装到 /vendor/bin/wpa_cli，
#   依赖只有 libcutils/liblog（Soong 自己拉 vendor 变体，
#   不会重演 tinymix 那个 "放进 /vendor/bin 但 .so 在 /system" 的坑）。
#   ⚙ 用法：wpa_cli -p /data/vendor/wifi/wpa/sockets -i wlan0 <cmd>
#   ⚠ 那个目录是 0770 wifi:wifi（见 wifi/wpa_supplicant.rc），
#     shell 不在 wifi 组里 ⇒ 要 root 才能连上。
PRODUCT_PACKAGES += \
    com.android.hardware.wifi \
    wpa_supplicant \
    wpa_cli

# ★ Wi-Fi 的 TCP 缓冲区上限调大（rro/Gaokun3WifiOverlay，#119）：国内到海外 CDN 的 RTT
#   约 300 ms，AOSP 默认 2 MB 的接收上限把单连接卡在 ~3.5 MB/s；8 MB 实测 ~9.5 MB/s。
#   系统内 OTA 就是单连接下载。
PRODUCT_PACKAGES += \
    Gaokun3WifiOverlay

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/wifi/wpa_supplicant.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/wpa_supplicant.rc \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml

# WCN6855 固件（board-2.bin 已验含 NTM_TW220，DTS qcom,calibration-variant 所需）
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/firmware/ath11k/WCN6855/hw2.0/amss.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ath11k/WCN6855/hw2.0/amss.bin \
    $(LOCAL_PATH)/firmware/ath11k/WCN6855/hw2.0/board-2.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ath11k/WCN6855/hw2.0/board-2.bin \
    $(LOCAL_PATH)/firmware/ath11k/WCN6855/hw2.0/m3.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ath11k/WCN6855/hw2.0/m3.bin \
    $(LOCAL_PATH)/firmware/ath11k/WCN6855/hw2.0/regdb.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ath11k/WCN6855/hw2.0/regdb.bin

PRODUCT_VENDOR_PROPERTIES += \
    wifi.interface=wlan0

# 实机芯片是 wcn6855 hw2.1（dmesg 实测）；上游 WHENCE 将 hw2.1 软链到 hw2.0，
# vendor 里直接把 hw2.0 文件再装一份到 hw2.1 路径
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/firmware/ath11k/WCN6855/hw2.0/amss.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ath11k/WCN6855/hw2.1/amss.bin \
    $(LOCAL_PATH)/firmware/ath11k/WCN6855/hw2.0/board-2.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ath11k/WCN6855/hw2.1/board-2.bin \
    $(LOCAL_PATH)/firmware/ath11k/WCN6855/hw2.0/m3.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ath11k/WCN6855/hw2.1/m3.bin \
    $(LOCAL_PATH)/firmware/ath11k/WCN6855/hw2.0/regdb.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ath11k/WCN6855/hw2.1/regdb.bin

# goldfish 命名空间：libwifi-hal-emu（mainline nl80211 通用 wifi HAL 实现）在里面
PRODUCT_SOONG_NAMESPACES += device/generic/goldfish

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/wifi/wpa_supplicant.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant.conf

# ─── 相机 HAL（AIDL，基于 libcamera 的软件 ISP）───
# 背景：三条便宜路都走不通，只能自己写 AIDL（docs/stage4-findings.md #91）：
#   ❌ HIDL + 上游 provider@2.4-legacy —— 本机 hwservicemanager 根本不存在，
#      且 FCM 202504 的兼容性矩阵里 camera.provider 只剩 format="aidl"
#   ❌ libcamera 的 V4L2 垫片 + AOSP 的 ExternalCameraProvider —— 软件 ISP
#      只出 RGB 族、没有 YUYV（和 #84 撞同一堵墙）
# ✅ libcamera 现在在 AOSP 里编（external/libcamera，见 patches/libcamera/）。
#   ⚠️ 仍欠一笔：那 5 个生成的 .cpp 与整批生成头要先跑一次 meson 产出再拷贝，
#   是手动步骤（#82/#85 的形状）。正解是 Soong genrule 跑 libcamera 自带的
#   Python 生成器。构建 ROM 前请确认 external/libcamera/generated/ 已就位。
PRODUCT_PACKAGES += \
    android.hardware.camera.provider-service.gaokun3 \
    libcamera_gk3 \
    libcamera_base_gk3 \
    libcamera_ipa_softisp_gk3

# ⚠️★★ 相机设备节点的权限**并进 ueventd.gaokun3.rc**，不要另开一个文件往
#   /vendor/etc/ueventd.rc 拷 —— 那个目标只能有一份，另拷一份会把原有的
#   GPU 渲染节点 / FastRPC 传感器 / Venus 三组规则全部覆盖掉。
#   （我 2026-09-13 就是这么干的，在设备上把它们冲了；构建期会表现为
#    PRODUCT_COPY_FILES 目标重复。）
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/camera/gaokun3-camera-features.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/gaokun3-camera-features.xml \
    $(LOCAL_PATH)/camera/ipa-data/softisp/hi846.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/ipa/softisp/hi846.yaml \
    $(LOCAL_PATH)/camera/ipa-data/softisp/ov13b10.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/ipa/softisp/ov13b10.yaml \
    $(LOCAL_PATH)/camera/ipa-data/softisp/uncalibrated.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/ipa/softisp/uncalibrated.yaml

# ⚠️★ 软件 ISP 的 IPA 【必须】能读到调优文件，否则 IPASoftIsp::init() 直接返回
#   错误 → "Failed to create software ISP, disabling software debayering"
#   → libcamera 退回原始拜耳 → 我们要的 RGB888 不可用 → configure 被调整成
#   SGBRG10_CSI2P/RAW → STREAMON 失败。整条因果链里**没有一处提到调优文件**。
#   路径布局是 <IPA_CONFIG_DIR>/<ipa名>/<文件名>（libcamera ipa_proxy.cpp:57），
#   而 IPA_CONFIG_DIR 由我们自己的 config.h 定成 /vendor/etc/libcamera/ipa。

# ─── Stage 4: 蓝牙（WCN6855 / hci_qca，AOSP 原装 HAL 直接可用）───
# ⚠️ 2026-08-19 发现：#34 记了"把这个 HAL 推进 vendor 即可"，但那句话
#    从没变成一行构建配置 —— Stage 4 是走 adb remount 的 overlay 推的。
#    这是本轮第三个同类漏网（前两个：拓扑固件名、audio-route.sh）。
#
# 为什么原装 HAL 就够（无需改一行代码）：
#   hardware/interfaces/bluetooth/aidl/default/BluetoothHci.cpp:172
#   先试 NetBluetoothMgmt::openHci()（BT 管理 socket + HCI_CHANNEL_USER），
#   失败才退回串口路径 —— 正好对上主线内核的 hci0。
# 模块自带 init_rc 与 vintf_fragments；android.hardware.bluetooth-V1-ndk
# 是它的 shared_libs，会自动随包安装（当初 overlay 手推才要单独补那个 .so，
#   少了它是 CANNOT LINK EXECUTABLE 的 5 秒重启循环）。
#
# ⚠️ 蓝牙能不能真的起来还取决于内核：CONFIG_RT_GROUP_SCHED 必须为 n，
#    否则 bt_main_thread 拿不到 SCHED_FIFO → bluetooth::log::fatal。
#    kb21 已经关掉，scripts/kernel-config-android.sh 里有断言守着。
PRODUCT_PACKAGES += \
    android.hardware.bluetooth-service.default

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml

# ─── 蓝牙 profile 开关（用户报「耳机配得上、用不了」，2026-08-23）───
# ★ 根因：AOSP 14 起每个 profile 由一条 sysprop 单独开关，而【不设 = 关闭】。
#   packages/modules/Bluetooth/.../a2dp/A2dpService.java:
#       public static boolean isEnabled() {
#           return BluetoothProperties.isProfileA2dpSourceEnabled().orElse(false);
#       }
#   `.orElse(false)` 就是判决书。本机此前一条 bluetooth.* 属性都没有
#   （实测 `getprop | grep -c "^\[bluetooth\."` = 0），于是只有 AdapterService
#   在跑 —— 配对是 adapter 的活所以配得上，放音是 profile 的活所以没有。
#
# ★ 属性名取自 system/libsysprop/srcs/android/sysprop/BluetoothProperties.sysprop
#   （android-16.0.0_r4），不是凭记忆写的。
# ★ 集合参照 LineageOS android_device_essential_mata/vendor.prop，两处不同：
#   · 去掉 sap.server —— SIM 卡访问，本机无 modem 无 SIM，开了没有意义
#   · LE Audio 全家（bap/ccp/csip/hap/mcp/vcp/bass）暂不开 —— 半开的 LE Audio
#     正是「连上了却没声」这一类故障的经典来源，等有设备能验再说
#
# 实测（运行期 setprop + 重启蓝牙，构建戳 1787436126）：
#   设之前 `setProfileServiceState` 一条不出、A2dpService 不存在；
#   设之后 12 个 profile 拉起，`btif_av.cc:3813 btif_av_source_execute_service:
#   enable=true`。
PRODUCT_VENDOR_PROPERTIES += \
    bluetooth.profile.gatt.enabled=true \
    bluetooth.profile.a2dp.source.enabled=true \
    bluetooth.profile.avrcp.target.enabled=true \
    bluetooth.profile.hfp.ag.enabled=true \
    bluetooth.profile.hid.host.enabled=true \
    bluetooth.profile.hid.device.enabled=true \
    bluetooth.profile.bas.client.enabled=true \
    bluetooth.profile.asha.central.enabled=true \
    bluetooth.profile.opp.enabled=true \
    bluetooth.profile.pan.nap.enabled=true \
    bluetooth.profile.pan.panu.enabled=true \
    bluetooth.profile.pbap.server.enabled=true \
    bluetooth.profile.map.server.enabled=true

# ─── Stage 4/5: 固件双路安装 ───
# 新增固件（从本机 Ubuntu /lib/firmware 提取，华为专有，不入版本库）：
#   qcdxkmsuc8280.mbn   GPU zap shader（freedreno 必需，缺则 GPU 锁在安全模式）
#   audioreach-tplg.bin 音频拓扑（缺则声卡不注册）
#   *.jsn               pd_mapper 服务表
# ★★ 拓扑固件必须装成【内核实际请求的那个名字】：
#     sound/soc/qcom/qdsp6/topology.c:1320 拼的是
#         qcom/<card->driver_name>/<card->name>-tplg.bin
#     本机 = qcom/sc8280xp/SC8280XP-HUAWEI-GAOKUN3-tplg.bin
#     （dmesg 实测：qcom-apm: tplg firmware loading ... failed -2，
#       见 docs/stage4-findings.md #33 第 219 行）
#     老规矩的 HUAWEI/gaokun3/audioreach-tplg.bin 内核【从不去读】，
#     两份内容 sha256 完全相同（24296 字节），所以同一个源文件装两遍。
#
# ⚠️ 这一条 Stage 4 只在实机 overlay 里手动补过，从没写进构建配置
#     —— 2026-08-19 转 crDroid 时才发现（否则 crDroid 首boot 声卡不注册）。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcdxkmsuc8280.mbn:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcdxkmsuc8280.mbn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sc8280xp/SC8280XP-HUAWEI-GAOKUN3-tplg.bin \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/adspr.jsn:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/adspr.jsn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/adspua.jsn:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/adspua.jsn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/battmgr.jsn:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/battmgr.jsn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/cdspr.jsn:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/cdspr.jsn

# ramdisk 副本：让 remoteproc/GPU/BT 在 /vendor 挂载前的首次 probe 就拿到固件
# （ramdisk 是第一阶段 rootfs，firmware_class.path 找不到会回落 /lib/firmware）
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn:ramdisk/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn:ramdisk/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn:ramdisk/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcdxkmsuc8280.mbn:ramdisk/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcdxkmsuc8280.mbn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin:ramdisk/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin:ramdisk/lib/firmware/qcom/sc8280xp/SC8280XP-HUAWEI-GAOKUN3-tplg.bin \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn:ramdisk/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/adspr.jsn:ramdisk/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/adspr.jsn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/adspua.jsn:ramdisk/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/adspua.jsn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/battmgr.jsn:ramdisk/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/battmgr.jsn \
    $(LOCAL_PATH)/firmware/qcom/sc8280xp/HUAWEI/gaokun3/cdspr.jsn:ramdisk/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/cdspr.jsn

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/firmware/qca/wcnhpbtfw21.tlv:ramdisk/lib/firmware/qca/wcnhpbtfw21.tlv \
    $(LOCAL_PATH)/firmware/qca/wcnhpnv21g.bin:ramdisk/lib/firmware/qca/wcnhpnv21g.bin

# ─── Stage 5 Phase B: 硬件 Vulkan（turnip / freedreno on 主线 msm DRM）───
# 构建流程见 docs/stage5-freedreno.md：
#   1. scripts/mesa-tool-fixes.py     补 meson_to_hermetic 与 mesa 25.3 的 API 落差
#   2. 生成器 + scripts/mesa-bp-merge.py 产出 external/mesa3d/Android.bp
#   3. mesa/turnip-shared.bp.in       把静态库包成 Android Vulkan HAL 共享库
# GLES 仍由 ANGLE 提供，但它的 Vulkan 后端从此跑在 Adreno 690 上而不是 SwiftShader。
# ⚠️ GPU 需要 zap shader 固件 qcdxkmsuc8280.mbn（见上面的固件双路安装），
#    缺了它 GPU 停在安全模式，adreno probe 会失败。
# Stage 6 M3（2026-08-19）：管线已在 crDroid 树上跑通并打开。
# crDroid 的 external/mesa3d 与 Stage 5 打补丁那棵是【同一个 commit】
# （d4b6f1eba289… @ android-16.0.0_r4，mesa 25.3.0-devel），
# 所以 Stage 5 的补丁树逐字可套，不需要重新对齐生成管线。
PRODUCT_PACKAGES += \
    vulkan.freedreno

# 排障开关：想回软渲染就把这行改回 pastel（vulkan.pastel 包仍然装着，
# 两个 HAL 共存于 /vendor/lib64/hw/，只由这条属性决定 libvulkan 加载谁）。
PRODUCT_VENDOR_PROPERTIES += \
    ro.hardware.vulkan=freedreno

# GPU SMMU stall 解锁器（常驻安全网）。装它的理由、时序约束与
# NCB=2 的血泪教训见 etc/smmustall.rc 与 bin/smmu-nostall.sh 的注释。
# ⚠️ 这两个文件在 Stage 5 只通过 adb push 进过设备，从没写进构建配置 ——
#    与 audio-route.sh / tplg.bin 是完全同一类漏网。照原样构建会得到
#    「跑着 turnip 但没有安全网」，第一次 GPU 页错误就永久挂死且无法自愈。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/bin/smmu-nostall.sh:$(TARGET_COPY_OUT_VENDOR)/bin/smmu-nostall.sh \
    $(LOCAL_PATH)/etc/smmustall.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/smmustall.rc

# ★ 资源 overlay —— 把 crDroid 的系统内 Updater 指向本项目（M6）。
#
# 为什么必须是【构建期】overlay 而不是运行时 RRO：
#   packages/apps/Updater 没有为这些字符串声明 <overlayable>（实测 grep 全空），
#   而 Android 10 起运行时 overlay 只能改目标声明过 overlayable 的资源，
#   RRO 会被直接拒掉。
# 为什么必须改资源而不是设个属性：
#   UpdatesNetworkDataSource.kt 只读 R.string.updater_server_url，
#   没有任何属性可以覆盖它（grep 确认）。
#
# overlay 目录下的路径要镜像【模块自己的 resource_dirs】：
#   packages/apps/Updater/app/Android.bp → resource_dirs: ["src/main/res"]
# 所以是 overlay/packages/apps/Updater/app/src/main/res/…
DEVICE_PACKAGE_OVERLAYS += device/huawei/gaokun3/overlay

# ★ 用户态 CPU 温控已退役（M6，2026-08-20）——改由 patches/0009 在 DTS 里根治。
#
# 原先这里装 bin/thermal-guard.sh + etc/thermalguard.rc，因为主线
# sc8280xp.dtsi 里总共只有一个 cooling-maps（在 gpu-thermal 下），
# 8 个 cpuN-thermal 只有一条 110C 的 critical trip、没有任何 cooling device
# —— CPU 会一路满频跑到内核紧急关机，中间没有渐进降频。
#
# patches/0009-arm64-dts-sc8280xp-add-cpu-cooling-maps.patch 给这 8 个温区
# 各加了一条 85C 的 passive trip 并绑到本簇的 cpufreq cooling device。
# 实机验证（同一台机器，换 DTB 前后对比）：
#     改前  cdev=0 trip=1
#     改后  cdev=1 trip=2   trip0=85000/passive  trip1=110000/critical
#           cpu0-thermal -> cpufreq-cpu0     cpu4-thermal -> cpufreq-cpu4
#
# ★ 为什么必须【撤掉】用户态那个，而不是"留着当双保险"：
#   温区一旦绑定 cooling device，cur_state 就归内核 thermal core 管
#   （step_wise 会持续写它）。用户态再去写同一个节点就是两个调节器互相打架,
#   互相覆盖对方的决定。脚本本身留在仓库里（带失效说明），给还在用旧 DTB
#   的人用。
#
# turnip 调试旗标加载器（快速迭代机制，见 docs/stage5-freedreno.md）
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/bin/tu_debug_loader.sh:$(TARGET_COPY_OUT_VENDOR)/bin/tu_debug_loader.sh

# ─── Stage 4 的音频路由（Android 没有 ALSA UCM，混音器要自己摆）───
# ⚠️ 2026-08-19 发现：这两个文件在 Stage 4 时【只通过 adb remount 的 overlay】
#    进过设备，从没写进构建配置 —— 和 SC8280XP-HUAWEI-GAOKUN3-tplg.bin
#    是完全同一类漏网。照原样构建 crDroid 会变成「声卡注册了但没人配路由」，
#    症状是能播放却没有声音，而且本机没有串口，只能靠 logcat 猜。
# 路由序列的来历、BOOST 关闭与 PA=12 的取值理由见 bin/audio-route.sh 的注释。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/bin/audio-route.sh:$(TARGET_COPY_OUT_VENDOR)/bin/audio-route.sh \
    $(LOCAL_PATH)/etc/audioroute.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/audioroute.rc

# 音频/蓝牙死锁取证看门狗（findings #38）。用户报过"长期运行后音频与蓝牙
# 可能死锁"，而我们一次都没复现过 —— 现实是死锁时用户只会重启，证据就没了。
# 探针只读 /proc 线程状态（不跑 dumpsys），60 秒一次、每次几毫秒；
# 判据是【同一个 tid 连续三次采样都在 D 状态】，不是"出现过 D"。
# 命中后采一份到 /data/vendor/gaokun3/hangdump-<uptime>/，每次启动只采一份。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/bin/gaokun3-hangdump.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gaokun3-hangdump.sh \
    $(LOCAL_PATH)/bin/gaokun3-rproc-kick.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gaokun3-rproc-kick.sh \
    $(LOCAL_PATH)/etc/hangdump.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hangdump.rc

# 挂起前把 a600000.usb 的 USB role 切到 host —— 那个控制器停在 role=device 时，
# 设备挂起阶段会【整板复位】且不留任何日志；而 USB adb 的 UDC 就在它上面，
# 所以不能简单把 DTS 改成 host。见 docs/stage4-findings.md #52 / #54 / #56。
# ★ 默认【启用】（persist.vendor.gaokun3.allow_suspend=1；2026-09-18～09-24 开发期曾是 0，见下面 ②）。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/bin/gaokun3-usbrole.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gaokun3-usbrole.sh \
    $(LOCAL_PATH)/etc/usbrole.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/usbrole.rc

# ⚠️★★ 2026-09-18 两处改动，都要读一遍再动：
#
# ① 属性【改名】persist.gaokun3.* → persist.vendor.gaokun3.*。
#    原名落在 property_contexts 的兜底 `*` 上 ⇒ default_prop ⇒
#    `neverallow { domain -init } default_prop:property_service set`
#    ⇒ 转 enforcing 之后连 adb shell setprop 都写不了。而 vendor 的
#    property_contexts 只许标注 vendor 前缀（VTS 强制），所以改名是唯一出路。
#    新名字的类型与权限见 sepolicy/property_contexts + sepolicy/vendor_gaokun3_props.te。
#    ⚠️ persist.sys.gaokun3.*（键盘、触摸模式）【不要】跟着改 —— 那两个靠
#      system_prop 才让 Parts 应用写得了，改成 vendor 前缀反而会写不了。
#
# ② 默认值 1 → 0（= 待机默认【关】）。这是用户 2026-09-18 的决定：
#    改名会让已装机器上现有的 persist.gaokun3.allow_suspend=0 失效，
#    而本机正是 0（见 CLAUDE.md 状态框）。默认改成 0，改名前后行为一致。
#    ⚠️ 代价说清楚：**新装机的用户默认也不进 s2idle**，息屏耗电按不睡算 ——
#      这与 v0.3.0～v0.6.2 的镜像默认相反，发版说明里必须写。
#      要开：adb shell setprop persist.vendor.gaokun3.allow_suspend 1（persist 属性，重启不丢）。
#    ★ 2026-09-23 用户定：开发期保持 0；【正式版】发布前改回 1（老用户 OTA 后不能丢待机，
#      TODO S1）。改回 1 之后，开发机自己 setprop … 0（persist 属性重启不丢）。
#    ✅ 2026-09-24 已改回 1（这一版起的构建就是 v0.6.3 的候选版）。开发机已显式
#      setprop persist.vendor.gaokun3.allow_suspend 0，并核对过它进了 /data/property/persistent_properties
#      （之前只有旧名字存着，新名字一直是镜像默认值，没落盘）。
#    ⚠️ 默认 1 + 持久 0 的开机时序：属性触发器在持久属性加载【之前】就按镜像默认值跑过一轮，
#      于是 init.gaokun3.rc 的 `=1` 触发器会放掉 gaokun3_nosuspend；挡住挂起的是 usbrole.rc
#      `on init` 拿的 gaokun3_usbrole（只有 allow_suspend=1 且息屏才会去放）。v0.6.2 时期本机就是
#      这个组合、没有睡过；装机后仍要 `cat /sys/power/wake_lock` 核对一次（TODO 验收清单）。
#
# 开启后：息屏切 role=host（挂起安全）、亮屏切回 device（USB adb 可用）。
# 实测 Android 真实挂起/唤醒 ×4 零复位、救援 Ubuntu systemctl suspend 3/3。
# ⚠️ 开启的可见代价：息屏时 USB device-mode adb 断开，亮屏恢复；TCP adb 不受影响。
#
# ★ 变量也换了：PRODUCT_PROPERTY_OVERRIDES 是 build/make 明确标了
#   "TODO(b/117892318) deprecate this ... in favor of PRODUCT_VENDOR_PROPERTIES"
#   的老写法（core/product.mk:92-93），两者都落到 /vendor/build.prop
#   （core/sysprop.mk:194-205 的 _prop_vars_）。既然要动这一行，顺手换成新的。
PRODUCT_VENDOR_PROPERTIES += \
    persist.vendor.gaokun3.allow_suspend=1

# ★ WindowManager 每显示器设置：关掉大屏默认的 ignoreOrientationRequest。
# 不装它 → 应用请求横屏时系统不转屏而是把应用信箱化（原神被压成 1600x1000）。
# 完整机制、实测症状与格式依据见 etc/display_settings.xml 的注释。
PRODUCT_COPY_FILES += $(LOCAL_PATH)/etc/display_settings.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display_settings.xml

# ★ tinyalsa 工具集 —— 又一个同类漏网（2026-08-19 M3 上机才发现）。
# audio-route.sh 第一件事就是找 tinymix，找不到就 `log 找不到 tinymix，放弃; exit 1`。
# 而 tinymix 从来【没有】被列进 PRODUCT_PACKAGES：Stage 4 时它是手动 push 进
# 设备的，于是 crDroid 上表现为「声卡注册了、服务也跑了，但混音器一个控件都没设」——
# 播放不报错、就是没声音，比彻底坏掉更难查。
# tinyplay/tinycap/tinypcminfo 一并装上：没有 framework 的时候，
# `tinyplay x.wav -D 0 -d 1` 是唯一能证明"硬件确实出声"的手段（扬声器是 hw:0,1）。
PRODUCT_PACKAGES += \
    tinymix \
    tinyplay \
    tinycap \
    tinypcminfo

# ─── Stage 6: 修正 /sys/fs/bpf 的 SELinux 标签（主线内核 vs Android 的不兼容）───
# 不装它 → ClatCoordinator 标签比对失败 → system_server 崩溃循环，开不进桌面。
# 完整机制、对照实验与时序依据见 bin/bpf-relabel.sh 的注释。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/bin/bpf-relabel.sh:$(TARGET_COPY_OUT_VENDOR)/bin/bpf-relabel.sh \
    $(LOCAL_PATH)/etc/bpfrelabel.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/bpfrelabel.rc

# ─── 传感器：hexagonrpcd 给 SLPI 上的 DSP 当只读文件服务器 ───
#
# 本机没有任何 AP 侧传感器芯片驱动，整套传感器（sh3001 IMU / tcs3701 光感 /
# 铰链角 …）跑在 SLPI DSP 上，AP 够不着那些总线。可行的通路是反过来：
# AP 通过 FastRPC 把一组配置文件【服务给 DSP 读】，DSP 起 SSC，
# 再由 QRTR 上的 QMI 服务 400 把读数送回来。
#
# 已在救援 Linux 上实测通过：静止时加速度计 Z≈9.87 m/s²，15 秒 131 行读数。
# 完整案卷 docs/stage4-findings.md #37，Linux 侧一键复现 scripts/slpi-sensors-setup.sh。
#
# ★ hexagonrpcd 上游自带 Android.bp 与 hexagonrpcd-sdsp.rc（cc_binary + vendor:true，
#   service 跑在 system:system，-R /vendor/etc/hexagonrpcd-root）——所以这里
#   只需要把包加进来 + 把 VFS 根装到那个路径。
#   项目由 manifests/local_manifest_gaokun3.xml 拉取；
#   \r 截断补丁由 scripts/crdroid-tree-fixes.py 打（上游没有，缺了 DSP 读不到 registry）。
# ⚠️ 前提是 CONFIG_QCOM_FASTRPC=y（buildbot 默认 =m，而 Android 不加载模块）
#   —— 已在 scripts/kernel-config-android.sh 里 enable + 断言。
# ⚠️ /dev/fastrpc-sdsp 的权限在 ueventd.gaokun3.rc 里给（默认 root 独占）。
#
# ⬜ 还缺 Android 侧的 sensors HAL（AIDL android.hardware.sensors）——
#   libssc 依赖 glib/gobject/libqmi 那一套，搬不进 Android，得照它的协议逻辑
#   重写（QRTR 上的极简 QMI 客户端 + 那 8 个 .proto）。所以装了本段之后
#   SensorService 仍然看不到传感器；本段的验收判据是 **Android 里出现
#   QRTR 服务 400**，证明 DSP 侧 SSC 已经跑起来。
PRODUCT_PACKAGES += \
    hexagonrpcd \
    hexagonrpcd-sdsp.rc

# QRTR 服务列举工具（自研，tools/qrtr-lookup/）。AOSP 里没有任何 QRTR 用户态
# 工具，而"SLPI 上的 SSC 起来了没有"唯一的判据就是服务 400 在不在。
# 随镜像发布：帮忙测传感器的人不该为一个 20 KB 诊断工具去搭 AOSP 构建环境。
PRODUCT_PACKAGES += \
    gaokun3-qrtr-lookup

# SSC 客户端 + 命令行验证工具（ssc/）。实测已能从 Android 直接读出
# 加速度计（Z≈9.88，accuracy=3）与陀螺仪 —— 这是 sensors HAL 逻辑的 90%。
# 将来的 AIDL HAL 直接链 libgaokun3ssc 静态库。
PRODUCT_PACKAGES += \
    gaokun3-ssc-test

# sensors HAL（sensors-hal/，AOSP 默认实现的改造副本）。
# 它一上来 SensorService 就能看到加速度计与陀螺仪 —— 自动旋转由此生效。
PRODUCT_PACKAGES += \
    android.hardware.sensors-service.gaokun3

# VFS 根 → /vendor/etc/hexagonrpcd-root/（路径由上游 rc 的 -R 决定，别改名）
# ★ 空 registry 是整套的关键：DSP 找不到覆盖值就用默认值（=全部传感器启用）。
#   它 0 字节且专有目录被 gitignore，所以单独放在 etc/ 受版本控制，
#   免得别人克隆后忘了建而"传感器静默失效"。
#   ⚠️ 别用 sscregistrygen 预生成——实测会把加速度计一起弄坏（#37）。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/etc/hexagonrpcd-empty-registry:$(TARGET_COPY_OUT_VENDOR)/etc/hexagonrpcd-root/sensors/registry/registry \
    $(LOCAL_PATH)/hexagonrpcd-root/sensors/sns_reg.conf:$(TARGET_COPY_OUT_VENDOR)/etc/hexagonrpcd-root/sensors/sns_reg.conf \
    $(LOCAL_PATH)/hexagonrpcd-root/dsp/sdsp/RSCS.bin:$(TARGET_COPY_OUT_VENDOR)/etc/hexagonrpcd-root/dsp/sdsp/RSCS.bin

# 26 个传感器 JSON 与 6 个 socinfo 文件按原相对路径批量装入
PRODUCT_COPY_FILES += $(foreach f,$(wildcard $(LOCAL_PATH)/hexagonrpcd-root/sensors/config/*.json),\
    $(f):$(TARGET_COPY_OUT_VENDOR)/etc/hexagonrpcd-root/sensors/config/$(notdir $(f)))
PRODUCT_COPY_FILES += $(foreach f,$(wildcard $(LOCAL_PATH)/hexagonrpcd-root/socinfo/*),\
    $(f):$(TARGET_COPY_OUT_VENDOR)/etc/hexagonrpcd-root/socinfo/$(notdir $(f)))

# ─── 让内核进入 OTA 范围 ───
#
# 按 Android 分区规范做：boot_a / boot_b 里放标准 Android boot 镜像（header v2，
# 一个分区装齐 kernel+ramdisk+dtb），boot 已进 AB_OTA_PARTITIONS，由
# update_engine 像刷别的分区一样刷。配置见 BoardConfig.mk。
#
# ⚠️ 过渡期多一步：UEFI + systemd-boot 【读不了】Android boot 镜像（它只会从
#   ESP 按 BLS 条目加载文件）。所以 OTA 的 postinstall 钩子用
#   gaokun3-bootimg-extract 把内核从【刚刷好的 boot_<目标槽>】解出来，放到
#   ESP 上该槽专属的目录。boot 分区是唯一真相源，ESP 上的文件只是派生物 ——
#   所以【不】把内核往 vendor 里塞一份，那只会让每个 payload 白背 26 MB。
#   自研 EFI 加载器（读 misc 选槽 + 解析 boot 镜像）就位后这一步即可退役。

# ★ 内核由 BoardConfig.mk 的 TARGET_PREBUILT_KERNEL 提供，Lineage 的 kernel.mk
#   会把它拷成 $(PRODUCT_OUT)/kernel（构建系统认的名字，
#   build/make/core/Makefile:1014），boot 镜像随即用它。
# ⚠️★ 这里【不能】再用 PRODUCT_COPY_FILES 往 `kernel` 拷一份 —— 两条规则会撞：
#   "overriding commands for target out/target/product/gaokun3/kernel,
#    previously defined at build/make/core/Makefile:148"（实测踩到）。
#   用的是 vmlinuz.efi（EFI_ZBOOT 自解压 PE，13 MB）而不是 Image（37 MB）：
#   内核最终要落到只有 300 MiB 的 ESP 上，且两个槽位各存一份。

PRODUCT_PACKAGES += \
    gaokun3-bootimg-extract

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/bin/gaokun3-ota-postinstall.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gaokun3-ota-postinstall.sh

# ═══════════ Venus 硬件视频编解码：Android 这一半（external/v4l2_codec2）═══════════
#
# 内核那一半 M14 就通了（/dev/video0 = qcom-venus-decoder、
# /dev/video1 = qcom-venus-encoder，见 docs/stage4-findings.md #41）。
# 缺的一直是一个跟 V4L2 说话的 Codec2 组件，所以 66 个解码器全是软解。
#
# ⚠️★ 上游 README 有三处会把人带沟里，逐条对着源码核过：
#  1. README 让装 `android.hardware.media.c2@1.0-service-v4l2` —— 那是 **HIDL**，
#     Android 15+ 随 hwservicemanager 一起没了。真实模块名见
#     service/Android.bp:`android.hardware.media.c2-service-v4l2`（libcodec2-aidl-defaults）。
#     本机 media.c2.hal.selection 早就是 aidl（#36 那一仗的成果），正好对上。
#  2. ★★ README **完全没提**每个组件都由一条属性把守：
#     v4l2/V4L2ComponentStore.cpp:29-79 里每个 builder.decoder()/encoder()
#     外面都套着 property_get_bool("ro.vendor.v4l2_codec2.*.supported.*", false)。
#     不设 = 服务正常起来、IComponentStore/default 也注册上、**但零个组件**，
#     而且不报任何错。
#  3. poolmask 不能抄 README 的 0xf50000（那是 ION）—— 本机没有 ION
#     （/dev/ion 不存在、CONFIG_ION 也不在），要用 BLOB 的 0xfc0000。
#  ★ 另外 libv4l2_codec2_vendor_allocator **不必装**：
#     plugin_store/VendorAllocatorLoader.cpp:26 的 dlopen 失败只 ALOGI 返回 nullptr，
#     是可选项（给安全播放用的，本机没有）。
#
# 顶层 Android.bp 里有 soong_namespace{}，所以命名空间必须显式加。
PRODUCT_SOONG_NAMESPACES += external/v4l2_codec2

PRODUCT_PACKAGES += \
    android.hardware.media.c2-service-v4l2

# 组件清单是 XML 决定的（Codec2InfoBuilder.cpp:543：不在 XML 里的组件直接跳过），
# 而 media_codecs_c2.xml 是被【单独】搜索的，不必从 media_codecs.xml <Include>。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/etc/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml

# ★ 每一条都对应 tools/v4l2-probe 的实测结果，不是照抄模板：
#   解码器认 H264 VP80 VP90 HEVC MPG2；编码器只出 H264 VP80 HEVC。
#   → av1 没有硬件；vp9 编码没有硬件；MPEG-2 与 HEVC 编码硬件有、
#     但 v4l2_codec2 没有对应组件（v4l2/V4L2ComponentCommon.cpp）。
#   → secure 变体全关：本机没有安全播放。
PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.v4l2_codec2.decoder.supported.h264=true \
    ro.vendor.v4l2_codec2.decoder.supported.hevc=true \
    ro.vendor.v4l2_codec2.decoder.supported.vp8=true \
    ro.vendor.v4l2_codec2.decoder.supported.vp9=true \
    ro.vendor.v4l2_codec2.decode_concurrent_instances=8

# 警告：编码器【故意不启用】—— 实测它在 surface 输入路径上根本走不通，而且会让
#   应用【失败而不是回退到软编】（组件 rank 0x80 压过软编的 0x200）。
#   实测日志（screenrecord，2026-08-22）：
#     E EncodeComponent: Unable to parse RGBX_8888 from IMPLEMENTATION_DEFINED
#     E EncodeComponent: Failed to get input block layout
#     E ...: Attempted to lock() a buffer that was not allocated with a
#            BufferUsage::CPU_* usage.
#   SurfaceFlinger 给的是 RGBX_8888/IMPLEMENTATION_DEFINED，而 Venus 编码器要
#   NV12，v4l2_codec2 的 EncodeComponent 不做这个转换 —— 不是配置能解决的。
#   录屏/录像继续走软编（能用）。要复测就把这两行加回上面的属性块：
#     ro.vendor.v4l2_codec2.encoder.supported.h264=true
#     ro.vendor.v4l2_codec2.encoder.supported.vp8=true
#     ro.vendor.v4l2_codec2.encode_concurrent_instances=8

# Codec2 的 pool mask：BLOB(19) 那一档。见上面第 3 条。
PRODUCT_VENDOR_PROPERTIES += \
    debug.stagefright.c2-poolmask=0xfc0000

# ★ 扩展 seccomp 策略：不装它，服务一开始真干活就被 SIGSYS 打死
#   （实测 `libminijail: blocked syscall: eventfd2`）。
#   服务源码 service.cpp:32-34 写死了这个路径，注释还明说"默认不存在"；
#   上游 README 里给的文件名（codec2.vendor.ext.policy）不是这个，照抄会无效。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/etc/media-c2-extended-seccomp.policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/android.hardware.media.c2-extended-seccomp_policy

# ★ 顺手修一个【与硬解无关、本机一直存在】的框架崩溃：
#   CCodec::createInputSurface() 在 CreateCompatibleInputSurface() 返回 null 时
#   不判空就解引用（frameworks/av CCodec.cpp:2189-2190），于是任何用 surface
#   输入编码的东西（screenrecord、录像）都是 SIGSEGV。本机够不到 Codec2 的
#   input surface 服务，而 OMX 回退路径随 HIDL 一起没了 —— 只剩
#   CCodec.cpp:3417 那条由属性开启的 AidlGraphicBufferSource 兜底。
#   实测：设了它，崩溃变成干净的错误返回。
PRODUCT_VENDOR_PROPERTIES += \
    debug.stagefright.c2inputsurface=-1

# 解码验证工具。本机【没有任何能放视频的应用】（gallery3d 是精简版，
# 连 MovieActivity 都没有），而"组件出现在 MediaCodecList"只证明能实例化、
# 不证明能解码。见 tools/decode-test.cpp。
PRODUCT_PACKAGES += \
    gaokun3-decode-test

# ═══════════ 亮度：真的 lights HAL ═══════════
#
# 出厂装的是 AOSP 示例实现（android.hardware.lights-service.example），只打日志。
# 实测后果：亮度滑条【完全无效】—— 框架从 1 调到 255，面板始终停在 512/4095
# （12.5%），机器永远是暗的；而以 root 直接写那个 sysfs 节点立刻生效。
# 见 lights/Lights.cpp 的注释与 docs/stage4-findings.md。
# ★ 排除示例实现靠的是 lights/Android.bp 里的 `overrides:` —— PRODUCT_PACKAGES
#   只能加不能减，两个 HAL 都装上会抢注册 ILights/default。
PRODUCT_PACKAGES += \
    android.hardware.light-service.gaokun3

# ═══════════ 磁吸键盘开关 ═══════════
#
# 后端：属性 persist.sys.gaokun3.keyboard → init 触发器 → 脚本写内核的
#       /sys/class/input/inputN/inhibited（实测本机这套键盘注册了 7 个 input
#       设备，全部按名字前缀 "HID 12d1:10b8" 匹配）。
# 前端：parts/ 里的小应用，用 Settings 注入（IA_SETTINGS）出现在「设置 → 系统」，
#       不需要改 packages/apps/Settings，也不依赖 LineageParts
#       （实测这个 ROM 没装 org.lineageos.lineageparts）。
PRODUCT_PACKAGES += \
    Gaokun3Parts

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/bin/gaokun3-keyboard.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gaokun3-keyboard.sh \
    $(LOCAL_PATH)/etc/keyboard.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/keyboard.rc

# ═══════════ 触摸手感模式（用户 2026-08-23 反馈手感不好）═══════════
#
# ★ 先说清楚一件事：我们内核里的 himax 驱动【就是】上游的 EGoTouchRev
#   （github.com/chiyuki0325/EGoTouchRev-Linux）。逐项比对过：14 个算法函数
#   逐字相同，20 项默认值只差一项，上游最新提交与 buildbot 那个补丁是同一天的。
#   ⇒ 「换成 EGoTouchRev」换不来任何东西，我们已经在跑它了。
#
# 真正的差距在【参数】：驱动跑的是日用默认值，而它的调参工具里另有一套
# game_preset。差三项，见 bin/gaokun3-touch-mode.sh。
#
# ⚠️★★★ 2026-09-14 更正（#114）：本节原先写着"这三项只关掉平滑与按下防抖
#   （少 2 帧延迟），不动任何信号处理门限"—— **第三项不是这样**。
#   `track_jump_dist2=6400` 不是"关掉"什么，而是【打开】了跳点检测；
#   而驱动里那个判据拿原始位移比阈值，等于一条 **1.0 m/s 的限速线**，
#   越过之后轨迹反复自毁、永远回不到可上报状态 ⇒ 快滑时**一个点都不上报**。
#   实测一次甩动被切成 87 条轨迹（详见 bin/gaokun3-touch-mode.sh 顶部）。
#   ★ 教训：照搬上游预设时，"另外两项是关掉东西，所以第三项也无害"是一次
#   **没做的功课**，不是一个结论。逐项问"它打开了什么"。
#   两版预设现在都把 JUMP 清 0，game 与 daily 只差 track_smoothing。
#
# 默认仍设 game：本机目标是跑手游，game 关掉坐标平滑（省约 25 ms 稳态滞后），
# 保留按下防抖（2 帧确认，约 8 ms，换噪声不易变成"按下"）。切回来：
#   setprop persist.sys.gaokun3.touch_mode daily
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/bin/gaokun3-touch-mode.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gaokun3-touch-mode.sh \
    $(LOCAL_PATH)/etc/touchmode.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/touchmode.rc

PRODUCT_VENDOR_PROPERTIES += \
    persist.sys.gaokun3.touch_mode=game
