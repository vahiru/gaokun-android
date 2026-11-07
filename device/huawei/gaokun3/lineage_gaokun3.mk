#
# Product definition for Huawei MateBook E Go (sc8280xp / gaokun3)
# —— crDroid 17.0 / crDroid 13（= LineageOS 24.0 布局，AOSP 基线 android-17.0.0_r1）
#    2026-10-25 起；此前是 crDroid 16.0（lineage-23.2，android-16.0.0_r4）
#
# 取代 Stage 2–5 时期的 aosp_gaokun3.mk（在 git 历史里）。
# 换轨理由见 docs/dev/android-bringup.md：手搓最小 AOSP 缺产品级配置
# （MediaCodecList 空、无铃声/UI 音效），而这些是真 ROM 设备树的标配。
#
# ⚠️ 产品名必须是 lineage_<codename>：vendor/lineage/build/envsetup.sh 的
#    breakfast/brunch 拼的就是
#        lunch lineage_$target-$aosp_target_release-$variant
#    而 vendor/lineage/vars/aosp_target_release = cp2a（17.0；16.0 时是 bp4a）。
#    （vendor/lineage 这个路径由 crdroidandroid/android_vendor_crdroid 提供，
#      不是 LineageOS 那个仓库 —— repo manifest 实名核实。）
#

# dalvik 堆：不配则 system_server 只有 16MB growth limit，boot 后必 OOM
# （java.lang.OutOfMemoryError 实测，Stage 2）。用 10 寸平板标准档。
$(call inherit-product, frameworks/native/build/tablet-10in-xhdpi-2048-dalvik-heap.mk)

# 64 位为主 + 32 位兼容（手游 arm64-v8a 直跑，但 armeabi-v7a 也要能装）
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)

# ★ AOSP 基座 —— 必须由设备树自己提供。
#
# 2026-08-19 实测教训：我一度以为 crDroid 自带整套应用，叠 full_base 会撞包，
# 于是把它拿掉了。结果构建"成功"但产物是空壳：
#     system.img 只有 29.8 MB（旧 AOSP 那份是 1.0 GB），
#     /system 里没有 apex/、没有 app/、连 framework/services.jar 都不存在。
#
# 原因：vendor/lineage/ 下的所有配置都是【补充】性质的 ——
#   common.mk        只 inherit vendor/extra、crdroid.mk、vendor/addons、audio.mk
#   common_mobile.mk 只补 frameworks/base/data/sounds/AudioPackage14.mk
#   tablet.mk        只补 $(SRC_TARGET_DIR)/product/large_screen_common.mk
# 整个 vendor/lineage/config/*.mk 里对 SRC_TARGET_DIR 的引用只有 tablet.mk 那一处。
# 也就是说 LineageOS/crDroid 的设备树【本来就该】自己 inherit 一个 AOSP base 产品，
# 这也是上游设备树模板的标准写法。
#
# full_base.mk 的链条（本地实读）：
#   full_base → generic_no_telephony → handheld_{system,system_ext,vendor,product}
#            → media_vendor → base_vendor（vendor_compatibility_matrix.xml、
#              shell_and_utilities_vendor = /vendor/bin/sh + toybox_vendor …）
#   full_base 还带 frameworks/base/data/sounds/AllAudio.mk（铃声）
# 正是我们旧的 aosp_gaokun3.mk 用过、并产出可用镜像的那条链。
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)

# ═══════════════ 开发构建 / 发布构建（B1，2026-10-04）═══════════════
#
# ★ 默认 = 发布构建 = 安全。只有构建时环境里 GAOKUN3_DEV_BUILD=1 才是开发构建：
#     GAOKUN3_DEV_BUILD=1 m bacon superimage
# 下面四样开发便利【只在开发构建里】给（此前开发与发布共用一份配置，
# v0.7.1 及以前的公开镜像全带着，见 docs/dev/release-checklist.md）：
#   ① WITH_ADB_INSECURE            adb 不要授权（ro.adb.secure=0）
#   ② system_ext 的 ro.debuggable=1 adb root / adb remount（D1：发布版一起关）
#   ③ PRODUCT_ADB_KEYS             开发机的个人公钥烤进 /product/etc/security/adb_keys
#   ④ persist.adb.tcp.port=5555    开机即开 TCP adb（在 device.mk）
# 发布构建四样都没有：ro.adb.secure=1（/system/build.prop 本来就写 1，见下面机制 1）、
# ro.debuggable=0、镜像里没有 adb_keys、不开 TCP adb；USB adb 默认关
# （init.gaokun3.usb.rc 只跟 persist.sys.usb.config 走，发布构建不设它，见那里的注释）。
# 用户要 adb：开发者选项里打开「USB 调试」（弹授权框）或「无线调试」（配对码）。
#
# ★ init 能不能 permissive 由 Soong 的 Debuggable 决定（只看变体：
#   refs/aosp-build/core/soong_config.mk:58 → refs/lineage-system-core/init/Android.bp:126-134
#   的 ALLOW_PERMISSIVE_SELINUX），与 ro.debuggable 无关 ⇒ 发布构建仍是 userdebug 变体、
#   cmdline 的 androidboot.selinux=permissive 照样生效（2026-10-06 D5 起 cmdline 默认是 enforcing，见 BoardConfig.mk）。
#   ✅ A17 核过：ProductNotDebuggableInUserdebug 只有 build/soong/scripts/gen_build_prop.py:350、:387-389 一个消费者
#   （refs/a17/build/make/core/soong_extra_config.mk:88 传进去），不改 Debuggable（soong_config.mk:60 只看变体）。
#   反过来这也意味着 adbd / adb_root 在发布构建里仍按"可调试"编译 —— A17 因此多出一条 adb root 的路，见 device.mk 开头。
#
# ⚠️ 开发机装发布构建（候选版就是发布构建 —— release.sh --no-build 发的必须是验过的那一版）
#   之前要先在开发机上把这几样持久化进 /data，否则装上后 adb 全断：
#   persist.sys.usb.config=adb、persist.adb.tcp.port=5555、开发主机公钥进 /data/misc/adb/adb_keys。
#   发布镜像的 release.sh 第 2 步逐条断言上面四样都不在。
ifeq ($(GAOKUN3_DEV_BUILD),1)

# ★ 关掉 adb 授权 —— 必须在 inherit crDroid 配置【之前】设。（仅开发构建）
#
# 2026-08-19 首次上机踩到：crDroid 起来了，但 adb 一直是 unauthorized，
# 而设备端没有 ssh、也就没法远程重启，等于失联（只能靠人去点屏幕）。
#
# vendor/lineage/config/common.mk:33-45 的逻辑：
#     ifeq ($(TARGET_BUILD_VARIANT),eng)      ro.adb.secure=0
#     else ifdef WITH_ADB_INSECURE            ro.adb.secure=0
#     else                                    ro.adb.secure=1
#                                             PRODUCT_NOT_DEBUGGABLE_IN_USERDEBUG := true
# 后一个分支还会把 userdebug 的 ro.debuggable 压成 0 ——
# 那样 adb root / adb remount 都不能用，而 M3 部署 turnip 全靠 overlay。
# （发布构建要的正是这个分支。⚠️ 那段逻辑是 crDroid 树里的 vendor/lineage，本地 refs 没有，
#   待构建机核实：`sed -n 25,50p vendor/lineage/config/common.mk`。）
#
# ⚠️ 我们 device.mk 里那句 PRODUCT_PROPERTY_OVERRIDES += ro.adb.secure=0
#    落在 vendor/build.prop，被 init 的 CheckPermissions 静默拒绝
#    （vendor context 无权设置 system 属主的 ro.adb.secure）。
#    详见下面那段对 property_service 加载语义的实测说明。
WITH_ADB_INSECURE := true

# ★ ro.debuggable=1 必须从 system_ext 发（不能从 vendor）。
#
# 2026-08-19 实测出来的两条机制，都和直觉相反：
#
# 1) build/soong/scripts/gen_build_prop.py:28 的 get_build_variant()
#    【没有 userdebug 这一档】—— 非 eng 一律按 user 处理：
#        if product_config["Eng"]: return "eng"
#        else:                     return "user"
#    所以 /system/build.prop 被硬写成 ro.adb.secure=1 + ro.debuggable=0
#    + ro.allow.mock.location=0，与 TARGET_BUILD_VARIANT=userdebug 无关。
#    ro.build.type=user / ro.build.flavor=gaokun3-user 也出自这里（实机 /system/build.prop
#    与 /product/etc/build.prop 都写 user），而指纹里是 :userdebug —— 不一致（SEC-5）。
#    没改：发布构建 ro.debuggable=0、ro.adb.secure=1，运行期行为本来就和 user 一致，
#    "user" 反倒是对外如实的身份；指纹里的 userdebug 是真实变体，也不该改。
#    ✅ 10-18 构建机核实：是 crDroid 自己的改动（soong 45aecb5b2 / build/make 74b87074f7，"Turn off debug props on
#    userdebug builds"），gen_build_prop.py:28-32 与 sysprop.mk 的分区 build.type 都改成"非 eng 即 user"；
#    指纹那头没改，由 scripts/crdroid-tree-fixes.py 的 tree-fix [23] 补齐。
#
# 2) init 的属性加载是【后来者覆盖】（property_service.cpp:807-815
#    的 map 插入：已存在且不同就 it->second = value），
#    加载顺序 /system → system_ext → vendor → odm → product。
#    但每条都要过 CheckPermissions(key, value, context, …)，
#    而 /vendor/* 用的是 vendor context —— ro.adb.secure / ro.debuggable
#    都是 system 属主的属性，vendor 无权设置，会被【静默拒绝】。
#    这就是为什么 device.mk 里 PRODUCT_PROPERTY_OVERRIDES 那两句
#    （落进 vendor/build.prop）一直没生效。
#
# system_ext 用 init context 且在 system 之后加载 → 能正确覆盖。
# WITH_ADB_INSECURE 走的就是这条路（PRODUCT_SYSTEM_EXT_PROPERTIES）。
#
# 连带效应：refs/aosp-build/tools/post_process_props.py:33-42 见到某个 build.prop 里
# ro.debuggable=1 就往【同一个文件】补 persist.sys.usb.config=adb（实机 system_ext 那份
# 的最后一行就是它）⇒ 开发构建开机即开 USB adb；发布构建没有这一行，也就没有那一条。
PRODUCT_SYSTEM_EXT_PROPERTIES += \
    ro.debuggable=1

endif # GAOKUN3_DEV_BUILD

# ★★ Codec2 HAL 选 AIDL —— 没有这行就一个解码器都没有。
#
# frameworks/av/media/codec2/hal/common/HalSelection.cpp:57
#     std::string selection = GetProperty("media.c2.hal.selection", "hidl");
#     if (selection == "aidl") return true;
#     else if (selection == "hidl") return false;
# 【默认是 hidl】。而 HIDL 的 Codec2 在 Android 15+ 已经彻底不可用 ——
# hwservicemanager 被移除，实机日志：
#     I HidlServiceManagement: Cannot list manifest for
#         android.hardware.media.c2@1.0::IComponentStore without hwservicemanager
# 于是 Codec2Client::CacheServiceNames() 拿到空列表：
#     I Codec2Client: No Codec2 services declared in the manifest.
# → MediaCodecList 为空 → App 一律 "Failed to create audio/mpeg decoder"，
#   screenrecord 报 "unable to create video/avc codec instance"。
#
# 这就是 docs/dev/audio-video.md 记录的"解码器一个都没有"。
# 它与 crDroid 无关 —— AOSP 16 上同样如此，只是真机设备树都会设这个属性，
# 我们这棵手搓的从来没设过。
#
# 运行时 setprop 之后 Codec2Client 立刻变成
#     I Codec2Client: Available Codec2 services: "software"
# 但必须在【开机时】就位：media.swcodec 自己也读它决定注册 AIDL 还是 HIDL。
#
# ⚠️ 走 PRODUCT_SYSTEM_EXT_PROPERTIES 而不是 PRODUCT_PROPERTY_OVERRIDES：
#    后者落进 vendor/build.prop，而该属性的 SELinux 上下文是
#    codec2_config_prop，vendor context 无权设置，会被静默拒绝
#    （同 ro.adb.secure / ro.debuggable 的坑，见本文件上面的说明）。
PRODUCT_SYSTEM_EXT_PROPERTIES += \
    media.c2.hal.selection=aidl

# ★ 把开发机的 adb 公钥烤进镜像 —— 不依赖 ro.adb.secure 的那条路。（仅开发构建）
#
# 2026-08-19 实测：即使 system_ext 里 ro.adb.secure=0 已经写进产物，
# 实机 adbd 仍然要求授权（adbd 的判定见 packages/modules/adb/daemon/main.cpp:223-226：
#   device_unlocked（我们 cmdline 带 verifiedbootstate=orange）为真 →
#   auth_required = GetBoolProperty("ro.adb.secure", false)），
# 说明运行时它读到的仍是 1 —— system_ext 的覆盖没有落地，原因待查。
#
# 而公钥这条路完全绕开属性：
#   packages/modules/adb/docs/dev/keystore.md
#     /adb_keys                 系统公钥，只读，随镜像出厂
#                               （system/core/rootdir/create_root_structure.mk:36
#                                把它做成指向 /product/etc/security/adb_keys 的符号链接）
#     /data/misc/adb/adb_keys   用户公钥，可写分区
#   adbd 收到 AUTH 挑战时会遍历这两处的所有 RSA 公钥。
#
# PRODUCT_ADB_KEYS → soong 的 AdbKeys（build/make/core/soong_config.mk:377），
# 且只在 eng/userdebug 保留（product_config.mk:493-494），正合我们。
# ⚠️ 反过来说：发布构建也是 userdebug，构建系统【不会】替我们清掉它
#    （refs/aosp-build/core/product_config.mk:488-495 只在非 eng/userdebug、或设了
#    RELEASE_BUILD_PURGE_PRODUCT_ADB_KEYS 时清）⇒ 必须由这个 ifeq 挡在发布构建之外。
#    ro.adb.secure=1 时这把钥匙就是"免确认进每一台用户机器"的后门（SEC-6）。
#    它是单值产品变量（refs/aosp-build/core/product.mk:252），本仓之外没人设（本地 refs
#    的 build/make 里没有；vendor/lineage 待构建机核实），release.sh 断言产物里没有它。
#
# ⚠️ adb_keys 文件本身【不入版本库】（.gitignore 挡着）——
#    它是开发机个人密钥，仓库是公开的。换机器时执行：
#        cp ~/.android/adbkey.pub device/huawei/gaokun3/adb_keys
ifeq ($(GAOKUN3_DEV_BUILD),1)
PRODUCT_ADB_KEYS := device/huawei/gaokun3/adb_keys
endif

# ★ A17（crDroid 17.0）：不要 Lineage 新加的「默认构建 fastbootd」。
#   refs/a17/vendor/lineage/config/common.mk:187-191：
#       ifneq ($(TARGET_DISABLE_FASTBOOTD),true)  PRODUCT_PACKAGES += fastbootd
#   （Lineage 提交 a438593b "Build fastbootd by default"，16.0 没有这一段）。fastbootd 是 recovery: true 的模块
#   （构建机 system/core/fastboot/Android.bp:142-146），会进 recovery ramdisk —— 而本机的 recovery ramdisk 随 vendor
#   走 OTA（AndroidBoard.mk）。A16 那份 recovery 里没有 fastbootd，
#   而上游 fastbootd 对 by-name 下任何节点都放行 flash / erase。
#   1.0 的 fastboot 由 gk3boot 执行端承担（tools/gk3boot/README.md）。
#   ⇒ 关掉，recovery 的内容保持与 A16 验过的那份一致。
#   不是产品变量（不在 build/make/core/product.mk 的列表里），是普通全局变量：放在本文件里，就在 common.mk 被
#   import 之前已经有值（inherit-product 只登记、之后才按序 include，product.mk:543-557）。
TARGET_DISABLE_FASTBOOTD := true

# crDroid 的「平板 + 无 modem」组合（叠在 AOSP 基座之上）：
#   common_full_tablet_wifionly.mk = common_mobile_full + tablet + wifionly
# Lineage 侧用 LOCAL_OVERRIDES_PACKAGES 顶掉 AOSP 的同类应用，不会真的撞包。
# ⓘ A17 变化（构建机 vendor/lineage `git diff crdroid/16.0 HEAD -- config/`，2026-10-25）：
#   · freeform_window_management 特性从 tablet.mk 挪到 common_mobile.mk —— 仍然装，结果不变；
#   · crdroid.mk 不再设 ro.surface_flinger.supports_background_blur=1 ⇒ 默认不开窗口模糊（A16 是开的）—— 由 device.mk 补回；
#   · crdroid.mk 的人脸解锁从「没设成 false 就装」改成「设成 true 才装」（refs/a17/vendor/lineage/config/crdroid.mk:68-77：
#     FaceUnlock + system_ext 的 android.hardware.biometrics.face.xml）。本树从没设 TARGET_FACE_UNLOCK_SUPPORTED ⇒
#     A16 装了 ParanoidSense，A17 默认【不装】。这等于替 v1.0-plan D23 / HW-10（人脸解锁保留与否，测完再定）做了「去掉」
#     的决定 —— 要保留就在这里加 TARGET_FACE_UNLOCK_SUPPORTED := true（同 TARGET_DISABLE_FASTBOOTD，普通全局变量）。⬜ 待用户定；
#   · common.mk 删掉了 debug.graphics.game_default_frame_rate.disabled=true —— 由 device.mk 补回，理由见那里；
#   · common.mk:243-246 起 userdebug 无条件装 adb_root（16.0 只在 WITH_SU=true 时装）—— 见 device.mk 同一节的说明。
$(call inherit-product, vendor/lineage/config/common_full_tablet_wifionly.mk)

# 设备配置（固件、init rc、HAL、图形、WiFi、音频 —— Stage 2–5 的全部成果）
# ★ 强制生成 OTA 包（M6）。
#
# build/make/core/Makefile:5793-5806（A17：refs/a17/build/make/core/Makefile:5864-5896）会在三种情况下把 build_ota_package 关掉，
# 我们中了两条：
#   1. INSTALLED_BOOTIMAGE_TARGET 为空 且 TARGET_NO_KERNEL=true
#      —— 本机内核在树外编，不产 boot.img；
#   2. 没有 recovery.fstab —— 本机 TARGET_NO_RECOVERY=true。
# 于是 `m otapackage` 报 "ninja: unknown target 'otapackage'"，
# 一个极具迷惑性的错误：看着像 target 名字写错了，其实是被条件编译掉了。
#
# 这一行会【一次跳过全部三条检查】。AOSP 自己的注释就写着它是给
# "A target without a kernel" 用的（同文件 5788-5792 行），正是我们这种
# 树外内核 + 无 recovery 的情况。我们的 AB_OTA_PARTITIONS 里也只有 super
# 里那四个动态分区，本来就不需要 boot/recovery 参与 OTA。
PRODUCT_BUILD_GENERIC_OTA_PACKAGE := true

# ★ Virtual A/B（M6）。launch.mk 而不是 compression.mk：
# 内核没有 CONFIG_DM_USER，压缩快照用不了（理由见 BoardConfig.mk 的 A/B 段）。
# launch.mk 只做三件事：PRODUCT_VIRTUAL_AB_OTA := true、
# ro.virtual_ab.enabled=true、装 e2fsck_ramdisk。
$(call inherit-product, $(SRC_TARGET_DIR)/product/virtual_ab_ota/launch.mk)

# ★ boot_control HAL —— 必须用我们自己那个，不能用 default。
# bootctl / update_engine_client 是排查 A/B 的标准工具，默认不装，这里显式带上
# （2026-08-20 实测：验证槽位状态时才发现两个都没有）。
# default 只把槽位写进 misc，然后指望 bootloader 去读；systemd-boot 不认识
# 那个结构。我们这个包住同一个 libboot_control，额外把槽位镜像进 ESP 的
# loader.conf。完整理由见 device/huawei/gaokun3/boot_control/Android.bp。
PRODUCT_PACKAGES += \
    android.hardware.boot-service.gaokun3 \
    update_engine \
    update_engine_sideload \
    update_verifier \
    bootctl \
    update_engine_client

$(call inherit-product, device/huawei/gaokun3/device.mk)

# ─────────────────────── GApps（MindTheGapps）───────────────────────
#
# ⚠️ **专有软件。** Google 的 APK 不在任何开源许可之下，把它们打进
#    【对外发布】的 ROM 等于替 Google 分发闭源应用。自用构建没问题。
#
# 用 inherit-product-if-exists：**没同步 vendor/gapps 也能正常构建**，
# 这样这棵公开的设备树对没有该仓库的人依然可用。
# 仓库来源见 manifests/local_manifest_gaokun3.xml。
#
# ⚠️ 不能走 Lineage 的 `WITH_GMS := true` —— 那条路 inherit 的是
#    vendor/partner_gms/products/gms.mk（Lineage 私有仓库的布局），
#    而 MindTheGapps 是 arm64/arm64-vendor.mk 布局，两者对不上。
$(call inherit-product-if-exists, vendor/gapps/arm64/arm64-vendor.mk)

# ★★ 去掉 Google 的开机向导与撞名的 libjni_latinimegoogle
#    —— **不在这里做**，见下面的理由。
#
# Google 的 SetupWizard 在初始化阶段强制连 Google 服务器，
# **中国大陆网络下会卡在欢迎页过不去，设备根本无法完成初始化**。
# crDroid 自带的 LineageSetupWizard 不依赖任何 Google 服务，保留它即可。
#
# ⚠️★ **`PRODUCT_PACKAGES := $(filter-out X,$(PRODUCT_PACKAGES))` 在这里是无效的。**
#   我先写了这一句，然后用 `get_build_var PRODUCT_PACKAGES` 实测 ——
#   `SetupWizard` 仍在 1005 个条目里。现代 AOSP 的产品配置会从**继承图**
#   重新推导 PRODUCT_PACKAGES，产品 makefile 中途的直接赋值会被丢弃。
#   ★ 判据就一句：get_build_var PRODUCT_PACKAGES 里还能不能 grep 到 SetupWizard。
#   （这个坑很贵：不实测的话，这一版会带着 Google 欢迎页发出去，
#     而我会以为已经删掉了。）
#
# ⇒ 正确做法是**改源头**：由 scripts/crdroid-tree-fixes.py 从
#   vendor/gapps/arm64/arm64-vendor.mk 里删掉这两个包名，
#   并从 vendor/gapps/arm64/Android.bp 里删掉 libjni_latinimegoogle 模块
#   （它与 packages/inputmethods/LatinIME 撞名，见那里的注释）。
#   repo sync 会还原 vendor/gapps，所以 tree-fixes 每次构建前都要跑（幂等）。

# 动态分区（super）。这是 product 变量，必须设在这里而不是 BoardConfig.mk
# —— 后者解析时它已经只读了（build/make/core/product.mk:311）。
PRODUCT_USE_DYNAMIC_PARTITIONS := true

PRODUCT_NAME   := lineage_gaokun3
PRODUCT_DEVICE := gaokun3
PRODUCT_BRAND  := Huawei
PRODUCT_MODEL  := MateBook E Go
PRODUCT_MANUFACTURER := Huawei

# ★ 声明本机是平板（issue #5：QQ 按 ro.build.characteristics 判断能不能走平板登录）。
#   此前从没设过 ⇒ 落到默认值 `default`（build/make/core/product_config.mk:425-428）。
#   ⚠️ 上面 inherit 的 common_full_tablet_wifionly.mk【不设】它 —— Lineage 的 tablet.mk
#   只补 large_screen_common.mk（见本文件开头那段），名字带 tablet 不等于声明了 tablet。
#   同一个值还作为 aapt 的 --product 传下去（product_config.mk:428 → TARGET_AAPT_CHARACTERISTICS
#   → soong_config.mk:103 的 AAPTCharacteristics、definitions.mk:2331），
#   即资源里 product="tablet" 的变体会被选中 —— 这正是平板该有的样子。
#   单值变量（product.mk:101），后写的覆盖先写的，所以放在所有 inherit 之后。
#   ⚠️ 写 build.prop 那一步在 soong 里，本地 refs 没有 soong 源码，**未逐行核对**；
#   构建后用 `grep ro.build.characteristics out/target/product/gaokun3/system/build.prop` 验。
PRODUCT_CHARACTERISTICS := tablet

# API 等级：36（= Android 16）。★ 换到 A17（SDK 37）后【不提】到 37（docs/dev/android-bringup.md）。
#   · 它决定 VSR_VENDOR_API_LEVEL（refs/a17/build/make/core/product_config.mk:660-663，sdk-to-vendor-api-level(36)
#     = 202504；本树不设 BOARD_SHIPPING_API_LEVEL，:664-675 那支不走）。
#   · 202604 起的新要求都按 VSR 判：例如 config.mk:469-485 只有 VSR >= 202604 才 TARGET_RESTRICTS_ASHMEM_USAGE
#     （要内核 memfd_class）；保持 36 就不触发。VINTF 的 target-level 也跟着留在 202504（manifest.xml）。
#   · 但有一样【不】看它：PRODUCT_ENFORCE_SELINUX_TREBLE_LABELING 按 RELEASE_BOARD_API_LEVEL（cp2a = 202604）
#     默认打开（product_config.mk:762-766），见下面 tracking list 那一段。
PRODUCT_SHIPPING_API_LEVEL := 36

# ─── SELinux Treble 标签检查（A17 新增，cp2a 默认强制）───
# Makefile:3675-3677（PLATFORM_SEPOLICY_VERSION >= 202604 时 droidcore 依赖 check-selinux-treble-labeling）+
# product_config.mk:762-766（RELEASE_BOARD_API_LEVEL >= 202604 且没设时置 true）⇒ 违例直接让 `m` 失败。
# 规则见构建机 system/sepolicy/tests/treble_labeling_tests.md：vendor 的 file_contexts 不许标 /system、/system_ext、
# /product 下的文件，等五条。
# 已知的一条违例不是我们写的：BoardConfig.mk 引入的 dragonboard minigbm_msm 策略
#   refs/a17/device/linaro/dragonboard/shared/graphics/minigbm_msm/sepolicy/file_contexts:8
#   /(system|vendor)/lib(64)?/libdrm.so  → same_process_hal_file
# 用官方的 tracking list 豁免这一条（只豁免列出的路径，其余照常强制），而不是整个关掉
# （PRODUCT_ENFORCE_SELINUX_TREBLE_LABELING := false 会把所有违例都降成警告）。
# 本树自己的 sepolicy/ 里没有标平台路径的行（2026-10-25 grep），也没有 seapp_contexts、没有 coredomain 类型。
# ⬜ 未编译。若 `m` 报出别的违例，先看是不是该改策略，确实要豁免再加进这个文件。
PRODUCT_SELINUX_TREBLE_LABELING_TRACKING_LIST_FILE := device/huawei/gaokun3/selinux_treble_labeling_tracking.yaml

# ─── 关掉 VINTF 的内核兼容性检查 ───
#
# ★ 有了真 boot.img 之后这个检查才真正生效（此前没有内核可提取，只会打一句
#   "Neither INSTALLED_KERNEL_TARGET nor INSTALLED_BOOTIMAGE_TARGET is defined"
#   的 warning）。开着会构建失败：
#     ERROR: No kernel entry found for kernel version 7.2 at kernel FCM
#            version 202504   （Minimum LTS: 6.12.0）
#   本机跑的是【主线 v7.2-rc2】，比任何 FCM 认识的版本都新，而且 checker 连
#   "7.2.0-rc2-gaokun3+" 都解析不了（日志里三行 Cannot parse 就是它）。
#
# 这个变量正是 AOSP 给出的逃生口（build/make/core/Makefile:5497 起整段由它控制，
# 那句 warning 自己也把它列为第 4 种解法）。本机不追 VTS/CTS、不启用 AVB，
# 关掉没有副作用 —— 但要记住：**这道检查本来是防"内核与框架要求不匹配"的**，
# 换 ROM 大版本时得自己留意内核是否还满足新框架的假设。
PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false
