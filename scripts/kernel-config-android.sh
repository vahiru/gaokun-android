#!/usr/bin/env bash
#
# 把 buildbot 的 gaokun3_defconfig 调整成能跑 Android 的配置。
# 在内核源码树里执行：bash kernel-config-android.sh <kernel-out-dir>
#
# 每一项都是实测必需，依据见 docs/stage2-findings.md。
set -u
OUT="${1:?用法: $0 <kernel-out-dir>}"

./scripts/config --file "$OUT/.config" \
    `# —— 动态分区：first-stage init 在 ramdisk 里就要用 DM，而 ramdisk 里没有模块 ——` \
    --enable BLK_DEV_DM --enable DM_VERITY --enable DM_BUFIO --enable DM_SNAPSHOT \
    \
    `# —— SELinux：Android 硬性依赖 ——` \
    --enable SECURITY --enable SECURITY_NETWORK --enable AUDIT \
    --enable SECURITY_SELINUX --enable SECURITY_SELINUX_BOOTPARAM \
    --enable SECURITY_SELINUX_DEVELOP --enable SECURITY_SELINUX_AVC_STATS \
    \
    `# —— 调试通道：本机无串口，崩溃日志只能走 EFI 变量 ——` \
    --enable PSTORE --enable PSTORE_RAM --enable PSTORE_CONSOLE --enable PSTORE_PMSG \
    --disable PSTORE_COMPRESS --enable EFI_VARS_PSTORE \
    --enable MAGIC_SYSRQ --enable DEBUG_FS \
    \
    `# —— adb / USB gadget ——` \
    `# ⚠️ USB_CONFIGFS_F_FS 等只是 tristate 父级下的 bool 子开关，` \
    `#    父级 =m 时它们照样显示 =y 但整个栈都在模块里 —— Android 无模块，` \
    `#    functionfs mount 报 ENODEV（未知文件系统类型同样是 ENODEV！）。` \
    `#    三个父级必须显式 =y。实测见 findings 第 8.3quinquies 节。` \
    --enable CONFIGFS_FS --enable USB_LIBCOMPOSITE --enable USB_CONFIGFS --enable USB_F_FS \
    --enable USB_CONFIGFS_F_FS --enable USB_CONFIGFS_ACM \
    --enable USB_CONFIGFS_MASS_STORAGE --enable USB_CONFIGFS_ECM \
    --enable USB_CONFIGFS_RNDIS --enable USB_CONFIGFS_EEM \
    `# "USB 用途"里的 MIDI：gadget 的 MIDI 功能（functions/midi.gs5，init.gaokun3.usb.rc），要 SND_RAWMIDI。` \
    --enable USB_CONFIGFS_F_MIDI \
    --enable OVERLAY_FS

# ---- 让【同一个内核】也能当救援/LiveCD 系统用（见 docs/stage7-live-installer.md）----
# 今天 ESP 上有两个内核：Android 一个、救援 Ubuntu 一个，白占 14 MiB，
# 而且两份要各自维护。统一成一个之后，救援系统只是"同一个内核 + 另一个 initramfs"。
# 这三项对 Android 是惰性的（不挂就没有代价），但缺了救援就起不来：
#   SQUASHFS  —— 救援 rootfs 是 squashfs；★ 它默认是 =m，而【救援 initramfs 里
#                没有模块】，和本仓踩过 13 次的「=m 坑」完全同类
#   NTFS3_FS  —— 双系统安装要缩 Windows 分区，得先能读它
#   NLS_UTF8  —— FAT 上的非 ASCII 文件名
# ⚠️★ NTFS3_FS 的 Kconfig 是 `depends on !NTFS_FS || m` —— 只要那个旧的
#    NTFS_FS 兼容壳还开着（主线删掉 fs/ntfs 之后留下的别名），ntfs3 就被
#    【钉死在 =m】，`--enable` 写进去也会被 olddefconfig 改回 m。
#    症状是断言报 "CONFIG_NTFS3_FS=m"，而看 --enable 那行完全看不出原因。
./scripts/config --file "$OUT/.config" --disable NTFS_FS
./scripts/config --file "$OUT/.config"     --enable SQUASHFS --enable SQUASHFS_ZSTD --enable SQUASHFS_XZ     --enable NTFS3_FS --enable NLS_UTF8

# ★ Android 的挂起框架依赖 /sys/power/wake_lock（CONFIG_PM_WAKELOCKS）。
#   缺了它 SystemSuspend 退化到 wakeup_count 模式，而本机 s2idle 恢复是坏的
#   （EC 挂起坑，Stage 3 起的已知问题），表现为：闲置 45–60 秒后
#       I PM : suspend entry (s2idle)
#   然后 adb/网络全断、醒不来，只能断电重启。
#   2026-08-19 排查 crDroid 时，这个坑把每次上机窗口压到 45 秒，
#   还一度被误判成"system_server 崩溃导致重启"。
#   有了 wake_lock 接口，init 就能在 early-init 无条件持锁挡住自动挂起
#   （见 device/huawei/gaokun3/init.gaokun3.rc），且与是否插电无关。
#
# ★ cgroup v1：6.12+ 拆分后默认关。Android 的 cgroups.json 要求 cpuset 走 v1，
#   缺了会 SetupCgroups 失败 -> bootstrap-apexd-failed 复位（findings 第 8 节）。
#
# ⚠️★ 2026-08-20 修掉的一个静默失效：下面这 5 项原先写在上面那条
#   ./scripts/config 的【续行里】，而它们前面就是上述那段普通 # 注释 ——
#   shell 里注释行会【终止续行】，于是那条命令提前结束，这 5 项一个都没被
#   应用，只在 stderr 留下一句 "--enable: command not found"。
#   现有机器没出问题纯属侥幸：.config 里早就有它们（更早的正确版本写进去的）；
#   在一棵新树上从 buildbot defconfig 出发就会重现 cgroup v1 开机失败。
#   → 所以这里【单独起一条命令】，注释保持普通 # 写法；并且把这 4 个符号
#     补进了下面的 MUST_Y 断言表，以后再断会当场报错而不是静默跳过。
./scripts/config --file "$OUT/.config" \
    --enable PM_WAKELOCKS \
    --enable CPUSETS_V1 --enable MEMCG_V1 \
    --enable UCLAMP_TASK --enable UCLAMP_TASK_GROUP

./scripts/config --file "$OUT/.config" \
    \
    `# —— buildbot defconfig 是 Ubuntu 取向：以下关键驱动全是 =m，` \
    `#    而 Android 侧没有任何模块加载机制，必须 =y。` \
    `#    实测后果（findings 第 8.3bis 节）：三个 dwc3 全部` \
    `#    "failed to initialize core"（缺 femto USB2 PHY + refgen 供电），` \
    `#    a9c000.i2c 不 probe -> EC 全灭，cpufreq 找不到 icc path。` \
    `#    名字全部从 Makefile 反查核实过，模块名 != config 名：` \
    `#      i2c_qcom_geni          -> I2C_QCOM_GENI` \
    `#      phy_qcom_snps_femto_v2 -> PHY_QCOM_USB_SNPS_FEMTO_V2` \
    `#      nvmem_qcom-spmi-sdam   -> NVMEM_SPMI_SDAM` \
    `#      spi_geni_qcom          -> SPI_QCOM_GENI` \
    --enable I2C_QCOM_GENI --enable PHY_QCOM_USB_SNPS_FEMTO_V2 \
    --enable REGULATOR_QCOM_REFGEN --enable INTERCONNECT_QCOM_OSM_L3 \
    --enable NVMEM_SPMI_SDAM --enable SPI_QCOM_GENI \
    --enable POWER_SEQUENCING_QCOM_WCN \
    \
    `# —— 前瞻项（来自平行项目 mainline-generic 的 gaokun3 fragment，` \
    `#    docs/parallel-mainline-generic.md）。netd/bpfloader/lmkd 到位后必炸的：` \
    --enable NETFILTER_XTABLES --enable IP_NF_IPTABLES --enable IP_NF_FILTER \
    --enable IP_NF_TARGET_REJECT --enable IP6_NF_IPTABLES --enable IP6_NF_FILTER \
    --enable IP6_NF_TARGET_REJECT --enable NETFILTER_XT_MATCH_BPF \
    --enable NETFILTER_XT_MATCH_OWNER --enable NETFILTER_XT_MATCH_MARK \
    --enable NETFILTER_XT_TARGET_IDLETIMER --enable NETFILTER_XT_TARGET_MARK \
    --enable KPROBES --enable BPF_EVENTS --enable BPF_LSM --enable BPF_JIT_ALWAYS_ON \
    \
    `# —— 框架/内存管理（同上来源）——` \
    --enable ZRAM --enable ZRAM_BACKEND_LZ4 --enable ZRAM_BACKEND_ZSTD \
    --enable ZRAM_WRITEBACK --enable ZRAM_MULTI_COMP \
    --enable INPUT_UINPUT --enable CFS_BANDWIDTH --enable TASK_DELAY_ACCT \
    --enable DM_UEVENT --enable DM_VERITY_FEC --enable DM_CRYPT \
    --enable FS_ENCRYPTION --enable FS_VERITY \
    --enable EROFS_FS --enable EROFS_FS_XATTR --enable EROFS_FS_POSIX_ACL \
    --enable F2FS_FS --enable F2FS_FS_XATTR --enable F2FS_FS_POSIX_ACL \
    --enable F2FS_FS_SECURITY
    `# ⚠️ 不要抄 DM_DEFAULT_KEY（android-common 专有，主线没有）`

# ─── Stage 4: WiFi + BT 全栈 =y（Android 无模块加载，第 11 次踩 =m 坑）───
#   PCI_PWRCTRL_PWRSEQ 是无提示隐藏项，由 ATH11K_PCI select（含 HAVE_PWRCTRL 链），
#   =m 时 WCN6855 无人上电 → PCI 域 0006 整个不枚举。
#   ⚠️ olddefconfig 必须带 ARCH=arm64，否则按 x86 Kconfig 重算会删光 arm64 符号！
./scripts/config --file "$OUT/.config" \
    --enable CFG80211 --enable MAC80211 --enable RFKILL \
    --enable ATH_COMMON --enable ATH11K --enable ATH11K_PCI \
    --enable QRTR --enable QRTR_MHI --enable QRTR_SMD --enable QRTR_TUN \
    --enable MHI_BUS --enable MHI_BUS_PCI_GENERIC \
    --enable QCOM_QMI_HELPERS --enable PCI_PWRCTRL_PWRSEQ \
    --enable BT --enable BT_BREDR --enable BT_LE \
    --enable BT_QCA --enable BT_HCIUART \
    --enable USB_STORAGE --enable USB_UAS
    `# BT_HCIUART_QCA 已默认 y（在 BT_HCIUART 之下）`
    `# USB_STORAGE/UAS：Android 下能看见 USB 棒上的 Ubuntu ESP，`
    `# 维护引导项/DTB 不用重启（kb18 仍是 =m，kb19 转正）`

# ★ 最关键也最容易漏的一步：
#   CONFIG_SECURITY_SELINUX=y 只是「编进内核」，不等于「被激活」。
#   真正决定哪些 LSM 生效的是 CONFIG_LSM 这个字符串。
#   buildbot 的默认值里只有 apparmor，没有 selinux，
#   结果 selinuxfs 从不注册，Android init 在 selinux_setup 阶段静默死亡。
#   SELinux 和 AppArmor 都是 major LSM，当前内核不能同时激活，必须去掉 apparmor。
./scripts/config --file "$OUT/.config" \
    --set-str LSM "landlock,lockdown,yama,integrity,selinux,bpf"

# ─── Stage 5: 音频链 + 蓝牙 profile + 温控（第 12 次踩 =m 坑）───
#   ★声卡不注册的真正源头：LPASS 的 pinctrl 是 =m。
#     rx/tx/wsa macro 的 pinctrl-0 指向 /soc@0/pinctrl@33c0000 下的
#     *-swr-default-state，fw_devlink 因此把 33c0000.pinctrl 当成 supplier；
#     驱动是模块 → 永不加载 → macro 永远 deferred → soundwire 等 macro →
#     sound 节点等 DAI → /proc/asound/cards 里 "no soundcards"。
#     实测 dmesg 原话：
#       platform 3200000.rxmacro: deferred probe pending:
#         platform: wait for supplier /soc@0/pinctrl@33c0000/rx-swr-default-state
#   SC_LPASSCC_8280XP：LPASS 时钟控制器，macro 的 mclk/npl 从这来。
#   SND_SOC_WSA883X：本机扬声器 wsa8830（DT compatible sdw10217020200
#     = mfg 0x0217 part 0x0202，与 ThinkPad X13s 同款），**原本压根没编**。
#   QRTR_SMD：QRTR 的 rpmsg 传输，pd-mapper 靠它跟 ADSP 说话
#     （之前虽写了 --enable 却仍是 =m —— 所以下面加了断言）。
#   BT：内核侧 hci0 已经能出来（BT_QCA + HCIUART_QCA 都是 y），
#     但 RFCOMM/HIDP/UHID 是 =m → 蓝牙键鼠/串口 profile 全废。
#   温控/带宽：QCOM_SPMI_ADC5 等是 =m → PMIC 温度传感器缺席；
#     ICC_BWMON 关系到内存带宽随负载升频（打游戏要）。
./scripts/config --file "$OUT/.config" \
    `# ★声卡链` \
    --enable PINCTRL_LPASS_LPI --enable PINCTRL_SC8280XP_LPASS_LPI \
    --enable SC_LPASSCC_8280XP --enable SND_SOC_WSA883X \
    --enable QRTR_SMD \
    `# 蓝牙 profile（hci0 已通，缺的是这些）` \
    --enable BT_RFCOMM --enable BT_RFCOMM_TTY --enable BT_HIDP --enable UHID \
    --enable HID_MULTITOUCH \
    `# 温度传感器 + 内存带宽调频 + 电源统计` \
    --enable IIO --enable QCOM_SPMI_ADC5 --enable QCOM_VADC_COMMON \
    --enable QCOM_SPMI_TEMP_ALARM --enable QCOM_ICC_BWMON \
    --enable QCOM_LMH --enable QCOM_SPM --enable QCOM_STATS --enable QCOM_SOCINFO \
    `# Android 基础设施：FUSE（外部存储）、熵源、AF_ALG` \
    --enable FUSE_FS --enable HW_RANDOM --enable HW_RANDOM_ARM_SMCCC_TRNG \
    --enable CRYPTO_USER_API --enable CRYPTO_USER_API_HASH \
    --enable CRYPTO_USER_API_SKCIPHER --enable CRYPTO_HMAC \
    --enable CRYPTO_SHA512 --enable CRYPTO_CMAC --enable CRYPTO_CRC32C \
    --enable CRYPTO_AES_ARM64_NEON_BLK --enable CRYPTO_AES_ARM64_BS \
    `# 杂项：外置盘、键盘灯、GENI DMA、i2c 调试` \
    --enable EXFAT_FS --enable LEDS_CLASS --enable INPUT_LEDS \
    --enable QCOM_GPI_DMA --enable I2C_CHARDEV --enable RESET_QCOM_PDC

# ★ v1.0 AV-4：USB 声卡（snd-usb-audio）。buildbot 的 gaokun3_defconfig 里是 =m（:317），
#   发布内核里是 =y 只是因为旧 .config 带下来的（v0.7.1 的 config 4373 行）—— 和上面 cgroup 那 5 项
#   "现有机器没出问题纯属侥幸"同一类：从 defconfig 起一棵新树就会静默变成 =m，USB 声卡不出现。
#   所以显式 =y 并进下面的 MUST_Y。Kconfig（构建机 ~/gk3-kernel-72y sound/usb/Kconfig）：
#     SND_USB        :4  menuconfig bool，depends on USB，default y
#     SND_USB_AUDIO  :13 tristate，select SND_HWDEP / SND_RAWMIDI / SND_PCM
#   刻意【不开】的：SND_USB_AUDIO_MIDI_V2（:28，用不上）、SND_USB_AUDIO_QMI（:191，DSP 侧 USB 音频
#   offload，要 SND_SOC_USB + USB_XHCI_SIDEBAND，ADSP 拓扑里也没有这条路）。
#   card0 留给内置声卡靠 cmdline 的 snd_usb_audio.index（BoardConfig.mk），不在这里。
./scripts/config --file "$OUT/.config" --enable SND_USB --enable SND_USB_AUDIO

# ★ 蓝牙栈起不来的真凶（与 HAL 无关）：RT cgroup 带宽管制。
#   实测报错：
#     bluetooth: message_loop_thread.cc:291 EnableRealTimeScheduling:
#       unable to set SCHED_FIFO priority 1 for bt_main_thread, error: Operation not permitted
#     → bluetooth::log::fatal → com.android.bluetooth abort → 开关蓝牙即崩溃循环
#   CONFIG_RT_GROUP_SCHED=y + CGROUP_SCHED 时，非 root cpu cgroup 的
#   rt_runtime_us 默认是 0 → 该 cgroup 里任何 sched_setscheduler(SCHED_FIFO)
#   一律 EPERM。Android 从不用 RT cgroup，GKI 里这项是关的。
#   （6.12+ 也可用 RT_GROUP_SCHED_DEFAULT_DISABLED，但直接关更干净。）
./scripts/config --file "$OUT/.config" --disable RT_GROUP_SCHED

# ─── Stage 6: 传感器（第 13 次踩 =m 坑）───
# ★ CONFIG_QCOM_FASTRPC 在 buildbot defconfig 里是 =m，而 Android 不加载模块
#   → /dev/fastrpc-* 四个节点【一个都不出现】→ hexagonrpcd 起不来 → 没有传感器。
#   DTS 里节点本来是齐的（remoteproc_slpi 下 fastrpc + compute-cb@1/2/3），
#   rpmsg 通道也在，只是没人 probe。
#   实测佐证：单独编出 fastrpc.ko 推到设备上 insmod（vermagic 匹配、模块签名关闭），
#   /dev/fastrpc-{sdsp,adsp,cdsp,cdsp-secure} 立刻全部出现。
#   ⚠️ 那只是【验证手段】，不是解法 —— 正解就是这里的 =y。
#   我们只用 sdsp（SLPI）；权限在 ueventd.gaokun3.rc 里给。
#   整条通路与实测读数（Z≈9.87）见 docs/stage4-findings.md #37。
./scripts/config --file "$OUT/.config" --enable QCOM_FASTRPC

# ─── Stage 6: EFI_ZBOOT（把内核编成自解压的 EFI 应用）───
# ★ 为什么要它：本机的内核是【ESP 上的文件】，而 ESP 只有 300 MiB。
#   A/B 两个槽位各存一份内核后，未压缩的 Image（约 39 MB）会把 ESP 挤爆
#   （还要和固件自己那个 73 MB 的 Persisted_Capsules.bin 共处）。
#   EFI_ZBOOT 产出 arch/arm64/boot/vmlinuz.efi —— 自解压的 PE，十几 MB。
# ★ 它【不影响】Image 的产出：两个都会有，可以并行对照。
#   依赖 EFI_GENERIC_STUB（本机已 =y）。
./scripts/config --file "$OUT/.config" --enable EFI_ZBOOT

# ★★ 2026-09-28 起（#128）视频编解码走 qcom-iris，【关掉】qcom-venus。
#   ⚠️ 此处原来写的是反的（"必须关 IRIS，它永远服务不了本机"），那句话**错了**：
#   v7.2-rc2 的 iris_probe.c:372 就认 "qcom,sm8250-venus"，而上游 v7.3 给 sc8280xp 的节点
#   写的是 "qcom,sc8280xp-iris", "qcom,sm8250-venus" —— 靠回落串直接用 sm8250_data。
#   当年只查了 of_match 的前几条，漏看了 sm8250-venus 那一条（它在 v7.2-rc2 里是有的）。
#   为什么 VENUS 必须关、不能两个都留着：
#     · 我们 venus 时代的 upstream-venus/0017/0018 在 IRIS=y 时编不过
#       （venus/core.c 的 #if (!IS_ENABLED(CONFIG_VIDEO_QCOM_IRIS)) 把它们引用的表编掉了）；
#     · 不打那两个，VENUS=y 虽能编过但永远绑不上新节点 —— 留着只是一个会误导人的 =y；
#     · 反过来的陷阱更糟：VENUS=y + IRIS=n + 新 DT，venus 会用 sm8250_res 去绑（没有 cp_* 配置）。
#   所以 VENUS 进 MUST_N，驱动选择在 .config 里一眼可见。
./scripts/config --file "$OUT/.config" --disable VIDEO_QCOM_VENUS

# ─── Stage 6 M14: 硬件视频编解码（V4L2 M2M；M14 时是 Venus，#128 起是 iris）───
# ★ 为什么现在能做：三个前提本地核实过，一个都不缺。
#   1. 时钟控制器【主线已有】：drivers/clk/qcom/videocc-sm8350.c 自己就认
#      "qcom,sc8280xp-videocc"（该文件 :537 和 :572 两处），不需要新驱动。
#   2. dt-bindings 头文件在：include/dt-bindings/clock/qcom,sm8350-videocc.h。
#   3. ★固件我们【一直在装】：DTS 补丁把 firmware-name 指向
#      qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn，而 firmware/README.md 里
#      那一行当初被我标成"语音服务（未用到，一并带上）"——
#      **VSS = Video SubSystem，不是 Voice**。设备上实测在，2035748 字节。
#
# ⚠️★ 又是那个"=m 坑"，而且整条链上有五个。刷机前的实测值：
#      CONFIG_MEDIA_SUPPORT=m   VIDEO_DEV=m   VIDEOBUF2_DMA_CONTIG=m
#      V4L2_MEM2MEM_DEV=m       SM_VIDEOCC_8350=m
#   Android 不加载任何模块（/vendor/lib/modules 不存在、lsmod 为空），
#   所以就算 --enable 了驱动本身（当时是 VIDEO_QCOM_VENUS），整条链照样静默缺席。
#   全部拉成 =y，并且下面 MUST_Y 里逐个断言 —— 这个坑本仓已经踩了 13 次。
#
# 依赖关系（drivers/media/platform/qcom/venus/Kconfig 原文）：
#   depends on V4L_MEM2MEM_DRIVERS / VIDEO_DEV && QCOM_SMEM / ARCH_QCOM &&
#              ARM64 && IOMMU_API
#   select OF_DYNAMIC / QCOM_MDT_LOADER / QCOM_SCM / VIDEOBUF2_DMA_CONTIG /
#          V4L2_MEM2MEM_DEV
# iris（drivers/media/platform/qcom/iris/Kconfig，v7.2-rc2）：
#   depends on VIDEO_DEV / ARCH_QCOM
#   select V4L2_MEM2MEM_DEV / QCOM_MDT_LOADER / QCOM_SCM / QCOM_UBWC_CONFIG / VIDEOBUF2_DMA_CONTIG
# ⚠️ iris 还有一个 Kconfig 里看不出来的硬依赖：PM_DEVFREQ。没有它 devfreq_recommended_opp()
#    是返回 -EINVAL 的桩（include/linux/devfreq.h），每次上电都失败。#24 里本来就是 =y，下面断言它。
# ⚠️ SM_VIDEOCC_8350 会 `select SM_GCC_8350`（drivers/clk/qcom/Kconfig:1365），
#    于是 SM8350 的 gcc 也会被编进来。无害（compatible 不匹配、永不 probe），
#    但看到它出现在 .config 里不要当成配错了。
./scripts/config --file "$OUT/.config"     --enable MEDIA_SUPPORT     --enable MEDIA_PLATFORM_SUPPORT     --enable VIDEO_DEV     --enable V4L_MEM2MEM_DRIVERS     --enable VIDEOBUF2_DMA_CONTIG     --enable V4L2_MEM2MEM_DEV     --enable SM_VIDEOCC_8350     --enable VIDEO_QCOM_IRIS

# ─── Stage 6 M22: 相机（CAMSS + 前摄 hi846）───
# 目标是【前摄】。上游作者自己在 camera.dtsi:158-166 写明后摄 s5k3l6 "画质差、
# 不打算提取下游寄存器配置……前摄够开会用了"，而且 "This sensor has never been
# detected on 2023 model"。
#
# ⚠️★ 又是"=m 坑"，这次 5 个（实测于设备正在跑的 .config）：
#      I2C_QCOM_CCI=m  LEDS_GPIO=m  SC_CAMCC_8280XP=m  VIDEO_HI846=m
#      VIDEO_QCOM_CAMSS=m
#   ★ 2026-09-14 第 15 个「=m 坑」：VIDEO_DW9714=m（后摄 OV13B10 的对焦马达）。它 =m 时
#     ov13b10 的 lens-focus 永远等不到 vcm@c，v4l2-async 卡住 ⇒ 前后摄一个 subdev 节点都不出（#106）。
#     后摄 OV13B10 本体也要 =y（上游驱动原本只有 ACPI 匹配，OF 匹配见 patches/0034）。
#   Android 不加载任何模块，所以 =m 等于不存在。
#   （VIDEOBUF2_DMA_SG 不用手动开 —— CAMSS 会 select 它，跟着变 =y。）
#
# 门禁本来就齐（实测全是 =y，不用动）：MEDIA_CAMERA_SUPPORT / V4L_PLATFORM_DRIVERS
# / VIDEO_CAMERA_SENSOR / MEDIA_CONTROLLER / V4L2_FWNODE / VIDEO_V4L2_SUBDEV_API
# / IOMMU_DMA / LEDS_CLASS。
#
# 依赖与 select（源码原文，带行号）：
#   drivers/media/platform/qcom/camss/Kconfig:1  VIDEO_QCOM_CAMSS
#       depends on V4L_PLATFORM_DRIVERS / VIDEO_DEV / (ARCH_QCOM && IOMMU_DMA)
#       select MEDIA_CONTROLLER / VIDEO_V4L2_SUBDEV_API / VIDEOBUF2_DMA_SG / V4L2_FWNODE
#   drivers/media/i2c/Kconfig:122                VIDEO_HI846（在 :28 的
#       menuconfig VIDEO_CAMERA_SENSOR 之下，该菜单 depends on MEDIA_CAMERA_SUPPORT
#       && I2C && HAVE_CLK）
#   drivers/i2c/busses/Kconfig:1050              I2C_QCOM_CCI
#   drivers/clk/qcom/Kconfig:819                 SC_CAMCC_8280XP（select SC_GCC_8280XP）
#   drivers/leds/Kconfig:402                     LEDS_GPIO（privacy LED，
#       camera.dtsi 里两个 sensor 节点都 `leds = <&privacy_led>`）
#
# ★ hi846 驱动的四个修复 buildbot 已经带了（patches/upstream/0020-0023：
#   write_reg_16 / link frequency / 6MP+8MP 模式 / 不同 lane 数下的模式处理），
#   我们的配方本来就打这 13 个 upstream 补丁，所以不用额外做什么。
./scripts/config --file "$OUT/.config" \
    --enable VIDEO_QCOM_CAMSS \
    --enable VIDEO_HI846 \
    --enable VIDEO_OV13B10 \
    --enable VIDEO_DW9714 \
    --enable I2C_QCOM_CCI \
    --enable SC_CAMCC_8280XP \
    --enable LEDS_GPIO

# ★ 后摄闪光灯（#110，patches/0036）：LED 挂在 PMIC pmc8280c（PM8350C）的闪光模块 1+4 路，
#   驱动 drivers/leds/flash/leds-qcom-flash.c（LEDS_QCOM_FLASH，依赖 LEDS_CLASS_FLASH）。
#   GPIO93 那个 gpio-led 实测不亮，已从 DT 删掉。内核 #19 起带；这里断言是免得从干净树重建时
#   悄悄丢掉 /sys/class/leds/white:flash（ROM 构建不会为此报错）。
./scripts/config --file "$OUT/.config" \
    --enable LEDS_CLASS_FLASH \
    --enable LEDS_QCOM_FLASH

# ─── 电源管理调试（★留着，它是 s2idle 那一仗的决胜工具）───
# 历史：s2idle 曾被判成"挂得下去、醒不回来的内核/EC 缺陷"，而当时卡死在
# 没法二分 —— /sys/power/pm_test 需要 CONFIG_PM_DEBUG，默认没开。
# 补上之后 pm_test=devices 成了最安全最快的复现器（5 秒自动返回、不需要唤醒源），
# 也正是它把故障夹到 dpm_suspend_start()+dpm_suspend_noirq() 之内，
# 最终定到 a600000.usb 的 role（2026-08-22 已修，见 docs/stage4-findings.md #52-#57）。
# ⚠️ 别因为"问题已解决"就把这几项关掉：下一个挂起类问题还得靠它。
#
#   PM_DEBUG        → /sys/power/pm_test（分层二分：freezer/devices/platform/
#                     processors/core）与 /sys/power/pm_print_times
#   PM_SLEEP_DEBUG  → /sys/power/pm_debug_messages
#   PM_ADVANCED_DEBUG → 每个设备的 power/ sysfs 属性
#   ★ DPM_WATCHDOG  → 某个设备的 suspend/resume 回调卡住时 panic 并打出该回调的栈，
#                     记录进 pstore；本机 efi_pstore 是通的，所以抓得到。
#
# ⚠️★ DPM_WATCHDOG 的依赖是 `PM_DEBUG && PSTORE && EXPERT`
#   （kernel/power/Kconfig）。本机 PSTORE 早就 =y，但 **EXPERT 没开**，
#   所以光 --enable DPM_WATCHDOG 会静默无效 —— 断言会当场抓住它。
#   故这里必须一并打开 EXPERT（它只是"取消隐藏"一批选项，不改已有取值）。
#
# ⚠️★ **`PM_TRACE_RTC` 在 arm64 上不存在** —— 它 `depends on X86`
#   （kernel/power/Kconfig）。而 `PM_TRACE` 是个没有 prompt 的 bool，只能由
#   PM_TRACE_RTC 去 select。这很可惜：那个机制（把最后执行的设备
#   suspend/resume 哈希写进 RTC，机器不干净复位后仍能读出来）
#   恰好就是为本机这种"userspace 已冻结、journald 来不及落盘、
#   clean hang 不产生 panic"的症状设计的。**别再去找它了。**
#   arm64 上的替代品就是上面的 DPM_WATCHDOG + pstore。
#
# ⚠️ DPM_WATCHDOG_TIMEOUT 保持默认 120 秒 —— 发布内核里不要压低，
#   否则某个合法的慢设备会被误判成挂死。调试时在测试内核里单独设成 10 秒
#   （本机 ~13 秒就复位，120 秒永远轮不到它开火）。
#
# ★★ 2026-10-04（issue #16）：上面这句"保持默认"**没落地** —— 发布内核 #13 实测
#   DPM_WATCHDOG_TIMEOUT=10、WARNING_TIMEOUT=10（`zcat /proc/config.gz`）：调试时设的 10 秒
#   留在了构建树的 .config 里，olddefconfig 不会把它改回默认。用户的机器上 ath11k 恢复失败后
#   10 秒就 panic（pstore 有整条链）。所以这里**显式写值、下面断言值**，不再依赖"默认"。
#   WARNING 设 60：60 秒先打一次卡住回调的栈（不 panic），120 秒才 panic
#   （kernel/power/Kconfig:271-283、drivers/base/power/main.c:595-619）。
#   调试时要短超时就单独编测试内核，别动这里。
# ★ PANIC_TIMEOUT=10：默认 0 = panic 后永远停住（lib/Kconfig.debug:1109-1117），用户看到的就是"睡死"、
#   只能长按电源键。10 秒足够 efi_pstore 落盘，然后自己重启回 ESP default。cmdline 的 panic= 仍可覆盖。
./scripts/config --file "$OUT/.config" \
    --enable EXPERT \
    --enable PM_DEBUG \
    --enable PM_SLEEP_DEBUG \
    --enable PM_ADVANCED_DEBUG \
    --enable DPM_WATCHDOG \
    --set-val DPM_WATCHDOG_TIMEOUT 120 \
    --set-val DPM_WATCHDOG_WARNING_TIMEOUT 60 \
    --set-val PANIC_TIMEOUT 10

# ─── 1.0 批 2：网络 / 诊断 / 兼容（docs/v1.0-plan.md 批 2 的内核配置项）───
# 发布内核 #15（v0.7.1 / 1.0.0-dev.2）的 `zcat /proc/config.gz` 里下面这些全是 =m 或未设。
# 每个符号都在 v7.2-rc2 的 Kconfig 里核实过（文件:行号写在各段）。
#
# ★ NET-3：USB 有线网卡与手机 USB 共享网络。buildbot defconfig 全是 =m（又一次「=m 坑」），
#   框架的 EthernetTracker 早就在等 (usb|eth)\d+，只差驱动。
#   drivers/net/usb/Kconfig：USB_NET_DRIVERS:8 RTL8152:99 USBNET:132 AX8817X:166 AX88179_178A:198
#     CDCETHER:216 CDC_EEM:244 CDC_NCM:258 HUAWEI_CDC_NCM:278 RNDIS_HOST:399；MII 在 drivers/net/Kconfig:29。
#   CDC_NCM / HUAWEI_CDC_NCM：较新的手机（含华为）做 USB 共享走 NCM，只开 RNDIS_HOST 不够（复核意见）。
#   ⚠️ RTL8153 的部分版本要 rtl_nic/ 固件，不带也能工作（驱动只是少打补丁），先不带。
./scripts/config --file "$OUT/.config" \
    --enable USB_NET_DRIVERS --enable USB_USBNET --enable MII \
    --enable USB_RTL8152 --enable USB_NET_AX8817X --enable USB_NET_AX88179_178A \
    --enable USB_NET_CDCETHER --enable USB_NET_CDC_EEM --enable USB_NET_CDC_NCM \
    --enable USB_NET_HUAWEI_CDC_NCM --enable USB_NET_RNDIS_HOST

# ★ NET-8：Android 内核网络基线。来源是 AOSP kernel/configs 的 b/android-6.12/android-base.config
#   （2026-10-05 从 tuna 镜像取的 bd79f386，与发布内核逐项比出的【网络部分】缺项；非网络的
#   缺项不在这里 —— 那些多是 ACK 专有符号或要单独评估的取舍）。最直接的用户影响：
#   INET_ESP=m / INET6_ESP 未设 ⇒ 系统自带 IKEv2/IPsec VPN 用不了；INET_DIAG_DESTROY 未设 ⇒
#   netd 断网时销毁不了旧 socket（SOCK_DESTROY）；NET_CLS_BPF / NET_ACT_BPF / NET_SCH_INGRESS
#   未设 ⇒ tethering offload 与 clat 的 tc-BPF 快路径回落。
#   net/ipv4/Kconfig：NET_IPGRE_DEMUX:180 NET_IPVTI:304 INET_ESP:354 INET_UDP_DIAG:439 INET_DIAG_DESTROY:455
#   net/ipv6/Kconfig：IPV6_ROUTER_PREF:21 IPV6_ROUTE_INFO:31 IPV6_OPTIMISTIC_DAD:39 INET6_ESP:62
#     INET6_IPCOMP:102 IPV6_MIP6:112 IPV6_VTI:150
#   net/sched/Kconfig：NET_SCH_HTB:48 NET_SCH_TBF:134 NET_SCH_INGRESS:347 NET_CLS_U32:516 NET_CLS_BPF:562
#     NET_CLS_MATCHALL:582 NET_EMATCH:592 NET_EMATCH_U32:635 NET_CLS_ACT:702 NET_ACT_POLICE:715 NET_ACT_BPF:841
#   drivers/net/Kconfig：IFB:149 —— ⚠️ 它 `depends on NET_ACT_MIRRED || NFT_FWD_NETDEV`，基线片段里
#     没写 NET_ACT_MIRRED（net/sched/Kconfig:742，GKI 是在别处开的），只 --enable IFB 会被
#     olddefconfig 静默丢掉（2026-10-05 本地试跑，断言当场抓到）⇒ 一并开 NET_ACT_MIRRED。
#   net/ipv4/netfilter/Kconfig：IP_NF_MATCH_ECN:154 IP_NF_MATCH_TTL:174 IP_NF_TARGET_NETMAP:244
#     IP_NF_SECURITY:313 IP_NF_ARPTABLES:327 IP_NF_ARPFILTER:343 IP_NF_ARP_MANGLE:357
#     （ARPTABLES/ARPFILTER 依赖 NETFILTER_XTABLES_LEGACY，发布内核里已是 y）
#   net/netfilter/Kconfig：NF_CONNTRACK_SECMARK:123 AMANDA:212 H323:239 IRC:258 NETBIOS_NS:277 PPTP:311
#     SANE:330；XT_TARGET_CLASSIFY:827 CONNSECMARK:849 CT:861 NFQUEUE:1002 TPROXY:1068 TRACE:1089
#     SECMARK:1101；XT_MATCH_CONNLIMIT:1228 HELPER:1338 IPRANGE:1365 LENGTH:1395 MAC:1414
#     STATISTIC:1597 STRING:1606 TIME:1629
#   crypto/Kconfig：CHACHA20POLY1305:752 MD5:885 XCBC:983（IpSec 的 AEAD / 认证算法）
#   ⚠️ 基线里另有 NETFILTER_XT_MATCH_QUOTA2_LOG、NF_CT_PROTO_DCCP、NF_CT_PROTO_UDPLITE，
#      这棵树的 Kconfig 里没有这三个符号，不写：QUOTA2_LOG 是 ACK 的 quota2 子选项，0017 只移植了
#      NETFILTER_XT_MATCH_QUOTA2 本身（源码里的 #ifdef 还在，但 Kconfig 没有这一项）；后两个在 v7.2-rc2 里不存在。
./scripts/config --file "$OUT/.config" \
    --enable INET_ESP --enable INET6_ESP --enable INET6_IPCOMP \
    --enable INET_DIAG_DESTROY --enable INET_UDP_DIAG \
    --enable IPV6_ROUTER_PREF --enable IPV6_ROUTE_INFO --enable IPV6_OPTIMISTIC_DAD \
    --enable IPV6_MIP6 --enable IPV6_VTI --enable NET_IPVTI --enable NET_IPGRE_DEMUX \
    --enable NET_SCH_HTB --enable NET_SCH_TBF --enable NET_SCH_INGRESS \
    --enable NET_CLS_U32 --enable NET_CLS_BPF --enable NET_CLS_MATCHALL \
    --enable NET_EMATCH --enable NET_EMATCH_U32 \
    --enable NET_CLS_ACT --enable NET_ACT_POLICE --enable NET_ACT_BPF \
    --enable NET_ACT_MIRRED --enable IFB \
    --enable IP_NF_MATCH_ECN --enable IP_NF_MATCH_TTL --enable IP_NF_TARGET_NETMAP \
    --enable IP_NF_SECURITY --enable IP_NF_ARPTABLES --enable IP_NF_ARPFILTER \
    --enable IP_NF_ARP_MANGLE \
    --enable NF_CONNTRACK_SECMARK --enable NF_CONNTRACK_AMANDA --enable NF_CONNTRACK_H323 \
    --enable NF_CONNTRACK_IRC --enable NF_CONNTRACK_NETBIOS_NS --enable NF_CONNTRACK_PPTP \
    --enable NF_CONNTRACK_SANE \
    --enable NETFILTER_XT_TARGET_CLASSIFY --enable NETFILTER_XT_TARGET_CONNSECMARK \
    --enable NETFILTER_XT_TARGET_CT --enable NETFILTER_XT_TARGET_NFQUEUE \
    --enable NETFILTER_XT_TARGET_TPROXY --enable NETFILTER_XT_TARGET_TRACE \
    --enable NETFILTER_XT_TARGET_SECMARK \
    --enable NETFILTER_XT_MATCH_CONNLIMIT --enable NETFILTER_XT_MATCH_HELPER \
    --enable NETFILTER_XT_MATCH_IPRANGE --enable NETFILTER_XT_MATCH_LENGTH \
    --enable NETFILTER_XT_MATCH_MAC --enable NETFILTER_XT_MATCH_STATISTIC \
    --enable NETFILTER_XT_MATCH_STRING --enable NETFILTER_XT_MATCH_TIME \
    --enable CRYPTO_CHACHA20POLY1305 --enable CRYPTO_MD5 --enable CRYPTO_XCBC

# ★ LIVE-8：卡死检测【只告警、不 panic】。驱动死锁（A1 那类）时机器冻住、硬件看门狗照样被喂，
#   以前 dmesg 里连一行栈都没有。lib/Kconfig.debug：SOFTLOCKUP_DETECTOR:1123
#   BOOTPARAM_SOFTLOCKUP_PANIC:1150 DETECT_HUNG_TASK:1269 DEFAULT_HUNG_TASK_TIMEOUT:1284
#   BOOTPARAM_HUNG_TASK_PANIC:1300（7.2 起是 int "几个 hung task 触发 panic"，0 = 不 panic）。
#   ⚠️ 不开 panic：固件加载、NVMe 之类长时间 D 状态会误报（复核意见）；要 pstore 就交给
#      hangdump / 看门狗链在超时后自己 panic。HARDLOCKUP_DETECTOR 在 arm64 上要 pseudo-NMI，不开。
#   两个 PANIC 值写死并断言：值以后若被"调试时顺手改一下"留在构建树 .config 里（#16 的教训）会被抓到。
./scripts/config --file "$OUT/.config" \
    --enable DETECT_HUNG_TASK --set-val DEFAULT_HUNG_TASK_TIMEOUT 120 \
    --set-val BOOTPARAM_HUNG_TASK_PANIC 0 \
    --enable SOFTLOCKUP_DETECTOR --set-val BOOTPARAM_SOFTLOCKUP_PANIC 0

# ★ LIVE-12：ANON_VMA_NAME（mm/Kconfig:1365）—— scudo / ART 靠它给匿名映射起名，
#   没有它 dumpsys meminfo 的 Java / Native 堆全是 0、全算进 Unknown。LRU_GEN 改回收行为，不在这里顺手开。
# ★ LIVE-13：LOG_BUF_SHIFT（init/Kconfig:804，范围 12–25；7.2.9 树 ~/gk3-kernel-72y 里是 :807）17 → 19，内核环形缓冲 128 KiB → 512 KiB。
# ★ PERF-11 / PWR-13：THERMAL_STATISTICS（drivers/thermal/Kconfig:29，7.2.9 同）—— cooling_device*/stats，
#   降频历史可查。（以上与下面 APP-17 的四个符号 2026-10-05 在 7.2.9 树上重 grep 过；该树的 .config 四项 + 这两项都已是期望值。）
# ★ APP-17：ARMv8 废弃指令模拟（arch/arm64/Kconfig：ARMV8_DEPRECATED:1826 SWP_EMULATION:1840
#   CP15_BARRIER_EMULATION:1863 SETEND_EMULATION:1879）。AOSP kernel/configs
#   b/android-6.12/android-base-conditional.xml:43-72 对 arm64 要求这四项 =y。
#   都只是编进去，运行时由 abi.* sysctl 控制（SWP 默认关）；本机 32 位 zygote 在跑（app_process32）。
./scripts/config --file "$OUT/.config" \
    --enable ANON_VMA_NAME \
    --set-val LOG_BUF_SHIFT 19 \
    --enable THERMAL_STATISTICS \
    --enable ARMV8_DEPRECATED --enable SWP_EMULATION \
    --enable CP15_BARRIER_EMULATION --enable SETEND_EMULATION

# ─── 用户态 39 位地址窗口（patches/0078，issue #17 / #19 / #20；用户 2026-10-08 定进发版）───
# 页表仍 48 位，64 位进程默认拿到的地址压到 2^39 以下（与 Android GKI 的 39 位布局相同）。
# 2026-10-08 A/B：英雄联盟手游 7.3.0.2813 在原内核 2 秒崩（il2cpp_init 写 0x10），开了它过 il2cpp_init。
# 符号由 0078 引入 —— 补丁没打，这里会在 MUST_Y 断言上失败，而不是静默编出 48 位窗口。
./scripts/config --file "$OUT/.config" --enable ARM64_USER_MMAP_WINDOW_39

# ★ 手写笔（M-Pencil，#26）：触摸从片驱动（patches/0079）。和主片 himax-spi 一样必须 =y
#   （Android 不加载模块）；把从片的帧变成触控笔的是 vendor 的 gk3pend（device/huawei/gaokun3/stylus）。
./scripts/config --file "$OUT/.config" --enable TOUCHSCREEN_HIMAX_HX83121A_PEN

# ★ UCSI（Type-C 口：插拔、供电方向、插头方向、DP）：两个口的 SBU 切换芯片（pi3usb102，设备树里的
#   usb0-sbu-mux / usb1-sbu-mux）归 gpio-sbu-mux 驱动。buildbot 配置里它是 =m，而 Android 不加载模块
#   ⇒ 驱动不在，typec_register_port 一直等不到这个 orientation-switch（-EPROBE_DEFER），UCSI 重试约
#   80 秒后 "PPM init failed, stop trying"，/sys/class/typec 为空（#27 / #112 / #118 里的"UCSI 坏了"）。
./scripts/config --file "$OUT/.config" --enable TYPEC_MUX_GPIO_SBU

# ★ USB 传文件（MTP/PTP）与"设置"里的 USB 页面：UsbService 只在 /sys/class/android_usb 存在时才建
#   UsbDeviceManager。主线没有这个类，patches/0096 补上它（连接状态由 USB 端口 HAL 写）。
./scripts/config --file "$OUT/.config" --enable USB_ANDROID_USB_STATE

# ─── olddefconfig + 断言（止损"=m 坑"）───
# 这个坑已经踩了 13 次：`scripts/config --enable X` 写进去了，olddefconfig
# 却可能因为依赖把它降回 =m（或压根没有该符号），而 Android **不加载任何模块**
# （/vendor/lib/modules 不存在、lsmod 为空），于是驱动静默缺席。
# 所以：olddefconfig 由脚本自己跑（顺手把 ARCH=arm64 这个致命参数固定住），
# 跑完立刻断言关键符号必须是 y，不是就非零退出。
# ---- ReSukiSU（root）：只有当 drivers/kernelsu 真的接上了才开 ----
# ★ 自动检测而不是加一个开关：开关会被忘记，而"目录在不在"是事实。
#   接法见 scripts/kernel-setup-resukisu.sh。
RESUKISU=0
if [ -e drivers/kernelsu/Kconfig ]; then
    RESUKISU=1
    echo "== 检测到 drivers/kernelsu，启用 CONFIG_KSU（tracepoint 钩子）=="
    # KSU 是 tristate。Android 侧【不加载模块】—— 本仓已经为 =m 付过 13 次代价，
    # 所以必须 --enable（=y）而不是 --module。
    ./scripts/config --file "$OUT/.config" --enable KSU
    # 钩子方式是一个 choice。上游默认就是 tracepoint，但显式写死：
    # 默认值会随上游变，而我们不希望"某天 upstream 改了默认"就静默换了实现。
    ./scripts/config --file "$OUT/.config" --enable KSU_TRACEPOINT_HOOK
    # ★ sys_enter tracepoint 由 FTRACE_SYSCALLS 提供，而它在本机
    #   【默认是关的】（HAVE_SYSCALL_TRACEPOINTS=y 只是说架构支持）。
    #   缺了它 hook/syscall_hook_manager.c 的 register_trace_prio_sys_enter 无处可注册。
    ./scripts/config --file "$OUT/.config" --enable FTRACE_SYSCALLS
    # ★ KALLSYMS_ALL：ReSukiSU 要解析 selinux 里几个 static 符号
    #   （sel_handle_status_ops / security_dump_masked_av / context_struct_compute_av …）。
    #   Kbuild:137-141 —— 只要 CONFIG_KALLSYMS_ALL=y，整个 tools/static_export_check.mk
    #   就不会被 include；否则它会 $(error) 逼你去 security/selinux/ 里
    #   逐个删掉 `static` 关键字。用一个 config 换掉 6 处内核源码改动，
    #   而且**跨内核升级不用重新对齐**，显然更划算。
    ./scripts/config --file "$OUT/.config" --enable KALLSYMS_ALL
fi

echo "== 跑 olddefconfig（ARCH=arm64 必带，否则 arm64 符号会被删光）=="
# ⚠️★ CROSS_COMPILE 必须带：olddefconfig 会用编译器去评估 CC_HAS_* 之类的
#   能力符号。在 x86 宿主上不带它就是用【宿主 gcc】评估 arm64 内核，
#   结果是一批符号被静默改掉（本仓 2026-08-20 之前就发生过 .config 漂移）。
#   可用 CROSS_COMPILE=... 覆盖；默认与 buildbot 的 CI 一致。
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" O="$OUT" olddefconfig >/dev/null || exit 1

MUST_Y="
BLK_DEV_DM SECURITY_SELINUX EROFS_FS F2FS_FS DM_VERITY PM_WAKELOCKS
CFG80211 MAC80211 ATH11K ATH11K_PCI PCI_PWRCTRL_PWRSEQ QRTR QRTR_SMD
BT BT_QCA BT_HCIUART BT_HCIUART_QCA BT_RFCOMM BT_HIDP UHID
PINCTRL_LPASS_LPI PINCTRL_SC8280XP_LPASS_LPI SC_LPASSCC_8280XP
SND_SOC_SC8280XP SND_SOC_WSA883X SND_SOC_WCD938X SOUNDWIRE_QCOM
SND_SOC_LPASS_RX_MACRO SND_SOC_LPASS_TX_MACRO SND_SOC_LPASS_VA_MACRO
SND_SOC_LPASS_WSA_MACRO SND_SOC_QDSP6 QCOM_PD_MAPPER SND_USB SND_USB_AUDIO
FUSE_FS IIO QCOM_SPMI_ADC5 QCOM_FASTRPC
CPUSETS_V1 MEMCG_V1 UCLAMP_TASK UCLAMP_TASK_GROUP EFI_ZBOOT EFI_STUB EFI_GENERIC_STUB
MEDIA_SUPPORT MEDIA_PLATFORM_SUPPORT VIDEO_DEV V4L_MEM2MEM_DRIVERS
VIDEOBUF2_DMA_CONTIG V4L2_MEM2MEM_DEV SM_VIDEOCC_8350 VIDEO_QCOM_IRIS
PM_DEVFREQ PM_OPP PM_GENERIC_DOMAINS_OF QCOM_UBWC_CONFIG QCOM_MDT_LOADER QCOM_SCM INTERCONNECT_QCOM_SC8280XP QCOM_RPMHPD
VIDEO_QCOM_CAMSS VIDEO_HI846 VIDEO_OV13B10 VIDEO_DW9714 I2C_QCOM_CCI SC_CAMCC_8280XP LEDS_GPIO LEDS_CLASS_FLASH LEDS_QCOM_FLASH
VIDEOBUF2_DMA_SG MEDIA_CAMERA_SUPPORT V4L_PLATFORM_DRIVERS VIDEO_CAMERA_SENSOR
EXPERT PM_DEBUG PM_SLEEP_DEBUG PM_ADVANCED_DEBUG DPM_WATCHDOG
SQUASHFS NTFS3_FS NLS_UTF8
USB_NET_DRIVERS USB_USBNET MII USB_RTL8152 USB_NET_AX8817X USB_NET_AX88179_178A
ARM64_USER_MMAP_WINDOW_39
USB_NET_CDCETHER USB_NET_CDC_EEM USB_NET_CDC_NCM USB_NET_HUAWEI_CDC_NCM USB_NET_RNDIS_HOST
USB_CONFIGFS_RNDIS USB_CONFIGFS_F_MIDI USB_F_MIDI
INET_ESP INET6_ESP INET6_IPCOMP INET_DIAG_DESTROY INET_UDP_DIAG
IPV6_ROUTER_PREF IPV6_ROUTE_INFO IPV6_OPTIMISTIC_DAD IPV6_MIP6 IPV6_VTI NET_IPVTI NET_IPGRE_DEMUX
NET_SCH_HTB NET_SCH_TBF NET_SCH_INGRESS NET_CLS_U32 NET_CLS_BPF NET_CLS_MATCHALL
NET_EMATCH NET_EMATCH_U32 NET_CLS_ACT NET_ACT_POLICE NET_ACT_BPF NET_ACT_MIRRED IFB
IP_NF_MATCH_ECN IP_NF_MATCH_TTL IP_NF_TARGET_NETMAP IP_NF_SECURITY IP_NF_ARPTABLES IP_NF_ARPFILTER IP_NF_ARP_MANGLE
NF_CONNTRACK_SECMARK NF_CONNTRACK_AMANDA NF_CONNTRACK_H323 NF_CONNTRACK_IRC NF_CONNTRACK_NETBIOS_NS
NF_CONNTRACK_PPTP NF_CONNTRACK_SANE
NETFILTER_XT_TARGET_CLASSIFY NETFILTER_XT_TARGET_CONNSECMARK NETFILTER_XT_TARGET_CT
NETFILTER_XT_TARGET_NFQUEUE NETFILTER_XT_TARGET_TPROXY NETFILTER_XT_TARGET_TRACE NETFILTER_XT_TARGET_SECMARK
NETFILTER_XT_MATCH_CONNLIMIT NETFILTER_XT_MATCH_HELPER NETFILTER_XT_MATCH_IPRANGE NETFILTER_XT_MATCH_LENGTH
NETFILTER_XT_MATCH_MAC NETFILTER_XT_MATCH_STATISTIC NETFILTER_XT_MATCH_STRING NETFILTER_XT_MATCH_TIME
CRYPTO_CHACHA20POLY1305 CRYPTO_MD5 CRYPTO_XCBC
DETECT_HUNG_TASK SOFTLOCKUP_DETECTOR ANON_VMA_NAME THERMAL_STATISTICS
ARMV8_DEPRECATED SWP_EMULATION CP15_BARRIER_EMULATION SETEND_EMULATION
TOUCHSCREEN_HIMAX_HX83121A_PEN TYPEC_MUX_GPIO_SBU USB_ANDROID_USB_STATE
"
# 接了 ReSukiSU 才断言 KSU —— 没接的树上断言它只会误报。
if [ "$RESUKISU" = 1 ]; then
    MUST_Y="$MUST_Y KSU FTRACE_SYSCALLS KALLSYMS KALLSYMS_ALL"
    # KSU_TRACEPOINT_HOOK 是 choice 里的 bool，单独断言（上面的循环只认 =y/=m）
    grep -q '^CONFIG_KSU_TRACEPOINT_HOOK=y' "$OUT/.config" || {
        echo "  ✗ CONFIG_KSU_TRACEPOINT_HOOK 不是 y —— 钩子方式被换掉了"; }
fi
bad=0
for s in $MUST_Y; do
    v=$(grep -E "^CONFIG_$s=" "$OUT/.config" | cut -d= -f2)
    case "$v" in
        y) ;;
        m) echo "  ✗ CONFIG_$s=m  ← Android 不加载模块，必须 =y"; bad=1 ;;
        *) echo "  ✗ CONFIG_$s 缺失/未启用（值='$v'）"; bad=1 ;;
    esac
done
# 反向断言：这些**必须关**，开着会主动破坏 Android
MUST_N="RT_GROUP_SCHED VIDEO_QCOM_VENUS"
for s in $MUST_N; do
    if grep -qE "^CONFIG_$s=(y|m)" "$OUT/.config"; then
        echo "  ✗ CONFIG_$s 开着 —— 必须 =n（见脚本内注释）"; bad=1
    fi
done
# 取值断言：这几个曾经"以为是默认、其实是残留的调试值"（issue #16）
for kv in DPM_WATCHDOG_TIMEOUT=120 DPM_WATCHDOG_WARNING_TIMEOUT=60 PANIC_TIMEOUT=10 \
          LOG_BUF_SHIFT=19 DEFAULT_HUNG_TASK_TIMEOUT=120 \
          BOOTPARAM_HUNG_TASK_PANIC=0 BOOTPARAM_SOFTLOCKUP_PANIC=0 \
          ARCH_MMAP_RND_BITS_MAX=24; do
    if ! grep -qx "CONFIG_$kv" "$OUT/.config"; then
        echo "  ✗ 期望 CONFIG_$kv，实际 '$(grep -E "^CONFIG_${kv%%=*}=" "$OUT/.config")'"; bad=1
    fi
done
# apparmor **编进内核无害**，致命的是它出现在 CONFIG_LSM 里（会挤掉 selinux）
if grep -qE "^CONFIG_LSM=.*apparmor" "$OUT/.config"; then
    echo "  ✗ CONFIG_LSM 里有 apparmor —— 会顶掉 selinux，Android init 静默死亡"; bad=1
fi
if ! grep -qE "^CONFIG_LSM=.*selinux" "$OUT/.config"; then
    echo "  ✗ CONFIG_LSM 里没有 selinux —— selinuxfs 不会注册"; bad=1
fi

if [ $bad -ne 0 ]; then
    echo "断言失败：上面的符号会导致对应硬件静默缺席或功能被内核拒绝。" >&2
    exit 1
fi
echo "== 断言通过：$(echo $MUST_Y | wc -w) 个必须 =y、$(echo $MUST_N | wc -w) 个必须 =n =="

echo "已写入 $OUT/.config，olddefconfig 已跑，可继续检查："
echo "  grep -E '^CONFIG_(BLK_DEV_DM|SECURITY_SELINUX|LSM|USB_CONFIGFS_F_FS)=' $OUT/.config"
echo
echo "启动后必须在运行时复验（只看 config 会误判）："
echo "  cat /sys/kernel/security/lsm       # 必须包含 selinux"
echo "  ls -d /sys/fs/selinux              # 必须存在"
echo "  ls -l /dev/mapper/control          # 必须存在"
