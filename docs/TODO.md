# 待办清单

最后更新：2026-08-22（v0.3.0-alpha 发布之后）

这份清单的排序原则是**用户能不能感觉到**，而不是有趣程度。每条都尽量写出
**具体的第一步** —— 没有第一步的条目只是愿望，不是待办。

状态表与公开招募项在 [`../README.md`](../README.md)；每条的证据在
[`stage4-findings.md`](stage4-findings.md) 等案卷里。

---

## A. 用户能感觉到的缺口

### A0. ⚠️★ 普通应用能把内核 panic 掉 —— 已定位，补丁待构建
案卷 [#58](stage4-findings.md)。用户报的「切到设置就卡死」**不是卡死，是内核
panic**：`kernel BUG at drivers/gpu/drm/drm_crtc.c:161`。

`drm_crtc` 的 `fence_to_crtc()` 在回调里**再查一遍** `fence->ops`，而
`dma_fence_signal_timestamp_locked()` 会在 signal 时把没有 `.release`/`.wait`
的 ops（`drm_crtc_fence_ops` 正是）置 NULL —— 两个 name helper 是"先取 ops 再
回调"，中间那个窗口一旦被 vblank 撞上就 `BUG()`。CRTC out-fence 每 vblank 都
signal（本机 120 Hz），Android 又在不停查 fence 名字。
两次实测事件分属两个不同的 app、两条不同的 ioctl 路径 ⇒ **与"设置"无关**。

★ 已确认**不是"升级内核就好"**：摘 ops 那套的四个提交相对 `v7.2-rc2` 全是
`behind`（已在我们内核里），而 master 的那个 `BUG_ON` 一字未改。

**第一步**：编一次内核，带上
`patches/0013-drm-crtc-drop-racy-BUG_ON-in-fence_to_crtc.patch`
（已用 `git apply --check` 对 v7.2-rc2 原文验过）。
验收判据不是"没崩" —— 而是**反复开合设置/权限页各几十次 + `/sys/fs/pstore/`
零新记录**。⬜ 之后值得把它报到 dri-devel（对外动作，等用户定）。

### A1. 音频与蓝牙长期运行后死锁 ⚠️ 次高优先
用户实机报告，我未复现、未定位（[#38](stage4-findings.md)）。
两者共用同一条到 DSP 的 QRTR/FastRPC 通路，而这条通路上**已经实测到过**
会话级卡死（使能光感会污染整个 SSC 会话）。

★ **取证看门狗已随 v0.2.0 起的镜像发布**（`bin/gaokun3-hangdump.sh`）：
现实是死锁时用户只会重启、证据就没了，所以证据必须自动留下。它 60 秒采一次
`/proc` 线程状态（刻意不跑 dumpsys，很便宜），判据是**同一个 tid 连续三次都在 D**
（≥2 分钟），命中后把 stack/wchan/QRTR 服务表/PCM 状态/binder 日志/logcat
写到 `/data/vendor/gaokun3/hangdump-<uptime>/`。

**第一步**：下次死锁后把那个目录整个要过来 —— 不必再追问"多久、什么负载"。
若目录是空的，说明它没判定成死锁（比如卡的不是 D 状态），那本身就是线索。
手工对照仍可用 `gaokun3-qrtr-lookup` 比服务表：少了哪个服务就指向哪个 DSP。
⚠️ 别把 `Handover signaled` 当崩溃证据，那是良性噪声（#37 已用对照实验证明）。

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

### A5. 恢复出厂设置不起作用
设置里那条路走 misc 的 BCB + recovery，而本机没有可用 recovery
（[#39](stage4-findings.md)）。实机证据：misc 里躺着一条没人消费的 `boot-recovery`。

**现在的替代**：从救援 Linux `mkfs.ext4 -F /dev/disk/by-partlabel/userdata`。
**真正的修法**：见 B3（EFI 加载器）或让 recovery 能启动（已搁置）。

### A6. USB-C 外接显示（UCSI）★ 现在还欠着待机那笔账
`PPM init failed -ETIMEDOUT`，本机主线已知缺陷，`/sys/class/typec/` 是空的。
代价还包括 USB 只有 high-speed（SuperSpeed 需要 UCSI 切 orientation）。

★ **它现在是三个问题的共同根因**：没有 role 源正是 `a600000.usb` 停在
半初始化 `device` 态的原因（[#52](stage4-findings.md)），而我们为此付的代价
就是**息屏时 USB adb 断开**。UCSI 修好 → role 被正确指派 → 那个取舍自动消失。
⚠️ 顺带说明这条为什么值得排在摄像头前面：**USB adb 掉线是本项目迄今最大的
效率税**（#27），每次都要靠扫网段 + TCP adb 找回来。

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

## A′. 最近关闭（记在这里，免得下次又被当成待办）

* **s2idle 待机** ✅ 2026-08-22 修好并随 v0.3.0-alpha 发版。
  真凶是**我们自己** Stage 2 为 USB adb 加的 `dr_mode="otg"` +
  `usb-role-switch`：本机 UCSI 坏 → 没有 role 源 → 控制器停在没有 gadget、
  没有 xhci 的 `device` 态 → 挂起阶段给它断电就**整板复位、零日志**。
  救援 Ubuntu 用 `system-sleep` 钩子，Android 用息屏切 host / 亮屏切回 device
  （`bin/gaokun3-usbrole.sh`）。案卷 [#52](stage4-findings.md) →
  [#57](stage4-findings.md)。
  ⚠️★ **这条留着是当方法论用的**：它在 Ubuntu 上用同一棵内核复现得一模一样，
  于是被判成"内核/EC 缺陷、与 Android 无关"——**排除得对，指向的层完全错**
  （设备树两边是同一份）。另外两道坎叠在一起 + 单次失败率 93%，
  让每个单变量实验都返回"无效"。剩下的尾巴只有一条：息屏时 USB adb 断（见 A6）。
* **耳机口 + 内置麦克风** ✅ 已由用户实机确认出声。真凶是 rx-macro 的插值器链
  从来没接上（内核对此**一行日志都不打**），外加策略没声明耳机设备、
  框架去看主线上不存在的 `/sys/class/switch/h2w`。
  ★ 配方来自救援 Ubuntu 上的上游 ALSA UCM2 —— 上游用
  `Regex "HUAWEI.*MateBook E.*"` 把本机直接 include 成 ThinkPad X13s。
  **本机凡是 LPASS 音频的事，先去抄那个目录。**[#40](stage4-findings.md)

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

### B9. SLPI 每 200 ms 一条 handover 噪声（根因未查）
详见 [#59](stage4-findings.md)。**它无害但有代价** —— 正是它把 [#58](stage4-findings.md)
那两次 panic 的调用栈从 pstore 里挤掉了（45 条记录里有用的不到 10 条）。
`patches/0014` 已把打印改成 ratelimited（治症状，正确且值得上游），
但**远端为什么每 200 ms 翻一次 smp2p 位仍然不知道**。

硬证据：SLPI 的 `q6v5 ready` 与 `q6v5 handover` 两条中断计数**完全相同、
同步增长**（5 秒 5475→5502），ADSP/CDSP 各只有 2。

**第一步**：5 Hz 很像一个采样节拍 —— 停掉 sensors HAL / `hexagonrpcd`
看频率变不变。⚠️ **别在没人看着时做**：M12 记过停/重启 HAL 会污染 SSC 会话，
自动旋转当场失效、要重启 `hexagonrpcd` 并等约 20 秒才恢复。

---

## C. 上游或硬件层面（本地做不了）

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

### D5. PR #3 待回复（已审完，等你定措辞）
线上那个 PR 动的正是内核预编译这一块。我把要问的整理好了，**没有发到 GitHub**
—— 对外发言等你。三个问题：`dr_mode=host` 是不是有意为之（那正是我们 #52 的
取舍另一半）；他们刷完之后 USB adb 还通不通；能不能公开那份内核 `.config`
（我们这边的断言是 52 条 MUST_Y + `VIDEO_QCOM_IRIS` MUST_N，可以对一遍）。

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
