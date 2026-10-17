#!/usr/bin/env bash
# 把本仓 patches/ 里【内核那一半】应用到一棵内核树。
#
# ★ 为什么需要这个脚本：patches/*.patch 长期以来【没有任何消费者】——
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
#    0020 / 0022 / 0055 / 0057 已被 stable 收入、移出本表；0056 rebased；新增 0075（撤回 stable 带进来的 0ac05c4）。
#    ⚠️ 本表【不再适用于 v7.2-rc2 的树】（~/gk3-kernel-iris，#16 及以前）—— 那棵树的配方在本仓 c1062f2。
#    在旧树上跑 --verify 会报 0056 / 0075 打不上，这是预期的。
#
# ⚠️★ --check 在【已经打满】的树上有一个已知盲区（#110）：叠加补丁里靠前的那个
#    （如 0018 删掉后摄节点、0032 又在同一位置写回）反向 --check 对不上，会被报成"打不上"，
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

# ❌ 上游 Venus 补丁集（patches/upstream-venus/0013–0020）从 2026-09-28 起【不再列入】：
#   视频编解码从 qcom-venus 换成了 qcom-iris（案卷 #128，补丁 0053–0060）。
#   0017/0018 在 VIDEO_QCOM_IRIS=y 时根本编不过（venus/core.c 的 #if !IS_ENABLED(IRIS) 把它们
#   引用的表编掉了），0019 的节点 iris 不认。文件留在原处作案卷，README 里写了为什么换。
#   ⚠️ 这里原来是一个 UPATCHES 数组。别留一个空数组：本机（macOS）的 bash 3.2 在 set -u 下
#      展开空数组 "${A[@]}" 会报 unbound variable（与提交 5013189 同一类）。

# ⚠️ 只列内核补丁。其余的归属别处：0003 AOSP glslang、0004/0005/0006 mesa、
#    0008 tinyalsa、0010 / 0051 / 0052 AOSP audio HAL —— 别往内核树上打。
KPATCHES=(
    0001-efi-pstore-register-backend-when-efivars-ops-arrive-.patch
    0002-arm64-dts-gaokun3-drive-ts-mode-gpio174-low.patch
    0007-bpf-inode-label-bpffs-lazily-for-android-genfscon.patch
    0009-arm64-dts-sc8280xp-add-cpu-cooling-maps.patch
    # ❌ 0011（gaokun3.dts 里 &venus { firmware-name; status = "okay"; }）故意【不列】：
    #    upstream-venus/0020 加的是【逐字相同】的块、同一位置，而且两者的上下文在打了对方之后
    #    仍然匹配 ⇒ 依次打会得到【两份】&venus 块（dtc 会合并、DTB 不变，所以谁都没发现）。
    #    2026-09-14 --verify 第一次跑就抓到：构建机树只有一份、重放出来两份。文件留作案卷。
    0012-arm64-dts-gaokun3-usb0-otg-for-usb-adb.patch
    0013-drm-crtc-drop-racy-BUG_ON-in-fence_to_crtc.patch
    0014-remoteproc-qcom-ratelimit-repeat-handover-error.patch
    0015-asoc-sc8280xp-retune-wsa-speaker-gain-ceilings.patch
    # ★★ 0016/0017 是 Android 起不来的硬前提（ashmem / xt_quota2，ACK 专有）。
    #    它们此前只活在构建机的内核树里，2026-09-08 照本仓配方重建的内核
    #    因此黑屏起不来 —— 见 docs/stage4-findings.md #79。别删。
    0016-staging-android-port-ashmem-from-ack.patch
    0017-netfilter-port-xt-quota2-from-ack.patch
    # 0018：前摄要工作就得去掉后摄节点（A/B 实测，#81）。只动 camera.dtsi。
    0018-arm64-dts-gaokun3-camera-drop-rear-s5k3l6.patch
    # ✅ 0020（上游 499b4cb6710f，camcc 不再注册 CAMCC_GDSC_CLK）**已被 v7.2.y stable 收入**
    #    （stable b425cc3913fb），基线追到 v7.2.9 起【不再列入】。文件留作案卷（v7.2-rc2 的配方见 c1062f2）。
    # ❌ 0021（给 titan_top 加 NoC 投票）**已被实测否掉**（内核 #6，见 #102），
    #    故意【不列】在这里。文件仍留在 patches/ 下，头部有醒目的"已否"横幅。
    # ✅ 0022（上游 86b23609d5e1，gdsc_unregister 拆 genpd）**已被 v7.2.y stable 收入**（stable 40bd77fa2857），
    #    v7.2.9 起【不再列入】。⚠️ 同批 stable 还带进 gdsc.c 的两处行为变化（fade2037ee2a：ALWAYS_ON 域
    #    gdsc_enable 失败要上报；02533b6cf5f6：poll_status 透传 check_status 的错误）—— 相机 titan 域要回归。
    #    下面的诊断补丁 0023/0028/0029 是在 v7.2-rc2 + 0022 上写的，在 v7.2.9 上【没试过】能不能打。
    # ⚠️ 0023 是【诊断补丁，不要进发版内核】：每次 titan 域翻转打两行寄存器转储。
    #    它存在的意义是把"读 GDSCR"从 /dev/mem（本机会静默死内核）换成 regmap。
    # ❌ 0024 / 0025 / 0026 三条试验补丁**已被内核 #10 实测否掉**（#104：上电后重试、
    #    塌缩前复位 CAMNOC+CPAS、塌缩前复位全部 21 个 BCR，都救不回来），故意【不列】。
    #    文件留在 patches/ 下作案卷。它们都依赖 0023，若要复现顺序不能反。
    # ★ 0027 是候选根因修复（#105）：sc8280xp 的 camcc 一个 GDSC 都没给等待值，
    #    gdsc_init() 就用 MSM8974 的 2/8/2 覆盖硬件复位值；同代同偏移的 sm8150/sc8180x
    #    都写 2/2/0xf。它改的是 camcc-sc8280xp.c 的 gdsc 结构体，与 0020 的 hunk 不相交。
    0027-clk-qcom-camcc-sc8280xp-gdsc-wait-vals.patch
    # ⚠️ 0028 是【诊断补丁，不要进发版内核】，**依赖 0023**（用它的 gdsc_is_watched）。
    #    debugfs gdsc-dbg/init_raw 读硬件复位值（核实 0027 的依据）、wait_override 运行时改等待值。
    # ❌ 0027 已被内核 #11 实测否掉（#105）：硬件复位值确实是 2/2/0xf（0028 读出），
    #    但改回去之后脏塌缩签名与上电冻死一字不差。留在列表里是因为它**就是正确的硬件值**
    #    （上游同代驱动全这么写），只是不是这个缺陷的根因。
    # ⚠️ 0029 是【诊断补丁，不要进发版内核】，依赖 0023/0028：裸翻转 GDSC、屏蔽 RETAIN_FF、塌缩前延时。
    # ⚠️ 0030 是【诊断补丁，不要进发版内核】：camss 按块跳过 s_power/s_stream（module 参数 dbg_skip）。
    # ★ 0031 候选根因修复（#105）：camnoc_axi / slow_ahb / fast_ahb 三个 RCG 标成 shared，
    #    关闭时停靠 XO。实测一次出流后 camnoc_axi_clk_src 指着已熄灭的 pll0_out_even。
    #    改的是 camcc-sc8280xp.c 里三个 .ops 行，与 0020/0027 的 hunk 不相交。
    0031-clk-qcom-camcc-sc8280xp-mark-camnoc-ahb-rcgs-shared.patch
    # ★ 后摄 OV13B10（#106）：0032 叠在 0018 之后，把后摄节点按板级电源表接回（@0x36）；
    #    0034 给上游 ov13b10 加 OF 匹配表与板级上电序列。三关（probe / 出帧 / HAL 枚举）都过了才进来。
    #    ⚠️ 需要 .config 里 VIDEO_OV13B10=y 与 VIDEO_DW9714=y（kernel-config-android.sh 已断言）。
    #    0033（s5k3l6xx 驱动）故意【不列】：本机后摄不是 S5K3L6，文件留作案卷。
    0032-arm64-dts-gaokun3-camera-rear-ov13b10-with-board-rails.patch
    0034-media-i2c-ov13b10-of-match-and-gaokun3-power-sequence.patch
    # ★ 0035（#108）：camss 等不到某颗传感器时（同板另一种后摄模组），超时后只带绑上的传感器
    #    完成 notifier —— 于是"后摄模组不对"只丢后摄，前摄照常。内核 #19 两路实测：
    #    正常 47 个 subdev；ov13b10.fail_probe=1 时 20 s 后回落、45 个 subdev、前摄出帧。
    0035-media-camss-continue-without-never-bound-sensors.patch
    # ★ 0036（#110）：后摄闪光灯 = PMIC pmc8280c 闪光模块 1+4 路（内核 #19 逐路点亮实测），
    #    并删掉不亮的 GPIO93 gpio-led。只动 camera.dtsi，叠在 0032 之后。
    #    ⚠️ 需要 LEDS_CLASS_FLASH=y LEDS_QCOM_FLASH=y（kernel-config-android.sh 已断言）。
    0036-arm64-dts-gaokun3-pmic-flash-led-channels-1-4.patch
    # ★ 0037（#113）：himax 触摸的坐标 fuzz 做成 0644 模块参数，可运行时改。
    #    默认仍是 8 ⇒ 单独打上【不改变行为】，要的是"调参不用重启"这个能力。
    #    调定之后把值写进 DT 的 touchscreen-fuzz-x/y（标准属性，touchscreen.c:89 会覆盖驱动默认）。
    0037-Input-himax-spi-runtime-tunable-coordinate-fuzz.patch
    # ★★ 0038（#114）：跳点检测的判据从【原始位移】改成【与预测位置的偏差】。
    #    原判据等于一条 1.0 m/s 的限速线，越过之后 debounce 永远回不到 0 ⇒
    #    手指只要持续快过它，驱动【一个点都不上报】。实测：一次甩动被切成 87 条轨迹。
    #    本补丁不改默认值，只改判据含义。
    0038-Input-himax-spi-jump-detection-vs-predicted-position.patch
    # ★ 0039（#114）：Z8 孤立尖峰过滤与边缘单像素豁免做成可调（iso_nbr_ratio_q8 /
    #    edge_min_area），并标注掌压规则 3 是死代码。三项默认值不变 ⇒ 单独打上不改变行为。
    0039-Input-himax-spi-tunable-isolated-spike-and-edge-gates.patch
    # ★ 0040（#115）：修好 SPI 读重试（tx/rx 同一块缓冲，重试发的是上次收到的数据 ⇒
    #    三次机会实际只有一次），并让事件栈一次传完（原先每帧切成 5129+3 两次 spi_sync）。
    0040-Input-himax-spi-fix-SPI-read-retry-and-split-event-stack.patch
    # ★★ 0041（#115）：连通域表满时原先是 return 而不是 break ⇒ 放弃扫描网格【剩下的全部】。
    #    噪声多（充电器耦合）时屏幕下半部分的真实手指整个消失。改成按 signal_sum 顶替最弱的。
    0041-Input-himax-spi-do-not-abandon-the-grid-when-zones-fill.patch
    # ★ 0042（#115）：掌压剔除改成【标记】而不是删除 —— BFS 是 8 连通，指尖挨着手掌会进同一个
    #    zone，删掉就把手指一起删了。默认行为不变（标记的 zone 不出峰值），但决定变得可见可逆。
    0042-Input-himax-spi-mark-palm-zones-instead-of-deleting-them.patch
    # ★★★ 0043（#115）：逐级计数器（algo/stats）、最近 16 个触点出生记录（algo/contacts_log）、
    #    debugfs 两个整帧导出（frame_raw = 面板产出的，frame = 流水线判定的）。
    #    不改变任何触摸行为，只是让行为可观测 —— #114 那一晚的推理以后是一次 cat 的事。
    0043-Input-himax-spi-per-stage-counters-and-raw-grid-dumps.patch
    # ★ 0044（#115）：上报坐标轴分辨率（此前 resolution=0 = 告诉用户态"不知道"）。10 单位/毫米。
    0044-Input-himax-spi-report-axis-resolution.patch
    # ★ 0045（#115）：修好 W=1 报的两处 kerneldoc。纯注释，改完这个驱动 W=1 完全干净。
    0045-Input-himax-spi-fix-two-kerneldoc-blocks.patch
    # ★★ 0046（#116）：坐标 fuzz 定案为 0，写进 DT 的标准属性 touchscreen-fuzz-x/y。
    #    实机量的：空载噪声 RMS 29.6 ⇒ 质心抖动 σ 只有 0.16/0.10 输出单位（不到半个单位），
    #    而 fuzz=8 的死区是 ±4 单位（大 8 倍）。慢速拖动 A/B：位移为 0 的帧 13.7% → 1.8%。
    0046-arm64-dts-gaokun3-touchscreen-fuzz-0.patch
    # ★ 0047（#116）：pressure_enabled 默认跟着轴走（轴存在却报常数是逻辑缺陷）；
    #    contacts_log 加 zone_area 列 —— ct->area 分不开手掌碎块和指尖（实测 8-20 vs 5-21），能分的是它。
    0047-Input-himax-spi-pressure-follows-axes-and-log-zone-area.patch
    # ❌ 0048（usb_0 用 UTMI 当 pipe 时钟）不再列入：它要绕开的"角色切换后 xhci -110 / gadget -524"，根因是
    #    vreg_l9d 随 combo PHY 断电（0086）；UCSI 工作以后它反而让 port0 上的 U 盘根本不枚举（USB3 链路起不来，
    #    又不退回 USB2）。0012 里的 maximum-speed = "high-speed" 同理去掉了。文件留作案卷。
    # ★ 0049：后摄 rotation 180 经用户目视确认（2026-09-24），只去掉 FIXME、值不变 ⇒ dtb 与 0048 逐字节同。
    #    ⚠️ 同号的前身（前 90 / 后 270，按 PR #6 描述）与目视结论相反，已作废 —— 构建机树上若还打着它，
    #    先 `git apply -R` 旧版（正文在 git 历史 cecb9ec 里）再打这一版。
    0049-arm64-dts-gaokun3-camera-rear-rotation-180-confirmed.patch
    # ⚠️ 0050（#124，指纹）已移到下面的 FP_PATCHES（v1.0 D21：只进指纹实验内核，--with-fp 才打）。
    # ★★ 0053–0065（#128；0058、0062 不列）：视频编解码 qcom-venus → qcom-iris。iris 靠 DT 的回落 compatible
    #    "qcom,sc8280xp-iris", "qcom,sm8250-venus" 直接用 v7.2-rc2 自带的 sm8250_data，驱动不用加平台。
    #    ⚠️ 需要 .config 里 VIDEO_QCOM_IRIS=y、VIDEO_QCOM_VENUS 不设（kernel-config-android.sh 已断言）。
    # 0053：sc8280xp.dtsi 加 iris + videocc 节点 + pil_video_mem（上游 v7.3 3a52eef16b97 的 backport）。
    #    ⚠️ 上下文按构建树生成（buildbot upstream/0002 多一行 qcom,scm.h）。树上还打着
    #    upstream-venus 时，fuzz 回落能把它硬打成两份节点 —— 所以下面进循环前先拒绝 venus 残留、打完再断言。
    0053-arm64-dts-qcom-sc8280xp-add-iris-and-videocc.patch
    # 0054：gaokun3 打开 &iris，firmware-name 指向华为签名的 qcvss8280.mbn。依赖 0053 的 iris label。
    0054-arm64-dts-gaokun3-enable-iris.patch
    # ✅ 0055 / 0057（上游 v7.3 的 iris 修复 f87d7ed / 75d7987）**已被 v7.2.y stable 收入**
    #    （stable 7a52e76b6309 / cb83cfff8dff），v7.2.9 起【不再列入】。文件留作案卷。
    # 0056：【本地】代替上游 b9c2215。b9c2215 在持 core->lock 断电时 disable_irq() 等中断线程，
    #    而线程开头就要这把锁 ⇒ 死锁；原版 nosync 又会让线程在断电后碰寄存器（本机 = 静默死机）。
    #    这里保留 nosync，线程拿到锁后看 core->hw_powered，断电了就不碰硬件。
    #    ⚠️ v7.2.y stable 收了 b9c2215 ⇒ 0056 rebased onto v7.2.9，多一行把 disable_irq() 改回 nosync。
    0056-media-iris-irq-thread-skips-hw-access-after-power-off.patch
    # ❌ 0058（上游 0ac05c4）故意【不列】：在 gen1 上 LOAD_RESOURCES 位一直置着，v7.2-rc2 原版的
    #    `!= DRAIN` 才让"drain 中途 seek 后的 STOP"照常放行；0058 反而把它变成 -EBUSY。见 0058 头部横幅、#128 §3。
    #    ⚠️ 可 v7.2.y stable 把它收进来了（a5e341a25de3）⇒ 下面 0075 把这一行撤回 rc2 的写法。
    # 0059：上游 v7.4 队列的 cff20ea4 —— UC_REGION 配置被拒时如实报错，而不是"启动成功"后挂死。
    0059-media-iris-fail-firmware-boot-on-invalid-uc_region.patch
    # 0060：【本地】解码器在第一次 SOURCE_CHANGE 前拒绝 CAPTURE G_FMT，让 v4l2_codec2 走 venus 时代
    #    验过的那条路。0644 模块参数 qcom_iris.venus_compat_gfmt（默认 Y），写 N 即上游行为。
    0060-media-iris-venus-compatible-decoder-capture-g_fmt.patch
    # 0061：上游 v7.4 队列的 e2e2bc05 —— 遍历实例链表时拿 core->lock（Codec2 探能力时的 open/close 会撞上）。
    0061-media-iris-take-core-lock-when-scanning-the-instance-list.patch
    # ❌ 0062（放宽 PC_READY 轮询窗口）故意【不列】：前提被 iris-k5 推翻 —— 健康的固件 PC_PREP 之后 1–3 us 就就绪，
    #    "断电被跳过"是固件已经卡死的后果（根因见 0065）。留着只会让卡死时持 core->lock 从 2.5 ms 变成 150 ms。#128 §9。
    # 0065：【本地】解码中 CAPTURE streamoff 除分辨率切换外一律发 HFI_FLUSH_ALL（与 venus 同）。上游发的
    #    HFI_FLUSH_OUTPUT 让华为固件卡死（k5 A/B：8/11 vs 0/15），之后断电永远跳过、硬解全挂到重启（#128 §9）。
    0065-media-iris-flush-all-on-capture-streamoff-except-drc.patch
    # 0066：【本地】gen1 解码器的 seek 学 venus：只 flush、会话不停（0644 qcom_iris.seek_mode，默认 1；0 = 上游）。
    #    上游 seek 是同一会话 STOP 后重新 START，华为固件断言（video_decoder_utils.c:3056，#128 §12–§13）。
    0066-media-iris-gen1-decoder-seek-mode-switch.patch
    # 0067：【本地】drain 之后不 streamoff 也能接着解：EOS 时不自动 FLUSH_OUTPUT（与 venus 同），START 时重置 LAST 状态。
    #    上游 ⇒ 播到结尾后的第一次 seek 没有输出、每隔一次 drain 收不到 EOS（#128 §15）。0644 qcom_iris.eos_flush，默认 N。
    0067-media-iris-no-output-flush-on-eos.patch
    # 0075：【本地，v7.2.9 起】撤回 stable 收进来的 0ac05c4（即被否的 0058），iris_allow_cmd 回到 rc2 的 `!= DRAIN`。
    0075-media-iris-restore-rc2-stop-check-reverting-0ac05c4.patch
    # 0070：【本地】MHI 给 ath11k 保留固件 DMA 表跨断电复用 —— issue #16 待机睡死的根治
    #   （恢复时不再赌 order-7 GFP_DMA 分配）。⬜ 实机 s2idle 循环待验（docs/TODO.md 的 issue 一节）。
    0070-bus-mhi-keep-firmware-images-across-power-cycles-for-ath11k.patch
    # 0071：【本地】DT 给默认 CMA 加 alloc-ranges（2–4 GiB）。原先 CMA 被放在 0x878000000，32 位 coherent 的设备
    #   （ath11k / MHI）用不上，每次恢复都去 ZONE_DMA 赌 order-7/9 连续块。#131。⚠️ ROM 的 prebuilt-boot dtb 要随之更换。
    0071-arm64-dts-gaokun3-keep-default-cma-below-4g.patch
    # 0072：【本地】电池驱动 0x82 按位解码（0x00 / 组合值不再沿用旧 status）、加 CAPACITY_LEVEL
    #   （0% 且没在净充电、或 EC 危急位且 <= critical_max_capacity ⇒ Critical —— Android 有了它就只看它关机）、
    #   只读 ec_raw 给实测用。1.0 计划 BATT-1/2/3。⬜ 带负载放电实测（current_now 正负号、危急位阈值）。
    0072-power-supply-gaokun-battery-decode-status-bits-and-capacity-level.patch
    # 0073：【本地】EC probe 时读一次盖子状态并补报 SW_LID（原来只在事件里报，合盖开机会被当成开着）。DISP-1。
    0073-platform-arm64-gaokun-ec-report-initial-lid-state.patch
    # 0074：【本地】后摄 OV13B10 节点补回 privacy LED（0018 删 s5k3l6 时连带删了）。只动 camera.dtsi。HW-9。
    0074-arm64-dts-gaokun3-camera-rear-privacy-led.patch
    # 0076：【本地，v7.2.9 起】撤回 stable 的 Revert "drm/msm: dsi: fix PLL init in bonded mode"（5de981b7db）——
    #    没有它本机双 DSI 绑定面板黑屏（触摸中断 0/s）；有它 118/s 有画面。2026-10-05 二分定案。
    0076-drm-msm-dsi-phy-7nm-reapply-bonded-pll-init-reverting-5de981b7db.patch
    # 0078：【本地】页表仍 48 位，64 位进程默认地址窗口压到 2^39（Kconfig ARM64_USER_MMAP_WINDOW_39，
    #    kernel-config-android.sh 打开并断言）。issue #17：英雄联盟手游在 48 位窗口下 il2cpp_init 必崩，
    #    2026-10-08 A/B 实测；用户同日定进发版（起初是 --with-va39w 的实验补丁）。只碰 arch/arm64 的 Kconfig 与 processor.h。
    0078-arm64-user-mmap-window-39-bits-on-48-bit-page-tables.patch
    # ★ 手写笔（M-Pencil，#26）：触摸从片（spi20）驱动与设备树。把从片的帧变成触控笔的是
    #   vendor 的 gk3pend（device/huawei/gaokun3/stylus）；驱动要 =y（kernel-config-android.sh）。
    0079-Input-add-HX83121A-cascade-slave-pen-driver.patch
    0080-arm64-dts-gaokun3-enable-spi20-and-the-pen-slave.patch
    # ★ 0081（#29）：峰值多于10个时原先展开的是最弱的10个（峰值按从弱到强排序）；改成跳过最弱的。
    0081-Input-himax-spi-expand-the-strongest-peaks-when-there-are-too-many.patch
    # ★★ 0082（#29）：手的位置图：掌根以外的手（指节、平放的手指、落下和抬起时的掌根）不再报成手指，
    #    事后才认出的触点以MT_TOOL_PALM撤回（Android发ACTION_CANCEL）。参数在algo/hand_*，hand_enabled=0关闭。
    0082-Input-himax-spi-hand-map-and-palm-cancel.patch
    # ★ 0084（#game-env）：netlink RTM_GETLINK/GETNEIGH 隐私边界 —— AOSP 策略有 readpriv/getneigh
    #    细分权限，主线 nlmsgtab 把它们全映射到 NLMSG_READ 导致检查形同虚设。钉死 remap。
    0084-selinux-netlink-route-remap-getlink-getneigh-priv-perms.patch
    # ★ 0085（#game-env）：UTS_RELEASE 运行时覆写 —— 不动 Makefile 版本号（会串 LINUX_VERSION_CODE），
    #    init 阶段把 uname 看到的版本改成 6.6.30-android15-8（版本串伪装，过 Duck KernelCheck）。
    #    UTS_RELEASE 宏不变 ⇒ vermagic 不变（vermagic.h:41-42、module/main.c:1105/2658）。用户 2026-10-11 定进发版。
    0085-init-version-spoof-uts-release-for-integrity-checks.patch
    # 原 0086（KernelSU 子目录）已改名 patches/resukisu/0003，由 kernel-setup-resukisu.sh 打、release.sh 核对。
    # ★★ 0086：vreg_l9d常开。原先只有usb_0_qmpphy用它，combo PHY一退出就断电，而port0的USB2、
    #    多口USB控制器和PCIe也靠它：device模式挂起整板复位（#52）、port0切换后坏掉（#27一类）都是它。
    0086-arm64-dts-gaokun3-vreg-l9d-always-on.patch
    # ★ 0087–0092：UCSI核心和ucsi_huawei_gaokun的六个修复，准备推主线（英文说明，Fixes指向主线提交）。
    #    0087：0c8ee850572b把"对端不支持USB数据⇒角色NONE"写反了，EC从不置这个标志，每次插拔都被切成host；
    #    0088：num_ports在probe时就设，否则插头方向报不出去；0089：离开DP模式时报HPD断开，否则另一个口的DP
    #    分不到资源；0090：不在EC中断线程里等USB事件（原来每次等满2秒）；0091：先按EC的USB事件设插头方向
    #    再切DP，否则翻面插显示器偶尔没画面；0092：EC丢掉的GET_CONNECTOR_STATUS重发，否则一直"忙"时两个口
    #    的插拔都没人处理。
    0087-usb-typec-ucsi-restore-the-USB-data-check-on-partner-change.patch
    0088-usb-typec-ucsi-huawei_gaokun-set-the-number-of-ports-at-probe.patch
    0089-usb-typec-ucsi-huawei_gaokun-report-HPD-when-leaving-DP-alt-mode.patch
    0090-usb-typec-ucsi-huawei_gaokun-dont-block-the-EC-notifier.patch
    0091-usb-typec-ucsi-huawei_gaokun-set-the-orientation-before-DP.patch
    0092-usb-typec-ucsi-huawei_gaokun-resend-dropped-GET_CONNECTOR_STATUS.patch
    # ★ 0093：UCSI不再设置usb_0的角色（update_connector里放掉它查到的role switch）。EC对什么都报对端类型
    #    UFP、从不置USB数据标志，UCSI定的角色没有依据；角色由USB端口HAL（device/huawei/gaokun3/usb）按电气事实决定。
    0093-usb-typec-ucsi-huawei_gaokun-leave-the-usb_0-role-to-userspace.patch
    # ★ 0094：EC挂起期间关掉中断（disable_irq），恢复后再打开。原来处理函数碰到suspended就返回，而中断是
    #    电平触发的，每次从休眠醒来在EC恢复之前都要空转上万次。0095：EC中断作为唤醒源，休眠中插拔充电器
    #    或数据线能唤醒（Android再按Wake on plug亮屏）。
    0094-platform-arm64-huawei-gaokun-ec-disable-the-IRQ-across-suspend.patch
    0095-arm64-dts-gaokun3-EC-IRQ-as-wakeup-source.patch
    # ★★ 0097–0099：ath11k固件崩溃恢复，准备推主线（英文说明，0097、0098的Fixes指向主线提交）。打开WoWLAN
    #    （#40）以后，固件偶尔在休眠中崩溃，崩溃恢复又和系统挂起撞在一起：内核panic，或者WLAN的PCIe链路断掉。
    #    0097：恢复失败时不再释放HAL。32be3ca4cf78以后成功路径只清零、复用HAL，失败路径却还释放它，下一次
    #    恢复就解引用空的srng_config；0098：挂起时先拿core_lock，等进行中的恢复做完，环被恢复释放了就不碰；
    #    WoW挂起在恢复期间改走不带WoW的挂起；0099：固件一崩溃就让发给它的命令立即失败。系统挂起、恢复期间
    #    （PM_SUSPEND_PREPARE到PM_POST_SUSPEND）不做重置，交给恢复：断电挂起醒来本来就重新加载固件，否则在
    #    ath11k_mac_op_start()里先断电、上电再让mac80211配置；醒着时照常重置，并在恢复完成之前不让系统挂起
    #    （wakeup source "ath11k_recovery"，最长60秒）。
    0097-wifi-ath11k-keep-the-HAL-when-crash-recovery-fails.patch
    0098-wifi-ath11k-dont-reap-rings-that-crash-recovery-frees-on-suspend.patch
    0099-wifi-ath11k-keep-firmware-crash-recovery-away-from-system-suspend.patch
    # ★★ 0100–0102：接着0097–0099，待机测试里又找到的三处，同样准备推主线。0100：醒来时发现固件在睡眠中崩了
    #    （RDDM/SYS_ERR）就交给崩溃恢复：原来强行恢复MHI，等满超时失败，mac80211判定硬件丢失、关掉接口，Wi-Fi到
    #    下次冷启动前都起不来，之后每次唤醒还要多等约20秒；0101：崩溃恢复成功后再保持唤醒10秒
    #    （ATH11K_RECOVERY_SETTLE_MS），否则系统立刻挂起，WoW落在刚重启、还没连上的固件上，又把它弄崩，循环下去；
    #    0102：“断线唤醒”对应固件的四个唤醒事件（WOW_DEAUTH_RECVD_EVENT、WOW_DISASSOC_RECVD_EVENT、WOW_BMISS_EVENT、
    #    WOW_CSA_IE_EVENT），都是用来发现已有连接断开的；原来没连上路由器时进WoW也打开，现在只在arvif->is_up时打开。
    0100-wifi-ath11k-hand-a-firmware-crash-found-on-resume-to-crash-recovery.patch
    0101-wifi-ath11k-stay-awake-for-a-while-after-crash-recovery.patch
    0102-wifi-ath11k-arm-the-WoW-disconnect-events-only-when-connected.patch
    # ★★ 0103–0105：休眠中的Wi-Fi。0103（mac80211）：驱动挂起失败（返回负值）时清掉local->suspending，
    #    否则系统一直醒着，到下一次成功挂起之前收不到信标也不探测AP；0104：扫描期间持wakeup source "ath11k_scan"
    #    （最长到扫描超时），扫完再保持5秒，留给上层取结果、发起连接。息屏时Android的软件PNO靠闹钟把系统叫醒去扫描，
    #    却不为扫描持wakelock，系统马上又挂起，mac80211中止扫描，休眠中断线后就一直连不回来；0105：固件拒绝进入WoW
    #    （htc suspend not complete）时让这次挂起失败、稍后重试（连续最多3次），不再退回到断开连接、断电挂起。
    #    0103、0105准备推主线；0104是为Android做的取舍，不推。
    0103-wifi-mac80211-clear-the-suspending-flag-when-the-driver-fails-to-suspend.patch
    0104-wifi-ath11k-keep-the-system-awake-while-a-scan-runs.patch
    0105-wifi-ath11k-retry-the-suspend-when-the-firmware-declines-WoW.patch
    # ★★ 0106–0110：主线7.3的5个补丁原样移植（cd054a6e272c、eb667d0fbdd3、541735571578、b9b23e72abef、
    #    b05482e7ce1b）。LPASS的四个编解码宏（WSA/VA/TX/RX）改用PM clock框架，空闲时撤掉向ADSP投的
    #    LPASS_HW_MACRO/LPASS_HW_DCODEC票。原来从probe起一直投着，ADSP从不睡，系统睡眠时内存也停在1017MHz；
    #    打上以后ADSP能睡，内存降到200MHz。
    0106-ASoC-codecs-lpass-wsa-macro-Switch-to-PM-clock-framework-for-runtime-PM.patch
    0107-ASoC-codecs-lpass-va-macro-Switch-to-PM-clock-framework-for-runtime-PM.patch
    0108-ASoC-codecs-lpass-wsa-macro-Use-devm_clk_hw_register-for-MCLK-output.patch
    0109-ASoC-codecs-lpass-tx-macro-switch-to-PM-clock-framework-for-runtime-PM.patch
    0110-ASoC-codecs-lpass-rx-macro-switch-to-PM-clock-framework-for-runtime-PM.patch
)

# ⚠️ 诊断补丁【不进发版内核】：只在带 --with-diag 时打。顺序有依赖：0028/0029 依赖 0023，
#   0029 依赖 0028；0030 独立。反向撤掉时要倒序（0030 0029 0028 0023）。
#   它们是 #103–#105 取证用的（regmap 转储 / debugfs 旋钮 / camss 按块跳过），
#   发版内核里留着只会在每次 titan 域翻转时刷两行 pr_err。
DIAG_PATCHES=(
    0023-clk-qcom-gdsc-dump-gdscr-on-toggle-and-timeout.patch
    0028-clk-qcom-gdsc-debugfs-init-raw-and-wait-override.patch
    0029-clk-qcom-gdsc-debugfs-raw-toggle-flags-mask-pre-off-delay.patch
    0030-media-camss-dbg-skip-per-block-stream-power.patch
    # 0064（#128 §8–§9）：iris 收尾 / 断电全程跟踪 + A/B 开关。独立于上面四个（只动 iris 目录），打在 0065 之上。
    0064-media-iris-DIAG-pc-teardown-trace-and-ab-switches.patch
)
# ★ 指纹实验补丁（v1.0-plan D21，用户 2026-10-04 采纳"只放进实验内核"）：
#   0050（#124/#125）：QSEECOM APP_START/SHUTDOWN + listener，把 TZ 内存约束到 32 位。只碰 drivers/firmware/qcom/qcom_scm.c
#   与其头文件。2026-09-24 在本机把签名 TA 加载成功（app_id=5），但 client driver / HAL 都还没有 ⇒ 发版内核里它只是
#   一段没人用的 SMC 通路。v0.6.x–1.0.0-dev.8 的发版内核都带着它（休眠，#125）；从下一次内核构建起不带。
#   原来排在 0049 之后、0053 之前；它与 0053–0076 不相干（--verify 会在干净 worktree 上重放证明这一点）。
FP_PATCHES=(
    0050-firmware-qcom-scm-qseecom-app-load-shutdown-listener.patch
)
WITH_FP=0
for a in "$@"; do [ "$a" = "--with-fp" ] && WITH_FP=1; done
if [ "$WITH_FP" = 1 ]; then
    KPATCHES+=("${FP_PATCHES[@]}")
    echo "⚠️ --with-fp：指纹实验补丁也会打（${#FP_PATCHES[@]} 个，排在链尾），这不是发版内核"
else
    for p in "${FP_PATCHES[@]}"; do
        f="$REPO/patches/$p"
        [ -f "$f" ] || continue
        if git -C "$TREE" apply --check -R "$f" 2>/dev/null; then
            echo "✗ 指纹实验补丁 $p 还在树里 —— 发版内核不带它（D21）。撤掉：git apply -R patches/${p}；或者这是实验内核，加 --with-fp" >&2
            exit 1
        fi
    done
fi

WITH_DIAG=0
for a in "$@"; do [ "$a" = "--with-diag" ] && WITH_DIAG=1; done
if [ "$WITH_DIAG" = 1 ]; then
    KPATCHES+=("${DIAG_PATCHES[@]}")
    echo "⚠️ --with-diag：诊断补丁也会打（${#DIAG_PATCHES[@]} 个），这不是发版内核"
else
    # 发版路径：诊断补丁若还在树里，要大声说出来 —— 否则会把带 pr_err 转储的内核发出去
    for p in "${DIAG_PATCHES[@]}"; do
        f="$REPO/patches/$p"
        [ -f "$f" ] || continue
        if git -C "$TREE" apply --check -R "$f" 2>/dev/null; then
            echo "✗ 诊断补丁 $p 还在树里 —— 发版内核不能带它。撤掉：git apply -R patches/${p}（倒序）" >&2
            exit 1
        fi
    done
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
#    `patches/0009`（CPU cooling maps）的第一条合格新增行是
#        polling-delay-passive = <250>;
#    而这行在【未打补丁的】sc8280xp.dtsi 里本来就有一次（主线自带的 gpu-thermal 区）
#    ⇒ grep 命中 ⇒ 判成"已应用" ⇒ 静默跳过 ⇒ **重建的内核悄悄失去 CPU 温控降频**。
#    这正是 M17 写这个脚本要防的那件事，结果被脚本自己的判据复现了。
#    实测：新树里 cooling-maps 只有 1 处（主线的），旧树 9 处。
#
# ★ 两处加固，都便宜：
#    ① 探针改挑【最长】的新增行 —— 越长越不可能与既有代码撞车
#       （0009 的最长行是 `cooling-device = <&cpu0 THERMAL_NO_LIMIT ...>` 64 字符，
#        在未打补丁的文件里根本不存在）。
#    ② 取最多 3 条并要求【全部】命中 —— 单条撞车是偶然，三条同时撞车基本不可能。
#
# ⚠️ 为什么不怕假阴性：fuzz 影响的是【上下文】匹配，新增行本身一定是逐字写入的。
#    万一仍误判成"没打过"，后面 `git apply --check` 会失败并走 fuzz，
#    fuzz 再失败就【大声报错】—— 方向是对的：宁可吵，也不要静默跳过。
#    ③ ⚠️★ 2026-09-13 又补一条：**把"只是被搬家"的行从探针里剔掉。**
#       `patches/0022` 把 `gdsc_pm_subdomain_remove()` 与 `of_genpd_del_provider()`
#       两行调了个个儿，于是它们同时出现在 `-` 和 `+` 两侧 ——
#       而 `+` 侧的行**在未打补丁的文件里本来就在**，三条探针会全部命中，
#       整个补丁被静默跳过。这与 0009 那次是**同一类事故**（探针撞上既有代码），
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
#   upstream-venus/0019 当年是 fuzz 打进去的，0053 在那样的树上 git apply 会失败 ——
#   可下面的回落逻辑会改用 patch --fuzz=3 把它【硬打进去】，得到两份 videocc / pil_video_mem、
#   两个 video-codec@aa00000，脚本还照样报成功（审查 2026-09-28 实测推出来的，不是假想）。
#   所以在进循环之前就拦下来。0017/0018 还在的话 IRIS=y 也根本编不过。
DTSI=arch/arm64/boot/dts/qcom/sc8280xp.dtsi
GK3=arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dts
if grep -q 'compatible = "qcom,sm8350-venus"' "$DTSI" 2>/dev/null \
   || grep -q '^&venus {' "$GK3" 2>/dev/null \
   || grep -q 'sc8280xp_res\|sm8350_res' drivers/media/platform/qcom/venus/core.c 2>/dev/null; then
    echo "✗ 这棵树上还打着 patches/upstream-venus（venus 时代的节点 / 资源结构体）。" >&2
    echo "  从 2026-09-28 起视频编解码走 iris（0053–0060），两者不能叠。做法（二选一）：" >&2
    echo "  ① 推荐：git -C $TREE worktree add --detach <新路径> HEAD，在新 worktree 上跑本脚本（再接 KernelSU）；" >&2
    echo "  ② 就地撤：倒序撤 upstream-venus 的 0020 0019 0018 0017 0016 0015 0013 ——" >&2
    echo "     0017/0018/0019 当年用了 fuzz，要 patch -R -p1 --fuzz=3；其余 git apply -R。别用 git checkout（CLAUDE.md 运维坑 4）。" >&2
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
    #   ⚠️★★ 这一步必须排在指纹判据【前面】（2026-09-13 踩的坑）：patches/0031 的新增行是
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
        # ★ 回落到模糊匹配。当年 upstream-venus/0019 就需要这个（上下文里的 #include 列表与
        #   v7.2-rc2 差一行 qcom,scm.h；它已随 #128 退役，现役列表零 fuzz）。⚠️ 用了 fuzz 必须【明说】，
        #   静默的模糊匹配是灾难的开始 —— 0053 在 venus 旧树上就会被它硬打成两份节点，所以上面才有那道拦截。
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

# ★ 打完之后断言 iris 的 DT 状态（#128）。指纹判据和 fuzz 回落都可能让 0053/0054 "看起来打上了"
#   而实际没有 —— 0054 唯一的指纹行 firmware-name = "...qcvss8280.mbn" 在旧的 &venus 块里一字不差。
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
