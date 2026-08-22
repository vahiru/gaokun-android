# 项目：MateBook E Go (sc8280xp / gaokun) 移植 Android

## 目标

在华为 MateBook E Go（Snapdragon 8cx Gen 3 / sc8280xp，代号 gaokun）上跑原生 AOSP，
最终目标是能稳定运行 arm64 手游。

**当前阶段：Stage 6 M18 — ★★★ 光感（自动亮度）定性收敛：驱动在固件里、类型已声明、SEE 确实在 probe，**但芯片在 I²C 上不应答**；`bus_instance` 0–7、`bus_type` 0–3、`rail_on_state` 1/2 全部扫空，#43/#68/#70 的历次归因一并作废。本轮真正的产出是**可信实验循环**（`scripts/ssc/`，60–90 秒一次、带阳性/阴性对照）。同期已装机验收：亮度真 HAL、键盘开关（设置项 + 控制中心磁贴）、WSA 音量上限 90（实测 +5.7 dB）。⚠️ 当前机上构建 m21 带 venus 调试插桩，**不可发布****（每次开工时更新这一行）

> **★★★ Stage 6 M18（2026-08-22）：光感 —— 先造工具，再做实验。**
> ★★ **纠正一条本仓记错的事实：SLPI 停得掉。** 此前写着"`echo stop` 返回 0
> 但状态还是 running（attached mode）"，实测直接变 `offline`
> （`remoteproc remoteproc0: stopped remote processor slpi`）。
> 判据一眼可见：sysfs 显示的是 **`running`** 而不是 **`attached`**，
> 且 `firmware` 属性有值 ⇒ 是 Linux 引导的。
> ⇒ "每个实验都得重启整机"不成立，循环降到 **60–90 秒**。
> - ★★★ **补上了 #70 缺的那一环：证明 DSP 真读了我的目录。**
>   阳性对照 = 把加速度计 JSON 从自定义根里删掉 → 该传感器消失而 SSC 照常上线。
>   同时再次确认 #71：只重启 hexagonrpcd 时，根目录换成**空目录**加速度计
>   照样满血 ⇒ SEE 初始化一次后不再依赖文件服务器。
> - ★★ **阴性对照定出失败形状**：把能用的加速度计地址改错（54→99），
>   失败签名与光感**一模一样** ⇒ 驱动在跑、在 probe，**芯片不应答**。
> - ★ **新结构：二供料。** IMU（sh3001 ✅／t1000）在 bus 1，
>   光感（tcs3701／sy3133cs）都在 bus 5。t1000 同参数却没注册 ⇒
>   SEE 的"探测择优"本身正常。
> - ❌ `is_dri = 0` ⇒ 光感**本来就轮询**，#43/#68 的"DRI 中断到不了"**不成立**；
>   TLMM 32 与 127 在 Linux 侧都是 `UNCLAIMED`。
> - ★ **`hw_platform = QRD` 是我们自己编出来喂 DSP 的**（主线不导出它）
>   ⇒ 这套是**高通参考设计**的配置，板级差异原在 Windows DriverData 注册表，
>   已随抹除 Windows 丢失（#37 当时只当是丢了校准精度）。
> - ⚠️ 做完实验**必须恢复**：SLPI 起回来、hexagonrpcd 用回 `/vendor` 根，
>   并且**验到框架层**（`dumpsys sensorservice`）—— SSC 通不代表 HAL 会话还在。
> - 完整案卷 `docs/stage4-findings.md` #72；工具与纪律 `scripts/ssc/README.md`。


> **★★★ Stage 6 M17（2026-08-22）：一个用户报告，一路查到上游活着的缺陷。**
> ★ **`ESR=0xf2000800` → EC=0x3C（BRK）⇒ 是 `BUG()` 断言，不是空指针。**
> 这一个数字就定了方向：去找那行 `BUG_ON`，别查内存越界。
> - ★ **救回证据的是 pstore**（45 条 efi_pstore 记录，两次事件）。
>   logcat 是内存的、`/data/anr/` 是空的、hangdump 看门狗也没命中
>   （它判 D 状态 ≥2 分钟，而机器压根没活到两分钟）。
>   Stage 0 布下的"没串口就走 EFI 变量"这条通路，**第一次在用户报告的问题上
>   付清成本**。⚠️ 读它要 root，而本机 `adb root` 前得先
>   `setprop service.adb.root 1`。
> - ★★ **与"设置"无关**：两次事件分属 `org.lineageos.updater` 与
>   `com.android.permissioncontroller`，两条不同的 ioctl 路径
>   （`dma_fence_driver_name` / `dma_fence_timeline_name`）。
>   任何 app 的 RenderThread 查 present fence 的名字都能触发
>   ⇒ **普通应用能把整台机器 panic 掉**。
> - ⚠️★ **我的第一个假说是"7.2-rc2 缺保护、后来修了"** —— 顺，但错。
>   救我的是没停在假说上：用 GitHub compare API 逐个查那四个提交在不在
>   `v7.2-rc2` 里，**全是 `behind`（早就在）**；再拉 master 对比，
>   那个 `BUG_ON` 一字未改。"应该已经修了"和"确认在不在"差着一次 panic。
> - 修法 `patches/0013`：**只删那一行 `BUG_ON`**。`fence_to_crtc()` 只有两个
>   调用者，而它们本身就是那张 ops 表的成员 —— 断言的是调用路径已经保证过的
>   事，事后再查只能引入竞态。
> - ★ 顺带堵住一个长期漂移源：`patches/*.patch` **此前没有任何消费者**
>   （全靠手动 `git apply`，本会话刚为此付过一次代价）。新增
>   `scripts/kernel-apply-patches.sh`——只打内核那几个、幂等、失败非零退出。
> - ⚠️ `Handover signaled` 噪声在崩溃日志里刷了 163 行，**把 pstore 的有效
>   内容挤掉了**（45 条里真正有用的不到 10 条）。它本身无害（#37 已证），
>   但它损害取证能力，值得单独治。

**上一阶段：Stage 6 M16 — ★★★★★ s2idle 两端都修好并已发版（v0.3.0-alpha，构建戳 `1787335922`）：Android 真实挂起/唤醒 ×4 零复位，救援 Ubuntu `systemctl suspend` 3/3。真凶是我们自己 Stage 2 加的 `dr_mode="otg"`——`a600000.usb` 的 role 停在 `device`（无 gadget、无 xhci），断电即整板复位；Android 的修法是息屏切 `host`、亮屏切回 `device`（代价：息屏时 USB adb 断，TCP adb 不受影响）。README/TODO/设备树注释已同步到这个事实**

> **★★ M17 补记（构建 + 上机）**：`kernel-apply-patches.sh` **第一次跑就拦下
> 两处真实回归** —— `patches/0009`（CPU cooling maps）从构建机内核树上掉了
> （没拦住的话新内核会**悄悄失去 CPU 温控降频**，无风扇平板会一路满频跑到
> 紧急关机，症状要等某次长时间游戏后突然关机才出现）；上游 Venus 补丁集
> **也只活在构建机工作区**，`sc8280xp.dtsi` 被回退过，`patches/0011` 依赖的
> `venus` label 没了 ⇒ **从干净的 v7.2-rc2 照本仓 `patches/` 根本重建不出
> 发版内核**。已入库 `patches/upstream-venus/`（8 个原样保存，0014 故意不用）。
> ⚠️ 其中 `0019` 需要 `patch --fuzz=3`，脚本会**明确打印用了 fuzz** ——
> 静默的模糊匹配是灾难的开始。
> - ⚠️★ **`patches/0014` 被自己的实测否掉**：`dev_err_ratelimited` 只抑制
>   **62%**（事件 5.05 Hz → 打印 1.95/s，正好是默认的 10 条/5 秒预算），
>   而它的立项理由是"别让噪声把 panic 栈挤出 pstore"，2/s 一小时仍七千行。
>   已改成 `DEFINE_RATELIMIT_STATE(60 * HZ, 1)`。
>   ★ **"用了标准做法"不等于"达成了目标"**，差一次测量就会把 62% 当成完成。
> - ⚠️★ 新坑：**`BOARD_PREBUILT_DTBIMAGE_DIR` 会把目录里所有 `*.dtb` 拼接**。
>   陈旧残留让 boot.img 带了两份 DTB（346052 = 恰好 2×173026），
>   **构建全程不报一声，唯一线索是大小是整数倍**。已在 `release.sh` 做成断言
>   （数 `d00dfeed`，≠1 就 die），并**拿真样本正反验过**（boot_a→1、boot_b→2）。
> - 运维：直连构建机只有 1.32 MB/s，**R2 中转 41 MB/s**，把 14 分钟计费空转
>   压成约 2 分钟。⚠️ 这个版本的 `update_engine_client` **没有 `--status`**，
>   判断装完看 `bootctl get-active-boot-slot` 是否切槽。

> **★ Stage 6 M16（2026-08-22）：发版 v0.3.0-alpha + 一次全仓文档对账。**
> 发布物就是**在硬件上验过的那一版**（`--no-build`，构建戳 `1787335922`）；
> R2 五条路径全 200，`release.sh` 三条断言全过。
> ★ 随后做了一次**文档与现实的对账**，结果值得记：**公开 README 仍在首屏写着
> "The machine cannot suspend"**，而那正是这一版的头号卖点；zh-CN 更落后一整版
> （耳机口还是 ❌ 且写着**已被 #40 推翻**的旧诊断、Venus 还是 ❌、
> 招募项还在说 `CONFIG_QCOM_FASTRPC` 是 `=m`——M13 早就实测 `=y`/0 模块）。
> 设备树注释同样：`init.gaokun3.rc` 整段还在讲"机器不真的睡"，
> `usbrole.rc` 与 `device.mk` 一个说"默认不启用"、一个默认设成 1，自相矛盾。
> ⚠️★ **教训：发版说明写了不等于文档更新了。** 首屏状态、状态表、招募项、
> 以及**代码里的成段注释**都是独立的副本，每一份都会各自变质。
> ★ 下次发版的收尾清单里必须有一条"grep 一遍旧结论的关键词"
> （`cannot suspend` / `不能待机` / `=m` / `❌`）。
> - 顺带修掉两处会误导人的产物/文档不一致：`ota/gaokun3.json` 的仓库副本
>   还是 `createjson.sh` 生成的 **sourceforge** 下载地址（线上那份是对的，
>   `release.sh` 在上传时重写；但仓库里留着假地址会让人以为发错了），
>   已改成与线上**逐字节相同**；`INSTALL.md` 说"v0.3 起带 `recovery-ramdisk.img`"
>   —— **没有任何一版发过它**（recovery 还在复位循环），已改成实话。

> **★★ Stage 6 M15（2026-08-21）：s2idle 从"内核缺陷、无从下手"变成两个可定位的问题。**
>
> ★★★**最重要的一条：复位发生在【挂起进入】，不是唤醒。**
> 把 RTC 闹钟从 40 秒改成 **180 秒**，机器仍在几秒内复位（adb 27 秒就回来）。
> ⇒ **M4/#47 那句"挂起成功、死在任何唤醒"是错的**，也正因为这个错误方向，
> 之前十几轮"解绑某某再试"必然全部无效。
> - ★ **`pm_test` 平反了**：#49 判它"本平台无效判据"，理由是"它自己就会复位"
>   —— **那个理由本身就是结论**。同一次开机跑：`pm_test=freezer` **rc=0 活着**、
>   `pm_test=devices` **复位** ⇒ 故障夹在 `dpm_suspend_start()` +
>   `dpm_suspend_noirq()` 之内。而且它 5 秒自动返回、**不需要唤醒源**，
>   是目前最安全最快的复现器。
> - ★★ **唯一出现过成功的配置是 `pcie_aspm=off` + `nvme_core.default_ps_max_latency_us=0`**
>   （两个都必需；**光去掉我们自己加的 `pcie_aspm.policy=powersupersave` 不够**）。
>   正式测量：**基线约 16 次开机 0 次通过；该组合 5 次开机有 3 次通过，
>   其中一轮 `pm_test=devices` 连过 10/10。** 差异是真的（p≈0.002）。
>   ⚠️ **但不是确定性修复** —— 同一条 cmdline 另有 2 次开机第 1 次就死。
>   ★ 而**同一次开机内行为高度一致**（要么连过 10 次、要么第 1 次就死）
>   ⇒ 决定性因素是**开机时确定下来的状态**，不是每次挂起的随机性。**这一层未查。**
>   ⚠️ 因此**暂不把它写进发版 cmdline**：那会让待机变成"有时能睡、有时炸"，
>   比确定不能睡更糟。
> - ★ 该组合下唯一那次真实挂起**睡进去了但没醒** ⇒ **"醒不回来"是另一个
>   独立问题**，被"挂起时复位"盖了整整两轮。
> - ⚠️★★ **本轮最贵的教训是关于判据的**：这个故障的**单次失败率约 93%**，
>   所以"改一次、试一次、炸了"对任何配置都是大概率事件，**几乎没有证据力**。
>   我因此被 #39/#43（同一 cmdline 一正一反）带偏两轮。
>   正确做法 = **每次开机把 `pm_test=devices` 跑到失败或跑满 10 次，只数 `rc=0`**。
>   ⚠️ 工具本身也有坑：`pm_test` 周期后会留 pending 唤醒事件，紧接着再挂起
>   会**立刻 `-EBUSY` 返回**；第一版脚本把那个非零返回当成"通过"，
>   于是报出假的 10/10。判据是**耗时**（真实周期 5–8 秒，`-EBUSY` 是 0–1 秒）。
> - ★**一整批假说被实测否掉**（都不是推理）：EC 的 PM 回调 / EC 的中断 /
>   **EC 本身（解绑）** / Venus / geni I²C 的 noirq 重构 / PDC 唤醒映射 /
>   **我们全部 20 个 buildbot 补丁**（零补丁 v7.2-rc2 同样复位）/
>   cpuidle 深空闲态 / 硬件看门狗 / 音频+USB+三个 remoteproc 全解绑。
> - ⚠️★ **#49 的两行数据不可信**：那张表里"纯 7.2"的行与本轮受控重做
>   （`git checkout v7.2-rc2` + 我们自己的 .config）**结果不符**。
>   ★ 教训：对照实验的两格若**失败模式不同**（一个干净 abort、一个硬复位），
>   它们比较的就不是同一件事。
> - ⚠️★ **本轮让用户按了三次电源键**，是我的纪律问题。已定规矩：
>   **"醒不回来"没解决之前不跑真实挂起**（`pm_test=devices` 够用且安全）；
>   实验条目加 **`panic=10`** 配合已开启的 `CONFIG_DPM_WATCHDOG`；
>   改 DTB/引导链前先明说可能要按电源键。
> - ★ 运维：让脚本**只准备不挂起**（"stay 模式"），机器停在救援 Ubuntu 且
>   **ssh 可达**，一次开机能连做多个实验。⚠️ 救援机 IP 会漂
>   （本轮 `.230`，Android 在 `.46`），用"ping 全网段填 ARP → 逐个取 hostname"找。
> - 完整案卷：`docs/stage4-findings.md` #50 / #51。
>
> **★★ Stage 6 M14（2026-08-21）：耳机口 + Venus 内核侧 + 一条运维能力翻案。**
> 新 ROM 经**真实 OTA 通路**（update_engine + postinstall + boot_control）装机验收。
> **设备现在跑的是 slot_a、构建戳 `1787247612`、内核 `#19`，
> 与 R2 上待发版的那一组产物完全同源。**
> 四条音频通路**开机即用、零手工步骤**实测：耳机 PCM0 `RUNNING`、
> 扬声器 PCM1 `RUNNING`、内置麦 PCM3 与耳机麦 PCM2 各录到 384000 帧。
>
> ★**耳机口不出声，真凶是 RX 插值器链从来没接上** —— 不是"后端打不开"。
> 我原先只设了 `RX_MACRO RXn MUX` 就以为通路成立，实际 rx-macro 内部还有一级
> 插值器停在默认值（`RX INTn_1 MIX1 INP0 = ZERO`、`RX INTn DEM MUX =
> NORMAL_DSM_OUT`、`CLSH/LO Switch = Off`）。DAPM 路径不完整 → 后端 DAI 拿不到
> 有效通路 → PCM open 失败，而**内核不为此打任何日志**，所以看起来像后端坏了。
> ⚠️ 我在 #40 里给的三个候选（拓扑缺 APM 图 / soundwire 没上电 / q6apm 静默失败）
> **一个都不是**，案卷保留了它是怎么错的。
> 补齐 9 个控件后实测：`tinyplay -D 0 -d 0` rc=0、`pcm0p` `state: RUNNING`、
> **hw_ptr 2 秒前进 97920 帧 = 48960 帧/秒**（正好实时 48 kHz，证明 DMA 在真实
> 消耗数据而不是空转）。**新镜像开机自动应用，无需手工**。
> - 另外两处阻塞：策略里压根没声明耳机设备（已加 `WIRED_HEADPHONE` /
>   `WIRED_HEADSET` / `IN_WIRED_HEADSET`，地址 `CARD_0_DEV_0` / `_2`）；
>   框架 `WiredAccessoryManager` 找的是 `/sys/class/switch/h2w`，**本机 ENOENT**
>   （主线没有 h2w 驱动），开关是框架资源 `config_useDevInputEventForAudioJack`。
>   那个 evdev 插孔设备**本来就在、而且已经在被读**（`Classes: KEYBOARD | SWITCH`）。
> - ★**配方来源：救援 Ubuntu 上的上游 ALSA UCM2** ——
>   `sc8280xp.conf` 用 `Regex "HUAWEI.*MateBook E.*"` 把本机直接 include 成
>   `LENOVO-X13s.conf`。**上游本来就把华为 MateBook E 与 ThinkPad X13s 视为同一套。**
>   ★ 方法论：本机凡是 LPASS 音频的事，先去抄那个目录，别自己推 DAPM 图。
> - ★ HPH 音量的方向**从内核算，不猜**：`wcd938x.c:2620`
>   `SOC_SINGLE_TLV(..., 0x18, 1, line_gain)` + `:192 DB_SCALE(-3000, 150, 0)`
>   → 控件值 v = −30+1.5v dB。**默认 24 = +6 dB**（满增益直接进耳朵），
>   上游的 2 = −27 dB。交叉验证同配方里 `ADC2 Volume 10` = +15 dB 麦增益，合理。
> - **仍需用户做一件事**：插一次耳机听。框架资源只能构建期 overlay，
>   `WiredAccessoryManager` 在 SystemServer 启动时只读一次，框架层也没有
>   插拔模拟命令（`cmd audio help` 里没有 device/connect/jack）。
>
> ★**Venus 硬件视频编解码：内核这一半打通了**（据我们所知 sc8280xp 首次）。
> `/dev/video0` = `qcom-venus-decoder`、`/dev/video1` = `qcom-venus-encoder`，
> `aa00000.video-codec` 绑 `qcom-venus`、`abf0000.clock-controller` 绑
> `sm8350-videocc`，供应方（videocc / IOMMU / rpmhpd / 4 条 interconnect）
> 全部解析，延迟 probe 队列空，**固件加载失败 0 行**。
> - ★**固件我们一直在装，只是名字骗了我**：`firmware-name` 指向
>   `qcvss8280.mbn`，而 README 里那一行当初被我标成"语音服务（未用到）"
>   —— **VSS = Video SubSystem，不是 Voice**。
> - ★**时钟控制器主线已有**：`videocc-sm8350.c` 自己就认 `qcom,sc8280xp-videocc`。
> - 8 个上游补丁打 7 个（0014 纯格式清理、主线已分叉，跳过）。
>   compatible 选 `sc8280xp` 而非 `sm8350`：两个资源结构只差 `freq_tbl`，
>   `sm8350_res` 借用 sm8250 的表，`sc8280xp_res` 才是本 SoC 自己的。
> - ⚠️★ **必须关 `CONFIG_VIDEO_QCOM_IRIS`**，否则 Venus 编不过而**报错完全
>   看不出跟它有关**（`sm8350_reg_preset` / `VPU_VERSION_IRIS2` undeclared，
>   报在 core.c，像补丁打错）。主线 v7.2 用 `#if !IS_ENABLED(IRIS)` 把本机需要
>   的资源全编掉了，而 iris 的 of_match 里**没有 sc8280xp**，永远服务不了本机。
> - ⚠️★ 又是"=m 坑"，这次整条链上**五个**（`MEDIA_SUPPORT` / `VIDEO_DEV` /
>   `VIDEOBUF2_DMA_CONTIG` / `V4L2_MEM2MEM_DEV` / `SM_VIDEOCC_8350`）。
>   MUST_Y 断言 44 → 52 条。
> - **还没做的另一半**：Android 需要一个跟 V4L2 说话的 Codec2 组件，
>   66 个解码器仍全是软解。★ `external/v4l2_codec2` **本来就在 manifest 里**。
>   ⚠️ poolmask 不能照抄它 README 的 `0xf50000`（那是 ION，本机**没有 ION**），
>   要用 BLOB 的 `0xfc0000`。
>
> ★★**推翻 M4/M6：Android 上【能】写 EFI 变量。** cmdline 里确实有
> `efi=noruntime`，但 `mount -t efivarfs` 挂得上、78 个变量读得出、
> `LoaderEntryOneShot` **写得进且回读正确**（属性 `0x07` + UTF-16LE + 双 NUL；
> 覆盖已存在的变量前要 `chattr -i`）。
> ★ 机制**我们 Stage 0 就记下来了**（`hw-inventory.md` 第 8 节）：本机 EFI 变量走
> 高通 TrustZone 的 `uefisecapp` 后端、不依赖 EFI 运行时服务，所以 `efi=noruntime`
> 本来就不影响读写。M4/M6 那个判断与本仓自己的记录是矛盾的 ——
> **跨阶段的结论要回头对一遍旧案卷**，否则会重新发明一个错误。
> **意义：Android 自己就能安排"下次进救援系统"，而且是 oneshot，失败自动回落。**
> 本轮就靠它安全地试了 Venus 内核和新槽位 —— `default` 全程保持已知可用的那个。
> - ⚠️ 顺带查明：**boot_control HAL 每次启动都把 `default` 改成当前槽位**，
>   于是 `INSTALL.md` 承诺的"挂死→拍电源键→自动回落到可远程接入的系统"
>   **在首次进 Android 之后就静默失效了**。症状只是"`adb reboot` 本想去 Ubuntu
>   却又回到 Android"。已记 TODO D4，正解是让 HAL 只管槽位、`default` 永远留救援。
>
> ★**死锁取证看门狗**（#38 用户报过但我们从未复现）。现实是死锁时用户只会重启、
> 证据就没了，所以证据必须自动留下。探针刻意做得很便宜：**只读 `/proc` 线程状态、
> 不跑 dumpsys**，60 秒一次；判据是**同一个 tid 连续三次采样都在 D**（≥2 分钟），
> 不是"出现过 D"。命中后写 stack/wchan/QRTR 服务表/PCM 状态/binder 日志/logcat
> 到 `/data/vendor/gaokun3/hangdump-<uptime>/`，每次启动只采一份。
>
> ★**光感收敛到一处嫌疑**（#43）。逐字段对照能用的加速度计：
> ❌ 排除"DSP 够不到 PMIC 轨"（两者用**同一条** `/pmic/client/sensor_vddio`）；
> ❌ "SLPI 用不了主 SoC TLMM 脚"也站不住（加速度计同样 `irq_is_chip_pin=1`）；
> ★ 收敛到 **SLPI 侧 I2C 实例号 1（能用）vs 5（不通）**，
> 这也正好解释"启用光感会污染整个 SSC 会话"。
>
> ⚠️**三个自己造成的坑，都值得记**：
> 1. `boot_control/main.cpp` 删函数时留下多余花括号 → 构建 3 分钟就失败。
>    本地编不了 Android 目标，至少要过一遍**花括号平衡体检**。
> 2. ★**`make ... | tail -30` 之后取 `$?` 拿到的是 tail 的退出码** ——
>    第一次内核构建报 `KBUILD_RC=0` 而 `drivers/media` 其实失败了。
>    判据要看**产物时间戳**（`Image` 还停在旧的）。本仓在 az CLI 上记过同一个坑。
> 3. ★**"toybox mount 要求 /etc/fstab"是错的** —— 干净 A/B（root 身份、把 fstab
>    移走）证明 efivarfs 与 vfat 都照样 rc=0。真因是**非 root 挂载**时 toybox 会去
>    查 fstab。我当时同时改了两个变量（造 fstab + 重新拿 root），所以归错了因。
>    ⚠️ `adb root` 每次重启都要重做。
>
> ⚠️**`update_engine` 拒绝在 overlayfs（`adb remount`）生效时应用 OTA**
> —— `ErrorCode::kOverlayfsenabledError (64)`。要先 `adb enable-verity`
> （会报 `boot_b does not look like a vbmeta footer`，无害）**并重启**才生效。
>
> ★**内置麦克风此前【完全是断的】，而且谁都没发现**（#40 末尾）。
> 策略里 `Built-In Mic` 指的是 `CARD_0_DEV_3`，而那个 PCM 压根打不开
> —— 和耳机同一类：前端混音器没接（`MultiMedia4 Mixer VA_CODEC_DMA_TX_0` = Off）
> 且 va-macro 的 DMIC 序列一条没设。补齐后安静房间 **RMS −30.5 dBFS**（健康电平）。
> ⚠️ **两个采集 PCM 都只支持双声道**，传 `-c 1` 会得到 `cannot set hw params`
> —— 我因此一度以为耳机麦也坏了，其实它一直是好的。
>
> ⚠️★**#44 是一次我自己的误判，值得记**：装 OTA 时看到 10 KB/s + 45% 丢包 +
> dmesg 刷 `ath11k_pci: msdu_done bit in attention is not set`，我就断定是
> ath11k 的负载触发缺陷 —— 时间相关性极诱人（25 条全落在下载那几分钟，
> 开机到下载前一条都没有，且是唯一一种 ath11k 报错）。**三个实验推翻了它**：
> 空闲 ping 仍 40% 丢包但 msdu_done 新增 **0**；27.5 MB 小下载 881 KB/s、
> msdu_done **0**；★**ping 网关 0% 丢包**（1400 字节大包也是）；
> ★★**从本机拉 200 MB 跑到 61.7 MB/s、msdu_done 仍是 0**。
> → **WiFi 与 ath11k 都没问题**；丢包只在到 1.1.1.1 的 WAN 路径上（多半 ICMP 限速）。
> 真正剩下的问题是**设备的 WAN 吞吐只有 PC 的 1/20**（1–2 MB/s vs 36.9 MB/s，
> 同一网络同一 URL），**原因未定**，别再往 ath11k 上记。
> ★ 方法论：**"同时出现"不等于"因果"** —— 只要多做一步"到网关 ping"
> 和"LAN 大流量"，结论就整个反过来。
>
> ★ 顺带得到一条很实用的运维手段：**`update_engine` 支持 `file://`**。
> 在这台没有 recovery、没有 sideload 的机器上，
> **R2 → 本机 → USB → 设备**再 `--payload=file:///data/local/tmp/payload.bin`
> 是最快也最可控的装机路径 —— 1.1 GB 的包 **76 秒**装完
> （让设备自己走 WiFi 那次要二十多分钟）。五条链路的实测速度见 #44。
>
> **发版已备好但【未发布】**：产物在 R2 的
> `staging/crDroidAndroid-16.0-20260820-gaokun3-v12.11/`
> （`super.img.zst` / `boot.img` / OTA zip / sha256 / gaokun3.json），
> **`ota/gaokun3.json` 清单一字未动**，所以没有任何用户会收到。
> 发布是对外动作，等用户定；发版说明草稿在 `scratchpad/relnotes-030.md`。
> ★ 发布用 `scripts/release.sh`（TODO B5 已做，本轮端到端跑通）。
> ⚠️ 发那一版要带 **`--no-build`** —— build stamp 每次构建都变，重跑构建
> 发出去的就不是验过的那一版。我今晚正是这么踩的：设备上验了 `1787246871`，
> `release.sh` 又构建一次，staging 里成了 `1787247612`；
> 收尾是把 `1787247612` 也装到机器上重验了一遍。

> **★★ Stage 6 M13（2026-08-20）：按 Android 分区规范把内核纳入 OTA。**
>
> 用户明确要求"遵守 Android 分区规范"，所以不再让内核留在 OTA 之外。
> **终局是自研 EFI 加载器**（读 misc 选槽 + 解析 boot 镜像），本轮完成前两段。
> - ★**内核 `CONFIG_QCOM_FASTRPC=y` 实机验证**：`#18` 内核起来后
>   `/dev/fastrpc-*` 四个节点都在而 `/proc/modules` 是 **0 行** ——
>   不再需要 insmod，第 13 个「=m 坑」关闭。
> - ★**`CONFIG_EFI_ZBOOT`**：`Image` 39,422,464 → `vmlinuz.efi` **14,021,120**
>   字节（37→13 MB，2.8 分之一），`PE32+ EFI application`，
>   **经 systemd-boot 实机启动成功**。这是能在 300 MiB 的 ESP 上放两份内核的前提。
> - ★**真 `boot_a`/`boot_b`**（各 64 MiB，nvme0n1p5/p6），`boot` 进
>   `AB_OTA_PARTITIONS`，`TARGET_NO_KERNEL := false` + header **v2**
>   （一个分区装齐 kernel+ramdisk+dtb；v3+ 会强制引入 vendor_boot）。
>   构建日志实证：`--partition_names boot:product:system:system_ext:vendor`、
>   `Using generator FullUpdateGenerator() for partition boot`。
>   空间来自**删除 userdata-old**（用户授权），余 63.9 GiB 未分配；GPT 已备份。
> - ★**ESP 改成每槽独立目录** `slot_a/` `slot_b/`，两个 BLS 条目各指自己的目录
>   —— 于是 postinstall 只写"刚刷好的那个槽"，**绝不碰正在运行的内核**，
>   回滚天然安全。实机启动验证通过。
> - ★**过渡期的那一步**：`gaokun3-bootimg-extract`（include 树内 `<bootimg.h>`，
>   不手抄偏移）+ `gaokun3-ota-postinstall.sh`。argv[1] 是目标槽位整数 ——
>   出处 `postinstall_runner_action.cc:355-357`，不是猜的。
>   ⚠️ `POSTINSTALL_OPTIONAL=false` 是故意的：宁可 OTA 失败，也不能出现
>   "新 system + 旧内核"。
> - ★**两个解包实现交叉校验**（设备上的 C++ 与安装器里的 python）：
>   同一个 boot.img，三个文件 sha256 逐个一致。偏移抄错的典型后果是
>   "magic 通过但内容错位"，只有对校验和才看得出来。
>
> **本轮拆掉/发现的坑（都值得记）**：
> - ⚠️★ `boot_control` HAL 里的 `EnsureSlotProbeHints()` **已删除**。它把
>   `by-name/boot_a|b` symlink 到 `/dev/null` 好让 libboot_control 数出 2 槽；
>   现在那两条路径是真实分区，一旦它抢在 ueventd 之前跑成功，
>   update_engine 就会把 boot 镜像写进 `/dev/null`，**静默毁掉整个更新**。
> - ⚠️★ `deploy-from-ubuntu.sh` 的 `.bak-prev` 备份**已去掉** —— 那正是把
>   296 MiB 的 ESP 吃到 95%（63 MB 全是备份）的元凶，再部署一次就会写出被
>   截断的内核。**现在"另一个槽就是备份"**，这才是 A/B 的意义。
> - ⚠️★ 变量名是 **`BOARD_INCLUDE_DTB_IN_BOOTIMG`**（BOOTIMG 连写）。写错不会被
>   忽略，而是在 **lunch 阶段**炸成 `Don't have a product spec for:
>   'lineage_gaokun3'` —— 极易误判成 manifest 没拉全设备树。
>   ★**判据**：grep 一个变量名**返回空**本身就是信号。
> - ⚠️★ 必须用 `TARGET_PREBUILT_KERNEL`。Lineage 的 `kernel.mk:171-176` 另有一条
>   "扫 PRODUCT_COPY_FILES 里 dest=kernel"的分支，但那段写的是
>   `$(ifeq kernel,$(_dest), ...)` —— **`ifeq` 不是 make 函数**，整个展开成空，
>   那条路根本不生效（Lineage 自己的 bug）。设了它之后再用
>   `PRODUCT_COPY_FILES` 拷一份会撞 `overriding commands for target .../kernel`。
> - ⚠️★ `PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false`。有了真 boot.img
>   这道检查才生效，而它不认识主线 7.2：`No kernel entry found for kernel
>   version 7.2 at kernel FCM version 202504`（Minimum LTS 6.12.0），连
>   `7.2.0-rc2-gaokun3+` 都解析不了。
> - ⚠️★ `BOARD_KERNEL_CMDLINE` **早已与实机 BLS 条目漂移**（此前它没有消费者，
>   漂了没人发现）：原写 `deferred_probe_timeout=30` 而实际是 10，且缺
>   `veritymode`/`flash.locked`/`verifiedbootstate`/`iommu.*` 四项。已对齐。
>   **教训：没有消费者的配置一定会漂。**
> - ⚠️ 编内核**两处**都要 `CROSS_COMPILE=aarch64-linux-gnu-`：编译时不带会在
>   `prepare0` 报 `gcc: unrecognized command-line option '-mlittle-endian'`；
>   `olddefconfig` 不带会用**宿主 gcc** 评估 `CC_HAS_*`，静默改掉一批符号。
> - ⚠️ `bacon` **不重建 super.img** —— 发布安装包要单独 `m superimage`。
> - ⚠️★ `scripts/kernel-config-android.sh` 里有 5 项（`PM_WAKELOCKS`/`CPUSETS_V1`/
>   `MEMCG_V1`/`UCLAMP_TASK(_GROUP)`）**一直没被应用**：它们写在续行里，而前面
>   是**普通 `#` 注释** —— shell 里注释会终止续行，命令提前结束，只留一句
>   `--enable: command not found`。现有机器没出问题纯属侥幸（.config 里早就有）；
>   新树上会重现 cgroup v1 开机失败。已拆成独立命令并补进断言（37→44 项）。
>
> ★★**新 ROM 已部署并验收**（`ro.build.date = Thu Aug 20 11:06:26 UTC 2026`）：
> `bash scripts/verify-kernel-ota.sh` **13 项全过 / 0 失败** —— 内核、
> FASTRPC=y（4 个节点 / 0 模块）、boot_a 的 Android 镜像、bootctl 2 槽、
> 解包器与 postinstall 钩子、ESP 两槽文件齐、以及★**传感器开机自启**
> （`hexagonrpcd` 由镜像拉起，框架里 Z≈9.88，`gaokun3-ssc-test` 3 秒 151 条）。
> **`scripts/sensors-up-android.sh` 那套手动步骤从此不需要了。**
>
> ⚠️★**运维：不要让构建机干等大文件传输 —— 用 R2 中转。** 本轮直连
> 构建机→救援机只有 1 MB/s（2.7 GB 要 45 分钟，而机器按分钟计费）；
> 改成 zstd 压到 1.2 GB → 上传 R2 **27 秒（43 MB/s）** → 停机 → 本机拉 →
> 局域网推到救援机 **51 秒（23 MB/s）**。R2 出站免费。
> - ⚠️★ **别在上传前去探对象**：我先 curl 了一次那个还不存在的路径，
>   Cloudflare 把 **404 缓存住了**，上传成功后照样 404，
>   加个 `?cb=…` 才拿得到。差一点误判成"上传失败"。
> - ⚠️★ **沙箱代理会掐断到构建机的 ssh**：连续几次 `Connection closed by
>   ...port 22`，一度以为机器挂了（Azure 查询显示 running）。
>   同一条命令绕开沙箱就通 —— 长任务与大流量的 ssh 一律绕沙箱。
> - ⚠️ 家里到 Cloudflare 只有 **71 KB/s**（救援机 WiFi 直连），
>   所以"让设备自己从 R2 拉"这条路走不通，得经本机中转。
>
> ⬜ **还欠**：端到端 OTA 实测；
> 第 3 段的 **EFI 加载器**（读 misc + 解析 boot 镜像 + 装 initrd/DTB 协议，
> 用 systemd-boot 的 `efi` 指令 chainload，起不来就选别的条目 —— 安全阀）。
> ⬜ 另记用户提议：**把救援 Ubuntu 换成/补上 Android recovery**。我的判断是
> 补充而非替代 —— recovery 也是 boot 镜像格式（systemd-boot 同样读不了），
> 而它的主要能力 `adb sideload` 已被系统内 OTA 覆盖；而 `sgdisk`/`resize2fs`/
> sshd 这些**本轮重新分区真正用到的东西** recovery 给不了。
> 合理的优化是给 Ubuntu 瘦身（最小 rootfs 1–2 GiB，现在 24.6 GiB）。

> **★★ Stage 6 M12（2026-08-20）：sensors HAL 落地，自动旋转接上。**
>
> ★**实机验证**（`dumpsys sensorservice`）：
> ```
> Active sensors: SH3001 Accelerometer (handle=0x00000001, connections=2)
> SH3001 Accelerometer: last 50 events
>      1 (ts=612.371743828, wall=17:42:46.858) -0.04, 0.05, 9.88,
> ```
> 两个消费者正是自动旋转的 `WindowOrientationListener$AccelSensorJudge`
> 与 `FaceDownDetector`。★**框架还自动融合出 Game Rotation Vector /
> Gravity / Linear Acceleration** —— 有 accel+gyro 就有，游戏体感由此可用。
> - **做法是改造 AOSP 默认实现**（`hardware/interfaces/sensors/aidl/default`），
>   不是从零写。⚠️ **必须复制而不能继承**：那个 `Sensors` 类的传感器列表在
>   构造函数体里硬加而 `mSensors` 是 private，且 `libsensorsexampleimpl` 的
>   `visibility` 只放行 `hardware/interfaces` 子包，设备树链不了。
>   改动只三处：列表收敛为 accel+gyro（★那 7 个假传感器**必须删**，留着框架会
>   以为本机有气压计/湿度计）、两个 `readEventPayload` 换成真数据（没数据时报
>   UNRELIABLE，**不编一个 9.8 出来**）、`SensorInfo` 写真名字。
> - ⚠️★**最贵的一个坑：HAL 自己的重试把传感器枚举弄坏了。** 初版在查不到
>   传感器时每 10 秒**重建整个 SSC 会话**，结果之后连独立命令行客户端都报
>   「没有传感器提供 data_type=accel」，必须重启 hexagonrpcd 才恢复。
>   干净 A/B：停掉 HAL、重建会话，独立客户端立刻又读到 Z≈9.88。
>   根因是每次重建都在 SSC 上留下一个**被丢弃的客户端** —— 与「使能光感会
>   污染会话」是同一类现象。改成**在同一个 client 上重试 FindSensor**。
> - ★**必知时序**：`registry` 服务比**物理传感器先注册**，所以 WaitForService
>   成功后立刻查 accel 会得到「没有传感器提供」，要再等约 20 秒。
> - ⚠️ **VINTF 清单是 servicemanager 开机时读的**：运行时把 fragment 丢进
>   `/vendor/etc/vintf/manifest/` 它看不见，`addService` 会返回 -3 并让
>   `CHECK_EQ` abort（症状是 HAL 静默退出、日志全空，只有 tombstone 能看出来）。
> - ⚠️ **`adb shell stop; start` 会连带断 WiFi**（网络由框架管），
>   adb over TCP 当场掉线，而且回来时 IP 会变。别当成机器挂了。
> - ⬜ **欠三样**：sepolicy（现 permissive，logcat 一串 avc denied）、
>   ~~轴向标定~~ ★**已由用户实机确认方向正确，不需要纠正**（安装矩阵全零
>   → 单位矩阵，而传感器坐标系本来就与面板一致，运气好）、
>   `CONFIG_QCOM_FASTRPC=y`（现仍 `=m`，重启后要跑
>   `scripts/sensors-up-android.sh` 手动补）。
> - ⚠️ 另记：**boot_control HAL 会把默认启动项改成当前槽位的 Android 条目**
>   （M6 的设计），所以「默认永远留救援系统」这条纪律**装了 A/B 之后已不成立**
>   —— 本轮重启就直接回到了 Android。远程救援仍可用（Android 里 adb 可达），
>   但要恢复原纪律得动那个 HAL。

> **★★ Stage 6 M11（2026-08-20）：Android 侧传感器读数打通。**
>
> ★**自研客户端一次跑通**（`device/huawei/gaokun3/ssc/`，实机 Android）：
> ```
> 传感器 accel 的 UID = 61ab5376b4a5c9aa58442ede47acd316
>   X=-0.086191 Y= 0.052672 Z= 9.883265  accuracy=3
> ```
> Z≈9.88 = 重力，accuracy **3**（最高）。这条链路 = QRTR lookup → QMI 组包
> → protobuf → SUID 查找 → 使能 → 解读数，**就是 sensors HAL 逻辑的 90%**。
> - ★**陀螺仪也通了**（静止各轴≈0 rad/s，accuracy=3）。**Linux 侧从未验证过**
>   —— 那边装的 `ssccli` 压根不支持 gyro。于是不只自动旋转，**游戏体感/陀螺仪
>   瞄准**也有了基础。
> - ★**本机传感器清单（SSC 亲口回答，不是推测）**：`accel` ✅、`gyro` ✅、
>   **`mag` ❌ 没有磁力计**（所以没有指南针）、`rotv` ❌ 未注册（多半要磁力计）、
>   `ambient_light` ❌ 污染会话。⚠️ `mag`/`rotv` 查询失败**不会**污染会话。
> - ★**协议规格 `docs/sensors-ssc-protocol.md` 已被实现验证** —— 里面的线格式
>   （7 字节 service_header + TLV 的 u16 长度前缀）、消息 ID、哨兵 UID 全对，
>   后人可以照抄不必再逆向。libssc **不能移植**（glib/gobject/libqmi），但
>   `.proto` 只有几百行、AOSP 自带 protoc → 重写便宜得多。
> - ⚠️ 两个构建坑：①`proto: { canonical_path_from_root: false }` 必须加，否则
>   生成的头带全仓库前缀、`.proto` 之间的 import 也解析不了；
>   ②**protobuf 要静态链接** —— `libprotobuf-cpp-lite.so` 只在 `/system/lib64`
>   且文件名带版本号，vendor 命名空间看不见（`tinymix`/`libtinyalsa` 同一个坑）；
>   静态版又引用 `__android_log_write`，所以还得补 `shared_libs: ["liblog"]`。
> - ⬜ **只剩 AIDL `android.hardware.sensors` HAL**。要额外处理：安装矩阵全零
>   （轴向得实机对着屏幕标定一次）、采样率/batching/flush 语义、sepolicy。

> **★★ Stage 6 M10（2026-08-20）：传感器管道在 Android 上打通。**
>
> ★**判据达成**：Android 里 `gaokun3-qrtr-lookup` 列出 **`400 1 9 13`**
> —— Snapdragon Sensor Core 服务上线，与 Linux 侧观测一致。逐段确认：
> `/dev/fastrpc-sdsp` 出现 → AOSP 编出的 `hexagonrpcd` 运行（进程停在
> `fastrpc_internal_invoke`，20 秒内 DSP 发来 **2880 行**文件请求，序列与
> Linux 上完全一致）→ SSC 在 DSP 上起来并注册服务 400。
> - ★**hexagonrpcd 上游自带完整 Android 构建支持**（Android.bp ×5，
>   `hexagonrpcd-sdsp.rc` 里 service 跑 system:system、`-R /vendor/etc/hexagonrpcd-root`）
>   —— 不用我们写 bp，工作量远小于预估。它**只依赖 libc**（3443 行 21 个文件）。
> - ★**bionic 的 `uapi/misc/fastrpc.h` 逐符号比对通过**：需要的 6 个 ioctl
>   （含关键的 `FASTRPC_IOCTL_INIT_ATTACH_SNS`）与 5 个 uapi struct 全都有。
> - ★**协议规格已整理成 `docs/sensors-ssc-protocol.md`** —— 把"逆向 libqmi +
>   libssc"变成"照规格实现"，每条都注了来源行号。最容易记错的一条：
>   **QRTR 上的 QMI 没有 QMUX 头也没有 marker 字节**，上线就是 7 字节
>   `service_header` + TLV（`qmi-endpoint-qrtr.c:547` 注释原文）。
>   **libssc 不能移植**（拖着 glib/gobject/libqmi），但 `.proto` 只有 389 行，
>   而 AOSP 自带 protoc + libprotobuf-cpp-lite → 重写比移植依赖便宜。
> - ⬜ **还缺**：QMI/protobuf 客户端（下一个可验证增量 = 独立命令行客户端，
>   对标 `ssccli`，跑通 SUID 查找→使能→打印 XYZ，那就是 HAL 逻辑的 90%）、
>   AIDL `android.hardware.sensors` HAL、sepolicy。
>   另外 `CONFIG_QCOM_FASTRPC` 仍是 `=m`（本轮靠 insmod 验证），
>   已进 `kernel-config-android.sh` 断言，**待重编内核**（第 13 次 =m 坑）。
> - ⚠️★**拆掉一颗地雷：`int-crdroid.conf` 已不可启动。** A/B 化后 fstab 四条
>   logical 行都带 `slotselect`，first-stage mount 必须靠 cmdline 的
>   `androidboot.slot_suffix` 才能把 `system` 解析成 `system_a`；两个条目的
>   kernel/dtb/initrd **完全相同**，唯一差别就是 `android-a.conf` 带
>   `slot_suffix=_a`。少了它就是 M6 那个"进 Android 就重启、pstore 全空"的坑。
>   `deploy-from-ubuntu.sh` 的默认值和本文说明都已改指 `android-a.conf`。
> - ⚠️ 更正一条自己的判断：`device_kernel_headers` **不用我们提供**，
>   AOSP 在 `build/soong/Android.bp:56` 就用 `kernel_headers` 类型定义了它。
>   我建了空壳导致构建报 `module already defined` —— 因为当时**只 grep 了
>   `device/` 目录**。判断"树里有没有这个模块"要 grep 全树。
> - ⚠️ 又被同一个转义坑咬了两次（`
` / `
` 被中间层 collapse 掉，一次把真 CR
>   写进文档、一次让生成的 python 字符串断行）。**结论固化进脚本注释：
>   生成代码时一律用 `chr()`，不写反斜杠转义。**


> **★★ Stage 6 M9（2026-08-20）：传感器翻案 —— 加速度计真的通了。**
>
> ★**推翻 M5 的"主线此路不通"**（下面那段 M5 原文保留，但结论已作废）。
> 一位贡献者给了 Linux 侧部署指南（`docs/contrib/slpi-sensors-deploy.md`），
> 在救援 Ubuntu 上逐条复现：**静止时 Z≈9.87 m/s²，15 秒 131 行读数**。
> 这一个数字验证了整条通路
> `fastrpc → hexagonrpcd 的 VFS → SLPI 上的 DSP 注册表 → SSC → QMI → libssc`。
> 关键在于**思路要换**：不是 AP 去驱动芯片（AP 确实无总线可达，M5 这半截是对的），
> 而是**AP 给 DSP 当只读文件服务器**。
> 一键复现 `scripts/slpi-sensors-setup.sh`，案卷 `docs/stage4-findings.md` #37。
> - ★**于是「自动旋转」对本机是可达的**，缺的只是 **Android 侧 sensors HAL**
>   （把 hexagonrpcd 移过去 + 拿 libssc 包一个 AIDL HAL）。README 招募项已重写
>   —— 难的那一半做完了，据我们所知其他 sc8280xp 设备都没跑通过。
> - ❌ **光感（tcs3701）不通**：使能后从不返回读数，而且**会污染整个 SSC 会话**
>   —— 之后连加速度计也读不到，必须重启 hexagonrpcd（★还要等约 20 秒沉降，
>   6 秒就读实测是 0 行；**"读不到"不等于"坏了"**）。
> - ❌ **负面结果：别用 `sscregistrygen` 预生成注册表。** 生成 142 个文件后
>   加速度计一起坏掉，挪走 141 个只留**空 registry** 当场恢复（干净 A/B）。
> - ★**"给 hexagonfs 加写入"不是小补丁**：`hexagonfs.h:34-45` 的 ops 表只有
>   close/openat/readdir/read/stat/seek，**没有 write，只读是设计使然**；
>   且 DSP 想写的那个目录是 `hfs_mkdir` 出来的虚拟目录，没有后端可写。属上游活。
> - ❌ **出厂校准永久丢失**：它存在本机 Windows 的 DriverData 里、不在任何驱动包中，
>   而本机 Windows 已抹除 → **安装矩阵全零**（libssc 退回单位矩阵）。
>   ★**但实测无害**：M12 用户确认自动旋转方向正确 —— 单位矩阵恰好与面板方向
>   一致，不需要在上层纠正。丢的只是 bias 补偿那点精度。
>   ⚠️ **给还留着 Windows 的人：先把那个 registry 目录拷出来再装系统。**
> - ⚠️ 两条我自己下错又更正的判断：①"registry 服务起不来所以光感失败"——错，
>   那份 debug 日志是在**我自己装出来的坏状态**下抓的（**诊断日志必须在已知
>   good 状态下重抓**）；②"`Handover signaled` 是 SLPI 崩溃循环"——错，良性噪声，
>   对照实验里**工作正常的加速度计同样每 12 秒 13 条**。
> - ⚠️ 环境坑：`droid-juicer` 0.4.2 会无限 `openat` 死循环把 apt 卡死 43 分钟
>   （`systemctl mask` 掉）；`libssc` 上游已删 `-Dmocking` 选项，照指南抄会报错。

> **★★ Stage 6 M5（2026-08-20）：M4 的成果全部固化进镜像 + 传感器判死
> （⚠️「判死」这半截已被上面的 M9 推翻）。**
>
> ★**用户实测：原神画质开到极高流畅可玩。** GPU 91% 时间待在最低档 270 MHz、
> 峰值 690 MHz、最高温 50 °C、零降频、`GMU 错误 0 / a6xx_recover 0 / SMMU fault 0`。
> 余量非常大。**唯一遗憾是清晰度**：渲染缓冲被钉在 1080×1728，这是原神按
> **设备白名单**给的档位，不是我们的技术限制（实测把逻辑分辨率改小，缓冲不变
> → 绝对上限而非比例）。要突破只能伪装机型，**有账号风险，留给用户决定，未做**。
>
> ★**新 super 已构建、刷入、验收通过**
> （sha256 `131c7f67…`，`ro.build.date = Aug 19 20:26:05 UTC`）。
> M4 那 4 个"只活在 overlay 里"的修复现在真在镜像里了：
> `display_settings.xml`（横屏）、`thermal-guard.sh` + `thermalguard.rc`（CPU 温控）、
> 删掉 MMAP 的音频策略。**验收判据是「overlay 里 0 个文件」** ——
> 刷 super 会连带抹掉 scratch，所以 overlay 空了还一切正常，就证明东西在镜像里。
> 只刷了 super：产物 ramdisk 与 ESP 上那份 sha256 完全相同，内核也没重编。
> - 流程：**先比两棵设备树的 md5 清单再传**。这次差异正好是预测的 3 缺 2 改，
>   且**"VM 上没有本地缺失的文件"**——确认了这条才敢整树覆盖，
>   否则不入库的华为固件会被 tar 抹掉，而且要到下次刷机声卡不注册才发现。
> - ⚠️ 两个会误报的核对坑：`grep -c mmap_no_irq_out` 得 1（那是**注释**，
>   判据要写 `grep -c 'name="mmap_no_irq_out"'`）；**`debugfs` 读得了
>   `vendor.img` 却读不了 `system.img`**（shared-blocks 去重），
>   `tinymix` 就是这样被我误报成缺失的 —— 它本来就该在 `/system/bin`。
> - **拆掉两颗地雷**：`deploy-from-ubuntu.sh` 的 `flash_boot` 一直往
>   `$ESP/Image` / `ramdisk.img` 写（**旧 AOSP 条目**的文件名），而
>   `crdroid.conf` 读的是 `Image-kb23` / `ramdisk-crdroid.img`
>   —— 跑 `all` 会"更新成功"却毫无作用，是 M3 那个坑的翻版；已改成从 BLS
>   条目里解析实际路径。另一颗是 `bootctl set-oneshot` 失败时静默，已改成炸出来。
>   > 附带查明：**`efi=noruntime` 不妨碍 `bootctl`**，oneshot 实测可写可回读，
>   > 远程救援闭环是可靠的。
>
> ★**传感器：不是缺 DTS 节点，是整套跑在 SLPI DSP 上。**
> ⚠️**"主线此路不通"这个结论已被 M9 推翻**（加速度计实测已通）——
> 下面的"AP 无总线可达"是对的，错的是由此推出"不可达"：
> 正解是 AP 给 DSP 当文件服务器，不是 AP 直接驱动芯片。
> 从本机 Windows 分区读出驱动库：没有任何 AP 侧传感器芯片驱动，只有
> `qcsensors.inf` + `qcsensorsconfigqrd8280`（里面是 `sns_*` 的 SEE 模块配置
> 和 **`libsdsprpc.dll` = Sensor DSP RPC**）。器件是 `sh3001`(IMU)、
> `tcs3701`(ams 光感+接近, I2C 0x39)、`sy3133cs`、`t1000`、`stm_lid_angle`(铰链角)，
> **全挂在 SLPI 自己的总线上，AP 够不着**。
> → ~~自动旋转、自动亮度在主线上做不了~~ **已作废，见 M9**：加速度计通了，
> 自动旋转可达（缺 Android HAL）；光感仍不通，自动亮度确实做不了。
> ★**游戏不受影响。**
> 工具 `scripts/probe-windows-sensors.sh`，完整证据 `docs/stage4-findings.md` #37。
>
> **★★ Stage 6 M7/M8（2026-08-20）：温控根治 + 120 Hz + 取消刷机后脚本。**
>
> ★**CPU 温控从 DTS 层根治**（`patches/0009`）。主线 `sc8280xp.dtsi` 全文只有
> 一个 `cooling-maps`（在 `gpu-thermal` 下），8 个 `cpuN-thermal` 只有一条
> 110 °C critical、没有任何 cooling device —— CPU 一路满频跑到内核紧急关机。
> 补丁给 8 个温区各加 85 °C passive trip 并绑本簇 cpufreq cooling device。
> 同机只换 DTB 实测：**cdev 0→1、trip 1→2**，`cpu0→cpufreq-cpu0`、
> `cpu4→cpufreq-cpu4`；反解 DTB 也核过（cooling-maps 1→10 处）。
> ★因此 `thermal-guard.sh` **从镜像撤掉** —— 不是"留着当双保险"：温区绑定后
> `cur_state` 归内核 thermal core（step_wise 持续写），用户态再写就是打架。
>
> ★**渲染解锁 120 Hz**。面板硬件模式本来就是 120（`activeMode vsyncRate=120`），
> 钉住渲染的是 `config_defaultRefreshRate = 60`
> （`frameworks/base/core/res/res/values/config.xml:5870`）。
> ⚠️**`Settings.System.peak_refresh_rate` 改不动它** —— 实测设 120 后调度器
> duration 仍是 16.67 ms；管事的是 base 不是 peak。用框架资源 overlay 设成 120，
> 实机验证 `defaultRefreshRate: 120`、**SF duration 15.67→7.33 ms**。
> 选"钉住"而非 peak=120+default=0：60/120 两个模式在**不同 mode group**，
> 之间切换是非无缝 modeset。
>
> ★**刷机后脚本 `android-post-flash.sh` 已删除**，七件事逐条有下落：
> 两条本来多余（`stay_on_while_plugged_in` 的 AOSP 默认就是 false；
> 横屏已由 `/vendor/etc/display_settings.xml` 负责）；
> 两条 adb 属性**早就在 device.mk:36**（adbd 先读 `service.` 回落 `persist.`，
> 全新刷机时 persist 那份已生效 —— 本轮实测：`service.adb.tcp.port` 为空而
> TCP adb 正常连着）；
> 三条新编进去（`def_screen_off_timeout` 120000、captive portal 两条 URL）。
> 唯一仍需人工的是"首次带密码手连一次 WiFi"（#29 的解禁条件，无法预置）。
>
> ⚠️**captive portal 的正确改法**（源码明写）：必须覆盖
> `config_captive_portal_*_url` 而**不是** `default_*` ——
> `packages/modules/NetworkStack/res/values/config.xml:10-15` 说 `default_*`
> 不可 overlay、改它 "will break if the enforcement of overlayable starts"。
> `config_*` 在该模块 `<overlayable name="NetworkStackConfig">` 里（policy
> product|system|vendor），且它在 `res/values/config.xml:41` **定义为空串**，
> `NetworkMonitor.java:2671-2674` 非空则用、否则回落 `default_*`。
> ★**不新建 RRO**：树里已有 `vendor/lineage/overlay/rro_packages/
> NetworkStackOverlay`，打包/manifest/targetName 都对，只需往它 res 里加资源；
> 但**必须配 `<add-resource>`** —— 构建期 overlay 只能覆盖目标资源表里已存在
> 的条目，那个 RRO 自己只定义了 `config_dhcp_client_hostname`。
> 实机验证：探测日志里实际用的是 `connect.rom.miui.com/generate_204`，
> `isValidated=true`。（`config_captive_portal_fallback_urls` 未覆盖，
> 日志里还能看到一次 `play.googleapis.com`，待办。）
>
> ⚠️**两个自己造成的构建失败，都值得记**：
> 1. `<add-resource>` 缺失 → `error: resource ... does not override an
>    existing resource`。构建期 overlay ≠ 能新增资源。
> 2. ★**XML 注释里不能有 `--`**。我在注释里原样引用 aapt2 的提示
>    `--auto-add-overlay`，于是 `xml parser error: not well-formed (invalid
>    token)` 且报在 **line 0**，极具迷惑性。现在所有 overlay XML 都过一遍
>    解析体检。
>
> ⚠️**GPU SMMU 中断：查清了但没动**。实际 DT 是全局 672/673、context bank
> 从 **678** 起（678=CB0 … 689=CB11）。对照 D6 观测（硬件拉 675/680）：
> **680 声明了但被分给 CB2，675 整张表里根本没有**（674–677 是空的），
> 很像 CB 起始偏移就错了。但只凭"675/680 挂起"推不出正确映射
> （若 CB0=675 则 680 会是 CB5，而实现的 CB 只有 2 个，自相矛盾）。
> **改错了没有任何征兆，只是继续收不到 fault**，故未改。
>
> ⚠️**运维两条**：
> * **`gaokun3-rescue.local` 在 git-bash 的 ssh 里解析不了**（MSYS 走 Cygwin
>   解析器、不做 mDNS；Windows 自己的 `ping` 能解）。可靠做法是**按 hostname
>   扫网段**：ping 全段填 ARP，再逐个 ssh 取 `hostname` 比对。
> * **USB adb 会掉**（#27），而且 IP 会漂。本轮就靠**扫 5555 端口 + 比对
>   `ro.crdroid.device`** 找回设备。这正是把 `persist.adb.tcp.port=5555`
>   编进镜像的价值 —— 实测它救了场。
>
> **★★ Stage 6 M6（2026-08-20）：公开发版 + A/B OTA 打通。**
>
> ★**系统内 OTA 已接通并实机验证**（截图为证：「系统更新」页显示
> 「由 vahiru 维护」、「上次检查 11:02」、「暂无可用更新」）。
> 托管在 **Cloudflare R2 + 自定义域 `ota.072172.xyz`**（桶 `gaokun-android`）。
> 布局：`ota/gaokun3.json`（清单）、`builds/<zip>`、`install/<ver>/…`。
> - crDroid 的 `org.lineageos.updater` 本来就装着，**只差把它指向我们的清单**。
>   接线只有一条路：`R.string.updater_server_url`，**没有属性可覆盖**
>   （`UpdatesNetworkDataSource.kt:20`）。而且必须是【构建期】overlay ——
>   Updater 没为这些字符串声明 `<overlayable>`，运行时 RRO 会被拒。
>   overlay 路径要镜像模块的 `resource_dirs`：
>   `overlay/packages/apps/Updater/app/src/main/res/`。
> - ⚠️★**抓清单的客户端设了 `.followRedirects(false)`**，任何 3xx 直接失败。
>   实测（从**设备**测，沙箱自己挡出站 HTTP，那边的结论不可信）：
>   `github.com/…/raw/…` = **302 会挂**；`raw.githubusercontent.com` 与
>   R2 自定义域 = 200。它还 `require()` 强制 https。
>   **下载链接反而可以重定向**（走 `HttpURLConnectionClient`，默认跟随）。
>   ⚠️ 别在这个主机名上加 Redirect/Page Rule —— 报错只有
>   `Unexpected HTTP status: 301`，绝对想不到是那条规则。
> - ⚠️ 不用桶自带的 `pub-*.r2.dev`：Cloudflare 明确说它是开发用途、限速、
>   不走完整缓存。**但自定义域也没缓存住 1 GB 的 zip**
>   （实测 `cf-cache-status: BYPASS`，免费版单文件缓存上限 512 MB）——
>   好在 **R2 出站免费**，成本没问题，只是没有边缘加速。
> - ★**清单的 `timestamp` 必须等于该构建的 `ro.build.date.utc`**。
>   `createjson.sh` 有时填的是【打包时刻】（`bacon` 之后又跑 `m` 就会错开），
>   那会让装上此版本后 Updater **永远显示"有更新"**，而那就是同一个版本。
>   判据在 `UpdatesRepository.kt:113-115`（`==` 视为当前版本、`<` 视为更旧）。
> - 上传用 `scripts/r2-upload.py`（纯标准库、凭据只从环境变量读）。
>   **云到云传**：构建机 → R2，2.4 GiB 用 66 秒；从本机传要 8 分钟。
>   只把 S3 密钥送上构建机，**绝不送账户级 API 令牌**（后者能改 DNS、删桶）。
>   **顺序：产物先传，清单最后传**，否则中间清单会指向不存在的文件。
> - ⚠️★**救援系统的 IP 会漂**（DHCP，实测 .230 → .54，害我死等了 7 分钟）。
>   **avahi 在跑，用 `gaokun3-rescue.local`**，别再写死 IP。
>
> **仓库已公开**：https://github.com/vahiru/gaokun-android
> （★2026-08-20 起改为 **GPL-3.0-or-later**；发版当时是 Apache-2.0）。
> Release **v0.1.0-alpha** 含全新安装产物（`super.img.zst` / `Image` / dtb /
> `ramdisk.img` + sha256 清单）与 A/B 更新包
> （`crDroidAndroid-16.0-20260820-gaokun3-v12.11.zip`）+ `gaokun3.json`。
> - 公开前**改写了历史**（用户选的）：`git filter-branch` 抹掉构建机静态公网 IP
>   与家里 WiFi 的 SSID，77 个提交全改。**验证要点：`--all` 会把
>   `refs/original/*` 和 `refs/remotes/origin/*` 算进去，那会让你以为没抹干净**
>   —— 只验要推的 `main`（`git log main -S` + 遍历 `rev-list --objects main`
>   的全 blob 扫描，两法都是 0 才算过）。
>   ⚠️ 强推后 GitHub 仍保留旧对象（按 SHA 可取，但 SHA 从未公开过，风险≈0）；
>   要硬保证就删库重建（我们有 verified bundle）。**真正的补救是把 NSG 的 22
>   端口锁到自己的出口 IP** —— 那个 IP 是静态的，deallocate 也不释放。
> - 历史审计结论：**固件 blob 与密钥从未进过历史** ✅。
>
> ★**A/B 走的是 Virtual A/B，分区布局一个字节没改** —— 每槽镜像合计 2.53 GiB，
> 12 GiB 的 super 连传统 A/B 都放得下，而 Virtual A/B 连扩都不用扩。
> 只新增了 4 MiB 的 `misc`（本机用的是盘头 GPT 之后闲置的 1007 KiB，
> 扇区 34–2047，没动那个 64 GiB 备份）。
> 内核前提：`CONFIG_DM_SNAPSHOT=y` ✅，`CONFIG_DM_USER` 无 → 只能非压缩快照，
> 故只 inherit `virtual_ab_ota/launch.mk`。
>
> ★**自研 boot_control HAL**（`device/huawei/gaokun3/boot_control/`）。
> 标准 `boot-service.default` 把槽位写进 misc 就指望 bootloader 去读，
> 而 systemd-boot 不认识那个结构。我们这个原样复用 `libboot_control`
> （misc 仍是唯一真相源，`bootctl`/update_engine 行为不变），额外把槽位
> 镜像进 ESP 的 `loader.conf`（写 glob `default *-android-a.conf`，
> 于是不必知道 machine-id）。
> **实机验证**：`bootctl get-number-slots`=2、当前槽 `_a`、
> slot0 bootable+successful、HAL 日志 `systemd-boot default now: ...`、
> `vold`/`update_engine` 都报 `Using AIDL version of IBootControl`。
>
> ⚠️**M6 踩的两个坑，都是"看着完全无关"的那种**：
> 1. ★**fstab 的 logical 行必须带 `slotselect`**。少了它 fs_mgr 去找字面叫
>    `system` 的逻辑分区，而 A/B 的 super 里只有 `system_a`/`system_b`
>    → first-stage mount 失败 → **init 主动 `reboot()` 而非 panic**
>    → pstore 全空、屏幕一闪而过，**唯一症状是"进 A 槽就重启"**。
>    我第一反应是"ramdisk 没跟着换"，换了照样重启（猜错一次）。
>    判据：`device/linaro/dragonboard/fstab.common` 是树内现成的 A/B 设备，
>    每条 logical 行都带 `slotselect`，逐字对照即得。
> 2. ★**`nb_slot` 会是 4 而不是 2**。`InitDefaultBootloaderControl()` 靠
>    `stat()` 探测 `<miscdir>/boot_a..boot_d` 数槽位，本机**没有 boot 分区**
>    （内核/ramdisk 是 ESP 上的文件），探不到就按设计回退成 `kMaxNumSlots=4`。
>    后果很阴：`(cur+1)%4` 让**第二次** OTA 从 `_b` 切到不存在的 `_c`，
>    故障晚一个版本才爆。修法：HAL 的 `main()` 里把两个探测路径 symlink 到
>    `/dev/null`（`stat` 跟随符号链接会成功，正好数出 2；boot 不在
>    `AB_OTA_PARTITIONS` 里，没人读写它们）。
>
> ⚠️**`otapackage` target "不存在"是假象**：`build/make/core/Makefile:5793-5806`
> 在 `TARGET_NO_KERNEL=true` 且无 boot 镜像、以及无 recovery.fstab 时把
> `build_ota_package` 关掉。加 `PRODUCT_BUILD_GENERIC_OTA_PACKAGE := true`
> 一次跳过全部三条（AOSP 注释明说就是给树外内核用的）。
> crDroid 的打包 target 叫 **`bacon`**，它顺带跑 `createjson.sh` 生成
> Updater 要的 `gaokun3.json`。
>
> **安装器**：`scripts/install-gaokun3.sh` —— 从任意 arm64 Linux live 环境
> 一条命令装好（分区 + super + systemd-boot + 救援系统 + 两个槽位条目）。
> 默认启动项给**救援系统**而不是 Android：默认落点必须是能远程接入的系统。
> LiveCD 尚未打包，但这个脚本就是它的内核。
>
> ★★**Windows 已抹除，整机归 Android；U 盘不再是必需品**（2026-08-20，用户授权
> "数据都备份了，直接不要"）。
> - **引导链已搬进内置 ESP**（`scripts/esp-migrate-to-internal.sh`，纯增量）。
>   **拔盘实测通过**：`/dev/block/sd*` 不存在，Android 照常启动。
> - **删除 Windows 的 p2/p3/p4/p5/p6/p7**。现在盘上只有：
>   p1 ESP 300M / **p2 userdata 376G** / p8 super 12G /
>   p9 `userdata-old` 64G（迁移前备份，暂留）/ p10 metadata 32M /
>   **p3 Ubuntu 救援 24.6G**。未分配 1007 KiB。
> - ★**`/data` 62 GiB → 370 GiB（可用 294 GiB）**。此前它**已经 100% 满**
>   （原神 34G + 明日方舟 19G，非 root 只剩 235 MiB，装不下任何东西）。
>   做法是**新建 + `dd` 整盘克隆 + `resize2fs` + 换 PARTLABEL**，不是原地扩容：
>   `dd` 逐字节复制，SELinux 扩展属性/capabilities/硬链接零解释带过去；
>   `fstab.gaokun3` 用的是 `by-name/userdata`（**PARTLABEL**）所以改标签即可，
>   fstab 一字未动；**旧 p9 全程只读并保留**，出问题重启就是原来那台机器。
>   校验：文件数 49999=49999、字节数 63 237 600 073 相同、大文件 md5 抽查 3/3。
> - **Ubuntu 救援系统已搬进内置盘**（`rsync -aHAXx` 克隆活动根，134403 个文件），
>   hostname `gaokun3-rescue`、root=`nvme0n1p3`、ssh 仍是 192.168.31.230。
> - ⚠️★**固件的启动优先级会变，别当成一次测定的事实**：装引导链时
>   `LoaderDevicePartUUID` = `d5cb76b5…`（U 盘）；**删掉 Windows 分区之后变成
>   `825eaf3a…`（内置盘）**。于是"内置 ESP 只在 U 盘不在时才用到"当场失效，
>   而它的默认项当时是 Android → **自动回落安全网悄悄断了**，
>   症状只是"莫名其妙进了 Android"。动过分区表就要重读这个变量确认。
> - **现役闭环（两个方向都实测通过）**：默认 → 内置救援 Ubuntu；
>   `sudo bootctl set-oneshot <mid>-android-a.conf` → Android；
>   Android 里 `adb reboot` → 自动回落救援系统。
>   ⚠️ 条目名带 **`int-`** 前缀，U 盘那套旧名（`<mid>-crdroid.conf`）已非现役。
>
> **★★ Stage 6 M4（2026-08-19 夜）：s2idle 定性 + 音频解锁。**
>
> ★**"不能待机"的判决：挂起成功、resume 失败、然后整机被复位 —— 而且在
> Ubuntu 里用同一棵内核复现得一模一样。** 所以与 Android、与 SystemSuspend、
> 与我们的设备树**全都无关**，是内核/EC 层面的缺陷，根治属上游活。
> 证据：Ubuntu 侧 `echo mem > /sys/power/state` 后日志停在那一行，
> 紧随的 "resume 成功" 标记从未写出，机器回来后 `uptime` 是全新启动。
> RTC 闹钟**确实按时触发**，约 13 秒后才重启 → **坏在 resume，不在 suspend**。
> - **三个元凶已排除**（各自卸掉再挂起，照样醒不来）：himax 触摸驱动、
>   三个 remoteproc（ADSP/CDSP/SLPI）、★**EC 驱动本身**
>   —— 最后这条**推翻了 CLAUDE.md 从 Stage 3 起的预言**（"EC 挂起/恢复会先炸"）。
> - 那两个"本该修好它"的补丁**其实一直都在**（buildbot 无条件 `git am`
>   `patches/upstream/*` 与 `patches/others/*`）：`upstream/0012`
>   （EC 的 PM 回调 NOIRQ→SYSTEM_SLEEP，自述就是修 "resume fail silently"）、
>   `others/0017`（EC 加 `device_init_wakeup`）。都在，照样挂 → **别再指望它们**。
> - **没法继续二分**：内核没开 `CONFIG_PM_DEBUG`，`/sys/power/pm_test` 不存在
>   （两侧都没有）；挂起瞬间的日志也拿不到（userspace 已冻结，journald 来不及落盘；
>   clean hang 不产生 panic，efi_pstore 抓不到）。→ 真要修，第一步是编个带
>   `CONFIG_PM_DEBUG` 的内核。
> - **落地取舍**：wakelock **保留**（这是正确的工程决定，不是偷懒），
>   加一条逃生口 `persist.gaokun3.allow_suspend=1` 供将来复测；
>   但把 `svc power stayon true` 与 `screen_off_timeout=INT_MAX` **删掉** ——
>   持 wakelock 时息屏是安全的。**于是本机的"待机" = 息屏但机器不真睡。**
> - ⚠️**会浪费两小时的陷阱**：持有 wakelock 时读 `/sys/power/wakeup_count`
>   会**永久阻塞**（实测 cat 挂死 120s）。Android 的 SystemSuspend 就卡在这一读上，
>   这也解释了 `suspend_stats/success` 恒为 0。别在探测脚本里 cat 它。
>
> ★**远程救援闭环本轮实战验证成功**：Android 挂死 → 自动复位 → 默认启动项
> Ubuntu → ssh 进去 → `bootctl set-oneshot ...crdroid.conf` → 回 Android。
> **全程不需要有人在机器边。**"默认启动项永远留 Ubuntu"这条纪律兑现了价值。
>
> **音频**：`tinymix` 首次真的编进来了（M3 只是排了队）。硬件路径**实测通**：
> 291 个混音器控件、路由回读正确（`>AIF1_PB`/`>RX0`/DAC on/BOOST off/PA=12）、
> `tinyplay` 让 `/proc/asound/card0/pcm1p/sub0/status` 变成 **`state: RUNNING`**
> 且 DMA 实时消耗。★**2026-08-20 用户实机确认：音频可用（听到声音）**。
> 框架路径于此确证 —— 此前 `Total writes: 0` 只是因为这个 ROM 里没有任何应用
> 能处理音频、我试过的每种无头触发都没能让 AudioTrack 起来，属"未测"而非"坏"。
> ⚠️ `tinymix` 动态链接 `libtinyalsa.so`，放 `/vendor/bin/` 时那个 .so
> 必须一起进 `/vendor/lib64/`（vendor 命名空间搜不到 system 的那份）。
>
> **蓝牙：撤销 #30**。实测 `state: ON`、地址读出、`crashed 0 times`。
> `android-post-flash.sh` 里的禁用两行已删（留着会在每次重刷 userdata 后
> 把好的蓝牙重新关掉）。
>
> **搁置并说明理由**：传感器（⚠️**此处的"此路不通"已被 M9 推翻** ——
> 整套确实跑在 SLPI DSP 上、AP 无总线可达，但经 hexagonrpcd 给 DSP 当文件
> 服务器后**加速度计已实测通**，见 #37；光感仍不通，故自动亮度做不了，
> 自动旋转则只差 Android HAL；**游戏不受影响**）、
> Venus 硬解、UCSI、SELinux 转 enforcing。
> ⚠️**记一条将来的地雷**：热管理 HAL 是 AOSP mock，它报的 skin/battery
> **SHUTDOWN 阈值只有 36.0 °C**，而 `ThermalManagerService.shutdownIfNeeded()`
> 到 SHUTDOWN 会直接 `powerManager.shutdown()`。现在因 mock 值恒定打不到，
> **将来换成读 `/sys/class/thermal` 的真 HAL 时必须同时改阈值**，
> 否则开机几分钟就自动关机。
> 详见 `docs/stage6-crdroid.md` 的 M4 段。

> **★★ Stage 6 M3 已于 2026-08-19 完成：Android 跑在 Adreno 690 硬件 Vulkan 上。**
> `Turnip Adreno (TM) 690`、`boot_completed` t+48s、锁屏/桌面渲染正常
> （3.83 MB screencap 逐像素对）；22 分钟带负载浸泡：**GMU 错误 0 /
> a6xx_recover 0 / SMMU fault 0**，桌面四进程 PID 全程不变。
> 一键复验 `bash scripts/verify-turnip.sh`，案卷 `docs/stage6-crdroid.md`。
>
> 做法是**把 Stage 5 的补丁树整棵铺回来**，不是重跑生成管线；
> 另外首次把 `smmu-nostall.sh`（GPU SMMU stall 解锁器，常驻安全网）
> 写进了构建配置。
>
> **M3 顺带推翻了三条旧结论（都写进了案卷）**：
> 1. ★"crDroid 与我们的 mesa 是同一个 commit" —— **假阳性**。
>    `git log -1` 比的是 `.git` 的 HEAD，而 mesa 26 当年是**铺在工作树上
>    从未提交**的，所以两棵树必然显示同一个 commit。实际是
>    25.3.0-devel vs **26.0.3**，`git diff` 3791 个文件。
>    判"两棵源码树是否相同"，`git status --porcelain | wc -l` 才是那一句。
> 2. ★"s2idle 的 **wakelock 挡不住**" —— 挡是一直挡住了（⚠ 别把这条读成 "s2idle 是好的"；M4 已证明 **resume 确实坏**）。`/sys/power/wake_lock` 是
>    `radio:wakelock` 0660，shell 连读都读不了，我把 `cat` 的
>    "Permission denied" 当成了"内容为空"。实测 26 分钟 `suspend entry` = 0。
> 3. ★"`adb root` 不生效" —— **两步可解**：
>    `adb shell setprop service.adb.root 1` 然后 `adb root`
>    （permissive 下 shell 能写这个属性，而 adbd 自己那一步没生效）。
>    顺带查明 `ro.build.type` 是 **user** 而非 userdebug。
>    ★ 这把钥匙的真正价值是**远程救砖**：拿到 root 就能挂 U 盘 ESP
>    （`/dev/block/sda1`，**不是**内置盘 `nvme0n1p1`）直接改 `loader.conf`。
>
> **M3 最费时间的坑**：刷完 super 后 oneshot 到了 `android.conf`，
> 那是**旧 AOSP 的 Image(kb18)+ramdisk**，crDroid 要用 `crdroid.conf`
> （`Image-kb23`）。用错内核 → 没有 `patches/0007` → bpffs 标签崩溃循环，
> 症状一路指向刚换的 turnip 而其实毫无关系。
> **判据：`adb shell uname -a` 的编译时间必须与本次内核一致。**
> `scripts/deploy-from-ubuntu.sh` 已改成设 oneshot 到 `crdroid.conf`，
> 且不再篡改默认启动项（默认必须留 Ubuntu，那是唯一的自动回落安全网）。
>
> **M4 起点（已实测）**：触摸设备在（`Himax Capacitive TouchScreen`）、
> 声卡注册、解码器 66、蓝牙 HAL 装着但被 `android-post-flash.sh` 禁着；
> ⚠️ **音频当前是断的** —— `tinymix` 从没进过 `PRODUCT_PACKAGES`
> （`audio-route.sh` 找不到它就直接放弃），已补进 device.mk 待下次构建；
> ⚠️ **WiFi 硬件全通但网络被框架永久禁用**
> （`NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT`，stage4 #29 复发），
> 解禁需要一次带密码的用户发起连接。

> **★★ Stage 6 M2 已于 2026-08-19 完成：crDroid 16.0 完整启动进桌面。**
> `sys.boot_completed=1`（t+40s），surfaceflinger/system_server/systemui/launcher3
> 全部在跑；**解码器 66 个**（含 mp3/aac/flac/amrnb/amrwb/g711）；
> 铃声 130 + 通知音 92 + 闹铃 45 + UI 音效 25。执行案卷 `docs/stage6-crdroid.md`。
>
> **四个真凶（都不是配置写错，是真实的不兼容）**：
> 1. ★**`media.c2.hal.selection` 默认是 `hidl`** —— 这就是追了两个阶段的
>    "解码器一个都没有"（#36）。HIDL Codec2 在 Android 15+ 已不可用
>    （hwservicemanager 被移除）。**与 crDroid 无关，AOSP 16 上同样如此**，
>    只是真机设备树都会设它。必须走 `PRODUCT_SYSTEM_EXT_PROPERTIES`
>    （该属性上下文 `codec2_config_prop`，vendor 无权设）。
> 2. ★**bpffs 的 SELinux 标签**（`patches/0007` 内核补丁）——
>    主线 bpffs 在 inode 创建时急切赋标签，Android 依赖的 genfscon
>    惰性路径匹配失效 → ClatCoordinator 逐字比对标签失败 → system_server
>    崩溃循环。用户态无法修（`chcon` 报 ENOTSUP，因为策略对 bpf 用 genfscon
>    而非 `fs_use_xattr`）。**这个坑对任何"Android on 新主线内核"都成立。**
> 3. ★**crDroid 的 `SetSafetyNetProps()`**（`property_service.cpp:1168`）
>    在解析 cmdline 之前硬写一整张表，把 `ro.debuggable`/`ro.adb.secure`/
>    `verifiedbootstate`/`flash.locked` 全部盖成"已锁定已验证 user 版"。
>    这让 `WITH_ADB_INSECURE`、`PRODUCT_SYSTEM_EXT_PROPERTIES`、cmdline
>    三条路改了都没用，极具迷惑性。开关 `SPOOF_SAFETYNET` 只在 eng 变体关，
>    故用 `scripts/crdroid-tree-fixes.py` 改默认值（eng 会关 dexpreopt，不可取）。
> 4. **AOSP 基座必须由设备树自己 inherit**（`full_base.mk`）——
>    `vendor/lineage/` 下全是补充配置。少了它构建"成功"但产出空壳
>    （`system.img` 29.8 MB、无 apex/app/services.jar）——**比构建失败更危险**。
>
> **已补上（M3 复核）**：s2idle 休眠其实一直是好的 —— init 的
> `write /sys/power/wake_lock` 从第一次就成功了，我把 `cat` 的
> "Permission denied"（节点是 `radio:wakelock` 0660，shell 读不了）
> 当成了"内容为空"。实测 uptime 21 分钟时 `suspend entry` 计数 **0**
> （修之前 45–60 秒必挂且醒不来）。
>
> **还欠的**：见首屏 M3 段的「M4 起点」——音频（tinymix 漏装，已补待构建）、
> WiFi（网络被框架永久禁用，解禁要密码）、蓝牙（装着但禁用中）。

> **★决策（2026-08-19）：硬件使能告一段落，下一步转 crDroid 移植。**
> 理由：剩下卡住的东西（App/媒体没声音）**不是硬件或内核问题**，
> 而是这棵手搓最小 AOSP 缺产品级配置 —— `MediaCodecList` 是空的
> （`/vendor/etc/media_codecs.xml` 原本不存在，拷进去仍空，
> `ro.media.xml_variant.*` 全未设）、`/system/media/audio/` 整个缺失
> （铃声/UI 音效一个没有）。这些是 Lineage/crDroid 设备树的标准组成部分，
> 换轨后大概率自动消失。详见 `docs/stage4-findings.md` #36。
> - **已定：直接上 crDroid**（不先过 Lineage）。执行案卷 `docs/stage6-crdroid.md`。
>   - crDroid `16.0` = LineageOS 23.2 布局，AOSP 基线 tag **`android-16.0.0_r4`**；
>     `lunch lineage_gaokun3-bp4a-userdebug`（release config `bp4a`，实名核实）。
>   - manifest 1180 个项目，本机只缺 `device/linaro/dragonboard`
>     → `manifests/local_manifest_gaokun3.xml`。
>   - ★**铃声缺口 crDroid 自带解决**（`vendor/lineage/audio/audio.mk` 装 44 个音频到
>     `product/media/audio/`）。
>   - ★**解码器缺口的最终结论（已解决）**：不是 `media_codecs.xml` 缺失，
>     也不是"servicemanager 的 declared 集"问题（这两个归因都被推翻了）。
>     真凶是 **`media.c2.hal.selection` 默认为 `hidl`**
>     （`frameworks/av/media/codec2/hal/common/HalSelection.cpp:57`），
>     而 HIDL Codec2 在 Android 15+ 已随 hwservicemanager 一起消失。
>     设成 `aidl` 后解码器从 0 → 66。详见首屏的 M2 段与 `docs/stage6-crdroid.md`。
>   - ★**mesa 管线一个都不能丢**：lineage-23.2 的 `external/mesa3d` 就是 AOSP 那份，
>     `BOARD_MESA3D_*` 是平行项目自带 mesa 仓库的机制，不是 Lineage 的
>     （作废 `docs/stage5-freedreno.md:212` 的猜测）。
>     ⚠️⚠️ **"两边是同一个 commit"这条结论是错的，2026-08-19 M3 当场推翻**：
>     `git log -1` 在两棵树上都给 `d4b6f1eba289…`，但那是 **`.git` 的 HEAD**，
>     而 Stage 5 的 mesa 是**直接铺在工作树上、从未提交**的上游 mesa
>     —— 于是这个对比必然给出假阳性。逐字节比才看得见真相：
>     crDroid 树内 = **mesa 25.3.0-devel**，Stage 5 补丁树 = **mesa 26.0.3**，
>     `git diff` 3791 个文件 / 36.5 万行，是真实的上游版本差
>     （`gl_shader_stage`→`mesa_shader_stage` 这类重命名、新增文件都在）。
>     ⚠️ 连带作废：早先那句"'mesa 26' 是被本地改过的 VERSION 文件误导"也是错的。
>     **正解 = 铺回归档树** `~/keep/mesa3d-patched.tar.zst`（M3 已这么做）：
>     它就是实测 SMMU fault=0 的那棵，patches/0004 v3 全在里面。
>     退路：`git checkout . && git clean -fd` 一句话回到 crDroid 原版 25.3。
>   - 构建机磁盘：**整棵删了 `~/aosp`**（157G → 451G 可用）。
>     "只删 out 腾到 264G"的算术不够 —— crDroid 源码+.repo≈190G + out 100–130G。
>     删之前已归档 `~/keep/`（super/ramdisk/turnip.so/mesa 全树，`sha256sum -c` 通过）。
> - **可平移的成果（换 ROM 不用重做）**：kb21 内核配置 +
>   `scripts/kernel-config-android.sh` 的断言、DTB（含 gpio174 触摸补丁）、
>   固件集与 audioreach 拓扑固件的**正确路径名**、mesa turnip
>   `apply-0004v3.py`（ANB 延迟绑定，纯 mesa 修复）、
>   `smmu-nostall.sh`（SMMU stall workaround）、
>   `audio-route.sh`（混音器路由）、蓝牙用 AOSP 原装 HAL 即可、
>   以及 docs/ 里全部踩坑记录。
> - **要重做的**：Lineage 布局的设备树、UEFI 引导集成（无 fastboot）、
>   整棵 ROM 的构建。参考 `docs/parallel-mainline-generic.md`（同款内核的
>   Lineage 系平行项目，可借它的 gaokun3 配置）。

> **Stage 4 音频/蓝牙已于 2026-08-19 完成（硬件层）**：
> 声卡 `SC8280XP-HUAWEI-GAOKUN3` 注册、内置扬声器实机出声（用户确认，
> 整曲播放通过）；蓝牙 adapter `state: ON`、地址从芯片读出、零崩溃。
> 三个 `=m` 断点（LPASS pinctrl ×2 + LPASS 时钟）+ 未编的 `SND_SOC_WSA883X`
> + 拓扑固件路径名 + `RT_GROUP_SCHED=y`（挡住蓝牙的 SCHED_FIFO）——
> 全部记在 `docs/stage4-findings.md` #33–#36。
> ⚠️ 起停爆音源是功放 BOOST 升压器（A/B 实听定案），默认已关。

> **Stage 5 GPU 战况（2026-08-19 凌晨）：★GPU 攻坚完成 —— Android 用硬件
> turnip 启动进桌面，SMMU fault 归零，帧读回正常。** 案卷
> `docs/stage5-freedreno.md` D4–D10，工具集 `scripts/gmu-forensics/`
> （**11 条会反复中招的坑，动手前先读 README**）。
> - **"GMU 必死"从头到尾不是电源管理问题**，是一条空指针放大链：
>   turnip 没实现 ANB 延迟绑定 → image 没内存、`iova` 恒 0 →
>   **GPU 往地址 0 写** → GPU SMMU translation fault →
>   本平台 fault 中断打不到 CPU → `SCTLR.CFCFG=1` 永久 stall →
>   AHB 总线 stall → CP 断粮 → `GX_BW_PERF_VOTE` 超时 → 看门狗 →
>   stall 拖住掉电（`cx gdsc didn't collapse`）→ 死循环。
>   `GX_BW_PERF_VOTE 超时`是**果**，不是因。
> - ★**真凶（D10 三探针实测定性）**：Android 的 libvulkan 走**延迟绑定**——
>   `vkCreateImage` 时不带 ANB，gralloc buffer 是在 `vkBindImageMemory2`
>   那一刻才用 `pNext` 的 `VkNativeBufferANDROID` 递进来的
>   （实测 pNext=`{NATIVE_BUFFER_ANDROID, BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR}`，
>   调用链 ANGLE → libvulkan → turnip）。mesa 那句
>   `/* TODO handle VkNativeBufferANDROID */` 说的就是这条路，**从没人实现**。
>   **权威修复 = `scripts/gmu-forensics/apply-0004v3.py`**
>   （patches/0004 的 v1/v2 都是错的，v2 的"安全跳过"正是残余 fault 的来源）。
>   编译只需 `m vulkan.freedreno`（约 1.5 分钟），部署
>   `scripts/gmu-forensics/deploy-turnip.sh`（overlayfs，**不刷 super**）。
> - **实测对比**：SMMU fault **66 → 0**；未绑定 image 建 view **96 → 0**；
>   `screencap` 从**永久卡死 → rc=0 出 3.27MB 正常图**（截图逐像素正确，
>   证明按 gralloc 真实 modifier 重算布局那步是对的）；
>   t=36s 进桌面，GMU 错误 0，`a6xx_recover` 0，桌面四进程 PID 稳定不变。
> - ★**D6 悬案的物理机制找到了**：fault 当场读 `GICD_ISPENDR22` 发现
>   SMMU 拉的是 **SPI 675 / 680**，而 DT 声明、内核注册并使能的是
>   **SPI 678/679**（`/proc/interrupts` 计数恒 0）。**内核在听 678，
>   硬件在喊 675** —— 不是"SMMU 不拉中断"也不是"内核没使能"。
>   ⚠️ 顺带作废我自己一度下的"DT 跳号正常"判断。
>   下一步可根治：改 DTB 的 gpu_smmu context interrupts（不用重编内核），
>   成功就能彻底丢掉轮询脚本。
> - **常驻 workaround**：`scripts/gmu-forensics/smmu-nostall.sh`（轮询清
>   `SCTLR.CFCFG` 让 fault 走 terminate + 抓 FSR/FAR/GICPEND）。
>   ⚠️⚠️ **只能扫 CB0/CB1（`NCB=2`）**：实现了几个 CB 看
>   `/proc/interrupts` 的 `arm-smmu-context-fault` 条数；扫到未实现的 CB
>   → external abort → **内核静默死亡**（Android 连续三次启动到
>   post-fs-data 后消失、无 tombstone 无 pstore 无 adb）。
> - **运维**：cmdline 已带 `androidboot.flash.locked=0
>   androidboot.verifiedbootstate=orange` → `adb remount` 走 overlayfs，
>   改 vendor 不用开构建机（每次重启挂回 ro，写前重跑 remount）。
>   **默认启动项已改成 Ubuntu**（Android 挂死拍电源键自动回落）→ 要进
>   Android 得在 Ubuntu 里 `bootctl set-oneshot …android.conf`
>   （deploy 脚本已内置中转）。adb 彻底不通时走
>   `scripts/gmu-forensics/overlay-rescue.sh` 离线读写 overlay
>   （scratch 是 super 里 4 段 extent 拼的 f2fs，要 dm-linear 拼回去，
>   且得用 `ubuntu-kb19` 启动项才有 f2fs）。
> - ⚠️ **旧结论作废**：属性名是 `debug.mesa.tu.debug`（不是 `debug.tu.debug`），
>   故 2026-08-18 前所有 tu_debug 实验旗标从未生效；
>   `msm.enable_preemption=0` 是有害参数（内核判断语义反转）已从 cmdline 删。
> - mesa 26 的 AOSP 构建管线已全套入库：`scripts/mesa-tool-fixes.py`
>   + `scripts/mesa-bp-merge.py` + `scripts/join_meson_continuations.py`
>   + `patches/0003..0006` + `device/huawei/gaokun3/mesa/`。
>   软渲染兜底仍在（`ro.hardware.vulkan=pastel` 一行可切回）。
> - **下一场**：音频 —— 内核链全 =y、ADSP 三兄弟 running、固件（含
>   audioreach-tplg.bin）进 ramdisk+vendor，但声卡未注册（macro/soundwire
>   的 deferred probe 不收敛，`suppress_bind_attrs` 封死手动补绑定）。
>   另有蓝牙（#30）、s2idle、以及可选的 DTB 中断根治。

> **Stage 4 触摸已于 2026-08-17 完成：触摸丝滑可用。**
> 根因是 gpio174（模式选择脚）无人驱动，见 `docs/stage4-findings.md` #26
> 和 `patches/0002-*.patch`。✅ 补丁已应用进 VM 内核树（kb18 起自带）。
>
> **Stage 4 WiFi 已于 2026-08-17 完成：冷启动免干预自动连网。**
> 内核 kb18（ath11k 全家 =y + PWRSEQ）+ 晚绑定 + goldfish wifi HAL +
> supplicant 配置 + 国内验证端点，全程见 `docs/stage4-findings.md`
> #28/#29。adb over TCP 已开（5555 端口，缓解 #27）。
> ⚠️ 重刷 userdata 后必须跑 `scripts/android-post-flash.sh`。
> 剩余：音频、蓝牙（#30，无 HCI HAL 暂禁用）、挂起/恢复（s2idle）、
> UCSI 拔插（#27）、Ubuntu 侧 DTB 触摸补丁（等 USB_STORAGE=y 或进 Ubuntu 手做）。

> **Stage 3 已于 2026-08-17 完成验收：Android 桌面完整渲染**
> （Launcher3 + SystemUI 稳定，1600×2560，截图为证）。
> 图形 = swangle 软渲染（Phase A）；Phase B 换 freedreno 见
> `docs/parallel-mainline-generic.md` 路线图。
> ⚠️ 已确认坑：闲置 52 秒自动 s2idle 休眠后醒不来（CLAUDE.md 预言的
> EC 挂起坑），临时用 `svc power stayon true` 顶着，Stage 4 正修。

> Stage 0 / Stage 1 已于 2026-08-13 完成验收。
> **Stage 2 已于 2026-08-17 完成验收：`adb shell` 通，Android 16 稳定运行**
> （`gaokun3 device product:aosp_gaokun3`，内核 7.2.0-rc2-gaokun3+）。
> 全部 12 个实测问题及修复见 `docs/stage2-findings.md`。
> Stage 3 起点：surfaceflinger 崩于 "couldn't find an OpenGL ES
> implementation"（mesa/gralloc/hwc 未装，abort message 见
> `docs/stage2-acceptance-live.txt`）。

---

## ⚠️ 给 AI 助手的强制规则

**这个平台没有任何 Android 移植的前人成果。** 训练数据里不存在这台机器上的
Android 相关知识。因此：

1. **任何具体的 kernel config 名、AOSP property 名、HAL 接口名、文件路径，
   必须从本地 checkout 的源码树里 grep 出来，并给出文件路径和行号。**
   不允许凭记忆给出。记忆里的名字在这个平台上大概率是错的或过时的。

2. **不确定就明说"我不确定，需要验证"**，不要给出听起来笃定的猜测。
   在这个项目里，一个自信的错误答案比"我不知道"贵得多——用户要花几小时
   才能发现你编的那个 config 项根本不存在。

3. **实机行为以 dmesg / logcat / 用户的实际观察为准，与你的判断冲突时以实机为准。**

4. 涉及 sc8280xp 硬件细节时，优先查 `refs/linux-gaokun` 和 `refs/jhovold-linux`；
   涉及 AOSP-on-mainline 的组织方式时，优先查 `refs/aospm-*`。

---

## 硬件事实

| 项目 | 值 |
|---|---|
| SoC | Qualcomm Snapdragon 8cx Gen 3 / **SC8280XP** |
| 设备代号 | gaokun3（8cx Gen 3 机型）；gaokun2 是另一套 EC 协议 |
| 型号 | **HUAWEI GK-W7X，SKU C233，2022 款，CSOT 面板，触摸固件 `41 07`** |
| BIOS | **2.16**（2023-01-31）⚠️ **不要升级到 2.17** —— 触摸的 SPI 总线和 GPIO 编号两版完全不同，上游驱动是按 2.16 开发的（reset=99 / IRQ=175 / 12 MHz）|
| GPU | **Adreno 690** —— mesa freedreno + turnip，主线支持成熟 |
| 屏幕 | **Himax HX83121A / ppc357db11 WQXGA**，MIPI-DSI。**与三星 Galaxy Tab S7 FE 同款面板** |
| WiFi/BT | WCN6855 —— ath11k + hci_qca，主线驱动 |
| EC | 华为自研，主线驱动 6.15 进；UCSI 6.16；**DSI 面板 7.1 进** |
| 存储 | NVMe（**不是 UFS**，不是手机那套分区布局） |
| 引导 | **UEFI，不是 fastboot**。可关 Secure Boot。GRUB/systemd-boot 加载 |
| 虚拟化 | KVM/EL2 可用 |
| 已知不支持 | 指纹（FocalTech FTE7001）、TPM。**s2idle 已实测：挂得下去、醒不回来、约 20–40s 后整机复位；Ubuntu 同样复现 → 内核/EC 缺陷**（M4 定性）。深度休眠(S4)仍未测 |

## 关键约束（每次都要记住）

- **没有高通 Android BSP。** 8cx 系列从来只发 Windows/Linux 驱动。
  不存在可扒的 vendor blob，所有 HAL 必须基于主线内核自建。
  → 路线只能是 **AOSP on mainline**，参考 aospm 项目。

- **不是 fastboot 设备。** 所有假设 `fastboot flash` / `by-name` 软链接 /
  A/B 槽位的 AOSP 常规流程都要改写。
  ⚠️ **fstab 用 PARTUUID，不能用 PARTLABEL** —— 实测内置盘上 Windows 建的分区
  PARTLABEL 全都是 `Basic data partition`，不唯一。见 `docs/hw-inventory.md` 第 8 节。

- **没有暴露的串口。** 早期启动失败 = 纯黑屏零信息，adb 要等 init 起来才有。
  → ✅ **已解决，走 `efi_pstore`（EFI 变量），不是 ramoops。**
  **ramoops 在这台机器上不可能工作** —— 固件每次复位都重新初始化 DRAM，
  低位 `0xae900000` 和高位 `0x865d38000` 都实测过，内容一律不存活。
  崩溃日志现在会自动落到 `/var/lib/systemd/pstore/`。
  详见 `docs/hw-inventory.md` 第 7bis 节，工具 `scripts/pstore-ctl.sh`。

- **无 modem。** aospm 的 libqril/qrild/qrtr 那一套全部跳过。

- **arm64 原生。** 手游 arm64-v8a 包直接跑，不需要任何转译层。

## 环境

- **编译机：Dell G15（x86_64 Linux）** —— AOSP 编译需要 ~16GB+ RAM、250–400GB 磁盘。
  不要在 Ego 上编译 AOSP。
- **目标机 A：** 保持可用状态（Windows 或稳定 Linux），作为参照和日常用
- **目标机 B：** 随便刷的实验机
- 两台机器的意义：A 永远能开机，用来对比"正常应该是什么样"

---

## 本地参考树（clone 到 `refs/` 下，供 AI 直接读源码）

```
refs/linux-gaokun/           github.com/right-0903/linux-gaokun          本机内核核心
refs/matebook-e-go-linux/    github.com/whitelewi1-ctrl/matebook-e-go-linux   GK-W7X patch + GRUB 配置
refs/boot-works/             github.com/matalama80td3l/matebook-e-go-boot-works  面板驱动
refs/jhovold-linux/          github.com/jhovold/linux (wip/sc8280xp-6.16)  ⚠️ 已停更，仅作历史对照
refs/gaokun-buildbot/        github.com/KawaiiHachimi/linux-gaokun-buildbot  ⭐ 现役内核基线
refs/egotouchrev-linux/      github.com/chiyuki0325/EGoTouchRev-Linux    触摸 SPI 驱动
refs/aospm-device-sdm845/    github.com/aospm/android_device_generic_sdm845    ⭐ 设备树模板
refs/aospm-manifests/        github.com/aospm/android_local_manifests
refs/aospm-system-core/      github.com/aospm/platform_system_core       看 diff 知道要改什么
refs/aospm-tinyhal/          github.com/aospm/tinyhal                    音频 HAL
```

> ⚠️ **jhovold 树已不是基线。** 它停在 6.16（2025-09 最后推送），
> 缺 HX83121A 面板驱动（7.1 才进主线）。现役基线是
> **mainline v7.2-rc2 + `refs/gaokun-buildbot/patches/` 20 个补丁**。

固件来源：`matebook-e-go/uup-drivers-sc8280xp`（Windows 驱动扒 blob）+
linux-firmware ≥ **20241210**

**Stage 2 打 vendor 分区要抓的固件（实测 dmesg 加载路径）：**

```
qcom/a660_sqe.fw          qcom/a660_gmu.bin              GPU (Adreno 690)
qca/wcnhpbtfw21.tlv       qca/wcnhpnv21g.bin             蓝牙 WCN6855
qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn              ADSP
qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn              CDSP
qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn              SLPI
```

华为专有路径下那三个 `.mbn` 不在 linux-firmware 里，必须自己带。

---

## 阶段计划与验收

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| **0** ✅ | 主线 Linux 跑通 + 配好崩溃日志 + 采集素材 | 全部通过（pstore 走 efi_pstore） |
| **1** ✅ | 内核转 Android 配置 | 全部通过（UDC 出现，主机端 `configured` 枚举）|
| **2** ✅ | 引导链 + AOSP 启动 | **全部通过**（2026-08-17）：adb shell 通，keystore2/zygote/adbd 稳定运行。12 个问题的完整记录见 `docs/stage2-findings.md` |
| **3** ✅ | 图形栈（minigbm + drm_hwcomposer + swangle） | **全部通过**（2026-08-17）：桌面完整渲染。freedreno 留待 Phase B |
| **4** | 输入 / 音频 / WiFi / 电源 | 触摸可用、有声音、能联网 |
| **5** | 游戏适配 | 目标游戏能启动并稳定运行 |

**Stage 0 必须采集并记录在 `docs/hw-inventory.md` 的东西：**
- `.config` 中所有 QCOM / ath11k / hid 相关项
- `dmesg | grep -i firmware` 的完整固件加载路径
- **ALSA UCM2 配置文件**（Stage 4 要翻译成 mixer_paths.xml）
- `modetest` 完整输出：connector 名、plane 数量、**支持的 format 和 modifier**
  （Stage 3 配 minigbm 的关键依据）
- 触摸屏 / 键盘 / 触控板的 evdev 名和 evtest 输出
- 触摸屏走 SPI 还是 I2C

---

## 已知坑

- 触摸屏 I2C 模式有间歇性失灵，SPI 模式更稳
- **触摸 IC 的工作模式由 gpio174 在固件重载瞬间的电平决定**（低=SPI 高=I2C HID），
  上游两棵 DTS 都没配这个脚，全靠 UEFI 遗留电平碰运气；显示复位还会静默触发
  固件重载。症状是"触摸随机死亡/幽灵触点风暴/驱动探测成功但全聋"。
  修复=pinctrl 恒拉低，见 `patches/0002-*.patch` + `docs/stage4-findings.md` #26。
  空闲 IRQ 速率是状态指纹：≈显示扫描率=正常；0=IC 停摆；乱=模式错乱。
- **`timeout N getevent > 文件` 会因块缓冲丢光全部输出**——采集 evdev 要用
  `cat /dev/input/eventX` 录二进制再离线解码。见 `docs/stage4-findings.md` #26 方法论。
- EC 挂起/恢复：Android 的 suspend 模型比 Linux 激进，预期这里会先炸
- ~~DSI panel 的 KMS plane 数量少时 drm_hwcomposer 会 fallback 到 GPU 合成~~
  ✅ **担心不成立。** 实测 **25 个 plane / 6 个 CRTC**，硬件合成资源充裕。
  modifier 只有 `LINEAR` 和 `QCOM_COMPRESSED`(UBWC, `0x500000000000001`)，
  支持 UBWC 的 format 见 `docs/hw-inventory.md` 第 3 节 —— 那就是 minigbm 的配置依据。

- **UCSI 有缺陷**（`refs/linux-gaokun/README.MD:86-87`），常见
  `error -ETIMEDOUT: PPM init failed`，此时 `/sys/class/typec/` 为空。
  ✅ 但**不影响 adb**：`dr_mode = "otg"` 无 role 源时落到 device 侧，
  USB 数据通路不经 UCSI。代价是只有 high-speed，SuperSpeed 需要 UCSI 切 orientation。

- **DT label 编号与物理地址不对应**：`usb_0` 是 `a6f8800`，`usb_1` 才是 `a8f8800`。
  改 dwc3 前务必 `readlink -f /sys/block/sda` 确认启动介质在哪个控制器上。

- **`super.img` 是 Android sparse 格式，不能 `dd`** —— 必须 `simg2img` 展开。
  头部魔数 `0xED26FF3A` 是 sparse 标志，**不是** LP metadata。误 dd 会让 init
  读不到元数据、挂载失败后主动复位，且不留任何日志。见 `docs/stage2-findings.md` 第 1 节。

- **`CONFIG_SECURITY_SELINUX=y` 不等于 SELinux 已启用** —— 还必须出现在
  `CONFIG_LSM` 字符串里。buildbot 默认值只有 apparmor，导致 selinuxfs 从不注册、
  Android init 静默死亡。**只看 config 会误判，必须查 `/sys/kernel/security/lsm`。**

- **Android init 失败时是主动 `reboot()`，不是 panic** —— 所以 pstore 抓不到。
  抓日志要靠改造 ramdisk 写内置盘 ESP，方法见 `docs/stage2-findings.md` 第 5 节。
  `androidboot.init_fatal_panic=true` 可以把 LOG(FATAL) 类失败转成真 panic 走 pstore，
  但服务级失败（`reboot_on_failure`）仍是正常 shutdown，两条通路都要布。
- **cgroup v1 在 6.12+ 拆到 `*_V1` 选项后面且默认关** —— `CONFIG_CPUSETS=y` 只给 v2。
  Android 的 cgroups.json 要求 cpuset 走 v1，缺 `CONFIG_CPUSETS_V1` 时
  `SetupCgroups` 失败 → 所有服务起不来 → `bootstrap-apexd-failed` 复位。
  一并要 `MEMCG_V1` / `UCLAMP_TASK(_GROUP)`（task_profiles.json 引用）。
  见 `docs/stage2-findings.md` 第 8 节。
- 游戏多走 GLES，freedreno GL 路径和 zink-over-turnip 都试，先通再选

## 捷径备忘

- **面板与 Galaxy Tab S7 FE（gts7fe / SM-T733）同款** → DPI、时序、背光曲线
  可直接参考 Tab S7 FE 的 AOSP 设备树
- Stage 3 卡死时的止损方案：Cuttlefish（`cvd start --gpu_mode=gfxstream`），
  KVM 可用，是真 Android VM 不是容器

## 协作

- gaokun 社区（Linux 侧）：面板、EC、休眠、触摸的问题问他们
- aospm 社区（Android-on-mainline 侧）：HAL、设备树组织方式问他们
- ~~这两个圈子此前没有交集，本项目是第一个连接点~~
  **已有平行项目**：LineageOS 系 mainline-generic 正在做 gaokun3 live-ISO
  （同款 buildbot 内核），双方结论交叉验证一致。他们的 config fragment
  和模块清单是我们 Stage 3/4 的路线图，见 `docs/parallel-mainline-generic.md`。
