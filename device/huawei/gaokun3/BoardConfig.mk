#
# BoardConfig for Huawei MateBook E Go (sc8280xp / gaokun3)
#
# Bring-up target: Stage 2 == "adb shell works, black screen is fine".
# Deliberately minimal -- every HAL added here is another way for the first
# boot to fail, and this machine has no serial console to tell us which one.
#
# Everything marked [measured] came from the Stage 0 hardware inventory on the
# real device (docs/hw-inventory.md), not from a template.
#

# ---------------------------------------------------------------- 架构
# [measured] dmesg: "Booting Linux on physical CPU 0x0000000000 [0x410fd4b0]"
#   0x41 = ARM, part 0xd4b = Cortex-A78C  ->  ARMv8.2-A
TARGET_ARCH             := arm64
TARGET_ARCH_VARIANT     := armv8-2a
TARGET_CPU_VARIANT      := generic
TARGET_CPU_ABI          := arm64-v8a

TARGET_2ND_ARCH         := arm
TARGET_2ND_ARCH_VARIANT := armv8-2a
TARGET_2ND_CPU_VARIANT  := generic
TARGET_2ND_CPU_ABI      := armeabi-v7a
TARGET_2ND_CPU_ABI2     := armeabi

TARGET_BOARD_PLATFORM     := sc8280xp
TARGET_BOOTLOADER_BOARD_NAME := gaokun3

# ------------------------------------------------------- 没有 bootloader
# 本机是 UEFI + systemd-boot，不是 fastboot 设备，也没有 recovery 分区。
TARGET_NO_BOOTLOADER := true
# ★★ 2026-08-20：开始构建【独立的】recovery.img。
#
# 为什么要：本机没有 recovery，于是 misc 的 BCB 里那条 `boot-recovery` 命令
# 没人消费（实机确认过它就躺在那儿）—— 也就是说"重启进 recovery"和设置里的
# 恢复出厂设置这类请求，在本机是发出去就掉进黑洞的。顺带 fastbootd 也没有，
# 因为它住在 recovery 的 ramdisk 里。
#
# ⚠️★ 绝对不要用 BOARD_USES_RECOVERY_AS_BOOT：board_config.mk:463 一旦看到它
#   就把 BUILDING_BOOT_IMAGE 关掉，那会直接推翻我们的 boot.img（A/B 的常规做法
#   是 recovery 合进 boot，但那条路和本机"独立 boot 镜像"的方案互斥）。
#   所以走"独立 recovery.img"这条老路：BUILDING_RECOVERY_IMAGE 的四个入口里
#   （board_config.mk:502-518）我们用的是
#   「定义 BOARD_RECOVERYIMAGE_PARTITION_SIZE 且 TARGET_NO_KERNEL/TARGET_NO_RECOVERY
#    都不为 true」这一个。产物 = $(PRODUCT_OUT)/recovery.img（Makefile:285-298）。
#   已核实 AB_OTA_UPDATER=true 与独立 recovery 之间没有冲突检查。
#
# ⚠️ recovery 不会有自己的分区。它会作为文件随 vendor 走 OTA，由 postinstall
#   钩子铺到 ESP —— 这样【已装的机器一次普通 OTA 就能拿到 recovery】，
#   而不必像 boot_a/boot_b 那样重装。理由：安装器把剩余空间全给了 userdata，
#   已装机器没有空间再切分区。
TARGET_NO_RECOVERY   := false
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 67108864
# ★ 复用主 fstab，不另写一份 recovery.fstab —— 今天刚被 BOARD_KERNEL_CMDLINE
#   与 BLS 条目漂移教育过：没有消费者的副本一定会漂。
#   （默认查找位置是 $(TARGET_DEVICE_DIR)/recovery.fstab，Makefile:2620。）
TARGET_RECOVERY_FSTAB := device/huawei/gaokun3/fstab.gaokun3
BOARD_USES_GENERIC_KERNEL_IMAGE := false

# ★★ 2026-08-20 起【生成真的 boot.img】，并把 boot 纳入 A/B OTA 范围。
#
# 此前是 TARGET_NO_KERNEL := true —— 内核/ramdisk/dtb 是 ESP 上的裸文件，
# 于是内核完全在 OTA 之外，换内核得手工拷。现在按 Android 分区规范来：
# boot_a / boot_b 分区里放标准 Android boot 镜像，由 update_engine 直接刷。
#
# ⚠️ 但 UEFI/systemd-boot【读不了】Android boot 镜像，所以过渡期还要一步：
#   OTA 的 postinstall 钩子用 gaokun3-bootimg-extract 把内核从刚刷好的
#   boot_<目标槽> 解出来放到 ESP 上该槽的目录，systemd-boot 照旧启动。
#   boot 分区因此是唯一真相源，ESP 上的文件只是派生物。
#   自研的 EFI 加载器（读 misc 选槽 + 解析 boot 镜像）就位后这一步即可退役。
#
# 内核走 PRODUCT_OUT/kernel（device.mk 里用 PRODUCT_COPY_FILES 提供）——
# 出处 build/make/core/Makefile:1009-1017：TARGET_NO_KERNEL != true 且
# BOARD_KERNEL_BINARIES 为空时 INSTALLED_KERNEL_TARGET := $(PRODUCT_OUT)/kernel。
TARGET_NO_KERNEL := false

# ★ 告诉 Lineage 的 kernel.mk 用预置内核，不要去找内核源码树。
#   （vendor/lineage/build/tasks/kernel.mk:159-163、166-204）
# ⚠️ 必须用这个变量。kernel.mk 里另有一条"扫 PRODUCT_COPY_FILES 里 dest=kernel
#   的项来识别预置内核"的分支，但那段写的是 `$(ifeq kernel,$(_dest), ...)`
#   —— ifeq 不是 make 函数，整个 $(ifeq ...) 展开成空字符串，所以那条路
#   【根本不生效】（kernel.mk:171-176，Lineage 自己的 bug）。
#   只靠 PRODUCT_COPY_FILES 的结果是 "BOARD_KERNEL_IMAGE_NAME not defined"。
# 设了它之后 kernel.mk 会把这个文件拷成 $(PRODUCT_OUT)/kernel
#   （NEEDS_KERNEL_COPY），boot 镜像就用它。
# 它会打印一句 "Using prebuilt kernel binary ... THIS IS DEPRECATED" ——
# 对本机不适用：内核本来就必须在树外编（主线 + gaokun3 补丁栈）。
TARGET_PREBUILT_KERNEL := device/huawei/gaokun3/prebuilt-boot/vmlinuz.efi

# ★ 刻意用 header v2：一个分区就装齐 kernel + ramdisk + dtb。
#   v3 及以上会把 dtb 与 vendor ramdisk 拆到 vendor_boot
#   （board_config.mk:522-526 一旦 >=3 就进 vendor_boot 分支），
#   对本机是纯粹的额外复杂度。
BOARD_BOOT_HEADER_VERSION := 2
BOARD_MKBOOTIMG_ARGS += --header_version $(BOARD_BOOT_HEADER_VERSION)
# ⚠️ 变量名是 BOARD_INCLUDE_DTB_IN_BOOTIMG（BOOTIMG 连写，不是 BOOT_IMG）。
#   写错了不会被忽略，而是报 "BOARD_PREBUILT_DTBIMAGE_DIR with
#   BOARD_INCLUDE_DTB_IN_BOOTIMG != true is not supported"，
#   而且这个错在 lunch 阶段就炸，表现成 "Don't have a product spec for"，
#   非常容易误判成设备树没被 manifest 拉全。
BOARD_INCLUDE_DTB_IN_BOOTIMG := true
BOARD_PREBUILT_DTBIMAGE_DIR := device/huawei/gaokun3/prebuilt-boot/dtb
# 定义这个变量就会打开 boot 镜像生成（board_config.mk:467-468）。
# 内容约 27 MB（zboot 内核 13 + ramdisk 13 + dtb 0.17），64 MiB 留足余量。
BOARD_BOOTIMAGE_PARTITION_SIZE := 67108864

# ---------------------------------------------------------------- cmdline
# ★ 现在它【会被嵌进 boot.img】，不再只是记录 —— 自研 EFI 加载器将来直接用它
#   （槽位后缀由加载器按 misc 里的槽位自己追加，所以这里不写 slot_suffix）。
#   过渡期实际生效的仍是 systemd-boot BLS 条目的 options 行，两处必须一致。
#
# ⚠️★ 2026-08-20 发现这两处早就漂移了 —— 此前 TARGET_NO_KERNEL=true，这个变量
#   没有任何消费者，所以漂了也没人发现：这里原写 deferred_probe_timeout=30 而
#   实际条目是 10，并且缺 androidboot.veritymode / flash.locked /
#   verifiedbootstate 与 iommu.passthrough/strict 四项。已按实机在用的
#   android-a.conf 逐项对齐。教训：没有消费者的配置一定会漂。
#
# [measured] boot_devices 来自
#   /sys/devices/platform/soc@0/1c20000.pcie/pci0002:00/.../nvme/nvme0/nvme0n1
#
# ⚠️ 绝对不要加 earlycon：强烈怀疑 earlycon=efifb 会挂死本机启动，
#    见 docs/stage1-kernel-plan.md 第 1.0 节。
BOARD_KERNEL_CMDLINE := \
    androidboot.flash.locked=0 androidboot.verifiedbootstate=orange \
    iommu.passthrough=0 iommu.strict=0 \
    androidboot.hardware=gaokun3 \
    androidboot.boot_devices=soc@0/1c20000.pcie \
    androidboot.selinux=permissive androidboot.veritymode=disabled \
    firmware_class.path=/vendor/firmware/ \
    init=/init printk.devkmsg=on deferred_probe_timeout=10 \
    console=tty0 \
    clk_ignore_unused pd_ignore_unused arm64.nopauth efi=noruntime \
    fbcon=rotate:1 usbhid.quirks=0x12d1:0x10b8:0x20000000

# ------------------------------------------------------------ 分区布局
# 动态分区（super）而不是分立分区：AOSP 16 默认如此，构建直接产出
# super.img；关掉它反而要逆着构建系统走。
#
# ⚠️ PRODUCT_USE_DYNAMIC_PARTITIONS 是 **product** 变量
#    （build/make/core/product.mk:311），不是 board 变量。
#    product 配置先于 BoardConfig.mk 解析完并转为只读，在这里赋值会报
#    "cannot assign to readonly variable"。它设在 aosp_gaokun3.mk 里。
BOARD_SUPER_PARTITION_SIZE   := 12884901888        # 12 GiB
BOARD_SUPER_PARTITION_GROUPS := gaokun3_dynamic_partitions
BOARD_GAOKUN3_DYNAMIC_PARTITIONS_PARTITION_LIST := system system_ext product vendor
# 组大小留出 metadata 余量，官方建议比 super 小一些
BOARD_GAOKUN3_DYNAMIC_PARTITIONS_SIZE := 12683575296   # super - 200 MiB

# [实测必需] 只有设了这一项，AOSP 才会在 system 镜像根目录创建 /metadata 挂载点。
# 缺了它，init 在 switch_root 时会因为
#   "Unable to move mount at '/metadata' to '/system/metadata'"
# 而失败并复位。见 docs/stage2-findings.md 第 4 节。
# 注意：不能事后往镜像里 mkdir 补 —— system.img 按内容精确打包，可用空间为 0。
BOARD_USES_METADATA_PARTITION := true

BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_USERDATAIMAGE_FILE_SYSTEM_TYPE := ext4
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_USERIMAGES_USE_F2FS := true

TARGET_COPY_OUT_VENDOR      := vendor
TARGET_COPY_OUT_SYSTEM_EXT  := system_ext
TARGET_COPY_OUT_PRODUCT     := product

BOARD_FLASH_BLOCK_SIZE := 262144

# --------------------------------------------------------- A/B OTA（M6）
# ★ 这里曾经是 `AB_OTA_UPDATER := false`，注释写着"没有 A/B，没有 fastboot，
#   不做 OTA"。前半句现在不成立了：**没有 fastboot 并不妨碍 A/B**。
#
# 走的是 **Virtual A/B**：super 里仍然只存【一份】动态分区，更新写成
# userdata 上的 COW 快照，切槽后再合并。选它而不是传统 A/B 的理由：
#   * 传统 A/B 要求 super 能放下两份。本机每槽镜像合计 2.53 GiB
#     （system 1022 + system_ext 944 + product 505 + vendor 241 MB），
#     12 GiB 的 super 其实放得下 —— 但 Virtual A/B 连扩都不用扩，
#     **分区布局一个字节都不用动**，首批测试者将来不必重刷。
#   * 这是 AOSP 16 的主路：device/google/cuttlefish 与
#     device/linaro/dragonboard 都走这条（实名核实）。
#   * 快照落在 userdata，本机 /data 有 294 GiB 余量。
#
# ⚠️ 内核前提（已实测 ~/gaokun/kernel-out/.config）：
#     CONFIG_DM_SNAPSHOT=y  ✅ Virtual A/B 的基础
#     CONFIG_DM_USER        ✗  没有 → **不能用压缩快照**，
#                              故不 inherit compression.mk，只用 launch.mk。
#
# ⚠️ PRODUCT_VIRTUAL_AB_OTA 是 **product** 变量，不能写在这里
#    （同本文件上面 PRODUCT_USE_DYNAMIC_PARTITIONS 那条坑）。
#    它由 lineage_gaokun3.mk 里 inherit 的
#    $(SRC_TARGET_DIR)/product/virtual_ab_ota/launch.mk 设置。
AB_OTA_UPDATER := true
AB_OTA_PARTITIONS += \
    boot \
    system \
    system_ext \
    product \
    vendor

# ★ boot 已在上面的列表里（2026-08-20）。它是标准 Android boot 镜像，
#   由 update_engine 按规范直接刷进 boot_a / boot_b。
#
# ★ 但内核【已经进入 OTA 范围】（2026-08-20），走的是另一条路：
#   Image / DTB / ramdisk 装进 vendor 分区（它在上面的列表里），payload 刷完后
#   由 postinstall 钩子复制到 ESP 上目标槽位专属的目录 slot_a/ slot_b/。
#   两个槽位的 BLS 条目永久指向各自目录，所以钩子只放文件、不改条目 ——
#   绝不会碰到正在运行的那个槽，回滚也就天然安全。
#
# ⚠️ POSTINSTALL_OPTIONAL 故意设为 false：宁可整个 OTA 失败，也不能让某个槽
#   位上出现"新 system + 旧内核"。最常见的失败原因是 ESP 空间不足
#   （只有 300 MiB，两个槽各约 53 MiB），脚本会当场报出来。
# ★ argv[1] 是目标槽位整数、argv[2] 是状态 fd —— 出处
#   system/update_engine/payload_consumer/postinstall_runner_action.cc:355-357。
AB_OTA_POSTINSTALL_CONFIG +=     RUN_POSTINSTALL_vendor=true     POSTINSTALL_PATH_vendor=bin/gaokun3-ota-postinstall.sh     FILESYSTEM_TYPE_vendor=ext4     POSTINSTALL_OPTIONAL_vendor=false

# ----------------------------------------------------- Stage 3: 图形栈
# 模板：device/linaro/dragonboard/shared/graphics/（db845c = 树内同款高通主线）。
#
# ⚠️ 实测教训（2026-08-17）：AOSP 16 树内的 external/mesa3d 是 gfxstream 向
# fork，【不含 freedreno】；BOARD_MESA3D_* / BOARD_USE_CUSTOMIZED_MESA 在
# 本 manifest 里【无任何消费者】（grep 全树验证），设了会被静默忽略，
# 且缺失的 PRODUCT_PACKAGES 因 ALLOW_MISSING_DEPENDENCIES 不报错。
# → Phase A（现在）：swangle = ANGLE over SwiftShader Vulkan，纯树内，
#   CPU 渲染先出画面（dragonboard/shared/graphics/swangle/ 同款）。
# → Phase B（Stage 5 前）：引入 GloDroid 式 mesa 胶水构建 freedreno/turnip，
#   参考平行项目（docs/parallel-mainline-generic.md）。
PRODUCT_REQUIRES_INSECURE_EXECMEM_FOR_SWIFTSHADER := true

# --------------------------------------------------------------- sepolicy
# permissive 起步（cmdline 里也带了），但 policy 仍要能编过
BOARD_VENDOR_SEPOLICY_DIRS += device/huawei/gaokun3/sepolicy
# minigbm / swangle 的 sepolicy 直接复用 dragonboard 的（同一套 HAL）
BOARD_VENDOR_SEPOLICY_DIRS += device/linaro/dragonboard/shared/graphics/minigbm_msm/sepolicy
BOARD_VENDOR_SEPOLICY_DIRS += device/linaro/dragonboard/shared/graphics/swangle/sepolicy

# --------------------------------------------------------------- VINTF
DEVICE_MANIFEST_FILE := device/huawei/gaokun3/manifest.xml
# vendor 侧兼容性矩阵（"我们对 framework 的要求"）。内容为空，但文件必须存在
# —— 缺了它 checkvintf 会在 "Checking vendor matrix" 处报
#    "ERROR: Cannot fetch vendor matrix." 并让构建在 97% 中止（2026-08-19 实测）。
DEVICE_MATRIX_FILE += device/huawei/gaokun3/compatibility_matrix.xml

#
# 刻意【不】设置的项 —— 都已对 AOSP 16 源码核实：
#
#   BOARD_VNDK_VERSION       build/make/core/config.mk:1268-1270 会强制清空，
#                            VNDK 在 Android 16 已移除
#   PRODUCT_FULL_TREBLE      config.mk:773-782 已强制为 true 且 .KATI_READONLY，
#                            再设一次会报错
#   TARGET_USES_HWC2         构建系统里已无任何引用
#   WITH_DEXPREOPT_PIC       同上
#   BOARD_USES_DRM_HWCOMPOSER 同上 —— Stage 3 改用 minigbm + drm_hwcomposer_hwc3，
#                            三者（含 mesa3d）都已在 AOSP 16 树内
#
# 以上都是 aospm sdm845 模板（Android 13 时代）里有、但在 16 上会出问题的写法。
#

# WiFi
WPA_SUPPLICANT_VERSION := VER_0_8_X
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_WLAN_DEVICE := emulator
BOARD_HOSTAPD_DRIVER := NL80211

# 固件 blob（qcadsp/qccdsp/qcslpi/zap shader 的 .mbn）本身就是 ELF 格式，
# 而 AOSP 会拒绝 PRODUCT_COPY_FILES 里目标路径含 bin/lib/lib64 的 ELF 文件。
# ramdisk 副本必须落在 /lib/firmware（内核的固件回落搜索路径），只能开这个
# 官方逃生开关。vendor 路径（/vendor/firmware/...）不含 lib 组件，本来就豁免。
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true

# ══════════════════════════════ crDroid / LineageOS ══════════════════════════
# Stage 6：转 crDroid 16.0（= LineageOS 23.2 布局）。
#
# ⚠️ 必须在 include BoardConfigLineage.mk 【之前】关掉 recovery：
#    vendor/lineage/config/BoardConfigLineage.mk 第一行是
#        BOARD_USES_FULL_RECOVERY_IMAGE ?= true
#    `?=` 意味着我们先赋值就能覆盖它。本机是 UEFI + systemd-boot，
#    没有 recovery 分区也没有 fastboot，recovery 镜像既造不出也用不上。
BOARD_USES_FULL_RECOVERY_IMAGE := false

# ⚠️ 高通 CAF 那套（hardware/qcom-caf/*）绝对不能开：8cx 从来没有 Android BSP，
#    我们走的是纯主线内核 + 自建 HAL。BoardConfigLineage.mk 会按这个变量
#    决定是否 include BoardConfigQcom.mk。
BOARD_USES_QCOM_HARDWARE := false

# 内核在树外自己编（kb21 / v7.2-rc2 + gaokun3 补丁），不让 Lineage 的
# kernel.mk 去找 kernel/huawei/gaokun3。TARGET_NO_KERNEL 已在上面设过。
TARGET_KERNEL_SOURCE :=

# GApps 的板级片段（MindTheGapps 目前是空文件，为将来它加东西而 include）。
# 用 -include：没同步 vendor/gapps 时静默跳过。
-include vendor/gapps/arm64/BoardConfigVendor.mk

include vendor/lineage/config/BoardConfigLineage.mk
