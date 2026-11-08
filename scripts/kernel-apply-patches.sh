#!/usr/bin/env bash
# 把本仓 patches/kernel/ 下的内核补丁应用到一棵内核树。
#
# ★ 为什么需要这个脚本：patches/ 下的内核补丁长期以来【没有任何消费者】——
#   全靠人在构建机上手动 git apply。本会话已经为此付过一次代价：
#   两个 DTS 改动只活在构建机工作区里、从未入库（提交 ee5eca9 才补上）。
#   没有消费者的东西一定会漂。
#
# 用法：
#   bash scripts/kernel-apply-patches.sh /path/to/linux
#   bash scripts/kernel-apply-patches.sh /path/to/linux --check   # 只检查，不改动
#   bash scripts/kernel-apply-patches.sh /path/to/linux --verify  # ★ 精确核对：从 HEAD 起临时 worktree
#                                                                 #   按本表打满，逐文件与真实树比 md5
#
# 幂等：已经打上的补丁会被跳过（用反向 --check 判定），所以可以反复跑。
#
# ★★ 基线：mainline【v7.2.9 stable】（git.kernel.org stable 仓库的 tag v7.2.9 = 5fce161649b4）
#    + gaokun-buildbot 那一层（19 个提交，git am -3），本表打在它上面。
#
# ⚠️★ --check 在【已经打满】的树上有一个已知盲区：叠加补丁里靠前的那个
#    （如 kernel/dts/0004 删掉后摄节点、kernel/dts/0005 又在同一位置写回）反向 --check 对不上，会被报成"打不上"，
#    而树其实是对的。--verify 没有这个问题：它不逐个判定，而是把整条链在干净 worktree 上重放，
#    然后问"每个被补丁碰过的文件，真实树里的与重放出来的是否逐字节相同"。
set -uo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TREE=${1:-}
MODE=${2:-apply}

[ -n "$TREE" ] || { echo "用法: $0 <内核树路径> [--check]" >&2; exit 2; }
[ -f "$TREE/Makefile" ] && [ -d "$TREE/kernel" ] || {
    echo "✗ $TREE 看着不像内核树（缺 Makefile 或 kernel/）" >&2; exit 2; }

# ⚠️ 只列发版内核补丁（按 patches/kernel/<子系统>/ 分组，各子系统修改的文件集正交，子系统内部按本地编号 0001..NNNN 排列）。
#    每个补丁的背景、技术说明与上游状态均见补丁文件自身的 commit message 及 Origin / Upstream-Status trailer。
#    其余补丁归属各自目录：patches/android/（crdroid-tree-fixes.py）、patches/mesa/（mesa-a17/regen.sh）、
#    patches/libcamera/（setup-libcamera-tree.sh）、patches/resukisu/（kernel-setup-resukisu.sh）。
KPATCHES=(
    # ─── 1. kernel/core：核心内核、Android 基础支撑（ashmem / xt_quota2 / bpffs / SELinux）、架构与通用子系统 ───
    # 0006 引入 CONFIG_ARM64_USER_MMAP_WINDOW_39（kernel-config-android.sh 开启并断言）；
    # 0008 与 crdroid-tree-fixes.py [36][37] 配套。
    kernel/core/0001-efi-pstore-register-backend-when-efivars-ops-arrive-.patch
    kernel/core/0002-bpf-inode-label-bpffs-lazily-for-android-genfscon.patch
    kernel/core/0003-remoteproc-qcom-ratelimit-repeat-handover-error.patch
    kernel/core/0004-staging-android-port-ashmem-from-ack.patch
    kernel/core/0005-netfilter-port-xt-quota2-from-ack.patch
    kernel/core/0006-arm64-user-mmap-window-39-bits-on-48-bit-page-tables.patch
    kernel/core/0007-selinux-netlink-route-remap-getlink-getneigh-priv-perms.patch
    kernel/core/0008-kernel-report-a-set-release-to-android-apps.patch
    kernel/core/0009-misc-fastrpc-fix-context-leak-and-hang-on-signal-interrupted-invoke.patch

    # ─── 2. kernel/pcie-pm：PCIe、NVMe、cpuidle、系统休眠与唤醒基础设施 ───
    # 深度待机与 PCIe / NVMe 恢复处理详见 docs/dev/deep-sleep.md。
    # 0003 依赖 0001（L2 下保留 PCIe-MEM active-only 投票）；0011–0012 与 kernel/dts/0021、kernel/wifi/0027 配套。
    kernel/pcie-pm/0001-PCI-qcom-keep-an-active-only-PCIe-MEM-vote-in-L2-on-SC8280XP.patch
    kernel/pcie-pm/0002-serial-qcom-geni-add-force-suspend-resume-to-system-sleep-callbacks.patch
    kernel/pcie-pm/0003-PCI-Allow-D3-for-native-hotplug-capable-Root-Ports-on-non-x86-platforms.patch
    kernel/pcie-pm/0004-PCI-dwc-keep-the-host-up-if-the-link-fails-on-resume.patch
    kernel/pcie-pm/0005-PCI-dwc-retry-a-link-that-doesn-t-come-back-on-resume.patch
    kernel/pcie-pm/0006-PCI-dwc-panic-if-the-link-to-the-NVMe-disk-is-lost-on-resume.patch
    kernel/pcie-pm/0007-nvme-panic-when-a-disk-is-marked-dead.patch
    kernel/pcie-pm/0008-PCI-qcom-resume-the-SC8280XP-controllers-in-parallel.patch
    kernel/pcie-pm/0009-PCI-dwc-abort-suspend-when-a-device-has-signalled-PME.patch
    kernel/pcie-pm/0010-cpuidle-psci-wait-after-a-denied-cluster-request-in-s2idle.patch
    kernel/pcie-pm/0011-PCI-Add-support-for-PCIe-WAKE-interrupt.patch
    kernel/pcie-pm/0012-PM-sleep-disarm-wake-IRQs-before-resuming-device-IRQs.patch

    # ─── 3. kernel/wifi：ath11k、mac80211 与 MHI ───
    # 0012–0026 叠在 0002–0011 之上（0017、0023 修改 0011 引入的辅助函数；0025 覆盖 0023–0024 之后的失败路径）。
    kernel/wifi/0001-bus-mhi-keep-firmware-images-across-power-cycles-for-ath11k.patch
    kernel/wifi/0002-wifi-ath11k-keep-the-HAL-when-crash-recovery-fails.patch
    kernel/wifi/0003-wifi-ath11k-dont-reap-rings-that-crash-recovery-frees-on-suspend.patch
    kernel/wifi/0004-wifi-ath11k-keep-firmware-crash-recovery-away-from-system-suspend.patch
    kernel/wifi/0005-wifi-ath11k-hand-a-firmware-crash-found-on-resume-to-crash-recovery.patch
    kernel/wifi/0006-wifi-ath11k-stay-awake-for-a-while-after-crash-recovery.patch
    kernel/wifi/0007-wifi-ath11k-arm-the-WoW-disconnect-events-only-when-connected.patch
    kernel/wifi/0008-wifi-mac80211-clear-the-suspending-flag-when-the-driver-fails-to-suspend.patch
    kernel/wifi/0009-wifi-ath11k-keep-the-system-awake-while-a-scan-runs.patch
    kernel/wifi/0010-wifi-ath11k-retry-the-suspend-when-the-firmware-declines-WoW.patch
    kernel/wifi/0011-wifi-ath11k-end-a-reset-that-has-no-radio-to-restart.patch
    kernel/wifi/0012-wifi-ath11k-clear-the-started-flag-of-a-stopped-shadow-timer.patch
    kernel/wifi/0013-wifi-mac80211-set-RX-block-ack-sessions-up-again-after-WoWLAN.patch
    kernel/wifi/0014-wifi-ath11k-keep-the-registered-hw-when-a-restart-fails.patch
    kernel/wifi/0015-wifi-ath11k-don-t-disable-the-PCI-interrupts-twice.patch
    kernel/wifi/0016-wifi-ath11k-reset-again-when-the-firmware-crashes-during-a-restart.patch
    kernel/wifi/0017-wifi-ath11k-back-off-from-a-firmware-crash-loop-then-give-up.patch
    kernel/wifi/0018-wifi-ath11k-don-t-fail-the-removal-of-a-station.patch
    kernel/wifi/0019-wifi-mac80211-shut-down-RX-BA-session-timer-on-teardown.patch
    kernel/wifi/0020-wifi-ath11k-fix-locking-problem-in-ath11k_dp_rx_tid_del_func.patch
    kernel/wifi/0021-wifi-ath11k-avoid-IDR-mutation-during-pending-mgmt-TX-cleanup.patch
    kernel/wifi/0022-wifi-ath11k-avoid-IDR-mutation-during-vif-mgmt-TX-cleanup.patch
    kernel/wifi/0023-wifi-ath11k-don-t-fail-a-restart-on-a-firmware-crash.patch
    kernel/wifi/0024-wifi-ath11k-clear-CRASH_FLUSH-only-after-recovery-rebuilds-the-rings.patch
    kernel/wifi/0025-wifi-ath11k-cover-a-restart-that-fails-and-a-suspend-that-fails-after-it.patch
    kernel/wifi/0026-wifi-ath11k-handle-a-failed-power-up-in-a-reset.patch
    kernel/wifi/0027-wifi-ath11k-keep-the-system-awake-briefly-after-a-WoW-wakeup.patch

    # ─── 4. kernel/ec-usb：华为高鲲 EC、电池、Type-C UCSI、USB Gadget 与 xHCI ───
    # 0009 与 USB 端口 HAL（device/huawei/gaokun3/usb）配套；0010 与 kernel/dts/0015 配套；
    # 0011 引入 CONFIG_USB_ANDROID_USB_STATE=y（kernel-config-android.sh 开启并断言）。
    kernel/ec-usb/0001-power-supply-gaokun-battery-decode-status-bits-and-capacity-level.patch
    kernel/ec-usb/0002-platform-arm64-gaokun-ec-report-initial-lid-state.patch
    kernel/ec-usb/0003-usb-typec-ucsi-restore-the-USB-data-check-on-partner-change.patch
    kernel/ec-usb/0004-usb-typec-ucsi-huawei_gaokun-set-the-number-of-ports-at-probe.patch
    kernel/ec-usb/0005-usb-typec-ucsi-huawei_gaokun-report-HPD-when-leaving-DP-alt-mode.patch
    kernel/ec-usb/0006-usb-typec-ucsi-huawei_gaokun-dont-block-the-EC-notifier.patch
    kernel/ec-usb/0007-usb-typec-ucsi-huawei_gaokun-set-the-orientation-before-DP.patch
    kernel/ec-usb/0008-usb-typec-ucsi-huawei_gaokun-resend-dropped-GET_CONNECTOR_STATUS.patch
    kernel/ec-usb/0009-usb-typec-ucsi-huawei_gaokun-leave-the-usb_0-role-to-userspace.patch
    kernel/ec-usb/0010-platform-arm64-huawei-gaokun-ec-disable-the-IRQ-across-suspend.patch
    kernel/ec-usb/0011-usb-gadget-add-android_usb-class-for-UsbDeviceManager.patch
    kernel/ec-usb/0012-usb-gadget-configfs-take-none-as-unbind-and-wait-for-an-absent-UDC.patch
    kernel/ec-usb/0013-platform-arm64-huawei-gaokun-ec-report-the-real-lid-state-on-resume.patch
    kernel/ec-usb/0014-usb-xhci-don-t-fail-system-suspend-on-a-dead-host.patch
    kernel/ec-usb/0015-platform-arm64-huawei-gaokun-ec-use-an-edge-triggered-IRQ-while-suspended.patch
    kernel/ec-usb/0016-power-supply-huawei-gaokun-battery-report-temperature.patch

    # ─── 5. kernel/drm：DRM 核心、MSM DPU 色彩与平面、DSI PHY、Himax 面板、MSM GPU ───
    # 显示色彩详见 docs/dev/display-color.md。0003 与 kernel/dts/0017 配套；
    # 0005 依赖 0004 并与 android/drm-hwcomposer/0002（tree-fix [32]）配套；
    # 0007 与 android/drm-hwcomposer/0001（tree-fix [31]）配套。
    # 0016 需要 mesa/0012（Turnip VSC 修正）：内核报告抢占后 Turnip 走 a6xx 抢占路径。
    kernel/drm/0001-drm-crtc-drop-racy-BUG_ON-in-fence_to_crtc.patch
    kernel/drm/0002-drm-msm-dsi-phy-7nm-reapply-bonded-pll-init-reverting-5de981b7db.patch
    kernel/drm/0003-drm-panel-himax-hx83121a-read-the-panel-from-the-board-s-ID-pins.patch
    kernel/drm/0004-drm-msm-dpu-fix-the-CTM-to-PCC-coefficient-conversion.patch
    kernel/drm/0005-drm-msm-dpu-apply-the-gaokun3-factory-panel-calibration.patch
    kernel/drm/0006-drm-msm-dpu-dither-8-bpc-panels.patch
    kernel/drm/0007-drm-msm-dpu-expose-ABGR-and-XBGR-2101010-on-planes.patch
    kernel/drm/0008-drm-panel-himax-hx83121a-report-a-linear-backlight-scale.patch
    kernel/drm/0009-drm-msm-fix-barriers-accessing-ctx-vm.patch
    kernel/drm/0010-drm-msm-rework-queuelock.patch
    kernel/drm/0011-drm-msm-synchronize-vm-creation-on-ctxlock.patch
    kernel/drm/0012-drm-msm-gem-validate-lazy-vm-in-gem_new.patch
    kernel/drm/0013-drm-msm-allow-lazy-vm-creation-to-fail.patch
    kernel/drm/0014-drm-msm-fix-sched_prio-inversion-within-a-ring.patch
    kernel/drm/0015-drm-msm-a6xx-reset-preemption-records-across-suspend.patch
    kernel/drm/0016-drm-msm-a6xx-enable-preemption-on-a690.patch

    # ─── 6. kernel/iris：qcom-iris 视频解码驱动 ───
    # 配套 DT 见 kernel/dts/0009–0010；需 CONFIG_VIDEO_QCOM_IRIS=y、CONFIG_VIDEO_QCOM_VENUS 不设。
    # 0009 叠在 0006 之上；0012 需与 patches/mesa/0002 及 crdroid-tree-fixes.py [29][30] 一起生效。
    kernel/iris/0001-media-iris-irq-thread-skips-hw-access-after-power-off.patch
    kernel/iris/0002-media-iris-fail-firmware-boot-on-invalid-uc_region.patch
    kernel/iris/0003-media-iris-venus-compatible-decoder-capture-g_fmt.patch
    kernel/iris/0004-media-iris-take-core-lock-when-scanning-the-instance-list.patch
    kernel/iris/0005-media-iris-flush-all-on-capture-streamoff-except-drc.patch
    kernel/iris/0006-media-iris-gen1-decoder-seek-mode-switch.patch
    kernel/iris/0007-media-iris-no-output-flush-on-eos.patch
    kernel/iris/0008-media-iris-restore-rc2-stop-check-reverting-0ac05c4.patch
    kernel/iris/0009-media-iris-configure-output-after-flush-before-first-capture-streamon.patch
    kernel/iris/0010-media-iris-gen1-recover-from-firmware-watchdog.patch
    kernel/iris/0011-media-iris-keep-decoder-crop-across-capture-s_fmt.patch
    kernel/iris/0012-media-iris-gen1-decode-10-bit-to-p010.patch

    # ─── 7. kernel/camera：camcc 时钟/GDSC、camss 与前后摄传感器驱动（ov13b10 / hi846）───
    # 0003 与 kernel/dts/0005 配套，需 CONFIG_VIDEO_OV13B10=y 与 CONFIG_VIDEO_DW9714=y；
    # 0007 叠在 0003 之上，0008 叠在 0005–0006 之上。
    kernel/camera/0001-clk-qcom-camcc-sc8280xp-gdsc-wait-vals.patch
    kernel/camera/0002-clk-qcom-camcc-sc8280xp-mark-camnoc-ahb-rcgs-shared.patch
    kernel/camera/0003-media-i2c-ov13b10-of-match-and-gaokun3-power-sequence.patch
    kernel/camera/0004-media-camss-continue-without-never-bound-sensors.patch
    kernel/camera/0005-media-i2c-hi846-Report-the-pixel-array-rate-and-the-link-frequency.patch
    kernel/camera/0006-media-i2c-hi846-Keep-the-VBLANK-frame-length-when-setting-the-exposure.patch
    kernel/camera/0007-media-i2c-ov13b10-Update-the-exposure-limit-on-every-format-change.patch
    kernel/camera/0008-media-i2c-hi846-Update-the-exposure-limit-on-every-format-change.patch

    # ─── 8. kernel/audio：ASoC sc8280xp、LPASS 编解码宏、WSA883x 功放、SoundWire 与 Q6APM ───
    # 0007–0008 与 kernel/dts/0016 及 device/huawei/gaokun3/bin/audio-route.sh 配套；
    # 0011–0012 与内置麦克风阵列（device/huawei/gaokun3/micarray/）配套。
    kernel/audio/0001-asoc-sc8280xp-retune-wsa-speaker-gain-ceilings.patch
    kernel/audio/0002-ASoC-codecs-lpass-wsa-macro-Switch-to-PM-clock-framework-for-runtime-PM.patch
    kernel/audio/0003-ASoC-codecs-lpass-va-macro-Switch-to-PM-clock-framework-for-runtime-PM.patch
    kernel/audio/0004-ASoC-codecs-lpass-wsa-macro-Use-devm_clk_hw_register-for-MCLK-output.patch
    kernel/audio/0005-ASoC-codecs-lpass-tx-macro-switch-to-PM-clock-framework-for-runtime-PM.patch
    kernel/audio/0006-ASoC-codecs-lpass-rx-macro-switch-to-PM-clock-framework-for-runtime-PM.patch
    kernel/audio/0007-soundwire-qcom-let-the-last-data-port-be-allocated.patch
    kernel/audio/0008-ASoC-codecs-wsa883x-hand-the-PA-gain-over-to-DRE-after-unmute.patch
    kernel/audio/0009-ASoC-codecs-lpass-va-macro-mute-the-decimators-while-they-settle.patch
    kernel/audio/0010-ASoC-codecs-lpass-va-macro-restore-the-high-pass-corner-after-the-start.patch
    kernel/audio/0011-ASoC-qcom-sc8280xp-let-VA-capture-use-four-channels.patch
    kernel/audio/0012-ASoC-qcom-q6apm-dai-keep-the-source-s-channel-order-on-capture.patch

    # ─── 9. kernel/input：Himax SPI 触摸屏、HX83121A 级联手写笔从片与 USB HID ───
    # 0001–0010、0012–0021 按顺序叠在 drivers/input/touchscreen/himax/ 上，回放工具见 scripts/touch/hxsim；
    # 0011 与 kernel/dts/0013、device/huawei/gaokun3/stylus 配套，需 CONFIG_TOUCHSCREEN_HIMAX_HX83121A_PEN=y。
    kernel/input/0001-Input-himax-spi-runtime-tunable-coordinate-fuzz.patch
    kernel/input/0002-Input-himax-spi-jump-detection-vs-predicted-position.patch
    kernel/input/0003-Input-himax-spi-tunable-isolated-spike-and-edge-gates.patch
    kernel/input/0004-Input-himax-spi-fix-SPI-read-retry-and-split-event-stack.patch
    kernel/input/0005-Input-himax-spi-do-not-abandon-the-grid-when-zones-fill.patch
    kernel/input/0006-Input-himax-spi-mark-palm-zones-instead-of-deleting-them.patch
    kernel/input/0007-Input-himax-spi-per-stage-counters-and-raw-grid-dumps.patch
    kernel/input/0008-Input-himax-spi-report-axis-resolution.patch
    kernel/input/0009-Input-himax-spi-fix-two-kerneldoc-blocks.patch
    kernel/input/0010-Input-himax-spi-pressure-follows-axes-and-log-zone-area.patch
    kernel/input/0011-Input-add-HX83121A-cascade-slave-pen-driver.patch
    kernel/input/0012-Input-himax-spi-expand-the-strongest-peaks-when-there-are-too-many.patch
    kernel/input/0013-Input-himax-spi-hand-map-and-palm-cancel.patch
    kernel/input/0014-Input-himax-spi-narrow-the-track-match-radius.patch
    kernel/input/0015-Input-himax-spi-keep-edge-compensated-positions-on-the-panel.patch
    kernel/input/0016-Input-himax-spi-hold-a-touch-s-down-position-until-it-moves.patch
    kernel/input/0017-Input-himax-spi-filter-the-reported-position-by-speed.patch
    kernel/input/0018-Input-himax-spi-optional-EGoTouchRev-rebuild-style-detection.patch
    kernel/input/0019-Input-himax-spi-default-to-the-EGoTouchRev-rebuild-style-detection.patch
    kernel/input/0020-Input-himax-spi-release-the-tap-lock-on-signal-and-step.patch
    kernel/input/0021-Input-himax-spi-end-a-touch-early-when-the-firmware-sees-no-finger.patch
    kernel/input/0022-HID-usbhid-add-a-quirk-to-send-SET_IDLE-on-resume.patch

    # ─── 10. kernel/dts：SC8280XP 与 gaokun3 设备树 ───
    # 0004→0005→0006→0008→0012 依次叠在 sc8280xp-huawei-gaokun3-camera.dtsi 上（0006 需 CONFIG_LEDS_CLASS_FLASH=y、
    # CONFIG_LEDS_QCOM_FLASH=y）；0009→0010 启用 iris 视频解码节点；0011 调整默认 CMA 范围（影响 prebuilt-boot dtb）。
    kernel/dts/0001-arm64-dts-gaokun3-drive-ts-mode-gpio174-low.patch
    kernel/dts/0002-arm64-dts-sc8280xp-add-cpu-cooling-maps.patch
    kernel/dts/0003-arm64-dts-gaokun3-usb0-otg-for-usb-adb.patch
    kernel/dts/0004-arm64-dts-gaokun3-camera-drop-rear-s5k3l6.patch
    kernel/dts/0005-arm64-dts-gaokun3-camera-rear-ov13b10-with-board-rails.patch
    kernel/dts/0006-arm64-dts-gaokun3-pmic-flash-led-channels-1-4.patch
    kernel/dts/0007-arm64-dts-gaokun3-touchscreen-fuzz-0.patch
    kernel/dts/0008-arm64-dts-gaokun3-camera-rear-rotation-180-confirmed.patch
    kernel/dts/0009-arm64-dts-qcom-sc8280xp-add-iris-and-videocc.patch
    kernel/dts/0010-arm64-dts-gaokun3-enable-iris.patch
    kernel/dts/0011-arm64-dts-gaokun3-keep-default-cma-below-4g.patch
    kernel/dts/0012-arm64-dts-gaokun3-camera-rear-privacy-led.patch
    kernel/dts/0013-arm64-dts-gaokun3-enable-spi20-and-the-pen-slave.patch
    kernel/dts/0014-arm64-dts-gaokun3-vreg-l9d-always-on.patch
    kernel/dts/0015-arm64-dts-gaokun3-EC-IRQ-as-wakeup-source.patch
    kernel/dts/0016-arm64-dts-gaokun3-map-the-speaker-ports-statically.patch
    kernel/dts/0017-arm64-dts-gaokun3-add-the-panel-ID-pins.patch
    kernel/dts/0018-arm64-dts-gaokun3-vreg-l4b-always-on.patch
    kernel/dts/0019-arm64-dts-gaokun3-lower-the-DMIC-clock-to-3.2-MHz.patch
    kernel/dts/0020-arm64-dts-qcom-sc8280xp-huawei-gaokun3-keep-VCC_WLAN_3R9-on.patch
    kernel/dts/0021-arm64-dts-qcom-sc8280xp-huawei-gaokun3-move-wake-gpios-to-the-Wi-Fi-node.patch
    kernel/dts/0022-arm64-dts-qcom-sc8280xp-mark-the-CDSP-FastRPC-context-banks-dma-coherent.patch

    # ─── 11. kernel/fingerprint：QSEECOM 与指纹传输驱动（/dev/gaokun3_fpcmd，CONFIG_GAOKUN3_FPCMD 默认 n）───
    kernel/fingerprint/0001-firmware-qcom-scm-qseecom-app-load-shutdown-listener.patch
    kernel/fingerprint/0002-firmware-qcom-gaokun3-fingerprint-ioctl.patch
    kernel/fingerprint/0003-firmware-qcom-gaokun3-fpcmd-add-the-Goodix-GDIX5125-sensor.patch
)

# 初版 kernel/core/0008 曾直接改写 init/version.c 的 init_uts_ns；现已不再触碰该文件，
# --verify 不会对比它，因此单独拦截残留旧实现的内核树。
if grep -q spoof_uts_release "$TREE/init/version.c"; then
    echo "✗ 旧版 init/version.c (spoof_uts_release) 仍在树里，请先还原 init/version.c" >&2
    exit 1
fi

# ★★ 指纹判据：补丁是否【已在树里】。
#
# ⚠️ 为什么不能只靠 `git apply -R --check`：**用 fuzz 打进去的补丁，反向检查
#    一定失败**（上下文已经和补丁里的不一致了）。于是脚本会认为"没打过"，
#    再用 fuzz 打一遍 —— 结果就是【同一段代码出现两份】。
#
# 判据：从补丁里挑【最长的几条】新增行，到它要改的文件里 grep，要求【全部命中】。
#
# ★ 三处防碰撞约束：
#    ① 探针挑【最长】的新增行（> 25 字符）—— 越长越不可能与既有代码撞车
#       （例如 kernel/dts/0002 的第一条短行 `polling-delay-passive = <250>;` 在未打补丁的
#        sc8280xp.dtsi 里本来就有，挑最长行 `cooling-device = <&cpu0 THERMAL_NO_LIMIT ...>` 才唯一）。
#    ② 取最多 3 条并要求【全部】命中 —— 单条撞车是偶然，三条同时撞车基本不可能。
#    ③ 剔除同时出现在 `-` 侧（仅调整顺序或缩进）的行 —— 避免对调两行代码的补丁在未打补丁的树上
#       因原行本就存在而误判为"已应用"。
already_applied() {
    local f="$1" probes files ff hit n
    probes=$(grep -E '^\+[^+]' "$f" | sed 's/^+//' \
             | grep -vE '^[[:space:]]*$' \
             | grep -vxF -f <(grep -E '^-[^-]' "$f" | sed 's/^-//') \
             | awk 'length($0) > 25' | sort -u \
             | awk '{ print length($0), $0 }' | sort -rn | head -3 | cut -d' ' -f2-)
    [ -n "$probes" ] || return 1
    files=$(grep -E '^\+\+\+ b/' "$f" | sed 's|^+++ b/||')
    n=0
    while IFS= read -r probe; do
        [ -n "$probe" ] || continue
        n=$((n + 1))
        hit=0
        for ff in $files; do
            [ -f "$TREE/$ff" ] || continue
            if grep -qF "$probe" "$TREE/$ff"; then hit=1; break; fi
        done
        [ "$hit" = 1 ] || return 1
    done <<< "$probes"
    [ "$n" -gt 0 ] || return 1
    return 0
}

# ── --verify：干净 worktree 重放整条链，再与真实树逐文件比 ──
if [ "$MODE" = "--verify" ]; then
    WT=$(mktemp -d "${TMPDIR:-/tmp}/kap-verify.XXXXXX") && rmdir "$WT"
    git -C "$TREE" worktree add --detach -q "$WT" HEAD || { echo "✗ 建不了临时 worktree" >&2; exit 2; }
    trap 'git -C "$TREE" worktree remove --force "$WT" 2>/dev/null' EXIT
    echo "内核树: ${TREE}（HEAD $(git -C "$TREE" rev-parse --short HEAD)）"
    echo "重放到: $WT"
    fails=0; fuzzed=0
    for p in "${KPATCHES[@]}"; do
        f="$REPO/patches/$p"
        [ -f "$f" ] || { echo "✗ 缺文件 $p"; fails=$((fails + 1)); continue; }
        if git -C "$WT" apply "$f" 2>/dev/null; then
            :
        elif patch -p1 -d "$WT" -s --fuzz=3 < "$f" >/dev/null 2>&1; then
            echo "  ⚠️ 用了 fuzz=3  $p"; fuzzed=$((fuzzed + 1))
        else
            echo "✗ 干净树上打不上  $p"; fails=$((fails + 1))
        fi
    done
    # 被任何补丁碰过的文件，逐个比
    files=$(for p in "${KPATCHES[@]}"; do
                grep -hE '^\+\+\+ b/' "$REPO/patches/$p" 2>/dev/null; done | sed 's|^+++ b/||' | sort -u)
    diffs=0; same=0
    for ff in $files; do
        a=$(md5sum < "$WT/$ff" 2>/dev/null | cut -c1-32)
        b=$(md5sum < "$TREE/$ff" 2>/dev/null | cut -c1-32)
        if [ "$a" = "$b" ]; then same=$((same + 1)); else
            echo "✗ 与配方重放不一致  $ff"
            diff -u "$WT/$ff" "$TREE/$ff" 2>/dev/null | grep -E '^[-+][^-+]' | head -6 | sed 's/^/      /'
            diffs=$((diffs + 1))
        fi
    done
    echo
    echo "重放：打不上 $fails · 用了 fuzz ${fuzzed}；核对：一致 $same 个文件 · 不一致 $diffs 个"
    if [ "$fails" -eq 0 ] && [ "$diffs" -eq 0 ]; then
        echo "✓ 真实树里被补丁碰过的每个文件都与配方逐字节相同"
        exit 0
    fi
    exit 1
fi

cd "$TREE"
echo "内核树: $TREE"
echo "版本:   $(make kernelversion 2>/dev/null || echo 未知)"
echo

# ★★ 拦截残留旧版 venus 补丁的内核树：
#   视频编解码已迁至 qcom-iris（kernel/dts/0009–0010、kernel/iris/），若在残留 venus 节点的旧树上
#   继续运行，fuzz 回落会把 kernel/dts/0009 打成重复的 videocc / pil_video_mem 节点。
DTSI=arch/arm64/boot/dts/qcom/sc8280xp.dtsi
GK3=arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dts
if grep -q 'compatible = "qcom,sm8350-venus"' "$DTSI" 2>/dev/null \
   || grep -q '^&venus {' "$GK3" 2>/dev/null \
   || grep -q 'sc8280xp_res\|sm8350_res' drivers/media/platform/qcom/venus/core.c 2>/dev/null; then
    echo "✗ 这棵树上仍残留旧版 venus 节点或资源结构体，不能直接叠加 iris 补丁。" >&2
    echo "  请从干净 HEAD 新建 worktree（git -C $TREE worktree add --detach <新路径> HEAD）后再运行本脚本。" >&2
    exit 1
fi

applied=0; skipped=0; failed=0; fuzzed=0
for p in "${KPATCHES[@]}"; do
    f="$REPO/patches/$p"
    [ -f "$f" ] || { echo "✗ 缺文件 $p"; failed=$((failed + 1)); continue; }

    # 反向能打通 ⇒ 已经在树里了（精确应用过的走这条）
    if git apply --check -R "$f" 2>/dev/null; then
        echo "· 已应用，跳过  $p"
        skipped=$((skipped + 1)); continue
    fi
    # ★ 正向能干净打上 ⇒ 一定【还没打过】，直接打。
    #   ⚠️★★ 这一步必须排在指纹判据【前面】：例如 patches/kernel/camera/0002 的新增行是
    #   `.ops = &clk_rcg2_shared_ops,`，而文件里别处早有 21 行一模一样；若先跑指纹判据会误判"已应用"并静默跳过。
    #   正向 --check 成功是确定性的"没打过"，指纹判据只留给"正向打不上"之后的分流。
    if git apply --check "$f" 2>/dev/null; then
        if [ "$MODE" = "--check" ]; then
            echo "→ 可应用（--check 模式，未改动）  $p"
        else
            git apply "$f" && echo "✓ 已应用  $p"
        fi
        applied=$((applied + 1)); continue
    fi
    # ★ 指纹判据 —— 专治"用 fuzz 打进去过"的情况，反向检查对它们无效。
    #   少了这一步会把同一个补丁重复打进去（详见 already_applied 的注释）。
    if already_applied "$f"; then
        echo "· 已应用（指纹命中，当初多半是 fuzz 打的），跳过  $p"
        skipped=$((skipped + 1)); continue
    fi
    if ! git apply --check "$f" 2>/dev/null; then
        # ★ 回落到模糊匹配（现役补丁列表在基线树上应为零 fuzz；一旦用到 fuzz 必须显式告警）。
        if patch -p1 -d "$TREE" --dry-run --fuzz=3 < "$f" >/dev/null 2>&1; then
            if [ "$MODE" = "--check" ]; then
                echo "→ 可应用【需 fuzz=3】（--check 模式，未改动）  $p"
            else
                patch -p1 -d "$TREE" --fuzz=3 < "$f" | sed 's/^/    /'
                echo "✓ 已应用 ⚠️【用了 fuzz=3，不是精确匹配】  $p"
            fi
            applied=$((applied + 1)); fuzzed=$((fuzzed + 1)); continue
        fi
        echo "✗ 打不上（既不是已应用、也不干净、fuzz 也救不了）  $p"
        git apply --check "$f" 2>&1 | sed 's/^/    /'
        failed=$((failed + 1)); continue
    fi
    if [ "$MODE" = "--check" ]; then
        echo "→ 可应用（--check 模式，未改动）  $p"
    else
        git apply "$f" && echo "✓ 已应用  $p"
    fi
    applied=$((applied + 1))
done

echo
echo "可应用/已应用 $applied · 跳过 $skipped · 失败 $failed · 其中用了 fuzz $fuzzed"
[ "$failed" -eq 0 ] || exit 1

# ★ 打完之后断言 iris 的 DT 状态（kernel/dts/0009–0010）：
#   恰好一个 videocc、一个 iris 节点、gaokun3 里 &iris 打开了、没有任何 venus 节点。
if [ "$MODE" != "--check" ]; then
    n_vcc=$(grep -c 'videocc: clock-controller@abf0000' "$DTSI")
    n_iris=$(grep -c 'iris: video-codec@aa00000' "$DTSI")
    if [ "$n_vcc" != 1 ] || [ "$n_iris" != 1 ] || grep -q 'venus: video-codec' "$DTSI" \
       || ! grep -A2 '^&iris {' "$GK3" | grep -q 'status = "okay"'; then
        echo "✗ iris 的 DT 状态不对：videocc 节点 ${n_vcc} 个、iris 节点 ${n_iris} 个（都应为 1），" >&2
        echo "  且 gaokun3 里要有 &iris { ... status = \"okay\"; }、sc8280xp.dtsi 里不能有 venus 节点。" >&2
        exit 1
    fi
    echo "✓ iris DT 状态：1 个 videocc、1 个 iris 节点，gaokun3 已打开 &iris"
fi
