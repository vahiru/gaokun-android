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
    --enable OVERLAY_FS

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

# ⚠️★ 必须【关掉】CONFIG_VIDEO_QCOM_IRIS —— 否则 Venus 编不过，而报错完全
#   看不出跟它有关。主线 v7.2 引入了新的 iris 驱动接管 IRIS2 世代，于是 venus
#   里这些东西被条件编译掉了：
#     drivers/media/platform/qcom/venus/core.c:1017
#       #if (!IS_ENABLED(CONFIG_VIDEO_QCOM_IRIS))
#       这道守卫之后是 sm8250_freq_table / sm8250_bw_table_enc /
#       sm8250_bw_table_dec / sm8350_reg_preset
#     drivers/media/platform/qcom/venus/core.h:58 连 VPU_VERSION_IRIS2 也没了
#   而 sc8280xp_res 正好引用其中四个 → 一串 "undeclared here" 报在 core.c 里，
#   读起来像是补丁打错了。
#   ★ 关它是对的，不是权宜：iris 的 of_match 里只有 qcs8300 / sm8550 / sm8650 /
#     sm8750 / x1p42100，**没有 sc8280xp 也没有 sm8350** —— 它永远服务不了本机，
#     却把本机需要的代码删掉了。而且它是 =m，Android 压根不加载模块。
./scripts/config --file "$OUT/.config" --disable VIDEO_QCOM_IRIS

# ─── Stage 6 M14: Venus 硬件视频编解码（V4L2 M2M）───
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
#   所以就算 --enable VIDEO_QCOM_VENUS，整条链照样静默缺席。
#   全部拉成 =y，并且下面 MUST_Y 里逐个断言 —— 这个坑本仓已经踩了 13 次。
#
# 依赖关系（drivers/media/platform/qcom/venus/Kconfig 原文）：
#   depends on V4L_MEM2MEM_DRIVERS / VIDEO_DEV && QCOM_SMEM / ARCH_QCOM &&
#              ARM64 && IOMMU_API
#   select OF_DYNAMIC / QCOM_MDT_LOADER / QCOM_SCM / VIDEOBUF2_DMA_CONTIG /
#          V4L2_MEM2MEM_DEV
# ⚠️ SM_VIDEOCC_8350 会 `select SM_GCC_8350`（drivers/clk/qcom/Kconfig:1365），
#    于是 SM8350 的 gcc 也会被编进来。无害（compatible 不匹配、永不 probe），
#    但看到它出现在 .config 里不要当成配错了。
./scripts/config --file "$OUT/.config"     --enable MEDIA_SUPPORT     --enable MEDIA_PLATFORM_SUPPORT     --enable VIDEO_DEV     --enable V4L_MEM2MEM_DRIVERS     --enable VIDEOBUF2_DMA_CONTIG     --enable V4L2_MEM2MEM_DEV     --enable SM_VIDEOCC_8350     --enable VIDEO_QCOM_VENUS

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
./scripts/config --file "$OUT/.config" \
    --enable EXPERT \
    --enable PM_DEBUG \
    --enable PM_SLEEP_DEBUG \
    --enable PM_ADVANCED_DEBUG \
    --enable DPM_WATCHDOG

# ─── olddefconfig + 断言（止损"=m 坑"）───
# 这个坑已经踩了 13 次：`scripts/config --enable X` 写进去了，olddefconfig
# 却可能因为依赖把它降回 =m（或压根没有该符号），而 Android **不加载任何模块**
# （/vendor/lib/modules 不存在、lsmod 为空），于是驱动静默缺席。
# 所以：olddefconfig 由脚本自己跑（顺手把 ARCH=arm64 这个致命参数固定住），
# 跑完立刻断言关键符号必须是 y，不是就非零退出。
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
SND_SOC_LPASS_WSA_MACRO SND_SOC_QDSP6 QCOM_PD_MAPPER
FUSE_FS IIO QCOM_SPMI_ADC5 QCOM_FASTRPC
CPUSETS_V1 MEMCG_V1 UCLAMP_TASK UCLAMP_TASK_GROUP EFI_ZBOOT EFI_STUB EFI_GENERIC_STUB
MEDIA_SUPPORT MEDIA_PLATFORM_SUPPORT VIDEO_DEV V4L_MEM2MEM_DRIVERS
VIDEOBUF2_DMA_CONTIG V4L2_MEM2MEM_DEV SM_VIDEOCC_8350 VIDEO_QCOM_VENUS
EXPERT PM_DEBUG PM_SLEEP_DEBUG PM_ADVANCED_DEBUG DPM_WATCHDOG
"
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
MUST_N="RT_GROUP_SCHED VIDEO_QCOM_IRIS"
for s in $MUST_N; do
    if grep -qE "^CONFIG_$s=(y|m)" "$OUT/.config"; then
        echo "  ✗ CONFIG_$s 开着 —— 必须 =n（见脚本内注释）"; bad=1
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
