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

# thermal HAL 同样是 installable:false 的 APEX 打包件：
#   hardware/interfaces/thermal/aidl/default/Android.bp 的 cc_binary
#   android.hardware.thermal-service.example 带 installable: false，
#   binary 只出现在 apex "com.android.hardware.thermal" 里。
# 2026-08-19 实测：直接列 binary 名会让 kati 报
#   "includes non-existent modules in PRODUCT_PACKAGES" 并中止构建。
PRODUCT_PACKAGES += \
    com.android.hardware.thermal

# effects HAL 启动即退（"config file audio_effects_config.xml not found"，
# 实测）。默认配置的 prebuilt_etc 被 soong config 门控着
# （hardware/interfaces/audio/aidl/default/Android.bp:372-378）：
$(call soong_config_set_bool,hardware_interfaces_audio,use_default_audio_effects_config,true)
PRODUCT_PACKAGES += \
    audio_effects_config.xml

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
    frameworks/native/data/etc/android.hardware.vulkan.level-1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.level.xml \
    frameworks/native/data/etc/android.hardware.vulkan.version-1_1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.version.xml

#
# Stage 3 之后再加（每次只加一个）：
#   音频   tinyhal（refs/aospm-tinyhal），源是 LENOVO-X13s.conf / sc8280xp.conf
#   WiFi   wpa_supplicant + ath11k
#   传感器 / 相机 / 振动
#

# ─── Stage 4: WiFi（ath11k 主线 + AIDL HAL APEX + wpa_supplicant）───
PRODUCT_PACKAGES += \
    com.android.hardware.wifi \
    wpa_supplicant

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
    $(LOCAL_PATH)/etc/hangdump.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hangdump.rc

# 挂起前把 a600000.usb 的 USB role 切到 host —— 那个控制器停在 role=device 时，
# 设备挂起阶段会【整板复位】且不留任何日志；而 USB adb 的 UDC 就在它上面，
# 所以不能简单把 DTS 改成 host。见 docs/stage4-findings.md #52 / #54 / #56。
# ★ 默认【启用】（见下面的 persist.gaokun3.allow_suspend）。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/bin/gaokun3-usbrole.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gaokun3-usbrole.sh \
    $(LOCAL_PATH)/etc/usbrole.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/usbrole.rc

# ★ 默认【开启】待机（2026-08-22 起）。开关留着是为了排查时能一条属性关掉。
#   开启后：息屏切 role=host（挂起安全）、亮屏切回 device（USB adb 可用）。
#   实测 Android 真实挂起/唤醒 ×4 零复位、救援 Ubuntu systemctl suspend 3/3。
# ⚠️ 用户可见代价：息屏时 USB device-mode adb 断开，亮屏恢复；TCP adb 不受影响。
PRODUCT_PROPERTY_OVERRIDES += \
    persist.gaokun3.allow_suspend=1

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
    ro.vendor.v4l2_codec2.encoder.supported.h264=true \
    ro.vendor.v4l2_codec2.encoder.supported.vp8=true \
    ro.vendor.v4l2_codec2.decode_concurrent_instances=8 \
    ro.vendor.v4l2_codec2.encode_concurrent_instances=8

# Codec2 的 pool mask：BLOB(19) 那一档。见上面第 3 条。
PRODUCT_VENDOR_PROPERTIES += \
    debug.stagefright.c2-poolmask=0xfc0000
