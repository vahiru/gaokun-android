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
#    + gaokun-buildbot 那一层（19 个提交，git am -3），本表打在它上面。2026-10-05 SEC-9 从 v7.2-rc2 追上来：
#    archive/0020 / 0022 / 0055 / 0057 已被 stable 收入、移出本表；kernel/iris/0001（原 0056）rebased；
#    新增 kernel/iris/0008（原 0075，撤回 stable 带进来的 0ac05c4）。
#    ⚠️ 本表【不再适用于 v7.2-rc2 的树】（~/gk3-kernel-iris，#16 及以前）—— 那棵树的配方在本仓 c1062f2。
#    在旧树上跑 --verify 会报 kernel/iris/0001 / 0008 打不上，这是预期的。
#
# ⚠️★ --check 在【已经打满】的树上有一个已知盲区（#110）：叠加补丁里靠前的那个
#    （如 kernel/dts/0004 删掉后摄节点、kernel/dts/0005 又在同一位置写回）反向 --check 对不上，会被报成"打不上"，
#    而树其实是对的。--verify 没有这个问题：它不逐个判定，而是把整条链在干净 worktree 上重放，
#    然后问"每个被补丁碰过的文件，真实树里的与重放出来的是否逐字节相同"。
#    这也是 TODO B0（构建机的树 ≠ 本仓配方）唯一可靠的探测器。
set -uo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TREE=${1:-}
MODE=${2:-apply}

[ -n "$TREE" ] || { echo "用法: $0 <内核树路径> [--check]" >&2; exit 2; }
[ -f "$TREE/Makefile" ] && [ -d "$TREE/kernel" ] || {
    echo "✗ $TREE 看着不像内核树（缺 Makefile 或 kernel/）" >&2; exit 2; }

# ❌ 上游 Venus 补丁集（早期 upstream-venus/0013–0020）从 2026-09-28 起【不再列入】：
#   视频编解码从 qcom-venus 换成了 qcom-iris（kernel/dts/0009–0010、kernel/iris/0001–0004）。
#   0017/0018 在 VIDEO_QCOM_IRIS=y 时根本编不过（venus/core.c 的 #if !IS_ENABLED(IRIS) 把它们
#   引用的表编掉了），0019 的节点 iris 不认。已退役移除，详见 docs/dev/audio-video.md。
#   ⚠️ 这里原来是一个 UPATCHES 数组。别留一个空数组：本机（macOS）的 bash 3.2 在 set -u 下
#      展开空数组 "${A[@]}" 会报 unbound variable（与提交 5013189 同一类）。

# ⚠️ 只列发版内核补丁（按 patches/kernel/<子系统>/ 分组，各子系统修改的文件集正交，子系统内部按本地编号 0001..NNNN 排列）。
#    其余补丁归属各自目录：patches/android/（crdroid-tree-fixes.py）、patches/mesa/（mesa-a17/regen.sh）、
#    patches/libcamera/（setup-libcamera-tree.sh）、patches/resukisu/（kernel-setup-resukisu.sh）、
KPATCHES=(
    # ─── 1. kernel/core：核心内核、Android 基础支撑（ashmem / xt_quota2 / bpffs / SELinux）、架构与通用子系统 ───
    kernel/core/0001-efi-pstore-register-backend-when-efivars-ops-arrive-.patch
    kernel/core/0002-bpf-inode-label-bpffs-lazily-for-android-genfscon.patch
    kernel/core/0003-remoteproc-qcom-ratelimit-repeat-handover-error.patch
    # 0004/0005（原 0016/0017）是 Android 起不来的硬前提（ashmem / xt_quota2，ACK 专有）。
    #    它们此前只活在构建机的内核树里，2026-09-08 照本仓配方重建的内核
    #    因此黑屏起不来 —— 见 docs/dev/android-bringup.md。别删。
    kernel/core/0004-staging-android-port-ashmem-from-ack.patch
    kernel/core/0005-netfilter-port-xt-quota2-from-ack.patch
    # 0006（原 0078）：【本地】页表仍 48 位，64 位进程默认地址窗口压到 2^39（Kconfig ARM64_USER_MMAP_WINDOW_39，
    #    kernel-config-android.sh 打开并断言）。issue #17：英雄联盟手游在 48 位窗口下 il2cpp_init 必崩，
    #    2026-10-08 A/B 实测；用户同日定进发版（起初是 --with-va39w 的实验补丁）。只碰 arch/arm64 的 Kconfig 与 processor.h。
    kernel/core/0006-arm64-user-mmap-window-39-bits-on-48-bit-page-tables.patch
    # ★ 0007（原 0084，#game-env）：netlink RTM_GETLINK/GETNEIGH 隐私边界 —— AOSP 策略有 readpriv/getneigh
    #    细分权限，主线 nlmsgtab 把它们全映射到 NLMSG_READ 导致检查形同虚设。钉死 remap。
    kernel/core/0007-selinux-netlink-route-remap-getlink-getneigh-priv-perms.patch
    # 0008 (formerly 0085): the release Android apps are told, set from Settings. Everything else sees the real one.
    kernel/core/0008-kernel-report-a-set-release-to-android-apps.patch
    # 原 0086（KernelSU 子目录）已改名 patches/resukisu/0003，由 kernel-setup-resukisu.sh 打、release.sh 核对。
    # 0009（原 0171）：上游FastRPC修复v3的移植，还没进主线。被信号打断的invoke重试时接回原来的上下文，不再重发给DSP。
    #    不打的话每冻结一次泄漏一个ctx编号，255个用完后hexagonrpcd-sdsp起不来（ENOSPC，退出码4），重启才恢复
    #    （按代码SLPI重启也能恢复）。主线收入正式修复后去掉，去掉前先核对正式版本是否还带着v3的问题
    #    （补丁说明末尾的[Akisaira: ...]，例如硬中断里拿fl->lock）。
    kernel/core/0009-misc-fastrpc-fix-context-leak-and-hang-on-signal-interrupted-invoke.patch

    # ─── 2. kernel/pcie-pm：PCIe、NVMe、cpuidle、系统休眠与唤醒基础设施 ───
    # 0001–0007（原 0188、0190–0195）：深度待机，以及PCIe、NVMe恢复失败时的处理，说明见docs/dev/deep-sleep.md。
    #    配套 Wi-Fi / xHCI 恢复修复见 kernel/wifi/0011–0026、kernel/ec-usb/0014、kernel/dts/0020。
    # 0001（原 0188）：链路断开时留一票active-only的PCIe-MEM投票。不打的话两条链路都断开后整机复位。只对SC8280XP、SA8540P生效。
    kernel/pcie-pm/0001-PCI-qcom-keep-an-active-only-PCIe-MEM-vote-in-L2-on-SC8280XP.patch
    # 0002（原 0190）：上游d0cd9c8d0fd5。蓝牙串口系统休眠时放掉时钟和投票。不打的话开着蓝牙CX下不去。
    kernel/pcie-pm/0002-serial-qcom-geni-add-force-suspend-resume-to-system-sleep-callbacks.patch
    # 0003（原 0191）：上游d4c79b63d82d。根端口能进D3hot，链路才会走L2。不打的话SoC进不了深度待机。
    #    依赖0001（原 0188），单独打就走到无票路径。SC8280XP CRD上有这个提交导致挂起崩溃的报告（2026-09，未定论）。
    #    出问题时cmdline加pcie_port_pm=off回到不断链的待机。
    kernel/pcie-pm/0003-PCI-Allow-D3-for-native-hotplug-capable-Root-Ports-on-non-x86-platforms.patch
    # 0004（原 0192）：恢复时链路超时不再拆host。不打的话根端口和端点的访问会碰到断了时钟的DBI。
    kernel/pcie-pm/0004-PCI-dwc-keep-the-host-up-if-the-link-fails-on-resume.patch
    # 0005（原 0193）：恢复时链路没起来，把槽断开重来一次。不打的话这条链路一直断着，上面的设备无法使用。
    kernel/pcie-pm/0005-PCI-dwc-retry-a-link-that-doesn-t-come-back-on-resume.patch
    # 0006、0007（原 0194、0195）：系统盘没了就panic重启。不打的话/data不可用、也挂起不了，一直卡着。
    kernel/pcie-pm/0006-PCI-dwc-panic-if-the-link-to-the-NVMe-disk-is-lost-on-resume.patch
    kernel/pcie-pm/0007-nvme-panic-when-a-disk-is-marked-dead.patch
    # 0008（原 0196）：SC8280XP的PCIe控制器在系统挂起和恢复时并行处理。不打的话唤醒时两条链路依次重新训练，每次多等约0.24秒。
    kernel/pcie-pm/0008-PCI-qcom-resume-the-SC8280XP-controllers-in-parallel.patch
    # 0009（原 0197）：挂起时设备已经发了PME（要叫醒系统），就让这次挂起失败，不再发PME_Turn_Off。不打的话开着WoWLAN时，
    #    挂起途中Wi-Fi收到唤醒包会不回应PME_Turn_Off，主机照样断电，几次之后链路回不来。
    kernel/pcie-pm/0009-PCI-dwc-abort-suspend-when-a-device-has-signalled-PME.patch
    # 0010（原 0198）：s2idle里集群下电被固件拒绝（-EPERM）后先等3 ms再申请。不打的话被拒的CPU马上重新申请，几个CPU轮流被拒，
    #    这个环可能一直转到下次唤醒，这段时间SoC进不了深度待机。说明见docs/dev/deep-sleep.md。
    kernel/pcie-pm/0010-cpuidle-psci-wait-after-a-denied-cluster-request-in-s2idle.patch
    # ★ 0011–0012（原 0199、0201，配合 kernel/dts/0021 与 kernel/wifi/0027）：Wi-Fi用PCIe WAKE#把睡眠中的系统叫醒。
    #    依赖0001–0008的断链待机：从固件字符串和测试推断，固件要在链路走L2、PERST#拉低之后才拉WAKE#。
    #    0009让挂起途中碰上唤醒时不丢链路。
    # 0011（原 0199）：上游071e245ab749，PCI核心支持WAKE#中断。不打的话WAKE#没有人处理，Wi-Fi的唤醒要等下一个闹钟。
    kernel/pcie-pm/0011-PCI-Add-support-for-PCIe-WAKE-interrupt.patch
    # 0012（原 0201）：恢复时先撤掉唤醒中断的唤醒设置，再重新打开设备中断。不打的话Wi-Fi还拉着WAKE#时，电平触发的WAKE#中断
    #    在这段空档里反复触发，一次恢复多出几千到两万多个中断，拖慢恢复最多约0.25秒。
    kernel/pcie-pm/0012-PM-sleep-disarm-wake-IRQs-before-resuming-device-IRQs.patch

    # ─── 3. kernel/wifi：ath11k、mac80211 与 MHI ───
    # 0001（原 0070）：【本地】MHI 给 ath11k 保留固件 DMA 表跨断电复用 —— issue #16 待机睡死的根治
    #   （恢复时不再赌 order-7 GFP_DMA 分配）。⬜ 实机 s2idle 循环待验（docs/TODO.md 的 issue 一节）。
    kernel/wifi/0001-bus-mhi-keep-firmware-images-across-power-cycles-for-ath11k.patch
    # ★★ 0002–0004（原 0097–0099）：ath11k固件崩溃恢复，准备推主线（英文说明，0002、0003的Fixes指向主线提交）。打开WoWLAN
    #    （#40）以后，固件偶尔在休眠中崩溃，崩溃恢复又和系统挂起撞在一起：内核panic，或者WLAN的PCIe链路断掉。
    #    0002：恢复失败时不再释放HAL。32be3ca4cf78以后成功路径只清零、复用HAL，失败路径却还释放它，下一次
    #    恢复就解引用空的srng_config；0003：挂起时先拿core_lock，等进行中的恢复做完，环被恢复释放了就不碰；
    #    WoW挂起在恢复期间改走不带WoW的挂起；0004：固件一崩溃就让发给它的命令立即失败。系统挂起、恢复期间
    #    （PM_SUSPEND_PREPARE到PM_POST_SUSPEND）不做重置，交给恢复：断电挂起醒来本来就重新加载固件，否则在
    #    ath11k_mac_op_start()里先断电、上电再让mac80211配置；醒着时照常重置，并在恢复完成之前不让系统挂起
    #    （wakeup source "ath11k_recovery"，最长60秒）。
    kernel/wifi/0002-wifi-ath11k-keep-the-HAL-when-crash-recovery-fails.patch
    kernel/wifi/0003-wifi-ath11k-dont-reap-rings-that-crash-recovery-frees-on-suspend.patch
    kernel/wifi/0004-wifi-ath11k-keep-firmware-crash-recovery-away-from-system-suspend.patch
    # ★★ 0005–0007（原 0100–0102）：接着0002–0004，待机测试里又找到的三处，同样准备推主线。0005：醒来时发现固件在睡眠中崩了
    #    （RDDM/SYS_ERR）就交给崩溃恢复：原来强行恢复MHI，等满超时失败，mac80211判定硬件丢失、关掉接口，Wi-Fi到
    #    下次冷启动前都起不来，之后每次唤醒还要多等约20秒；0006：崩溃恢复成功后再保持唤醒10秒
    #    （ATH11K_RECOVERY_SETTLE_MS），否则系统立刻挂起，WoW落在刚重启、还没连上的固件上，又把它弄崩，循环下去；
    #    0007：“断线唤醒”对应固件的四个唤醒事件（WOW_DEAUTH_RECVD_EVENT、WOW_DISASSOC_RECVD_EVENT、WOW_BMISS_EVENT、
    #    WOW_CSA_IE_EVENT），都是用来发现已有连接断开的；原来没连上路由器时进WoW也打开，现在只在arvif->is_up时打开。
    kernel/wifi/0005-wifi-ath11k-hand-a-firmware-crash-found-on-resume-to-crash-recovery.patch
    kernel/wifi/0006-wifi-ath11k-stay-awake-for-a-while-after-crash-recovery.patch
    kernel/wifi/0007-wifi-ath11k-arm-the-WoW-disconnect-events-only-when-connected.patch
    # ★★ 0008–0010（原 0103–0105）：休眠中的Wi-Fi。0008（mac80211）：驱动挂起失败（返回负值）时清掉local->suspending，
    #    否则系统一直醒着，到下一次成功挂起之前收不到信标也不探测AP；0009：扫描期间持wakeup source "ath11k_scan"
    #    （最长到扫描超时），扫完再保持5秒，留给上层取结果、发起连接。息屏时Android的软件PNO靠闹钟把系统叫醒去扫描，
    #    却不为扫描持wakelock，系统马上又挂起，mac80211中止扫描，休眠中断线后就一直连不回来；0010：固件拒绝进入WoW
    #    （htc suspend not complete）时让这次挂起失败、稍后重试（连续最多3次），不再退回到断开连接、断电挂起。
    #    0008、0010准备推主线；0009是为Android做的取舍，不推。
    kernel/wifi/0008-wifi-mac80211-clear-the-suspending-flag-when-the-driver-fails-to-suspend.patch
    kernel/wifi/0009-wifi-ath11k-keep-the-system-awake-while-a-scan-runs.patch
    kernel/wifi/0010-wifi-ath11k-retry-the-suspend-when-the-firmware-declines-WoW.patch
    # 0011（原 0170）：Wi-Fi关着时固件崩溃，mac80211不走reconfig_complete，这次reset结束不了。不打的话之后每次加载固件都持60秒
    #    ath11k_recovery，Wi-Fi关着时每次挂起都给设备下电，所以每次醒来都多醒一分钟。
    kernel/wifi/0011-wifi-ath11k-end-a-reset-that-has-no-radio-to-restart.patch
    # ★ 0012–0026（原 0173–0187）：ath11k和mac80211在挂起恢复和固件崩溃恢复上的修复，打在0011之上（0017、0023都改它加的函数）。
    # 0012（原 0173）：停掉影子定时器时清started。不打的话挂起时碰上一次，这个定时器到重启前都不再运行。
    kernel/wifi/0012-wifi-ath11k-clear-the-started-flag-of-a-stopped-shadow-timer.patch
    # 0013（原 0174）：WoW醒来后对丢了RX BA会话的TID发DELBA，让路由器重新建立会话。不打的话TCP乱序一直持续到重新关联。
    kernel/wifi/0013-wifi-mac80211-set-RX-block-ack-sessions-up-again-after-WoWLAN.patch
    # 0014（原 0175）：重启失败时留着已注册的hw。不打的话会释放mac80211还在用的hw。和Julius Bairaktaris的列表补丁
    #    （ath11k 2026-October/009366）相同，多一行注释，那组补丁进了所用内核后去掉。
    kernel/wifi/0014-wifi-ath11k-keep-the-registered-hw-when-a-restart-fails.patch
    # 0015（原 0176）：不重复关PCI中断。不打的话中断在下次启动后还是关着，到重启前每次重启固件都超时。留着0016就要留着它。
    #    Florian Maurer的列表补丁（ath11k 2026-November/009504）修了外部中断那一半，它进了所用内核后这里收窄到CE中断。
    kernel/wifi/0015-wifi-ath11k-don-t-disable-the-PCI-interrupts-twice.patch
    # 0016（原 0177）：重启途中再崩也reset。不打的话固件已死、radio还开着，所有帧和WMI命令都失败，直到碰巧有一次挂起给设备下电。
    kernel/wifi/0016-wifi-ath11k-reset-again-when-the-firmware-crashes-during-a-restart.patch
    # 0017（原 0178）：打在0016之上。崩溃循环里前两次立刻reset，之后从30秒起翻倍等待，6次后放弃，等下一次给设备下电的挂起再试。
    #    不打的话固件一直崩就一直reset，系统被反复叫醒。
    kernel/wifi/0017-wifi-ath11k-back-off-from-a-firmware-crash-loop-then-give-up.patch
    # 0018（原 0179）：删除station不失败。不打的话固件崩溃后断开连接会触发mac80211的WARNING。
    #    列表补丁ath11k 2026-November/009513覆盖得更宽，它进了所用内核后去掉。两者文本上不冲突，要人工去掉。
    kernel/wifi/0018-wifi-ath11k-don-t-fail-the-removal-of-a-station.patch
    # 0019（原 0180）：主线e588e83e5c7a，拆除RX BA会话时停掉会话定时器。7.2.10已带（09b7d5e4f885），内核升到7.2.10时去掉。
    kernel/wifi/0019-wifi-mac80211-shut-down-RX-BA-session-timer-on-teardown.patch
    # 0020（原 0181）：主线c7555d47b1d0，ath11k_dp_rx_tid_del_func()的加锁。所用内核带上后去掉。
    kernel/wifi/0020-wifi-ath11k-fix-locking-problem-in-ath11k_dp_rx_tid_del_func.patch
    # 0021、0022（原 0182、0183）：主线3e7f5d362b2b、dc29226dada3，两处一边遍历IDR一边删除，要成对打。不打的话这两处都可能崩溃。
    #    所用内核带上后去掉。
    kernel/wifi/0021-wifi-ath11k-avoid-IDR-mutation-during-pending-mgmt-TX-cleanup.patch
    kernel/wifi/0022-wifi-ath11k-avoid-IDR-mutation-during-vif-mgmt-TX-cleanup.patch
    # 0023（原 0184）：重启途中崩溃时op_start、add_interface不失败。不打的话mac80211关掉接口，Android的WifiSelfRecovery跟着关Wi-Fi，要手动打开。
    kernel/wifi/0023-wifi-ath11k-don-t-fail-a-restart-on-a-firmware-crash.patch
    # 0024（原 0185）：Julius Bairaktaris的列表补丁v2（ath11k 2026-October/009368），和0023配对。环重建完才清CRASH_FLUSH。
    #    不打的话重建期间发WMI命令会解引用空指针。换成v3或正式版本时，要按它的错误路径核对0025。
    kernel/wifi/0024-wifi-ath11k-clear-CRASH_FLUSH-only-after-recovery-rebuilds-the-rings.patch
    # 0025（原 0186）：覆盖0023之后的失败路径。不打的话挂起失败后会在已释放的环上启动定时器，引发Oops。
    kernel/wifi/0025-wifi-ath11k-cover-a-restart-that-fails-and-a-suspend-that-fails-after-it.patch
    # 0026（原 0187）：reset里上电失败的处理。不打的话PCIe链路丢失后MHI准备失败，下一次给设备下电的挂起或关机会重复释放
    #    MHI的命令环并解引用空指针；op_start还要持着rtnl等满40秒。
    kernel/wifi/0026-wifi-ath11k-handle-a-failed-power-up-in-a-reset.patch
    # 0027（原 0202）：固件因为收到的东西叫醒系统后，ath11k_wow唤醒源保持1秒（ath11k.wow_wakeup_ms）。不打的话Android
    #    约0.1秒后就又挂起，收包的进程可能还没运行。
    kernel/wifi/0027-wifi-ath11k-keep-the-system-awake-briefly-after-a-WoW-wakeup.patch

    # ─── 4. kernel/ec-usb：华为高鲲 EC、电池、Type-C UCSI、USB Gadget 与 xHCI ───
    # 0001（原 0072）：【本地】电池驱动 0x82 按位解码（0x00 / 组合值不再沿用旧 status）、加 CAPACITY_LEVEL
    #   （0% 且没在净充电、或 EC 危急位且 <= critical_max_capacity ⇒ Critical —— Android 有了它就只看它关机）、
    #   只读 ec_raw 给实测用。1.0 计划 BATT-1/2/3。⬜ 带负载放电实测（current_now 正负号、危急位阈值）。
    kernel/ec-usb/0001-power-supply-gaokun-battery-decode-status-bits-and-capacity-level.patch
    # 0002（原 0073）：【本地】EC probe 时读一次盖子状态并补报 SW_LID（原来只在事件里报，合盖开机会被当成开着）。DISP-1。
    kernel/ec-usb/0002-platform-arm64-gaokun-ec-report-initial-lid-state.patch
    # ★ 0003–0008（原 0087–0092）：UCSI核心和ucsi_huawei_gaokun的六个修复，准备推主线（英文说明，Fixes指向主线提交）。
    #    0003：0c8ee850572b把"对端不支持USB数据⇒角色NONE"写反了，EC从不置这个标志，每次插拔都被切成host；
    #    0004：num_ports在probe时就设，否则插头方向报不出去；0005：离开DP模式时报HPD断开，否则另一个口的DP
    #    分不到资源；0006：不在EC中断线程里等USB事件（原来每次等满2秒）；0007：先按EC的USB事件设插头方向
    #    再切DP，否则翻面插显示器偶尔没画面；0008：EC丢掉的GET_CONNECTOR_STATUS重发，否则一直"忙"时两个口
    #    的插拔都没人处理。
    kernel/ec-usb/0003-usb-typec-ucsi-restore-the-USB-data-check-on-partner-change.patch
    kernel/ec-usb/0004-usb-typec-ucsi-huawei_gaokun-set-the-number-of-ports-at-probe.patch
    kernel/ec-usb/0005-usb-typec-ucsi-huawei_gaokun-report-HPD-when-leaving-DP-alt-mode.patch
    kernel/ec-usb/0006-usb-typec-ucsi-huawei_gaokun-dont-block-the-EC-notifier.patch
    kernel/ec-usb/0007-usb-typec-ucsi-huawei_gaokun-set-the-orientation-before-DP.patch
    kernel/ec-usb/0008-usb-typec-ucsi-huawei_gaokun-resend-dropped-GET_CONNECTOR_STATUS.patch
    # ★ 0009（原 0093）：UCSI不再设置usb_0的角色（update_connector里放掉它查到的role switch）。EC对什么都报对端类型
    #    UFP、从不置USB数据标志，UCSI定的角色没有依据；角色由USB端口HAL（device/huawei/gaokun3/usb）按电气事实决定。
    kernel/ec-usb/0009-usb-typec-ucsi-huawei_gaokun-leave-the-usb_0-role-to-userspace.patch
    # ★ 0010（原 0094）：EC挂起期间关掉中断（disable_irq），恢复后再打开。原来处理函数碰到suspended就返回，而中断是
    #    电平触发的，每次从休眠醒来在EC恢复之前都要空转上万次。配套唤醒源见 kernel/dts/0015（原 0095）。
    kernel/ec-usb/0010-platform-arm64-huawei-gaokun-ec-disable-the-IRQ-across-suspend.patch
    # ★ 0011（原 0096）：android_usb类（android0/state）。UsbService只在/sys/class/android_usb存在时才建UsbDeviceManager
    #    （USB用途、MTP/PTP）；连接状态由USB端口HAL写（device/huawei/gaokun3/usb）。要=y（kernel-config-android.sh）。
    kernel/ec-usb/0011-usb-gadget-add-android_usb-class-for-UsbDeviceManager.patch
    # ★ 0012（原 0112，#41）：configfs把"none"当作解绑（Android内核的行为）；UDC不在时驱动挂起等它出现，不再返回-EBUSY；
    #    注册了但没绑上时，再写同名就重新注册。port0当主机时UDC被注销，这期间切换USB功能，原来写UDC失败、回到设备
    #    模式也没人重写，只开MTP/PTP时接电脑枚举不到（#41评审）。只去掉match_existing_only不行，见补丁说明。
    #    原 0111 留给#46评审里拆出去的NVMe挂起下电（实验性，另提）。
    kernel/ec-usb/0012-usb-gadget-configfs-take-none-as-unbind-and-wait-for-an-absent-UDC.patch
    # 0013（原 0168）：恢复时按EC读到的状态上报盖子，原来一律报开着。不打的话，合着盖被闹钟等唤醒时Android当成开盖，屏幕会亮。
    kernel/ec-usb/0013-platform-arm64-huawei-gaokun-ec-report-the-real-lid-state-on-resume.patch
    # 0014（原 0172）：xHCI控制器死掉后，系统挂起和恢复都跳过它。不打的话xhci_suspend()一直返回-EINVAL，到重启前每次系统挂起都失败。
    #    USB设备本身还是要重启才回来。
    kernel/ec-usb/0014-usb-xhci-don-t-fail-system-suspend-on-a-dead-host.patch
    # 0015（原 0211）：EC的中断在挂起期间改成边沿触发，醒着时仍是电平触发。不打的话SoC进了最深的待机（AOSD）之后，
    #    睡眠中插上充电器多半叫不醒系统：PDC一直把SoC叫醒，GIC上却没有中断，要等下一个闹钟或电源键。
    kernel/ec-usb/0015-platform-arm64-huawei-gaokun-ec-use-an-edge-triggered-IRQ-while-suspended.patch
    # ★ 0016（原 0214）：电池温度上报（POWER_SUPPLY_PROP_TEMP）。EC状态块0x9c（ec_raw的w9c）以0.1 K为单位上报电池温度，
    #    换算为0.1 °C（raw - 2731）并限制在[-40.0 °C, 120.0 °C]范围内（越界返回-ENODATA，避免未初始化或读失败触发Android过温关机）。
    kernel/ec-usb/0016-power-supply-huawei-gaokun-battery-report-temperature.patch

    # ─── 5. kernel/drm：DRM 核心、MSM DPU 色彩与平面、DSI PHY、Himax 面板 ───
    kernel/drm/0001-drm-crtc-drop-racy-BUG_ON-in-fence_to_crtc.patch
    # 0002（原 0076）：【本地，v7.2.9 起】撤回 stable 的 Revert "drm/msm: dsi: fix PLL init in bonded mode"（5de981b7db）——
    #    没有它本机双 DSI 绑定面板黑屏（触摸中断 0/s）；有它 118/s 有画面。2026-10-05 二分定案。
    kernel/drm/0002-drm-msm-dsi-phy-7nm-reapply-bonded-pll-init-reverting-5de981b7db.patch
    # ★ 0003（原 0123，配套 DT 见 kernel/dts/0017）：面板驱动按主板的 ID 引脚（GPIO 102、91，UEFI 也读这两根）认出 BOE 屏，
    #    同一个镜像通吃两种屏。读到 BOE 的值才换初始化序列并更新设备树的 compatible（gk3pend 读它选列间距），其余情况保持 DT 写的 CSOT。
    kernel/drm/0003-drm-panel-himax-hx83121a-read-the-panel-from-the-board-s-ID-pins.patch
    # 0004–0007（原 0133–0136）：显示色彩，说明见 docs/dev/display-color.md。
    # 0004（原 0133）：修 CTM 写进 PCC 时丢符号和行列转置；不打的话色域转换时画面发绿。
    kernel/drm/0004-drm-msm-dpu-fix-the-CTM-to-PCC-coefficient-conversion.patch
    # 0005（原 0134）：读 ACPI DLUT 表的工厂校准，sRGB、P3 按连接器 Colorspace 切换，默认打开，msm.panel_calibration=0 关掉。
    #    依赖 0004，切 P3 要和 android/drm-hwcomposer/0002（原 0132，tree-fix [32]）一起生效。
    kernel/drm/0005-drm-msm-dpu-apply-the-gaokun3-factory-panel-calibration.patch
    # 0006（原 0135）：8 bpc 面板也开空间抖动，面板描述同时补上 .bpc。
    kernel/drm/0006-drm-msm-dpu-dither-8-bpc-panels.patch
    # 0007（原 0136）：平面列表加 ABGR、XBGR2101010；不打的话 android/drm-hwcomposer/0001（原 0131）判定不支持，帧缓冲保持 8 位。
    kernel/drm/0007-drm-msm-dpu-expose-ABGR-and-XBGR-2101010-on-planes.patch
    # 0008（原 0138）：背光寄存器和亮度成线性，报成 linear，否则 HWC 接管背光后会再叠一条 HLG 曲线。
    kernel/drm/0008-drm-panel-himax-hx83121a-report-a-linear-backlight-scale.patch

    # ─── 6. kernel/iris：qcom-iris 视频解码驱动 ───
    # 0001–0008（原 0056–0075，#128；archive/0058、0062 不列，配套 DT 见 kernel/dts/0009–0010）：
    #    视频编解码 qcom-venus → qcom-iris。iris 靠 DT 的回落 compatible "qcom,sc8280xp-iris", "qcom,sm8250-venus"
    #    直接用 v7.2-rc2 自带的 sm8250_data，驱动不用加平台。
    #    ⚠️ 需要 .config 里 VIDEO_QCOM_IRIS=y、VIDEO_QCOM_VENUS 不设（kernel-config-android.sh 已断言）。
    # ✅ archive/0055 / 0057（上游 v7.3 的 iris 修复 f87d7ed / 75d7987）**已被 v7.2.y stable 收入**
    #    （stable 7a52e76b6309 / cb83cfff8dff），v7.2.9 起【不再列入】。已从本仓移除。
    # 0001（原 0056）：【本地】代替上游 b9c2215。b9c2215 在持 core->lock 断电时 disable_irq() 等中断线程，
    #    而线程开头就要这把锁 ⇒ 死锁；原版 nosync 又会让线程在断电后碰寄存器（本机 = 静默死机）。
    #    这里保留 nosync，线程拿到锁后看 core->hw_powered，断电了就不碰硬件。
    #    ⚠️ v7.2.y stable 收了 b9c2215 ⇒ 0001 rebased onto v7.2.9，多一行把 disable_irq() 改回 nosync。
    kernel/iris/0001-media-iris-irq-thread-skips-hw-access-after-power-off.patch
    # ❌ archive/0058（上游 0ac05c4）故意【不列】：在 gen1 上 LOAD_RESOURCES 位一直置着，v7.2-rc2 原版的
    #    `!= DRAIN` 才让"drain 中途 seek 后的 STOP"照常放行；0058 反而把它变成 -EBUSY。见 archive/0058 头部横幅、#128 §3。
    #    ⚠️ 可 v7.2.y stable 把它收进来了（a5e341a25de3）⇒ 下面 0008（原 0075）把这一行撤回 rc2 的写法。
    # 0002（原 0059）：上游 v7.4 队列的 cff20ea4 —— UC_REGION 配置被拒时如实报错，而不是"启动成功"后挂死。
    kernel/iris/0002-media-iris-fail-firmware-boot-on-invalid-uc_region.patch
    # 0003（原 0060）：【本地】解码器在第一次 SOURCE_CHANGE 前拒绝 CAPTURE G_FMT，让 v4l2_codec2 走 venus 时代
    #    验过的那条路。0644 模块参数 qcom_iris.venus_compat_gfmt（默认 Y），写 N 即上游行为。
    kernel/iris/0003-media-iris-venus-compatible-decoder-capture-g_fmt.patch
    # 0004（原 0061）：上游 v7.4 队列的 e2e2bc05 —— 遍历实例链表时拿 core->lock（Codec2 探能力时的 open/close 会撞上）。
    kernel/iris/0004-media-iris-take-core-lock-when-scanning-the-instance-list.patch
    # ❌ archive/0062（放宽 PC_READY 轮询窗口）故意【不列】：前提被 iris-k5 推翻 —— 健康的固件 PC_PREP 之后 1–3 us 就就绪，
    #    "断电被跳过"是固件已经卡死的后果（成因见 0005）。留着只会让卡死时持 core->lock 从 2.5 ms 变成 150 ms。#128 §9。
    # 0005（原 0065）：【本地】解码中 CAPTURE streamoff 除分辨率切换外一律发 HFI_FLUSH_ALL（与 venus 同）。上游发的
    #    HFI_FLUSH_OUTPUT 让华为固件卡死（k5 A/B：8/11 vs 0/15），之后断电永远跳过、硬解全挂到重启（#128 §9）。
    kernel/iris/0005-media-iris-flush-all-on-capture-streamoff-except-drc.patch
    # 0006（原 0066）：【本地】gen1 解码器的 seek 学 venus：只 flush、会话不停（0644 qcom_iris.seek_mode，默认 1；0 = 上游）。
    #    上游 seek 是同一会话 STOP 后重新 START，华为固件断言（video_decoder_utils.c:3056，#128 §12–§13）。
    kernel/iris/0006-media-iris-gen1-decoder-seek-mode-switch.patch
    # 0007（原 0067）：【本地】drain 之后不 streamoff 也能接着解：EOS 时不自动 FLUSH_OUTPUT（与 venus 同），START 时重置 LAST 状态。
    #    上游 ⇒ 播到结尾后的第一次 seek 没有输出、每隔一次 drain 收不到 EOS（#128 §15）。0644 qcom_iris.eos_flush，默认 N。
    kernel/iris/0007-media-iris-no-output-flush-on-eos.patch
    # 0008（原 0075）：【本地，v7.2.9 起】撤回 stable 收进来的 0ac05c4（即被否的 archive/0058），iris_allow_cmd 回到 rc2 的 `!= DRAIN`。
    kernel/iris/0008-media-iris-restore-rc2-stop-check-reverting-0ac05c4.patch
    # ★ 0009（原 0125）：【本地】修 0006（原 0066）：第一次 CAPTURE streamon 之前 flush 过时输出口属性没发过，不能跳过，否则固件报 0x1009、硬解开播失败。
    kernel/iris/0009-media-iris-configure-output-after-flush-before-first-capture-streamon.patch
    # 0010（原 0126）：gen1 收到固件看门狗中断后走系统错误恢复（gen2 本来就有）；不打就卡死到重启。
    kernel/iris/0010-media-iris-gen1-recover-from-firmware-watchdog.patch
    # 0011（原 0127）：CAPTURE 的 S_FMT 不再把解码裁剪冲成缓冲尺寸；不打 1080p 就报成 1088。
    kernel/iris/0011-media-iris-keep-decoder-crop-across-capture-s_fmt.patch
    # 0012（原 0129）：gen1 解码器支持 10-bit（HEVC Main10、VP9 Profile 2），输出 P010。
    #    要和 mesa/0002（原 0130，Turnip）、crdroid-tree-fixes.py 的 [29][30] 一起生效，单独打没有实际用处。
    kernel/iris/0012-media-iris-gen1-decode-10-bit-to-p010.patch

    # ─── 7. kernel/camera：camcc 时钟/GDSC、camss 与前后摄传感器驱动（ov13b10 / hi846）───
    # ✅ archive/0020（上游 499b4cb6710f，camcc 不再注册 CAMCC_GDSC_CLK）**已被 v7.2.y stable 收入**
    #    （stable b425cc3913fb），基线追到 v7.2.9 起【不再列入】。已从本仓移除（v7.2-rc2 的配方见 c1062f2）。
    # ❌ archive/0021（给 titan_top 加 NoC 投票）**已被实测否掉**（内核 #6，见 #102），
    #    故意【不列】在这里。已从本仓移除。
    # ✅ archive/0022（上游 86b23609d5e1，gdsc_unregister 拆 genpd）**已被 v7.2.y stable 收入**（stable 40bd77fa2857），
    #    v7.2.9 起【不再列入】。⚠️ 同批 stable 还带进 gdsc.c 的两处行为变化（fade2037ee2a：ALWAYS_ON 域
    #    gdsc_enable 失败要上报；02533b6cf5f6：poll_status 透传 check_status 的错误）—— 相机 titan 域要回归。
    # ❌ archive/0024 / 0025 / 0026 三条试验补丁**已被内核 #10 实测否掉**（#104：上电后重试、
    #    塌缩前复位 CAMNOC+CPAS、塌缩前复位全部 21 个 BCR，都救不回来），故意【不列】。
    #    已从本仓移除。
    # 0001（原 0027）是候选修复（#105）：sc8280xp 的 camcc 一个 GDSC 都没给等待值，
    #    gdsc_init() 就用 MSM8974 的 2/8/2 覆盖硬件复位值；同代同偏移的 sm8150/sc8180x
    #    都写 2/2/0xf。它改的是 camcc-sc8280xp.c 的 gdsc 结构体，与 archive/0020 的 hunk 不相交。
    #    ❌ 已被内核 #11 实测否掉（#105）：硬件复位值确实是 2/2/0xf，
    #    但改回去之后脏塌缩签名与上电冻死一字不差。留在列表里是因为它**就是正确的硬件值**
    #    （上游同代驱动全这么写），只是不是这个缺陷的原因。
    kernel/camera/0001-clk-qcom-camcc-sc8280xp-gdsc-wait-vals.patch
    # 0002（原 0031）候选修复（#105）：camnoc_axi / slow_ahb / fast_ahb 三个 RCG 标成 shared，
    #    关闭时停靠 XO。实测一次出流后 camnoc_axi_clk_src 指着已熄灭的 pll0_out_even。
    #    改的是 camcc-sc8280xp.c 里三个 .ops 行，与 archive/0020、0001 的 hunk 不相交。
    kernel/camera/0002-clk-qcom-camcc-sc8280xp-mark-camnoc-ahb-rcgs-shared.patch
    # 0003（原 0034，后摄 OV13B10，#106，配套 DT 见 kernel/dts/0005）：给上游 ov13b10 加 OF 匹配表与板级上电序列。
    #    三关（probe / 出帧 / HAL 枚举）都过了才进来。
    #    ⚠️ 需要 .config 里 VIDEO_OV13B10=y 与 VIDEO_DW9714=y（kernel-config-android.sh 已断言）。
    #    archive/0033（s5k3l6xx 驱动）故意【不列】：本机后摄不是 S5K3L6，文件留作参考。
    kernel/camera/0003-media-i2c-ov13b10-of-match-and-gaokun3-power-sequence.patch
    # 0004（原 0035，#108）：camss 等不到某颗传感器时（同板另一种后摄模组），超时后只带绑上的传感器
    #    完成 notifier —— 于是"后摄模组不对"只丢后摄，前摄照常。内核 #19 两路实测：
    #    正常 47 个 subdev；ov13b10.fail_probe=1 时 20 s 后回落、45 个 subdev、前摄出帧。
    kernel/camera/0004-media-camss-continue-without-never-bound-sensors.patch
    # ★ 0005、0006（原 0207、0208）：前摄hi846的帧时间。0005让PIXEL_RATE报像素阵列的速率（各模式都是288 MHz），并补回LINK_FREQ控件
    #    给camss用；不打的话所有模式都报144 MHz，libcamera算出的帧长和曝光都翻倍，暗处也不会延长帧时间，camss给全尺寸
    #    模式推出的链路频率也是错的。0006让写曝光时沿用VBLANK设的帧长；不打的话每次写曝光都把帧长打回模式默认值，
    #    帧率控制在前摄上无效。
    kernel/camera/0005-media-i2c-hi846-Report-the-pixel-array-rate-and-the-link-frequency.patch
    kernel/camera/0006-media-i2c-hi846-Keep-the-VBLANK-frame-length-when-setting-the-exposure.patch
    # ★ 0007、0008（原 0209、0210）：换模式时按新模式的高度更新曝光上限（0007后摄ov13b10，0008前摄hi846）。不打的话VBLANK值恰好
    #    不变时（如ov13b10在2104x1560下设76行再切到4208x3120），开始出流之前曝光上限还是旧模式的（1628行，只有新模式
    #    一帧的一半），libcamera配置时读到的就是这个值。
    kernel/camera/0007-media-i2c-ov13b10-Update-the-exposure-limit-on-every-format-change.patch
    kernel/camera/0008-media-i2c-hi846-Update-the-exposure-limit-on-every-format-change.patch

    # ─── 8. kernel/audio：ASoC sc8280xp、LPASS 编解码宏、WSA883x 功放、SoundWire 与 Q6APM ───
    # 0001（原 0015）：扬声器 WSA 增益上限调整。
    kernel/audio/0001-asoc-sc8280xp-retune-wsa-speaker-gain-ceilings.patch
    # ★★ 0002–0006（原 0106–0110）：主线7.3的5个补丁原样移植（cd054a6e272c、eb667d0fbdd3、541735571578、b9b23e72abef、
    #    b05482e7ce1b）。LPASS的四个编解码宏（WSA/VA/TX/RX）改用PM clock框架，空闲时撤掉向ADSP投的
    #    LPASS_HW_MACRO/LPASS_HW_DCODEC票。原来从probe起一直投着，ADSP从不睡，系统睡眠时内存也停在1017MHz；
    #    打上以后ADSP能睡，内存降到200MHz。
    kernel/audio/0002-ASoC-codecs-lpass-wsa-macro-Switch-to-PM-clock-framework-for-runtime-PM.patch
    kernel/audio/0003-ASoC-codecs-lpass-va-macro-Switch-to-PM-clock-framework-for-runtime-PM.patch
    kernel/audio/0004-ASoC-codecs-lpass-wsa-macro-Use-devm_clk_hw_register-for-MCLK-output.patch
    kernel/audio/0005-ASoC-codecs-lpass-tx-macro-switch-to-PM-clock-framework-for-runtime-PM.patch
    kernel/audio/0006-ASoC-codecs-lpass-rx-macro-switch-to-PM-clock-framework-for-runtime-PM.patch
    # ★ 0007–0008（原 0113–0114，配套 DT 见 kernel/dts/0016）：扬声器压缩与总线冲突。0007修qcom.c分不出最后一个端口
    #    （主线6ccec91c3535引入）；0008让wsa883x播放时切到动态DRE（主线一直停在固定增益，压扩器的提升没人抵消）；
    #    kernel/dts/0016（原 0115）给两颗功放加qcom,port-mapping（不加时端口按顺序分配，audio-route.sh的BOOST 0 / VISENSE 1错位），
    #    右VI在8号端口、要0007。⚠️ 与audio-route.sh的数字音量78配套（脚本按DT里有没有映射来判断），要一起发。
    kernel/audio/0007-soundwire-qcom-let-the-last-data-port-be-allocated.patch
    kernel/audio/0008-ASoC-codecs-wsa883x-hand-the-PA-gain-over-to-DRE-after-unmute.patch
    # 0009（原 0166）：VA解码器启动时静音，40毫秒后解除。不打的话每段录音开头都有高通滤波器的阶跃，听起来是一声咔哒。
    kernel/audio/0009-ASoC-codecs-lpass-va-macro-mute-the-decimators-while-they-settle.patch
    # 0010（原 0167）：VA解码器的高通在启动后300毫秒恢复原值（4 Hz）。不打的话录音一直带着150 Hz高通，低频被削掉。
    kernel/audio/0010-ASoC-codecs-lpass-va-macro-restore-the-high-pass-corner-after-the-start.patch
    # ★ 0011、0012（原 0203、0204）：内置麦四路采集（麦克风阵列，device/huawei/gaokun3/micarray/）。0011让VA后端在前端要四路时保留四路，
    #    不打的话四路请求在后端被压回两路；0012让采集按源的顺序标声道，不打的话DSP的MFC把四路重新混过（第三路混进前两路）。
    kernel/audio/0011-ASoC-qcom-sc8280xp-let-VA-capture-use-four-channels.patch
    kernel/audio/0012-ASoC-qcom-q6apm-dai-keep-the-source-s-channel-order-on-capture.patch

    # ─── 9. kernel/input：Himax SPI 触摸屏、HX83121A 级联手写笔从片与 USB HID ───
    # 0001（原 0037，#113）：himax 触摸的坐标 fuzz 做成 0644 模块参数，可运行时改。
    #    默认仍是 8 ⇒ 单独打上【不改变行为】，要的是"调参不用重启"这个能力。
    #    调定之后把值写进 DT 的 touchscreen-fuzz-x/y（标准属性，touchscreen.c:89 会覆盖驱动默认，见 kernel/dts/0007）。
    kernel/input/0001-Input-himax-spi-runtime-tunable-coordinate-fuzz.patch
    # 0002（原 0038，#114）：跳点检测的判据从【原始位移】改成【与预测位置的偏差】。
    #    原判据等于一条 1.0 m/s 的限速线，越过之后 debounce 永远回不到 0 ⇒
    #    手指只要持续快过它，驱动【一个点都不上报】。实测：一次甩动被切成 87 条轨迹。
    #    本补丁不改默认值，只改判据含义。
    kernel/input/0002-Input-himax-spi-jump-detection-vs-predicted-position.patch
    # 0003（原 0039，#114）：Z8 孤立尖峰过滤与边缘单像素豁免做成可调（iso_nbr_ratio_q8 /
    #    edge_min_area），并标注掌压规则 3 是死代码。三项默认值不变 ⇒ 单独打上不改变行为。
    kernel/input/0003-Input-himax-spi-tunable-isolated-spike-and-edge-gates.patch
    # 0004（原 0040，#115）：修好 SPI 读重试（tx/rx 同一块缓冲，重试发的是上次收到的数据 ⇒
    #    三次机会实际只有一次），并让事件栈一次传完（原先每帧切成 5129+3 两次 spi_sync）。
    kernel/input/0004-Input-himax-spi-fix-SPI-read-retry-and-split-event-stack.patch
    # 0005（原 0041，#115）：连通域表满时原先是 return 而不是 break ⇒ 放弃扫描网格【剩下的全部】。
    #    噪声多（充电器耦合）时屏幕下半部分的真实手指整个消失。改成按 signal_sum 顶替最弱的。
    kernel/input/0005-Input-himax-spi-do-not-abandon-the-grid-when-zones-fill.patch
    # 0006（原 0042，#115）：掌压剔除改成【标记】而不是删除 —— BFS 是 8 连通，指尖挨着手掌会进同一个
    #    zone，删掉就把手指一起删了。默认行为不变（标记的 zone 不出峰值），但决定变得可见可逆。
    kernel/input/0006-Input-himax-spi-mark-palm-zones-instead-of-deleting-them.patch
    # 0007（原 0043，#115）：逐级计数器（algo/stats）、最近 16 个触点出生记录（algo/contacts_log）、
    #    debugfs 两个整帧导出（frame_raw = 面板产出的，frame = 流水线判定的）。
    #    不改变任何触摸行为，只是让行为可观测 —— #114 那一晚的推理以后是一次 cat 的事。
    kernel/input/0007-Input-himax-spi-per-stage-counters-and-raw-grid-dumps.patch
    # 0008（原 0044，#115）：上报坐标轴分辨率（此前 resolution=0 = 告诉用户态"不知道"）。10 单位/毫米。
    kernel/input/0008-Input-himax-spi-report-axis-resolution.patch
    # 0009（原 0045，#115）：修好 W=1 报的两处 kerneldoc。纯注释，改完这个驱动 W=1 完全干净。
    kernel/input/0009-Input-himax-spi-fix-two-kerneldoc-blocks.patch
    # 0010（原 0047，#116）：pressure_enabled 默认跟着轴走（轴存在却报常数是逻辑缺陷）；
    #    contacts_log 加 zone_area 列 —— ct->area 分不开手掌碎块和指尖（实测 8-20 vs 5-21），能分的是它。
    kernel/input/0010-Input-himax-spi-pressure-follows-axes-and-log-zone-area.patch
    # ★ 0011（原 0079，手写笔 M-Pencil，#26，配套 DT 见 kernel/dts/0013）：触摸从片（spi20）驱动。
    #    把从片的帧变成触控笔的是 vendor 的 gk3pend（device/huawei/gaokun3/stylus）；驱动要 =y（kernel-config-android.sh）。
    kernel/input/0011-Input-add-HX83121A-cascade-slave-pen-driver.patch
    # ★ 0012（原 0081，#29）：峰值多于10个时原先展开的是最弱的10个（峰值按从弱到强排序）；改成跳过最弱的。
    kernel/input/0012-Input-himax-spi-expand-the-strongest-peaks-when-there-are-too-many.patch
    # ★★ 0013（原 0082，#29）：手的位置图：掌根以外的手（指节、平放的手指、落下和抬起时的掌根）不再报成手指，
    #    事后才认出的触点以MT_TOOL_PALM撤回（Android发ACTION_CANCEL）。参数在algo/hand_*，hand_enabled=0关闭。
    kernel/input/0013-Input-himax-spi-hand-map-and-palm-cancel.patch
    # ★ 0014（原 0116）：触摸跟踪的配对范围默认420²（约44毫米）→ 115²（约12毫米）。手指刚抬起、跟踪还留着的3帧里，
    #    附近落下的手指会被接到旧跟踪上，Android收不到按下（音游吞点击，指纹见scripts/touch/merge-check.py）。
    kernel/input/0014-Input-himax-spi-narrow-the-track-match-radius.patch
    # ★ 0015（原 0118）：边缘补偿的上限从网格外收回到网格内最后一个位置。原来最后一列/行被推到2560/1600，比轴最大值多1
    #    （inverted-x后是-1），坐标回到范围内之前Android收不到这次触摸：贴边的角落点击整个丢失，从这两条边滑入（含返回手势）
    #    晚开始。
    kernel/input/0015-Input-himax-spi-keep-edge-compensated-positions-on-the-panel.patch
    # ★ 0016–0019（原 0119–0122）：触摸驱动优化。0016点击锁定（track_tap_radius）：手指移动超过半径前停在按下的位置，
    #    点击不再因为抬起时中心漂移被当成拖动。0017一欧滤波（track_filter_*）：慢速滑动不抖，快速滑动不拖。
    #    0018与EGoTouchRev-rebuild一致的检测（自适应基线+固件手指标志、峰合并、区域拆分、强触点第一帧上报等），
    #    参数默认保持原行为；0019把实测过的组合设为默认。回放工具scripts/touch/hxsim同步新参数。
    kernel/input/0016-Input-himax-spi-hold-a-touch-s-down-position-until-it-moves.patch
    kernel/input/0017-Input-himax-spi-filter-the-reported-position-by-speed.patch
    kernel/input/0018-Input-himax-spi-optional-EGoTouchRev-rebuild-style-detection.patch
    kernel/input/0019-Input-himax-spi-default-to-the-EGoTouchRev-rebuild-style-detection.patch
    # ★ 0020、0021（原 0142、0143）：触摸的flick和抬手。0020点击锁定改看信号和步长：信号塌下去时的跳动不解锁，一帧走过约1.5毫米就解锁，
    #    解锁时报接触点本身，一欧滤波beta提到30。0021固件说屏幕上没有手指时，丢失的跟踪只留track_fw_lost_frames帧
    #    （默认1）。回放工具scripts/touch/hxsim同步新参数，`-F`从录下的网格还原手指标志。
    kernel/input/0020-Input-himax-spi-release-the-tap-lock-on-signal-and-step.patch
    kernel/input/0021-Input-himax-spi-end-a-touch-early-when-the-firmware-sees-no-finger.patch
    # 0022（原 0169）：恢复时给键盘/笔MCU的HID接口发SET_IDLE，配合去掉cmdline里的USB复位。不打的话睡眠后没有压感。
    kernel/input/0022-HID-usbhid-add-a-quirk-to-send-SET_IDLE-on-resume.patch

    # ─── 10. kernel/dts：SC8280XP 与 gaokun3 设备树 ───
    # 0001（原 0002）：驱动 ts-mode GPIO174 拉低。
    kernel/dts/0001-arm64-dts-gaokun3-drive-ts-mode-gpio174-low.patch
    # 0002（原 0009）：sc8280xp CPU cooling maps。
    kernel/dts/0002-arm64-dts-sc8280xp-add-cpu-cooling-maps.patch
    # ❌ archive/0011（gaokun3.dts 里 &venus { firmware-name; status = "okay"; }）故意【不列】：
    #    archive/upstream-venus/0020 加的是【逐字相同】的块、同一位置，而且两者的上下文在打了对方之后
    #    仍然匹配 ⇒ 依次打会得到【两份】&venus 块（dtc 会合并、DTB 不变，所以谁都没发现）。
    #    2026-09-14 --verify 第一次跑就抓到：构建机树只有一份、重放出来两份。已从本仓移除。
    # 0003（原 0012）：usb0 OTG，供 USB adb / 角色切换使用。
    kernel/dts/0003-arm64-dts-gaokun3-usb0-otg-for-usb-adb.patch
    # 0004（原 0018）：前摄要工作就得去掉后摄 s5k3l6 节点（A/B 实测，#81）。只动 camera.dtsi。
    kernel/dts/0004-arm64-dts-gaokun3-camera-drop-rear-s5k3l6.patch
    # 0005（原 0032，后摄 OV13B10，#106，配套驱动见 kernel/camera/0003）：叠在 0004 之后，把后摄节点按板级电源表接回（@0x36）。
    kernel/dts/0005-arm64-dts-gaokun3-camera-rear-ov13b10-with-board-rails.patch
    # 0006（原 0036，#110）：后摄闪光灯 = PMIC pmc8280c 闪光模块 1+4 路（内核 #19 逐路点亮实测），
    #    并删掉不亮的 GPIO93 gpio-led。只动 camera.dtsi，叠在 0005 之后。
    #    ⚠️ 需要 LEDS_CLASS_FLASH=y LEDS_QCOM_FLASH=y（kernel-config-android.sh 已断言）。
    kernel/dts/0006-arm64-dts-gaokun3-pmic-flash-led-channels-1-4.patch
    # 0007（原 0046，#116）：坐标 fuzz 定案为 0，写进 DT 的标准属性 touchscreen-fuzz-x/y。
    #    实机量的：空载噪声 RMS 29.6 ⇒ 质心抖动 σ 只有 0.16/0.10 输出单位（不到半个单位），
    #    而 fuzz=8 的死区是 ±4 单位（大 8 倍）。慢速拖动 A/B：位移为 0 的帧 13.7% → 1.8%。
    kernel/dts/0007-arm64-dts-gaokun3-touchscreen-fuzz-0.patch
    # ❌ archive/0048（usb_0 用 UTMI 当 pipe 时钟）不再列入：它要绕开的"角色切换后 xhci -110 / gadget -524"，原因是
    #    vreg_l9d 随 combo PHY 断电（0014，原 0086）；UCSI 工作以后它反而让 port0 上的 U 盘根本不枚举（USB3 链路起不来，
    #    又不退回 USB2）。0003（原 0012）里的 maximum-speed = "high-speed" 同理去掉了。已从本仓移除。
    # 0008（原 0049）：后摄 rotation 180 经用户目视确认（2026-09-24），只去掉 FIXME、值不变 ⇒ dtb 与 archive/0048 逐字节同。
    #    rotation 180本身没错（设备树按横屏写），但它注释里的"preview upright, SENSOR_ORIENTATION 180"
    #    已被#23、#42推翻：HAL现在扣掉翻转、换算到竖屏后上报90（见camera/Device.cpp）。
    #    ⚠️ 同号的前身（前 90 / 后 270，按 PR #6 描述）与目视结论相反，已作废 —— 构建机树上若还打着它，
    #    先 `git apply -R` 旧版（正文在 git 历史 cecb9ec 里）再打这一版。
    kernel/dts/0008-arm64-dts-gaokun3-camera-rear-rotation-180-confirmed.patch
    # 0009（原 0053）：sc8280xp.dtsi 加 iris + videocc 节点 + pil_video_mem（上游 v7.3 3a52eef16b97 的 backport）。
    #    ⚠️ 上下文按构建树生成（buildbot upstream/0002 多一行 qcom,scm.h）。树上还打着
    #    archive/upstream-venus 时，fuzz 回落能把它硬打成两份节点 —— 所以下面进循环前先拒绝 venus 残留、打完再断言。
    kernel/dts/0009-arm64-dts-qcom-sc8280xp-add-iris-and-videocc.patch
    # 0010（原 0054）：gaokun3 打开 &iris，firmware-name 指向华为签名的 qcvss8280.mbn。依赖 0009 的 iris label。
    kernel/dts/0010-arm64-dts-gaokun3-enable-iris.patch
    # 0011（原 0071）：【本地】DT 给默认 CMA 加 alloc-ranges（2–4 GiB）。原先 CMA 被放在 0x878000000，32 位 coherent 的设备
    #   （ath11k / MHI）用不上，每次恢复都去 ZONE_DMA 赌 order-7/9 连续块。#131。⚠️ ROM 的 prebuilt-boot dtb 要随之更换。
    kernel/dts/0011-arm64-dts-gaokun3-keep-default-cma-below-4g.patch
    # 0012（原 0074）：【本地】后摄 OV13B10 节点补回 privacy LED（0004 删 s5k3l6 时连带删了）。只动 camera.dtsi。HW-9。
    kernel/dts/0012-arm64-dts-gaokun3-camera-rear-privacy-led.patch
    # ★ 0013（原 0080，手写笔 M-Pencil，#26，配套驱动见 kernel/input/0011）：启用 spi20 与触控笔从片节点。
    kernel/dts/0013-arm64-dts-gaokun3-enable-spi20-and-the-pen-slave.patch
    # ★★ 0014（原 0086）：vreg_l9d常开。原先只有usb_0_qmpphy用它，combo PHY一退出就断电，而port0的USB2、
    #    多口USB控制器和PCIe也靠它：device模式挂起整板复位（#52）、port0切换后坏掉（#27一类）都是它。
    kernel/dts/0014-arm64-dts-gaokun3-vreg-l9d-always-on.patch
    # ★ 0015（原 0095，配套驱动见 kernel/ec-usb/0010）：EC中断作为唤醒源，休眠中插拔充电器
    #    或数据线能唤醒（Android再按Wake on plug亮屏）。
    kernel/dts/0015-arm64-dts-gaokun3-EC-IRQ-as-wakeup-source.patch
    # ★ 0016（原 0115，配套驱动见 kernel/audio/0007–0008）：给两颗功放加 qcom,port-mapping。
    kernel/dts/0016-arm64-dts-gaokun3-map-the-speaker-ports-statically.patch
    # ★ 0017（原 0124，配套驱动见 kernel/drm/0003）：添加面板 ID 引脚（GPIO 102、91）。
    kernel/dts/0017-arm64-dts-gaokun3-add-the-panel-ID-pins.patch
    # 0018（原 0141）：vreg_l4b常开。原先只有usb_1_qmpphy用它，Type-C重配时断电，port1的USB2一侧可能冻住到重启（#53）。
    kernel/dts/0018-arm64-dts-gaokun3-vreg-l4b-always-on.patch
    # 0019（原 0165）：DMIC时钟从4.8 MHz降到3.2 MHz。4.8 MHz下麦克风录到密集的尖脉冲，App用的DMIC0、DMIC1一直有。
    kernel/dts/0019-arm64-dts-gaokun3-lower-the-DMIC-clock-to-3.2-MHz.patch
    # 0020（原 0189）：VCC_WLAN_3R9常开。不打的话pcie4断链时这路被关掉，WoW的固件却还在运行。Windows在D3时也不关这路。
    kernel/dts/0020-arm64-dts-qcom-sc8280xp-huawei-gaokun3-keep-VCC_WLAN_3R9-on.patch
    # 0021（原 0200，配套驱动见 kernel/pcie-pm/0011）：wake-gpios从pcie4搬到wifi@0，PCI核心只在设备自己的节点上找它。
    #    不打的话 kernel/pcie-pm/0011 找不到这根线。只能搬，不能两处都写：同一根GPIO引用两次会被当成共享GPIO。
    kernel/dts/0021-arm64-dts-qcom-sc8280xp-huawei-gaokun3-move-wake-gpios-to-the-Wi-Fi-node.patch
    # ★ 0022（原 0205）：CDSP的FastRPC上下文（compute-cb）标成dma-coherent，录音降噪服务（DeepFilterNet3在NPU上）要用。
    #    不打的话用户态FastRPC在CDSP上加载skel几乎每次都失败（AEE_EUNABLETOLOAD，与时序有关），降噪服务起不来。
    kernel/dts/0022-arm64-dts-qcom-sc8280xp-mark-the-CDSP-FastRPC-context-banks-dma-coherent.patch

    # ── 指纹传输驱动（QSEECOM + /dev/gaokun3_fpcmd，CONFIG_GAOKUN3_FPCMD 默认 n） ──
    #   0001（原 0050）：QSEECOM APP_START/SHUTDOWN + listener，把 TZ 内存约束到 32 位。
    #   0002（原 0212）：固定 ioctl 指纹传输驱动（/dev/gaokun3_fpcmd，FocalTech FTE7001）。
    #   0003（原 0213）：Goodix GDIX5125 传感器分支。
    kernel/fingerprint/0001-firmware-qcom-scm-qseecom-app-load-shutdown-listener.patch
    kernel/fingerprint/0002-firmware-qcom-gaokun3-fingerprint-ioctl.patch
    kernel/fingerprint/0003-firmware-qcom-gaokun3-fpcmd-add-the-Goodix-GDIX5125-sensor.patch
)

# The first version of kernel/core/0008 (formerly 0085) overwrote init_uts_ns in init/version.c. No patch in the table touches
# that file any more, so --verify would not compare it: a tree still carrying it would report the
# fake release to netd and the BPF loader, which no longer have ro.bpf.kver_override to correct it.
if grep -q spoof_uts_release "$TREE/init/version.c"; then
    echo "✗ 旧版 0085 还在树里（init/version.c 的 spoof_uts_release）。撤掉：git apply -R 旧补丁（git show 54eb84f:patches/0085-init-version-spoof-uts-release-for-integrity-checks.patch）" >&2
    exit 1
fi

# ★★ 指纹判据：补丁是否【已在树里】。
#
# ⚠️ 为什么不能只靠 `git apply -R --check`：**用 fuzz 打进去的补丁，反向检查
#    一定失败**（上下文已经和补丁里的不一致了）。于是脚本会认为"没打过"，
#    再用 fuzz 打一遍 —— 结果就是【同一段代码出现两份】。
#    本仓已经因此中招两次：venus 的 sc8280xp_freq_table / sc8280xp_res 被打成
#    两份，of_match 里 sc8280xp-venus 出现三条。**构建不一定报错**，
#    重复的 static 结构体只是浪费空间，重复的 of_match 条目只是第一条生效，
#    所以症状极其隐蔽。
#
# 判据：从补丁里挑【最长的几条】新增行，到它要改的文件里 grep，要求【全部命中】。
#
# ⚠️★★ 2026-09-11：这里原来是"挑第一条长度 > 25 的新增行"，**制造过一次真实事故**。
#    `patches/kernel/dts/0002`（原 `0009`，CPU cooling maps）的第一条合格新增行是
#        polling-delay-passive = <250>;
#    而这行在【未打补丁的】sc8280xp.dtsi 里本来就有一次（主线自带的 gpu-thermal 区）
#    ⇒ grep 命中 ⇒ 判成"已应用" ⇒ 静默跳过 ⇒ **重建的内核悄悄失去 CPU 温控降频**。
#    这正是 M17 写这个脚本要防的那件事，结果被脚本自己的判据复现了。
#    实测：新树里 cooling-maps 只有 1 处（主线的），旧树 9 处。
#
# ★ 两处加固，都便宜：
#    ① 探针改挑【最长】的新增行 —— 越长越不可能与既有代码撞车
#       （kernel/dts/0002 的最长行是 `cooling-device = <&cpu0 THERMAL_NO_LIMIT ...>` 64 字符，
#        在未打补丁的文件里根本不存在）。
#    ② 取最多 3 条并要求【全部】命中 —— 单条撞车是偶然，三条同时撞车基本不可能。
#
# ⚠️ 为什么不怕假阴性：fuzz 影响的是【上下文】匹配，新增行本身一定是逐字写入的。
#    万一仍误判成"没打过"，后面 `git apply --check` 会失败并走 fuzz，
#    fuzz 再失败就【大声报错】—— 方向是对的：宁可吵，也不要静默跳过。
#    ③ ⚠️★ 2026-09-13 又补一条：**把"只是被搬家"的行从探针里剔掉。**
#       早期 `0022` 把 `gdsc_pm_subdomain_remove()` 与 `of_genpd_del_provider()`
#       两行调了个个儿，于是它们同时出现在 `-` 和 `+` 两侧 ——
#       而 `+` 侧的行**在未打补丁的文件里本来就在**，三条探针会全部命中，
#       整个补丁被静默跳过。这与 kernel/dts/0002 那次是**同一类事故**（探针撞上既有代码），
#       只是来源从"巧合"变成了"必然"。判据：凡是也出现在删除行里的，不作数。
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

# ★★ venus 时代的树不能直接往上打 iris（#128）。
#   archive/upstream-venus/0019 当年是 fuzz 打进去的，kernel/dts/0009（原 0053）在那样的树上 git apply 会失败 ——
#   可下面的回落逻辑会改用 patch --fuzz=3 把它【硬打进去】，得到两份 videocc / pil_video_mem、
#   两个 video-codec@aa00000，脚本还照样报成功（审查 2026-09-28 实测推出来的，不是假想）。
#   所以在进循环之前就拦下来。0017/0018 还在的话 IRIS=y 也根本编不过。
DTSI=arch/arm64/boot/dts/qcom/sc8280xp.dtsi
GK3=arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dts
if grep -q 'compatible = "qcom,sm8350-venus"' "$DTSI" 2>/dev/null \
   || grep -q '^&venus {' "$GK3" 2>/dev/null \
   || grep -q 'sc8280xp_res\|sm8350_res' drivers/media/platform/qcom/venus/core.c 2>/dev/null; then
    echo "✗ 这棵树上还打着 早期 upstream-venus（venus 时代的节点 / 资源结构体）。" >&2
    echo "  从 2026-09-28 起视频编解码走 iris（kernel/dts/0009–0010、kernel/iris/），两者不能叠。做法（二选一）：" >&2
    echo "  ① 推荐：git -C $TREE worktree add --detach <新路径> HEAD，在新 worktree 上跑本脚本（再接 KernelSU）；" >&2
    echo "  ② 就地撤：倒序撤 archive/upstream-venus 的 0020 0019 0018 0017 0016 0015 0013 ——" >&2
    echo "     0017/0018/0019 当年用了 fuzz，要 patch -R -p1 --fuzz=3；其余 git apply -R。别用 git checkout。" >&2
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
    #   ⚠️★★ 这一步必须排在指纹判据【前面】（2026-09-13 踩的坑）：patches/kernel/camera/0002（原 0031）的新增行是
    #   `.ops = &clk_rcg2_shared_ops,`，而文件里别处早有 21 行一模一样 ⇒ 指纹"命中"、
    #   静默跳过，于是那一轮构建出来的内核根本没带修复，而脚本输出看起来完全正常。
    #   ★ 指纹只能回答"这些行在不在文件里"，回答不了"是不是【这个补丁】放进去的"；
    #   正向 --check 成功则是确定性的"没打过"。指纹判据只留给"正向打不上"之后的分流。
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
        # ★ 回落到模糊匹配。当年 archive/upstream-venus/0019 就需要这个（上下文里的 #include 列表与
        #   v7.2-rc2 差一行 qcom,scm.h；它已随 #128 退役，现役列表零 fuzz）。⚠️ 用了 fuzz 必须【明说】，
        #   静默的模糊匹配是灾难的开始 —— kernel/dts/0009（原 0053）在 venus 旧树上就会被它硬打成两份节点，所以上面才有那道拦截。
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

# ★ 打完之后断言 iris 的 DT 状态（#128）。指纹判据和 fuzz 回落都可能让 kernel/dts/0009–0010（原 0053/0054）"看起来打上了"
#   而实际没有 —— kernel/dts/0010 唯一的指纹行 firmware-name = "...qcvss8280.mbn" 在旧的 &venus 块里一字不差。
#   这里直接看结果：恰好一个 videocc、一个 iris 节点、gaokun3 里 &iris 打开了、没有任何 venus 节点。
if [ "$MODE" != "--check" ]; then
    n_vcc=$(grep -c 'videocc: clock-controller@abf0000' "$DTSI")
    n_iris=$(grep -c 'iris: video-codec@aa00000' "$DTSI")
    if [ "$n_vcc" != 1 ] || [ "$n_iris" != 1 ] || grep -q 'venus: video-codec' "$DTSI" \
       || ! grep -A2 '^&iris {' "$GK3" | grep -q 'status = "okay"'; then
        echo "✗ iris 的 DT 状态不对：videocc 节点 ${n_vcc} 个、iris 节点 ${n_iris} 个（都应为 1），" >&2
        echo "  且 gaokun3 里要有 &iris { ... status = \"okay\"; }、sc8280xp.dtsi 里不能有 venus 节点。见 #128。" >&2
        exit 1
    fi
    echo "✓ iris DT 状态：1 个 videocc、1 个 iris 节点，gaokun3 已打开 &iris"
fi
