# 待办清单

最后更新：2026-08-20（v0.2.0-alpha 发布之后）

这份清单的排序原则是**用户能不能感觉到**，而不是有趣程度。每条都尽量写出
**具体的第一步** —— 没有第一步的条目只是愿望，不是待办。

状态表与公开招募项在 [`../README.md`](../README.md)；每条的证据在
[`stage4-findings.md`](stage4-findings.md) 等案卷里。

---

## A. 用户能感觉到的缺口

### A1. 音频与蓝牙长期运行后死锁 ⚠️ 最高优先
用户实机报告，我未复现、未定位（[#38](stage4-findings.md)）。
两者共用同一条到 DSP 的 QRTR/FastRPC 通路，而这条通路上**已经实测到过**
会话级卡死（使能光感会污染整个 SSC 会话）。

**第一步**：拿到复现条件（多久、什么负载、是同时死还是各自死）。
死锁当场跑 `gaokun3-qrtr-lookup`（已随镜像发布）与正常时对比服务表 ——
少了哪个服务就指向哪个 DSP。⚠️ 别把 `Handover signaled` 当崩溃证据，
那是良性噪声（#37 已用对照实验证明）。

### A2. 硬件视频解码（Venus）— 内核这一半 ✅ 已通，Android 侧待做
详见 [#41](stage4-findings.md)。实机验证：`/dev/video0` = `qcom-venus-decoder`、
`/dev/video1` = `qcom-venus-encoder`，`aa00000.video-codec` 绑上 `qcom-venus`，
`abf0000.clock-controller` 绑上 `sm8350-videocc`，延迟 probe 队列空，
固件加载失败 0 行。★固件不用找 —— `qcvss8280.mbn` 我们一直在装，
只是当初被标成"语音服务"（**VSS = Video SubSystem**）。

打了 8 个补丁里的 7 个（0014 是纯格式清理且主线已分叉，跳过）。
两个非直觉的坑：**必须关 `CONFIG_VIDEO_QCOM_IRIS`**（否则 venus 编不过，
报错却指向 core.c 像补丁打错），以及整条媒体链上**五个 `=m`** 要拉成 `=y`。

**剩下的一半**：Android 需要一个跟 V4L2 说话的 Codec2 组件，现有 66 个解码器
仍全是软解。★ **`external/v4l2_codec2` 本来就在 crDroid 的 manifest 里**
（`LineageOS/android_external_v4l2_codec2`），不必新增仓库。

**第一步**（配方来自 `external/v4l2_codec2/README.md`，三个前提已在设备上核实）：
`PRODUCT_PACKAGES += android.hardware.media.c2-service-v4l2 libv4l2_codec2_vendor_allocator`，
装 `media_codecs_c2.xml` 到 `/vendor/etc/`，设两条
`ro.vendor.v4l2_codec2.*_concurrent_instances`，再看
`service list | grep media.c2` 有没有出现 `IComponentStore/default`
以及 `dumpsys media.player` 里的 `c2.v4l2.*`。

已核实的三点：
* ✅ **不会撞名** —— 设备上目前只有 `IComponentStore/**software**`，
  v4l2 HAL 要的 `IComponentStore/default` 是空的。
* ✅ `media.c2.hal.selection` 已经是 `aidl`（#36 那一仗的成果）。
* ⚠️★ **poolmask 不能照抄 README 的 `0xf50000`** —— 那是 ION 的值，
  而本机**没有 ION**（`/dev/ion` 不存在、`CONFIG_ION` 也不在）。
  我们有的是 DMABUF heaps（`system` / `linux,cma` / `default_cma_region`），
  所以要用 BLOB 的 **`0xfc0000`**。抄错这一个数字大概率就是"组件在、一解码就崩"。

⚠️ 别在没法看视频的时候合这个 —— 它替换的是整条媒体解码通路，
弄坏了比现在的软解更糟。

### A3. 自动亮度（环境光）— 已收敛到一处嫌疑
详见 [#43](stage4-findings.md)。把两份出厂配置逐字段对照之后：

* ❌ **"DSP 够不到 PMIC 电源轨"排除** —— 能用的加速度计走的是**同一条**
  `/pmic/client/sensor_vddio`。
* ❌ **"SLPI 用不了主 SoC 的 TLMM 脚做中断"也站不住** —— 加速度计同样
  `irq_is_chip_pin=1`。
* ★ **嫌疑收敛到 SLPI 侧 I2C 实例号：能用的是 `bus_instance=1`，
  光感是 `bus_instance=5`**（其次 `rail_on_state` 1 vs 2）。
  而且它正好解释"污染整个 SSC 会话"：往没起来的 I2C 控制器发事务会在 SEE 里挂住。

**第一步**：找 SLPI 侧 I2C 实例号 → 实际 QUP 控制器的映射，
再比对 AP 的 DTS 里哪些 i2c 节点是开着的，看是不是 AP 把 instance 5 占了。

### A4. 耳机口不出声 ✅ 已修，待用户插一次耳机验收
详见 [#40](stage4-findings.md)。**三个阻塞点，全部定位并修复**，内核侧一点没缺。

1. **RX 插值器链从来没接上** —— 这是"后端打不开"的真凶。我原先只设了
   `RX_MACRO RXn MUX`，而 rx-macro 内部还有一级插值器停在默认值
   （`RX INTn_1 MIX1 INP0 = ZERO`、`RX INTn DEM MUX = NORMAL_DSM_OUT`、
   `CLSH/LO Switch = Off`）。DAPM 路径不完整 → PCM open 失败，**内核不打任何日志**。
   补齐 9 个控件后当场通：`tinyplay -D 0 -d 0` rc=0、`pcm0p` `state: RUNNING`、
   hw_ptr 2 秒前进 48960 帧/秒（实时 48 kHz）。
   ⚠️ 我为此下过的错结论（"拓扑缺 APM 图 / soundwire 没上电 / q6apm 静默失败"）
   三个候选一个都不是 —— 案卷里保留了它是怎么错的。
2. **策略里没声明耳机设备** → 已加 `WIRED_HEADPHONE` / `WIRED_HEADSET` /
   `IN_WIRED_HEADSET`（`CARD_0_DEV_0` / `_2`）。
3. **框架找的是 `/sys/class/switch/h2w`，本机 ENOENT** →
   `config_useDevInputEventForAudioJack = true`，改从 evdev 取
   `SW_HEADPHONE_INSERT`。那个 input 设备本来就在、而且已经在被读。

配方来源：救援 Ubuntu 上的上游 ALSA UCM2 —— **上游用
`Regex "HUAWEI.*MateBook E.*"` 把本机直接 include 成 ThinkPad X13s**。
★ 方法论：本机凡是 LPASS 音频的事，先去抄那个目录，别自己推 DAPM 图。

**构建前已用 overlayfs 在设备上验过**：45 个控件零失败、策略被 audioserver 接受
（三个新端口带地址出现在 `dumpsys media.audio_policy`）、扬声器无回归。
**仍需用户做的**：新 ROM 起来后插一次耳机 —— 框架资源只能构建期 overlay，
且 `WiredAccessoryManager` 在 SystemServer 启动时只读一次，框架层也没有
插拔模拟命令可用。

### A9. ★s2idle：复位与"醒不回来"是**两个**问题，前者已找到绕过手段
案卷 [#50](stage4-findings.md) / [#51](stage4-findings.md)（2026-08-21 大幅改写；
#45–#49 的**归因**多已作废，实测数据仍保留）。

**★ 本轮确立的三件事**
1. **复位发生在挂起【进入】时，不是唤醒时。** 判据：RTC 闹钟设 180 秒，机器仍在
   几秒内复位。⇒ M4/#47 的"死在任何唤醒"是错的，之前所有"解绑某某"方向注定无效。
2. **`pm_test` 是本平台最好的复现器，不是"无效判据"。**
   `pm_test=freezer` rc=0、`pm_test=devices` 复位 ⇒ 故障夹在
   `dpm_suspend_start()` + `dpm_suspend_noirq()` 之内。
   它 5 秒自动返回、**不需要唤醒源**，因此**安全**（不会把机器睡死）。
3. **量化测量已建立**（每轮 = 一次开机，`pm_test=devices` 跑到失败或跑满 10 次，
   **只数 `rc=0`**）：

| 配置 | 有通过的开机 / 总开机 |
|---|---|
| 基线（`pcie_aspm.policy=powersupersave` + APST 开） | **0 / 约 16** |
| 只 `pcie_aspm=off` | 0 / 1 |
| 只 APST 关 | 0 / 1（另有一次挂死） |
| 去掉策略（ASPM 默认）+ APST 关 | 0 / 1 |
| **`pcie_aspm=off` + APST 关** | **3 / 5**（其中一轮连过 10/10） |

   ⇒ 两个都必需，且**光去掉我们自己加的 `powersupersave` 策略不够**。
   差异是真的（p≈0.002），但**不是确定性修复**。

**下一步（都不需要编内核）**
* ⬜ ★ **找那个"开机时确定下来的因素"** —— 这是唯一还有把握推进的方向。
  同一次开机内行为高度一致（连过 10 次 或 第 1 次就死），跨开机才翻转。
  做法：固定用那个组合配置连开机若干次，**每次都记录完整设备绑定清单 +
  `/sys/class/wakeup/` + dmesg**，然后比对"能过"与"不能过"的开机。
* ⬜ 只有弄清上面这层，才谈得上是否把 `pcie_aspm=off` 写进发版 cmdline。
  ⚠️ 现在就写会让待机变成"有时能睡、有时炸"，**比确定不能睡更糟**。
* ⬜ 若最终确认与 **NVMe** 有关：正解是给这块盘加 `NVME_QUIRK_NO_APST` /
  `NVME_QUIRK_SIMPLE_SUSPEND` quirk，而不是让所有用户加 cmdline。
  先 `nvme id-ctrl` 记下 vendor/device id 与 APST 表。
* ⬜ 复位解决后再攻 **"醒不回来"** —— M4 描述的原始症状，
  本轮唯一那次成功睡进去的观测（#39）就是死在这一步。

**⚠️ 判据纪律（本轮最贵的教训）**
★ 这个故障的**单次失败率约 93%**，所以"改一次、试一次、炸了"对任何配置都是
大概率事件，**几乎没有证据力**。#39/#43 用同一条 cmdline 一正一反，
把我带偏了两轮。**任何结论都必须来自"跑到失败或跑满 10 次"的循环。**
⚠️ 工具本身也有坑：`pm_test` 周期后会留 pending 唤醒事件，紧接着再挂起会
**立刻 `-EBUSY` 返回**；第一版脚本把非零返回当成"通过"，报出了假的 10/10。
判据是**耗时**（真实周期 5–8 秒，`-EBUSY` 是 0–1 秒）。
★ **工具已入库**：`scripts/s2idle/`（`s2fp.sh` 采开机指纹+循环、
`s2cmp.sh` 比对、`s2loop.sh` 轻量循环、`sx.sh` 安全单次挂起）。
那个 README 里写了三条动手前必读的坑，别跳过。

**⚠️ 靶场纪律（本轮让用户按了三次电源键，是我的问题）**
* **"醒不回来"没解决之前，一律不跑真实挂起** —— `pm_test=devices` 够用且安全。
* 实验用的 BLS 条目**加 `panic=10`**：内核已开 `CONFIG_DPM_WATCHDOG`，
  设备回调卡住会 panic，配 `panic=10` 就能自动重启回默认项。
* 改 **DTB / 引导链**这类可能连 userspace 都到不了的实验，
  动手前先明说"可能要按一次电源键"。
* ★ "stay 模式"（脚本只准备、不挂起）让机器停在救援 Ubuntu 且 ssh 可达，
  一次开机能连做多个实验，效率高很多。

**已被实测否掉的假说**（别再重试）：EC 的 PM 回调 / EC 的中断 / **EC 本身（解绑）** /
Venus / geni I²C 的 noirq 重构 / PDC 唤醒映射 / **我们全部 20 个 buildbot 补丁**
（零补丁 v7.2-rc2 同样复位）/ cpuidle 深空闲态 / 硬件看门狗 /
音频+USB+三个 remoteproc+ucsi 全解绑。

⚠️ 原 A9 里"问题 1 = ath11k 的 suspend 回调卡死"那条**并未被否定，但优先级降低**：
它是 WiFi 关联状态下挂起路上的一道坎，而本轮的复位在它之前就发生了。

### A5. 恢复出厂设置不起作用
设置里那条路走 misc 的 BCB + recovery，而本机没有可用 recovery
（[#39](stage4-findings.md)）。实机证据：misc 里躺着一条没人消费的 `boot-recovery`。

**现在的替代**：从救援 Linux `mkfs.ext4 -F /dev/disk/by-partlabel/userdata`。
**真正的修法**：见 B3（EFI 加载器）或让 recovery 能启动（已搁置）。

### A6. USB-C 外接显示（UCSI）
`PPM init failed -ETIMEDOUT`，本机主线已知缺陷，`/sys/class/typec/` 是空的。
代价还包括 USB 只有 high-speed（SuperSpeed 需要 UCSI 切 orientation）。

### A8. 设备的 WAN 吞吐只有 PC 的 1/20（原因未定）
详见 [#44](stage4-findings.md)。⚠️ **不是 WiFi、不是 ath11k** ——
那条归因我下错过并已推翻：ping 网关 **0% 丢包**（1400 字节大包也是），
从本机拉 200 MB 跑到 **61.7 MB/s**，全程 `msdu_done` 新增 **0**。

真正剩下的问题：设备从 R2 拉只有 **1–2 MB/s**，而同一网络里的 PC 拉同一个
URL 是 **36.9 MB/s**。对"用户走系统内 OTA 升级"有实际影响（1 GB 要二十多分钟）。

**第一步**：在设备上对同一个 URL 抓一次 `ss -ti` 看拥塞窗口与重传，
再换一个不同 CDN 的大文件对照 —— 先分清是"到 Cloudflare 这条路"还是
"设备的 TCP 行为"。

### A7. 摄像头
完全没碰。

---

## B. 工程债与正确性

### B1. SELinux 转 enforcing
现在是 `permissive`。影响 Play Integrity 与部分带反作弊的游戏。
需要写 policy 的至少有：`hexagonrpcd`、sensors HAL、`audioroute`、`smmustall`。
logcat 里现成一串 `avc: denied` 就是清单。

**第一步**：把现有 denial 收集成 `.te`，先让 `hexagonrpcd` 与 sensors HAL 干净。

### B2. 真温控 HAL
现在是 AOSP mock（温度恒定 30.1/30.2），框架完全没有真实温控感知。
⚠️★ **换成读 `/sys/class/thermal` 的真 HAL 时必须同时改阈值** ——
mock 报的 skin/battery SHUTDOWN 阈值只有 **36 °C**，而
`ThermalManagerService.shutdownIfNeeded()` 到 SHUTDOWN 会直接
`powerManager.shutdown()`。现在因为 mock 值恒定打不到，**换真 HAL 会开机
几分钟就自动关机**。

### B3. 自研 EFI 加载器（规范化的最后一段）
读 `misc` 的 `bootloader_control` 选槽 + 解析 Android boot 镜像 +
装 initrd/DTB 协议。做完之后：
* postinstall 钩子与 ESP 上的派生文件**全部可以退役**
* BCB 能被消费 → `adb reboot recovery` 与恢复出厂设置才有可能工作
* 是 AVB/verified boot 的前提

**安全阀**：用 systemd-boot 的 `efi` 指令 chainload 它 —— 起不来就在菜单里
选别的，救援 Linux 那条路一个字节都不动。
⚠️ 这是唯一一个"写坏就要人到机器旁"的部件，别在没有安全阀的情况下动它。

### B4. LiveCD 打包
`scripts/install-gaokun3.sh` **从未端到端跑过**。它就是 LiveCD 的内核，
但没有人用它从零装过一台机器。

**第一步**：在一台可牺牲的机器（或本机，数据已备份）上真跑一次。
在那之前，"别人能装"这件事是未经验证的。

### B5. 发版流程固化成脚本 ✅ 已做
[`scripts/release.sh`](../scripts/release.sh)。它存在的理由是里面那几条断言，
每一条都对应一次真实事故：**一次 `m bacon superimage`**（分两次调用会得到两个
build stamp，OTA 包与安装用的 super 互不相认）、**清单 `timestamp` 必须等于
`ro.build.date.utc`**（否则装上后 Updater 永远显示"有更新"）、
**产物先传、清单最后传**。另外加了一条本轮新学到的：
**boot.img 里的 kernel sha256 必须与 `prebuilt-boot/vmlinuz.efi` 相同**。

`--stage-only` 把产物传到 `staging/<ver>/` 而不更新 `ota/gaokun3.json` ——
可以先在自己机器上验一版而不惊动任何用户。**发布是对外动作，应该是显式的一步。**

### B6. GPU SMMU 中断根治
实际 DT 是全局 672/673、context bank 从 678 起；而硬件拉的是 675/680，
其中 680 被分给 CB2、675 整张表里根本没有。很像 CB 起始偏移就错了。
⚠️ 但只凭"675/680 挂起"推不出正确映射，而且**改错了没有任何征兆**
（只是继续收不到 fault）。做成之后可以丢掉常驻的 `smmu-nostall.sh` 轮询。

### B7. 救援 Ubuntu 瘦身
现在 24.6 GiB，一个最小 rootfs 1–2 GiB 就够。刚腾出的 63.9 GiB 未分配空间
让这件事不再紧迫，但它仍是本机最胖的一块。
⚠️ 别把它换掉 —— recovery 给不了 `sgdisk`/`resize2fs`/sshd，而这轮重新分区
正是靠它远程完成的。

### B8. `invalid volume index range in the curve` ×12（既有，非回归）
每次 audioserver 启动都吐 12 条 `E APM_AudioPolicyManager: invalid volume index
range in the curve:`（后面是空的，连哪条曲线都没说）。
**确认与耳机改动无关**：干净 A/B，旧策略 12 条、新策略 12 条。
来源应在 `audio_policy_volumes.xml` / `default_volume_tables.xml`（两份都是从
`frameworks/av/services/audiopolicy/config/` 原样拷的）与本机 `devicePorts`
的交集上。目前没有可观测的功能损害，故只记不修。

**第一步**：给那条日志找出打印点（`EngineBase`/`VolumeCurve`），看它校验的是
哪个字段，再对照我们装进去的两份 XML。

---

## C. 上游或硬件层面（本地做不了）

* **待机（s2idle resume）** —— 挂得下去、醒不回来，随后整机复位。
  **Ubuntu 上同款内核完全复现** → 内核/EC 缺陷，不是 Android 的问题。
  第一步是编一个带 `CONFIG_PM_DEBUG` 的内核（现在 `/sys/power/pm_test` 不存在），
  否则无法二分。三个元凶（himax / 三个 remoteproc / EC 驱动本身）已逐个排除。
* **磁力计** —— 本机**没有这个硬件**（SSC 亲口回答），所以没有指南针、
  没有 9 轴融合。不是缺驱动。
* **指纹（FocalTech FTE7001）、TPM** —— 没有任何驱动存在。
* **出厂传感器校准** —— 存在本机 Windows 的 DriverData 里、不在任何驱动包中，
  而 Windows 已抹除 → **永久丢失**。实测无害（单位矩阵恰好与面板方向一致），
  只影响 bias 精度。⚠️ 给还留着 Windows 的人：先把那个 registry 目录拷出来。

---

## D. 运维与安全（需要你动手）

1. ⚠️★ **轮换 R2 的 S3 密钥** —— 它们在聊天记录里出现过多次。
2. ⚠️ **把 Azure NSG 的 22 端口锁到你的出口 IP** —— 构建机是静态公网 IP，
   而它曾进过 git 历史（已 filter-branch 抹掉并强推，但 GitHub 仍保留旧对象）。
3. 构建机用完立刻 `az vm deallocate` 并**取真实退出码**（`| tail` 会吞掉失败）。
   ★ 大文件传输**走 R2 中转**，不要让按分钟计费的构建机干等：
   本轮直连 1 MB/s（2.7 GB 要 45 分钟）vs 上传 R2 43 MB/s（27 秒）。

### D4. ★boot_control HAL 把"默认启动项=救援系统"这条安全网覆盖掉了
`docs/INSTALL.md` 承诺"Android 挂死 → 拍电源键 → 自动回落到可远程接入的系统"，
安装器也确实把 `default` 写成救援 Ubuntu。但 boot_control HAL **每次 Android
启动都把当前槽位镜像进 `loader.conf`**（M6 的设计），于是首次进 Android 之后
`default` 就变成 `*-android-b.conf` —— **安全网静默失效**，
而症状只是"`adb reboot` 本想去 Ubuntu 却又回到 Android"。

★ 现在有更好的解法了：[#42](stage4-findings.md) 证明 **Android 能写 EFI 变量**，
`LoaderEntryOneShot` 可写可回读。

**第一步**：让 HAL 只维护槽位信息、把 `default` 永远留给救援系统，
需要切槽时写 oneshot 而不是改 default。顺带给设备加一个
`gaokun3-reboot-to-rescue` 小工具（写 oneshot + reboot），
远程救援就不再依赖 ESP 手术。

---

## E. 明确搁置（记录理由，不是忘了）

* **recovery** —— 启动即复位循环（[#39](stage4-findings.md)）。
  现阶段意义不大：sideload 被系统内 OTA 覆盖，而调试它需要人反复到机器旁
  （本机没有串口、recovery 没有网络栈、pstore 对这类失败无效）。
  真要做，**第一步是把 USB adb 在 recovery 里弄通**，那是唯一能看见内部的通道。
* **fastboot** —— bootloader 级**不可能**（固件是 UEFI，不是 fastboot 设备）。
  用户态的 `fastbootd` 住在 recovery 的 ramdisk 里，所以随 recovery 一起搁置。
* **GMS / Play 商店** —— 用户未提出需求。
* **突破原神 1080×1728 的渲染上限** —— 那是游戏按**设备白名单**给的档位，
  不是本机的技术限制。要突破只能伪装机型，**有账号风险**，留给用户决定。
