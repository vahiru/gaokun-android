# Stage 4 实测问题记录（输入 / 音频 / WiFi / 电源）

> 编号接续 `stage2-findings.md`（#1–#25 见彼处，含 Stage 3 冲刺）。
> 本文档记录 Stage 4 的实测问题、根因与修复。

---

## #26 触摸屏"随机"死亡 / 幽灵触摸 —— gpio174 模式选择脚无人驱动 ✅ 已修复

**现象（按时间线）**
1. kb17 首启：触摸可用但有幻影触点（单指出现多 slot 散点）
2. 60Hz 显示模式实验后：触摸彻底静默，evdev 零输出
3. 冷断电重启后：空闲幽灵触点风暴（不碰屏每秒冒随机按下事件，
   一次出现 6 个"手指"挤在 X≈877–1001 的窄带、Y 散布全屏），
   真手指画线反而完全不上报
4. 驱动重挂（unbind/bind）、`inplace_reset`、降 `peak_threshold` 至 400 都无效
5. **跨内核持久**：Ubuntu 7.1.0-rc3（buildbot 原装内核 + 树内
   himax_hx83121a_spi.ko）同样零输出——排除 Android 用户态、
   排除我们的 7.2 内核构建差异
6. Windows / BIOS 界面触摸始终正常

**根因**
`refs/linux-gaokun/README.MD` "Touchscreen / insights" 节：HX83121A
级联 IC 在**触摸固件重载瞬间采样 gpio174** 决定宿主传输模式——
低=SPI 原始数据模式（Linux 驱动要的），高=I2C HID 模式（UEFI/BIOS
用的）。固件重载会在 TS 复位（gpio99）后自动触发，而**显示复位
（gpio38）可能在驱动不知情时内部触发 TS 复位**。

buildbot 和 linux-gaokun 两棵树的 DTS 都只配置了 gpio175（IRQ）和
gpio99（复位），**gpio174 无人驱动**，电平全看 UEFI 退出时的遗留状态。
UEFI 自己用 I2C 模式，所以经常遗留高电平 → IC 锁进 I2C HID 模式 →
SPI 侧"探测成功但全聋"。KMS 模式切换（我们的 60Hz 实验）触发显示
复位 → 固件静默重载 → 模式翻转，营造出"随机死亡"的假象。

**修复**
DTS `ts0_default` pinctrl 加一组：

```dts
mode-pins {
    pins = "gpio174";
    function = "gpio";
    output-low;
};
```

见 `patches/0002-arm64-dts-gaokun3-drive-ts-mode-gpio174-low.patch`。
pinctrl default 状态在 himax-spi probe 时施加（先于驱动发起的复位→
固件重载），且此后恒为低——任何后续面板复位引发的重载都落在 SPI 模式。

**验证**
- Ubuntu 活体实验：`gpioset --chip gpiochip4 --hold-period 20s 174=0`
  期间重挂驱动 → 触摸完美（"非常跟手"）
- Android：直接改 ESP 上的 DTB（dtc 反编译→插节点→回编译，备份为
  `sc8280xp-huawei-gaokun3.dtb.bak-pre174`）→ "触摸非常丝滑"

**遗留事项**
- [x] ~~下次开构建机时把补丁应用进 VM 内核树~~ ✅ 2026-08-17 已进
  kb18（从源码编出的 DTB 与手术版 sha1 完全一致，零漂移）
- [ ] Ubuntu 7.1/7.2 启动项各自引用的 DTB 还没打补丁（在 Android 下
  无法挂 ESP 改，USB_STORAGE=m；下次进 Ubuntu 时用
  `/home/user/gk3-gpio174.dts.txt` 同法处理）
- [ ] 向 gaokun 社区（right-0903 / buildbot / mainline-generic live-ISO）
  报告：他们的 DTS 同样缺 gpio174，live-ISO 用户会随机踩坑

**诊断方法论沉淀**
- IC 空闲 IRQ 速率是状态指纹：**≈显示扫描率（120Hz 面板 ≈117/s）=
  SPI 原始模式正常流**；0/s = IC 停摆；与扫描率无关的风暴 + evdev
  静默 = 模式错乱
- 幽灵触点"整列电极同亮"（多 slot 同 X 窄带、Y 全屏散布）不一定是
  充电噪声——本例拔线后依旧，是模式错乱下的乱码帧
- **`timeout N getevent > file` 会因 stdio 块缓冲丢光输出**（SIGTERM
  不冲刷）；采集 evdev 用 `timeout N cat /dev/input/eventX > file` 录
  二进制流再离线解码（24 字节/事件：u64 sec, u64 usec, u16 type,
  u16 code, s32 value）

## #28 ath11k 内置驱动的固件竞速 + hw2.1 目录映射 ✅ 已修复

**现象**：kb18 后 PCI 域 0006 枚举成功（PWRSEQ 修复生效），但 ath11k
probe 报 `Direct firmware load for ath11k/WCN6855/hw2.1/amss.bin failed
with error -2` → `-110` 永久失败，无 wlan0。

**三层根因**
1. **芯片是 wcn6855 hw2.1 不是 hw2.0**（dmesg `wcn6855 hw2.1`；
   `ath11k/core.c` 的 hw_params 表把 hw2.1 硬映射到 `WCN6855/hw2.1` 目录）
2. **上游 linux-firmware 没有 hw2.1 实体文件**——WHENCE 里是
   `Link: ath11k/WCN6855/hw2.1/*.bin -> ../hw2.0/*.bin`，cgit 拉单文件 404。
   vendor 分区不做软链，把 hw2.0 文件在 hw2.1 路径再装一份即可
3. **内置(=y)驱动开机 ~5s 就 probe，/vendor 还没挂载**，request_firmware
   直接 -2，probe 失败后无人重试（msm GPU 能活是因为它懒加载固件，
   surfaceflinger 打开设备时 vendor 已在；ath11k 没这种运气）

**修复**
- device.mk：hw2.0 四件套同时装到 hw2.1 路径
- init.gaokun3.rc：`on post-fs-data`（vendor 已挂载）时
  `write /sys/bus/pci/drivers/ath11k_pci/bind "0006:01:00.0"` 手动补绑定

**教训**：内置驱动 + vendor 固件 = 天然竞速。任何要固件的 =y 驱动都要
检查它的固件加载时机（probe 时 vs 首次打开时），probe 时加载的一律需要
晚绑定兜底。蓝牙 hci_qca 同样在 4.6s 吃了 -2（待 BT 阶段一并处理）。

## #29 WiFi 用户态四连坑（HAL 空壳 / FW_PATH 毒药 / supplicant 配置 / 国内验证墙）✅ 已修复

kb18 内核就绪后（#26/#28），用户态又连过四关，全记录：

1. **libwifi-hal 空壳**：不设 `BOARD_WLAN_DEVICE` 时链接 fallback 实现，
   HAL `start()` 直接 Status 9。mainline nl80211 设备用
   `BOARD_WLAN_DEVICE := emulator`（goldfish 实现，CF 同款），并且
   **必须** `PRODUCT_SOONG_NAMESPACES += device/generic/goldfish`。
2. **`WIFI_DRIVER_FW_PATH_STA := ""` 是毒药**：空串被字面编译进
   libwifi-hal，configureChip 走 Broadcom 式固件模式切换 →
   `Failed to change firmware mode` → chip 配置失败。这些变量
   **完全不要定义**。
3. **supplicant 缺配置文件**：AIDL `addStaInterface` 硬要求
   `/data/vendor/wifi/wpa/wpa_supplicant.conf` 存在。goldfish 模板
   （disable_scan_offload=1 等三行）装到 vendor，rc 开机 copy 过去。
4. **国内连通性验证墙**：连接成功但框架访问 Google `generate_204`
   失败 → `NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT` →
   重启后永不自动回连（现象极具迷惑性：手动连每次都成）。
   换 `captive_portal_https_url` 为国内可达端点即验证通过
   （IS_VALIDATED），自动回连恢复。设置在 /data，重刷后要跑
   `scripts/android-post-flash.sh`。

**最终验收（2026-08-17）**：冷启动免手干预 → WiFi 自动连接 <SSID>
（11ax，2401Mbps，RSSI -27）→ DHCP + IPv6 GUA → 公网 ping 17ms →
adb over TCP（persist.adb.tcp.port=5555）双通道可用。

## #30 蓝牙崩溃循环——无 HCI HAL，先禁用（待做）

内核 BT 栈已 =y（kb18），但 vendor 没有 `com.android.hardware.bluetooth`
APEX → BT 应用起来就 LOG(FATAL) 崩溃循环（100 个墓碑，会喂 RescueParty）。
已 `settings put global bluetooth_on 0` 止血（进 post-flash 脚本）。
下一场双修：(a) 加 BT HCI HAL APEX；(b) hci_qca 固件竞速同 #28
（4.6s 就要 qca/wcnhpbtfw21.tlv，晚绑定或 rc 重触发）。

## #31 缺的固件其实一直躺在本机 Ubuntu 里 ✅ 已取用

排音频"无声卡"时顺手挖到的大礼包 —— Ego 的 Ubuntu 根（U 盘 sda2）的
`/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/` 下有我们缺的全部华为专有件：

| 文件 | 作用 | 缺了会怎样 |
|---|---|---|
| `qcdxkmsuc8280.mbn` | **GPU zap shader** | GPU 停在安全模式，freedreno 不可用（dmesg 刷 `adreno_zap_shader_load *ERROR*`）|
| `audioreach-tplg.bin` | **音频拓扑** | 声卡不注册 |
| `adspr.jsn` / `adspua.jsn` / `cdspr.jsn` / `battmgr.jsn` | pd_mapper 服务表 | ADSP 服务注册不全 |
| `qcvss8280.mbn` | 语音 DSP | （暂未用到）|

**教训**：这台机器的"专有固件从哪来"问题，答案不一定是 Windows 驱动包 ——
gaokun 社区的 Linux 镜像早就把它们凑齐了，本机 Ubuntu 就是现成的固件来源。
以后缺任何 blob，先 `mount /dev/block/sda2` 翻一眼再说。
（Android 侧要 `CONFIG_USB_STORAGE=y` 才看得见 U 盘，kb19 已开。）

## #32 固件竞速的根治：ramdisk 副本 + AOSP 的 ELF 检查

remoteproc（ADSP/CDSP/SLPI）、GPU zap shader、hci_qca 都在 `/vendor` 挂载前
probe，而音频那条链的驱动全带 `suppress_bind_attrs`（`bind`/`unbind` 文件
根本不存在），#28 那套"晚绑定补一刀"用不了。根治办法是让**首次 probe 就能
拿到固件**：把固件也装进 ramdisk 的 `/lib/firmware/`（ramdisk 是第一阶段
rootfs；cmdline 的 `firmware_class.path=/vendor/firmware/` 只是首选路径，
找不到会回落到 `/lib/firmware`）。

踩坑：AOSP 会拒绝 `PRODUCT_COPY_FILES` 里**目标路径含 `bin`/`lib`/`lib64`
组件的 ELF 文件**（`found ELF prebuilt in PRODUCT_COPY_FILES`）。
`.mbn` 固件本身就是 ELF 格式，而 ramdisk 目标必须落在 `/lib/firmware` →
只能开官方逃生开关 `BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true`。
（`/vendor/firmware/...` 不含 lib 组件，所以之前一直没触发。）

副作用：ramdisk.img 从 1.5 MB 涨到 12 MB，可接受。

## #27 拔插 USB 后 adb 不重枚举（UCSI 角色老毛病，缓解：adb over TCP）

拔掉 USB 线再插回后 gadget 不重新枚举，`adb devices` 空，需要整机
重启才恢复。与已知 UCSI PPM init 缺陷同源（dr_mode=otg 无 role 源
时靠初始 fallback 落到 device 侧，拔插后没有事件源驱动它再切回）。
Stage 4 电源/USB 项一并处理。

---

## 浸泡测试记录（2026-08-17 kb18 + build e）

冷启动后连续运行 ~2 小时（挂机 + 每 10 分钟 adb 采样 12 轮）：
崩溃 0（蓝牙禁用后 crash buffer 全程干净）、WiFi 全程在线、
最高温 44.8°C / 尾声 35.3°C、负载均值 ~1.1。
今日全部改动（gpio174 触摸、ath11k 晚绑定、wifi 栈、BT 禁用）无回归。

---

## #33 音频：声卡不注册的真正源头是三个 `=m` + 一个固件路径（2026-08-19 完成，实机出声）

`/proc/asound/cards` 一直是 `--- no soundcards ---`。链条自下而上：

```
CONFIG_PINCTRL_LPASS_LPI=m / PINCTRL_SC8280XP_LPASS_LPI=m
  → 33c0000.pinctrl 永不绑定（Android 不加载任何模块）
  → rx/tx/wsa macro 的 pinctrl supplier 缺席，永远 deferred
  → 三个 soundwire 控制器等 macro → sound 节点等 DAI → 无声卡
```

实测原话（logcat，kernel）：

```
platform 3200000.rxmacro: deferred probe pending:
  platform: wait for supplier /soc@0/pinctrl@33c0000/rx-swr-default-state
```

一起转正的还有：
- `SC_LPASSCC_8280XP=m` → `=y`（LPASS 时钟控制器，macro 的 mclk/npl 来源）
- `QRTR_SMD=m` → `=y`（QRTR 的 rpmsg 传输，pd-mapper 靠它与 ADSP 通信）
- **`SND_SOC_WSA883X` 压根没编** → 本机扬声器是 wsa8830
  （DT `compatible = "sdw10217020200"` = mfg 0x0217 / part 0x0202，
  由 `wsa883x.c` 认领，与 ThinkPad X13s 同款）

修完配置后 macro / soundwire / GPR 服务全部就位，卡在最后一步：

```
qcom-apm: tplg firmware loading qcom/sc8280xp/SC8280XP-HUAWEI-GAOKUN3-tplg.bin failed -2
snd-sc8280xp sound: ASoC: failed to instantiate card -2
```

**新内核的拓扑固件名是 `qcom/<card->driver_name>/<card->name>-tplg.bin`**
（`sound/soc/qcom/qdsp6/topology.c:1320` 用 `kasprintf` 拼，card 名来自 DT `model`），
而我们按老规矩装的是 `qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin`
—— 路径和文件名都不对。放到内核要的位置后声卡立刻注册：

```
0 [SC8280XPHUAWEIG]: sc8280xp - SC8280XP-HUAWEI-GAOKUN3
PCM：MultiMedia1/2 Playback + MultiMedia3/4 Capture
```

### 路由（Android 没有 ALSA UCM）

**华为 MateBook E 的 UCM 明确 include ThinkPad X13s 的配置**
（`conf.d/sc8280xp/sc8280xp.conf` 里 `If.HUAWEI → /Qualcomm/sc8280xp/LENOVO-X13s.conf`），
所以 X13s 那套控件序列与拓扑固件直接适用。PCM 映射：

| 用途 | PCM | 通路 |
|---|---|---|
| 扬声器 | **hw:0,1** | `WSA_CODEC_DMA_RX_0 ← MultiMedia2` |
| 耳机 | hw:0,0 | `RX_CODEC_DMA_RX_0 ← MultiMedia1` |
| 头戴麦 | hw:0,2 | `TX_CODEC_DMA_TX_3 → MultiMedia3` |
| 内置麦 | hw:0,3 | `VA_CODEC_DMA_TX_0 → MultiMedia4` |

开机自动摆路由：`device/huawei/gaokun3/bin/audio-route.sh` +
`etc/audioroute.rc`（等 `/dev/snd/controlC0` 出现后按 UCM 序列设 WSA 通路，PA=17）。

### 框架层接入

AOSP 的 AIDL 音频 HAL **自带 ALSA 后端**（`alsa/StreamAlsa.cpp` 等），
而它开哪张卡由 device port 的 **address 字符串**决定：
`primary/StreamPrimary.cpp getCardAndDeviceId()` 用
`sscanf("CARD_%d_DEV_%d")` 从 address 里解析，解析不到就退回内置默认值。
我们原本没写 address → 永远落不到扬声器。改成
`address="CARD_0_DEV_1"` 后 HAL 日志实证：

```
AHAL_StreamPrimary: getCardAndDeviceId: parsed with card id 0, device id 1
```

### 验收

`tinyplay /data/local/tmp/*.wav -D 0 -d 1` 实机出声（用户确认），
`/proc/asound/card0/pcm1p/sub0/status` 播放中为 `state: RUNNING`。
2026-08-19 用 ffmpeg 转码的整曲（48kHz 立体声 2'34"）完整放完。

### 起停爆音：BOOST 升压器（A/B 盲听定案）

用户反馈"开头结尾有破音"。三轮 A/B 实听把它逐步收窄：

1. 先怀疑我的测试音硬起停 → 加淡入淡出，**照样爆** → 不是文件问题。
2. 再怀疑我把 PA 推到 17 削波 → 降到 12/8，**照样爆** → 不只是增益。
3. 关掉 `SpkrLeft/Right BOOST Switch` → **爆音消失**（用户原话
   "第一遍没有爆音，很好"）。

→ 结论：爆音来自功放升压器使能瞬间。`bin/audio-route.sh` 默认改成
**BOOST 关 + PA=12（UCM 原厂值）**。代价是最大声压低一些，
对平板小喇叭是划算的取舍。
（另注：每次 `tinyplay` 都会重开 PCM，所以每段都上下电一次；
正常媒体播放时 audioserver 持有音频流，不会每首歌爆一次。）

### ⚠️ 设备上一个系统音效文件都没有

`/system/media/audio/` **整个目录不存在** —— 铃声、通知音、闹钟、UI 音效
（`Effect_Tick.ogg` 等）全都没装。所以：

- `cmd notification post`、音量键提示音、点击音效**天然无声**，
  不是通路问题（我一度用它们做验证，测不出东西）。
- 想要这些音效，需要在 device.mk 里 inherit AOSP 的音频资源包
  （`frameworks/base/data/sounds/` 下有若干 `.mk`：`AllAudio.mk` 是全量，
  另有若干按机型裁剪的 `AudioPackage*.mk`）。
  ⚠️ **具体该 inherit 哪个必须先在 AOSP 树里核实**（本项目规矩：不凭记忆写路径），
  下次开构建机时 `ls frameworks/base/data/sounds/*.mk` 确认后再加。
- 验证框架层媒体音只能靠真播放器：`com.android.music` 已装（AOSP 音乐播放器），
  但它只显示 MediaStore 里的内容，而 `adb push` 到 `/sdcard/Music/` 后
  MediaProvider 并未自动收录（`cmd media_provider` 在本版本不存在，
  `MEDIA_MOUNTED` 广播也没触发重扫）。

### 遗留

- 左功放（`sdw:1:0:0217:0202:00:1`）卡在 `Alert` 状态刷
  `Bus clash detected`（2607 次），右功放 `Attached` 正常；出声不受影响。
- `qcom-soundwire` 报 `din-ports/dout-ports mismatch with controller`（DT 端口数与
  控制器不符），暂未影响功能。
- 框架层只验证到"HAL 指向正确 PCM"，尚未用真播放器跑通媒体音
  （`cmd notification post` 默认无声音渠道，测不出来）。
- shell 用户不在 `audio` 组 → 非 root 下 `tinyplay` 会 "cannot open device"。

## #34 蓝牙：内核早就通了，缺 HAL + 一个调度器配置（2026-08-19 完成）

`hci0` 其实一直在（`BT_QCA`/`BT_HCIUART_QCA` 都是 `=y`，`hci_qca` 绑在
`988000.serial:0.0` 的 serdev 上）。#30 说的"无 HCI HAL"两步补齐：

1. **AOSP 自带的 HAL 就支持 Linux HCI**：
   `hardware/interfaces/bluetooth/aidl/default/BluetoothHci.cpp:172`
   先 `NetBluetoothMgmt::openHci()`（BT 管理 socket + `HCI_CHANNEL_USER`），
   失败才退回串口路径。**一行代码没改**，把
   `android.hardware.bluetooth-service.default` + 它的 `.rc` + VINTF 片段 +
   `android.hardware.bluetooth-V1-ndk.so` 走 overlay 推进 vendor 即可（不刷 super）。
   ⚠️ 少推那个 `-V1-ndk.so` 会得到 `CANNOT LINK EXECUTABLE ... not found`
   的 5 秒重启循环。
2. ★**真凶与 HAL 无关**：
   ```
   bluetooth: message_loop_thread.cc:291 EnableRealTimeScheduling:
     unable to set SCHED_FIFO priority 1 for bt_main_thread, error: Operation not permitted
   → bluetooth::log::fatal → com.android.bluetooth abort
   ```
   `CONFIG_RT_GROUP_SCHED=y` + `CGROUP_SCHED` 时，非 root cpu cgroup 的
   `rt_runtime_us` 默认是 0 → 该 cgroup 内任何 `sched_setscheduler(SCHED_FIFO)`
   一律 EPERM。**GKI 里这项是关的**，主线 defconfig 默认开。关掉即好。

验收：`svc bluetooth enable` → `dumpsys bluetooth_manager` 显示
`enabled: true / state: ON / address:（从芯片读出）/ name: MateBook E Go`，
`com.android.bluetooth` 崩溃 0 次，`EnableRealTimeScheduling` 报错 0 次。

## #35 方法论：`kernel-config-android.sh` 现在自带断言

"Android 不加载模块，`=m` 等于驱动缺席"这个坑本项目踩了 **12 次**
（WiFi 的 PWRSEQ、USB_STORAGE、这次的 LPASS pinctrl…）。脚本已改为：
自己跑 `olddefconfig`（把致命的 `ARCH=arm64` 固定住），跑完断言
**35 个必须 `=y`**、**1 个必须 `=n`**（`RT_GROUP_SCHED`），
另查 `CONFIG_LSM` 必须含 `selinux` 且不含 `apparmor`，不达标非零退出。

## #36 框架层媒体音的真正拦路虎：`MediaCodecList` 是空的（AOSP 产品配置缺失）

> ⚠️ **2026-08-19 更正**：本节把根因归给"`media_codecs.xml` 缺失 / 产品配置缺口"
> —— **这个结论是错的**。当天在同一台设备上把整条链路逐环量了一遍：
> XML（APEX 和 /vendor/etc 两份都在）、36 个软解码库、C2 服务已注册、
> VINTF 片段装着且 `vintf fm` 运行时确实列得出来、`ro.vendor.api_level=202504`
> 走 AIDL —— **每一环都是好的**。
> 真正断点是 `AServiceManager_forEachDeclaredInstance()` 返回空，
> 即 **servicemanager 的 "declared" 集里没有它**（registered ≠ declared）。
> 完整证据链与主嫌疑（servicemanager 的 VintfObject 快照早于 apexd 就绪，
> 而该声明带 `updatable-via-apex`）见 **`docs/stage6-crdroid.md`**。
> 本节下面的"排查过程"仍然有效，只是结论那一步要改读 stage6。

音频**输出**通路已经完备，有硬证据：

- HAL 指向正确的 ALSA 设备：`AHAL_StreamPrimary: getCardAndDeviceId: parsed with card id 0, device id 1`
- AudioFlinger 有两个 48kHz 输出线程（`AudioOut_D` / `AudioOut_1D`）
- `tinyplay` 直连 ALSA 出声正常（用户实听确认，整曲 2'34" 放完）

但任何 App/媒体播放都失败，栈底原因是**解码器一个都没有**：

```
E NuPlayerDecoder: Failed to create audio/mpeg decoder
E NuPlayer: received error(0x80000000) from audio decoder
D MediaPlayerService: OMX service is not available
dumpsys media.player → "Decoder infos by media types:" （空）
```

排查过程（都是实测，不是推断）：

| 检查 | 结果 |
|---|---|
| `media.swcodec` / `mediaserver` / `media.extractor` 进程 | 都在跑 |
| C2 软件服务注册 | 在：`android.hardware.media.c2.IComponentStore/software` |
| VINTF 声明 | 在：`/system/etc/vintf/manifest/manifest_media_c2_software.xml`（AIDL + HIDL 双声明）|
| `hwservicemanager` | **不在**（Android 15+ 已移除 HIDL），所以 HIDL 那条声明是死的，只能走 AIDL |
| `/vendor/etc/media_codecs.xml` | **原本不存在** → 从 `/apex/com.android.media.swcodec/etc/media_codecs.xml` 拷了一份进 vendor + 重启媒体栈 |
| 拷贝后 | `MediaCodecList` **仍然是空的**（decoders 和 encoders 都空）|
| `ro.media.xml_variant.*` 属性 | **全部未设置** |
| swcodec 进程日志 | 一行都没有（连启动信息都没打）|

→ 结论：这是 **AOSP 产品配置层的缺口**，不是硬件或内核问题。
一个正常 ROM 的设备配置会带齐 `media_codecs.xml` /
`media_codecs_performance.xml` / `media_profiles.xml` /
`ro.media.xml_variant.*` 一整套；我们这棵手搓的最小 AOSP 从来没配过。
同理 `/system/media/audio/`（铃声/UI 音效）也整个缺失（见上一节）。

**这条正是"换 crDroid（LineageOS 系）比继续修手搓 AOSP 更划算"的最好论据**：
这些产品级配置是 Lineage 设备树的标准组成部分，换轨后大概率自动消失，
而我们所有的硬件使能成果（内核配置、DTB、固件路径、turnip 补丁、
混音器路由脚本、SMMU workaround）**全部可平移**。

---

## #37 传感器：整套跑在 SLPI DSP 上 —— ★结论已修正，主线**能**做到（2026-08-20）

> ### ⚠️ 本条原先的结论是错的，2026-08-20 当天被推翻
>
> 我原先写的是"主线此路不通，任何 sc8280xp 设备都没人做到过"。
> **错。** 一位贡献者拿 **`hexagonrpcd`（linux-msm）+ `libssc`
> （Dylan Van Assche，codeberg.org/dylanvanassche/libssc）** 在本机型上把
> 三个传感器读出来了（light / accelerometer / gyroscope）。
>
> 下面的**结构性分析仍然成立**（传感器由 SLPI 托管、芯片挂在 SSC 侧总线、
> AP 侧没有任何传感器芯片驱动），错的只是"因此不可达"这一步推论 ——
> 可达路径是 **AP 通过 FastRPC 给 DSP 当文件服务器**，再用 QMI/protobuf
> 取读数，而不是 AP 直接驱动芯片。
>
> ★**一个强互证**：贡献者的部署指南里 socinfo 要填
> `QRD` / `Unknown` / `0` / `65536` / **`449`** / `3.1`，
> 与我从 Windows 驱动里独立读出的完全一致（`tcs3701.json` 里就是
> `"soc_id": ["449"]`、`hw_platform` 文件内容就是 `QRD`）。两边互相印证。
>
> ★**已经打通的第一步（实测）**：`CONFIG_QCOM_FASTRPC` 在本内核里是 **`=m`**，
> 而这棵树**不发模块**（设备上 `/proc/modules` 是 0 行、连模块目录都没有），
> 所以 `/dev/fastrpc-*` 从来不出现。DTS 里节点是齐的
> （`remoteproc_slpi` 下 `fastrpc` + `compute-cb@1/2/3`），
> rpmsg 通道也在（`2400000.remoteproc:glink-edge.*`）—— 只是没人 probe。
> 单独编出 `fastrpc.ko` 推上去 `insmod`（vermagic 完全匹配、模块签名关闭）：
> ```
> /dev/fastrpc-sdsp   /dev/fastrpc-adsp   /dev/fastrpc-cdsp   /dev/fastrpc-cdsp-secure
> ```
> **这和 #33 音频那三个 `=m` 断点是同一类问题。** 正解是 `=y` 并加进
> `scripts/kernel-config-android.sh` 的断言。
>
> ⚠️ 附带告警：`qcom,fastrpc 3000000.remoteproc:...: no reserved DMA memory
> for FASTRPC`（出现在 ADSP 节点，SLPI 侧未报），待观察。
>
> **还差什么，见本条末尾的「落地路线」。**

### 结构性分析（这部分是对的）

**加速度计、磁力计、光感、接近、铰链角度，全部由 SLPI 传感器 DSP 托管，
AP 侧根本没有到这些芯片的总线。** 因此：

- 「往 DTS 里加个 `accel@xx` 节点」这条路**不存在**——不是没人写，是没有节点可写。
- **自动旋转、自动亮度在主线内核上无法实现**，除非有人为 sc8280xp 写出
  SLPI SEE 的客户端（QMI/FastRPC 那一整套）。**据我所知没有任何
  sc8280xp 设备做到过（包括 ThinkPad X13s）。**
- 影响面：没有自动旋转、没有自动亮度、没有铰链角度。
  ★**游戏不受影响**（应用自己请求方向，`ignoreOrientationRequest=false`
  之后照常横屏，见 `docs/stage6-crdroid.md` §9）。

### 证据（从本机 Windows 分区只读挂载后直接读出，`scripts/probe-windows-sensors.sh`）

`/dev/nvme0n1p3` 的 `Windows/System32/DriverStore/FileRepository/` 里，
**没有任何一个具体传感器芯片的独立驱动包**，取而代之的是高通那一套：

```
qcsensors.inf_arm64_...                 (qcSensors.dll —— 传感器框架)
qcsensorsconfigqrd8280.inf_arm64_...    (配置 JSON + libsdsprpc.dll)
qcalwaysoncvsensor(_ext8280).inf        (常开视觉)
qchumanpresencesensor.inf               (人体存在)
```

★`libsdsprpc.dll` = **Sensor DSP RPC**。这个名字本身就说明数据通路是
**AP ⇄ DSP 的 FastRPC**，不是 AP ⇄ I2C 芯片。

配置包里的 JSON 一览（`sns_` 前缀是高通 SEE / Sensors Execution Environment
的模块命名，这些模块**跑在 SLPI 上，不是跑在 CPU 上**）：

| 类别 | 文件 |
|---|---|
| 物理器件 | `sh3001_0.json`（6 轴 IMU）、`sy3133cs_0.json`、`t1000_0.json`、`tcs3701.json`（ams 光感+接近）、`stm_lid_angle.json`（铰链角，节名 `hingeangle_0_platform`） |
| SEE 算法模块 | `sns_device_orient`（设备方向）、`sns_rotv`（旋转矢量）、`sns_geomag_rv`、`sns_gyro_cal`、`sns_mag_cal`、`sns_amd`、`sns_rmd`、`sns_tilt`、`sns_fmv`、`sns_cm`、`sns_dae`、`sns_aont` |

TCS3701 那份 JSON 解出来的接线（**注意这是 SSC 侧的编号，不是 AP 侧**）：

```
owner            sns_tcs3701      ← SLPI 上的驱动名
bus_type         0                ← I2C
bus_instance     5
slave_config     57               ← 0x39，ams TCS370x 的经典地址
dri_irq_num      127
irq_is_chip_pin  1
vddio_rail       /pmic/client/sensor_vddio
```

### ⚠️ 一个尚未排除的疑点（别把本条当成 100% 定论）

这个配置包叫 `qcsensorsconfig**qrd**8280` —— **QRD = 高通参考设计**，
包内 `hw_platform` 文件的内容也确实是 `QRD`。零售的华为机器**通常**会另有
一个 OEM 自己的 `qcsensorsconfig<oem>8280` 包，而 DriverStore 里没有。

两种可能：(a) 华为直接沿用了 QRD 配置；(b) 真正的配置在别处（比如
`C:\Windows\INF\oem*.inf` 或 EC/ACPI 里）。**芯片型号可能不准，
但"传感器挂在 SLPI 后面、AP 无总线可达"这个结构性结论不受影响** ——
因为整个 DriverStore 里根本不存在任何 AP 侧的传感器芯片驱动。

### 复现方法

```
# 在 Ego 的 Ubuntu 里（Android 侧读不了 NTFS）
bash scripts/probe-windows-sensors.sh
```

⚠️ 本机 Ubuntu **没有 `ntfs3` 内核模块**，`mount -t ntfs3` 会报
"未知的文件系统类型"；靠 `mount -o ro` 自动探测走 `fuseblk`（ntfs-3g）才挂得上。
⚠️ 包里那几个 `8280_qrd_*.json` 是 NTFS 重解析点（symlink），
ntfs-3g 显示为 `-> unsupported reparse tag 0x80000017`、`stat` 只有 34 字节，
**读它们会 FileNotFoundError**；要读的是同目录下的**去掉 `8280_qrd_` 前缀**
的那份实体文件（`sh3001_0.json` / `tcs3701.json` …）。

### 落地路线（2026-08-20 状态）

| 步骤 | 状态 |
|---|---|
| SLPI remoteproc running | ✅ 一直是 |
| QRTR（`/dev/qrtr-tun`） | ✅ 一直是 |
| **`/dev/fastrpc-sdsp`** | ✅ **已打通**，靠 `insmod fastrpc.ko`；正解是 `CONFIG_QCOM_FASTRPC=y` |
| Windows DriverStore 的传感器文件 | ✅ **已解决**（2026-08-20）—— 从 `uup-drivers-sc8280xp` 的 release 提取，不需要 Windows 分区，见下 |
| `hexagonrpcd`（给 DSP 当文件服务器） | ⬜ 需编译，且要打一个补丁 |
| `libssc` + `ssccli`（读数） | ⬜ 需编译 |
| **Android 侧 sensors HAL** | ⬜ 尚不存在，是独立的一大块 |

#### ✅ 原先的硬阻塞已解除：文件从公开源拿到了

**本机的 Windows 已在 2026-08-20 抹除**，我当时读过那些 JSON 但没拷出来。
但不需要它了 —— 全部文件都在 **`matebook-e-go/uup-drivers-sc8280xp`
的 release** 里（该项目用 forked UUPMediaCreator 从 Windows Update 抓驱动）：

| 需要的 | 出自 |
|---|---|
| 传感器全套 JSON、`sns_reg_config`（**407 B 文本格式**，与指南要求一致）、socinfo 原件 | `qcSensorsConfigQrd8280.cab` |
| **`RSCS.bin`**（1340 B）与 `qcslpi8280.mbn` | `qcsubsys_ext_scss8280.cab`（SCSS = Sensor Core SubSystem） |

★ **交叉校验通过**：cab 里的 `qcslpi8280.mbn` 与本仓在用的那份 sha256
**逐字节相同**（`9c1ce6f5…`），证明来源同出一脉。
★ **socinfo 原件的内容与指南要求逐字一致**：`QRD` / `Unknown` / `0` /
`65536` / `3.1`（`soc_id` 文件 cab 里没有，指南给 `449`，而这与
`tcs3701.json` 里的 `"soc_id": ["449"]` 一致）。
★ `sns_reg_config` 开头确实是 `version=1` + `file=hw_platform=/sys/devices/soc0/hw_platform`
—— 这也解释了**为什么必须有 hexagonrpcd 提供 VFS**：DSP 要按这些路径去读。

提取步骤与两个会绕人的坑（cab 静默解包失败、NTFS 上 JSON 是重解析点）
记在 `device/huawei/gaokun3/firmware/README.md`。

#### （历史）原先的阻塞描述

贡献者的 Phase 4 / Phase 10 需要从 Windows DriverStore 取三类文件：

* `qcsensorsconfigqrd8280*/*.json` —— 传感器驱动配置（我读过，**但没拷**）
* `sns_reg_config` —— DSP 注册表索引。★**必须是 DriverStore 的文本格式
  （约 407 B，`version=1` 开头），不能用 DriverData 的 JSON 格式（2423 B）**，
  后者会让 DSP 注册表初始化崩溃
* `RSCS.bin` —— SLPI 伴生固件

**本机的 Windows 已在 2026-08-20 抹除**（见 `docs/hw-inventory.md` 8quinquies），
所以只能从别处取：向贡献者索取、从 `uup-drivers-sc8280xp` 驱动包提取
（`device/huawei/gaokun3/firmware/README.md` 记的那个来源）、
或另一台仍装着 Windows 的 MateBook E Go。

#### 两个已知的坑（贡献者踩出来的，转录以免重犯）

1. **DSP 固件请求的路径带尾随 `\r`**（它是在 Windows 上编译的）。两处要分别处理：
   * socinfo 走真实文件系统 → 建 `名字
` 的 symlink 即可；
   * registry 走 hexagonfs 的**内部 VFS**、不经过内核 symlink 解析
     → **必须改 `hexagonrpcd/hexagonfs.c`**，在每段路径末尾截掉 `\r`。
     apt 里的现成版本不带这个补丁，所以必须自己编。
2. **`hexagonrpcd` 的 shell wrapper 不认识 sc8280xp**，会 fallback 到错误的
   DSP；必须直接调二进制并显式给 `-f /dev/fastrpc-sdsp -d sdsp -s -R <VFS 根>`。

#### 为什么先在救援 Linux 上验，再谈 Android

`hexagonrpcd` 与 `libssc` 都是 Linux 侧的守护进程/库，贡献者的指南也是针对
Linux 写的。先在内置的救援系统上跑通 `ssccli`，能一次性验证整条 DSP 通路
（fastrpc → hexagonfs → DSP 注册表 → SSC → QMI 读数）。
之后 Android 侧还需要：把 `hexagonrpcd` 移植进 Android（纯 C 守护进程，
用 fastrpc ioctl + 一个 VFS，可移植性不差，但要写 Android.bp 和 sepolicy），
再写一个 AIDL `android.hardware.sensors` HAL 把 libssc 的逻辑包起来喂
SensorService。**那一块目前不存在，是独立的工程量。**

---

### ★★ 实测结果（2026-08-20，救援 Linux 上全程实机）

**结论：加速度计真的通了；光感通不了。** 于是「自动旋转」在本机是**可达的**，
而「自动亮度」不可达。这是本条从"不可达"到"部分可达"的最终定性。

#### ✅ 加速度计：整条 DSP 通路验证通过

```
Accelerometer sensor measurement: X=-0.052672 Y=0.114922 Z=9.873688 m/s²
```

机器平放，Z ≈ **9.87 m/s²**（重力），X/Y 近零；15 秒稳定输出 **131 行**读数。
这一个数字同时证明了整条链路：
`fastrpc → hexagonfs（含 \r 截断补丁）→ DSP 注册表 → SSC → QMI → libssc`。

配置要点（与贡献者指南一致，逐条实测）：
* `hw_platform=QRD` / `soc_id=449` —— ★**独立佐证**：内核
  `/sys/devices/soc0/soc_id` **就是 449**、`machine` 是 `SC8280XP`；
  而 26 个 JSON 里 `QRD` 出现 25 次、`449` 出现 23 次。三方一致。
* `sensors/registry/registry` **必须是空文件**（DSP 找不到覆盖值就用默认值）。
* 恢复手段 = `systemctl restart hexagonrpcd`。⚠️ **需要沉降时间**：
  重启后 6 秒就读，实测拿到 0 行；隔久一点再读才有 131 行。
  所以"重启后读不到"**不等于**坏了，别据此下结论。

#### ⚠️ 光感（tcs3701）：使能后从不返回读数

硬件是在的（`tcs3701.json`，ams 光感+接近，I2C bus 5 / 地址 0x39）。
libssc 的日志显示 registry 服务可用、传感器被发现、
`Sensor enable request sent successfully` —— **然后就再也没有读数**。
同一次会话里 QRTR 节点 9 曾整体消失又重建（服务 400 一并消失）。
更麻烦的是：**尝试过 light 之后，连加速度计也读不到了**，必须重启
`hexagonrpcd` 才恢复 → 光感的使能会**污染整个 SSC 会话**。

⚠️ **两条我自己下错又更正的判断，记下来免得后人重犯**：
1. ❌ "`registry` 传感器起不来，所以光感失败" —— **错**。那份
   `G_MESSAGES_DEBUG` 日志是在**装了生成注册表的坏状态**下抓的。
   加速度计能出数本身就证明 registry 服务是可用的。
   **判据：诊断日志必须在已知good状态下重抓，否则读的是自己制造的故障。**
2. ❌ "`qcom_q6v5_pas 2400000.remoteproc: Handover signaled, but it already
   happened` 是 SLPI 崩溃循环" —— **错，那是良性噪声**。对照实验：空闲 12 秒
   0 条，跑 light 12 秒 13 条，**但跑加速度计（工作正常）12 秒也是 13 条**。
   任何传感器会话都会伴生它。（`2400000.remoteproc` 的 `name` 确实是 `slpi`。）

#### ❌ 负面结果：`sscregistrygen` 预生成注册表会**弄坏**加速度计

思路本来很顺：hexagonrpcd 自带 `tools/sscregistrygen`，用法就写在源码头上
（`-p <平台> -s <soc_id> <配置目录> <输出目录>`，按 JSON 里 `config`
子对象下的 `hw_platform`/`soc_id` 过滤）。跑 `-p QRD -s 449` 生成了
**142 个**文件，其中确实有 `default_sensors.ambient_light`。

**结果：光感照旧没有读数，而加速度计也一起坏了**（`Unable to initialize …`）。
把 141 个文件挪走、只留回空 `registry` 文件并重启 → **加速度计当场恢复**。
干净的 A/B，因果明确。→ **本机不要预生成注册表，空 registry 才是对的。**

#### ★ 为什么"实现写入"不是一个小补丁

DSP 每秒几十次请求写 `/persist/sensors/registry/registry/../temp.json`，
`hexagonrpcd/apps_std.c` 对 `w`/`a` 模式直接返回 `AEE_EUNSUPPORTED`。
本来以为补上写入就能解决，但查了 `hexagonfs.h:34-45` 的 ops 表：

```c
struct hexagonfs_file_ops {
        close / from_dirent / openat / readdir / read / stat / seek
};
```

**没有 write 钩子，整个 VFS 是设计上只读的。** 而且那个 `..` 解析出来的
`/persist/sensors/registry/` 是 `rpcd_builder.c:163-166` 用 `hfs_mkdir`
建的**虚拟目录**，不落盘 —— 就算加了写钩子也没有后端可写，还得先把它改成
映射到真实可写目录。这是给上游加功能，不是打补丁。**别低估这一块。**

#### ⚠️ 出厂校准已随 Windows 永久消失（安装矩阵全零）

每次读数都伴随一条告警：

```
Mount matrix provided by firmware is all 0, falling back to identity matrix!
```

安装矩阵（把芯片坐标系旋到屏幕坐标系）全零，libssc 退回单位矩阵。
指南 Phase 10 的校准来源是
`$WIN/DriverData/Qualcomm/fastRPC/persist/sensors/registry/registry` ——
那是**机器出厂时写在本机 Windows 里的**，不在任何驱动包里，
而本机 Windows 已于 2026-08-20 抹除 → **这份校准数据永久丢失，Phase 10 做不了。**
后果：没有出厂 bias 补偿。
★**但轴向无害**：2026-08-20 用户实机确认**自动旋转方向正确** ——
libssc 退回的单位矩阵恰好与面板方向一致，**不需要在上层纠正**。
（所以"安装矩阵全零"这条只影响精度，不影响可用性。）
⚠️ **给后人**：还留着 Windows 的机器，
**先把那个 registry 目录拷出来再装系统。**

#### 落地路线更新

| 步骤 | 状态 |
|---|---|
| `hexagonrpcd`（打 `\r` 截断补丁后自编） | ✅ **已通**（apt 版不带补丁；注意要连 `libhexagonrpc.so` 一起装 + `ldconfig`） |
| `libssc` + `ssccli` | ✅ **已通**（⚠️ 上游已删掉 `-Dmocking` 选项，照指南写会报 "Unknown option"） |
| **加速度计读数** | ✅ **已通**，Z≈9.87 |
| 光感读数 | ❌ 使能即污染会话，未解 |
| 出厂校准 / 安装矩阵 | ❌ 随 Windows 永久丢失 |
| **Android 侧管道** | ✅ **已打通**（2026-08-20）：hexagonrpcd 在 Android 上运行，**QRTR 服务 400 上线**（node 9 port 13）。工具 `tools/qrtr-lookup/` |
| **Android 侧读数** | ✅ **已通**（2026-08-20）：自研客户端 `device/huawei/gaokun3/ssc/` 在 Android 上读出 `accel` Z≈9.88 m/s² accuracy=3、`gyro` 静止≈0 rad/s。规格见 [`sensors-ssc-protocol.md`](sensors-ssc-protocol.md) |
| 本机传感器清单（SSC 亲口回答）| `accel` ✅ / `gyro` ✅ / `mag` ❌ **本机无磁力计** / `rotv` ❌ 未注册 / `ambient_light` ❌ 污染会话 |
| **AIDL sensors HAL** | ✅ **已实现并实机验证**（2026-08-20）：`device/huawei/gaokun3/sensors-hal/`，SensorService 里能看到 `SH3001 Accelerometer` / `SH3001 Gyroscope`，事件值 `-0.04, 0.05, 9.88` 正在流入，消费者是自动旋转的 `WindowOrientationListener`。★框架还自动融合出 Game Rotation Vector / Gravity / Linear Acceleration |
| 轴向 | ✅ **用户实机确认自动旋转方向正确**，单位矩阵即可，无需纠正 |
| 仍欠 | sepolicy（现 permissive）、`CONFIG_QCOM_FASTRPC=y`（重启后要跑 `scripts/sensors-up-android.sh` 手动补）、把这套编进 ROM |

⚠️ 另有两个环境坑（都会浪费大量时间）：
* `droid-juicer` 会**无限 `openat("/usr/share/droid-juicer/configs")` 死循环**
  （0.4.2 的 bug，`strace` 当场看到），把 apt 卡住 43 分钟。→ `systemctl mask`。
* `initramfs-tools` 的 postinst 在本机**必然失败**
  （`/etc/initramfs/post-update.d/systemd-boot` 返回 1，因为我们的 ESP 布局是自定义的）
  → initramfs 的改动不会自动传播，别以为装完就生效了。

---

## #38 音频与蓝牙在长期运行后可能死锁（用户报告，尚未复现定位）

**状态：用户实机报告，我未复现、未定位。** 记在这里是为了不让它丢掉，
以及给之后动手的人一个明确的起点 —— 不要把它当成已经查清的结论。

### 现象

长时间运行之后，**音频与蓝牙可能死锁**。

⚠️ 我手上没有更细的复现条件（多久、什么负载、是同时死还是各自死、
是整个进程卡住还是只是不出声/连不上）。下面的排查建议是基于本机已知结构
推导的，不是观测结论。

### 为什么值得认真对待

这两个子系统在本机**共享一条通路**，所以"一起死"是合理的：

* 蓝牙是 **WCN6855**，走 `hci_qca`；WiFi 是同一颗芯片（ath11k）。
* 音频跑在 **ADSP** 上（audioreach + 华为拓扑固件），
  而 ADSP 与 SLPI/CDSP 一样是 remoteproc + **QRTR/FastRPC**。
* 传感器（#37）也在这条 QRTR/FastRPC 通路上 —— 而我们已经**实测到过**
  这条通路的会话可以被弄坏：使能光感会污染整个 SSC 会话，之后连加速度计
  都读不到，必须重启 `hexagonrpcd`；HAL 早期版本频繁重建客户端也会把
  传感器枚举彻底弄坏。
  **同一类"会话/客户端泄漏导致整条通路卡住"的失效模式，完全可能出现在
  音频或蓝牙上。**

### 建议的排查顺序（下次动手时照这个来）

1. **先分清是哪一层死的**，不要一上来就怀疑 HAL：
   * `dumpsys media.audio_flinger` / `dumpsys bluetooth_manager` 还响应吗？
     不响应 = 进程级卡住；响应但不出声 = 数据面。
   * `cat /proc/asound/card0/pcm*/sub0/status` —— `state: RUNNING` 且 DMA
     计数在动，说明内核侧还活着，问题在上层。
   * `bootctl`/`ps -A` 看相关进程是否处于 `D` 状态（不可中断睡眠）。
2. **看 remoteproc 有没有崩过**：
   `dmesg | grep -iE "remoteproc|adsp|q6|fatal|watchdog"`。
   ⚠️ 注意 `Handover signaled, but it already happened` 是**良性噪声**
   （#37 已用对照实验证明：工作正常的加速度计同样每 12 秒 13 条），
   不要把它当成崩溃证据。
3. **QRTR 服务表**：本仓已有 `gaokun3-qrtr-lookup`（随镜像发布）。
   死锁时列一遍，和正常时对比 —— 少了哪个服务就指向哪个 DSP。
   这是本机唯一现成的 QRTR 诊断工具。
4. **抓 ANR/tombstone**：`/data/anr/`、`/data/tombstones/`。
   ⚠️ 本机没有串口，init 期的失败也不会进 pstore（init 是主动 `reboot()`
   而不是 panic），所以**别指望 pstore**，证据只能从这两处和 logcat 取。
5. 若确认是 DSP 侧会话卡住，参照 #37 的手法做**干净的 A/B**：
   重启相关守护进程/服务，看是否当场恢复。恢复即说明是会话泄漏而非硬件。

### 影响

* 音频死锁 → 播放/通话不可用，重启可恢复（未验证是否必须重启整机）。
* 蓝牙死锁 → 外设断连、开关蓝牙无响应。
* ⚠️ 对**游戏**的影响未知：如果只是音频输出停掉，游戏本身可能仍能玩。

---

## #39 recovery：镜像能造、能交付，但启动会复位循环（未解，且诊断手段在本机失效）

**状态：卡住。** 记录下来是为了让接手的人不必重走这条路，尤其是不要再用
`init_fatal_panic` 这条在本机注定无效的手段。

### 已经做成的部分（这些是对的，可复用）

* **构建**：`TARGET_NO_RECOVERY := false` + `BOARD_RECOVERYIMAGE_PARTITION_SIZE`
  → 独立的 `recovery.img`（29,161,472 字节）。
  ⚠️ 绝不能用 `BOARD_USES_RECOVERY_AS_BOOT`：`board_config.mk:463` 一看到它就把
  `BUILDING_BOOT_IMAGE` 关掉，会推翻本机的 boot.img。改后已验证
  `BUILDING_BOOT_IMAGE` 仍为 true。
* ★**它的内核与 boot.img 里的 sha256 完全相同**（`8e55f776…`），dtb 也一样
  → ESP 上不必再放一份内核，recovery 条目直接复用该槽的 `Image` 与 `gaokun3.dtb`，
  只需多搬 14 MB 的 ramdisk。
* ★**recovery 的内嵌 cmdline 与 boot 的逐字相同** —— 是 ramdisk 决定它是 recovery，
  不需要特殊 cmdline。
* **交付形态**：ramdisk 作为文件随 `vendor` 走 OTA，由 postinstall 钩子铺到 ESP。
  于是**已装的机器一次普通 OTA 就能拿到**，不必像 `boot_a/boot_b` 那样重装
  （安装器把剩余空间全给了 `userdata`，已装机器没有余地再切分区）。
* **ramdisk 结构完好**（本地解包逐项核对）：`system/bin/recovery` 2,849,968、
  `system/bin/init` 2,468,840、`/init` 是指向 `/system/bin/init` 的符号链接、
  `system/etc/recovery.fstab` 2,257、`system/bin/adbd`、
  `system/bin/update_engine_sideload` 都在，640 个条目，gzip cpio。
  ★ `res/images/fastbootd.png` 在里面 —— 说明 fastbootd 本来是白送的。
* **BLS 条目派生也是对的**：`bootctl list` 能正常列出
  `Recovery (gaokun3) — slot _b`，`initrd` 指向的文件存在且大小正确。

### 失败现象

用 oneshot 启动 recovery 条目后**进入复位循环**，用户手动按电源键才回到 Android。

关键观测：
* **Android 一次都没进** —— `persist.sys.boot.reason.history` 在整个循环期间
  没有新增条目（它只在 Android 启动时追加）。
* **没有任何 panic 记录** —— `/sys/fs/pstore/` 空、EFI 变量里没有 `dmesg-*`。
* `/data/misc/recovery/last_log` 与 `/cache/recovery/` 都是空的
  → recovery 没有正常退出过。
* `misc` 的 BCB 仍是 `boot-recovery`（recovery 完成动作才会清它）。

### ⚠️ 为什么"加 init_fatal_panic 抓 panic"这条路在本机无效

我试过给 recovery 条目加 `androidboot.init_fatal_panic=true panic=10`，
指望把失败转成 panic 让 `efi_pstore` 抓到 —— **一无所获**。原因本仓早有记录
（见"已知坑"）：**Android init 的服务级失败（`reboot_on_failure`）走的是正常
shutdown，不是 `LOG(FATAL)`**，所以 `init_fatal_panic` 覆盖不到，pstore 里
自然什么都没有。`panic=10` 也就无从触发。
→ **别再重复这个实验。**

### 本机的根本困难：没有任何早期启动的可观测通道

* **没有串口**（硬件上就没引出）。
* **recovery 里没有网络栈**（无 WiFi 驱动/supplicant），所以它不会出现在局域网上
  —— 无法像 Android 那样用 adb over TCP 观察。
* **USB adb 在本机是坏的**（#27），而 recovery 的 adbd 只走 USB。
* pstore 这条路如上所述对这类失败无效。

所以现在是"黑盒里循环"，而每次尝试都需要人到机器旁按电源键。

### 建议的下一步（按性价比，都不要再盲试重启）

1. ★**先把 USB adb 在 recovery 里弄通** —— 这是唯一能真正看见内部的通道。
   #27 说的是"拔插后掉"，而全新启动时插着线可能是好的。判据很简单：
   插好线启动 recovery，在主机上看 `adb devices` 有没有 `recovery` 状态的设备。
   通了之后 `adb shell`、`/tmp/recovery.log` 全都能看，这个问题大概率当场就清楚。
2. 若 USB 也不通，就**给 recovery 的 ramdisk 塞一个早期写盘的探针**
   （我们控制这个 ramdisk）：在 `init.recovery.gaokun3.rc` 里挂 ESP 并
   `echo` 阶段标记到文件。这样"走到哪一步"就能在事后从 ESP 上读出来。
3. 也可以先用**最小 recovery**（`TARGET_RECOVERY_UI_LIB` 之类全不带）排除
   图形/UI 初始化的可能 —— 我们连"是不是 minui 拿不到显示"都还不知道。

### 现在的保护措施

**默认不创建 recovery 启动项。** 条目一旦存在，谁在 15 秒菜单里误选一次就得
跑到机器旁按电源键。ramdisk 照样铺（无害，14 MB，将来验证要用）。
要调试：安装器 `ENABLE_RECOVERY_ENTRY=1`，OTA 钩子
`setprop persist.gaokun3.recovery_entry 1`。

---

## #40 ★耳机口不出声 —— 已解决：RX 插值器链从未接上 + 框架找的是不存在的 h2w（2026-08-20）

用户报告耳机接口不能用。实测下来是**三个独立的阻塞点**，全部已定位并修复；
内核侧一点没缺。⚠️ 下面「阻塞点 2」保留了我当时下的错结论与它是怎么错的，因为那个误判很典型。

### 内核侧是完全好的 —— 这点先说清，别再去查它

* **插孔检测在工作**：`/proc/bus/input/devices` 里有
  `"SC8280XP-HUAWEI-GAOKUN3 Headset Jack"`（input12），
  `capabilities/sw = 0xd4` → `SW_HEADPHONE_INSERT(2)` /
  `SW_MICROPHONE_INSERT(4)` / `SW_LINEOUT_INSERT(6)` /
  `SW_JACK_PHYSICAL_INSERT(7)` 四位都声明了。
* **插入被真的识别了**：`dumpsys input` 显示
  `SwState (pressed): SW_HEADPHONE_INSERT, SW_MICROPHONE_INSERT, SW_JACK_PHYSICAL_INSERT`。
* ★**编解码器不但活着，还量出了耳机阻抗**：`HPHL Impedance 62` / `HPHR Impedance 61`、
  `HPH Type 2`。阻抗检测要求 WCD938x 已上电并在通信，所以它没问题。
* **SoundWire 枚举正常**：`sdw:2:0:0217:010d:00:4` 与 `sdw:3:0:0217:010d:00:3`
  = mfg 0x0217 / part 0x010d = **WCD938x**，在 RX 与 TX 两条链路上都在。
  （另外两个 `0217:0202` 是 WSA8830 扬声器。）
* `3200000.rxmacro` 已 probe（driver=rx_macro），
  `/sys/kernel/debug/devices_deferred` 是**空的**。

### 阻塞点 1：Android 音频策略里根本没有耳机设备

`primary_audio_policy_configuration.xml` 里声明的输出设备只有
`AUDIO_DEVICE_OUT_SPEAKER` 与 `AUDIO_DEVICE_OUT_TELEPHONY_TX`；输入只有
`BUILTIN_MIC` / `FM_TUNER` / `TELEPHONY_RX`。
**没有 `WIRED_HEADPHONE` / `WIRED_HEADSET`，也没有耳机麦克风的输入设备。**

实机对应现象：`dumpsys audio` 里只有 `speaker(2)`，
logcat 里 `WiredHeadsetManager: ACTION_HEADSET_PLUG event, plugged in: false`。
→ 即使底层能出声，框架也永远不会往那边路由。**这一条我们自己能修。**

### ★ 阻塞点 2（已解决 2026-08-20）：RX **插值器链**从来没接上

**先记原始症状**，因为它极具误导性：整条混音器通路都能配起来、全部能回读，
但 `tinyplay … -d 0`（MultiMedia1）返回 `Error playing sample`，
`/proc/asound/card0/pcm0p/sub0/status` 保持 `closed`，
而**内核一条错误都没有**。

当时那个 A/B 是对的、但不完整：把已知能用的前端 **MultiMedia2** 从 WSA 后端
改接到 RX 后端，**同样失败**；恢复 WSA 后同一个文件立刻又能播。
这正确地把责任定位到了「RX 这条链」，但我由此下的结论
（"后端 `RX_CODEC_DMA_RX_0` 本身开不起来，原因无定论，候选是拓扑缺 APM 图 /
soundwire 没上电 / q6apm 静默失败"）**是错的** —— 三个候选一个都不是。

**真凶：我配的通路中间断了一节。** 我只设了 `RX_MACRO RX0/RX1 MUX = AIF1_PB`
就以为数据能走到 HPH，实际 rx-macro 内部还有一级**插值器（interpolator）**，
它的输入选择器和解调器输出都停在默认值：

```
RX INT0_1 MIX1 INP0 = ZERO             ← 插值器混音器没有选任何输入
RX INT1_1 MIX1 INP0 = ZERO
RX INT0 DEM MUX     = NORMAL_DSM_OUT   ← 解调器没切到 class-H 输出
RX INT1 DEM MUX     = NORMAL_DSM_OUT
CLSH Switch         = Off              ← class-H 本身没开
LO Switch           = Off
RX_HPH PWR Mode     = ULP
RX_COMP1/2 Switch   = Off
```

DAPM 路径不完整 → 后端 DAI 拿不到有效通路 → PCM open 失败。
**内核不为此打任何日志**，这就是为什么它看起来像"后端坏了"。

补齐后当场通了（同一台机、同一个内核、同一份拓扑，只多设了 9 个控件）：

| 判据 | 修之前 | 修之后 |
|---|---|---|
| `tinyplay -D 0 -d 0` | `Error playing sample` | **rc=0**，正常排空 |
| `pcm0p/sub0/status` | `closed` | **`state: RUNNING`** |
| `hw_ptr` 2 秒增量 | — | 141119 → 239039 = **48960 帧/秒**（正好实时 48 kHz）|
| dmesg | 无 | 无（零报错）|

hw_ptr 按实时速率前进是关键判据 —— 它证明 DMA 在**真实消耗**数据，
不是"打开了但空转"。

### ★ 配方的来源：上游 ALSA UCM2，而且上游本来就把本机当 X13s

不用猜控件顺序。救援 Ubuntu 上 `/usr/share/alsa/ucm2/Qualcomm/sc8280xp/`
就有官配，而 `sc8280xp.conf` 里明写

```
Regex "HUAWEI.*MateBook E.*"  →  include LENOVO-X13s.conf
```

**上游把华为 MateBook E 和 ThinkPad X13s 视为同一套配置**（拓扑固件同理）。
耳机那份配方分散在四个 include 里，缺一节就是上面那个症状：

* `codecs/wcd938x/HeadphoneEnableSeq.conf` —— RDAC / HPH / **CLSH / LO** / `RX HPH Mode CLS_H_ULP`
* `codecs/qcom-lpass/rx-macro/HeadphoneEnableSeq.conf` —— **插值器那 6 条** + PWR Mode + COMP
* `codecs/qcom-lpass/rx-macro/init.conf` —— `RX_RXn Digital Volume 84`
* `Qualcomm/sc8280xp/LENOVO-X13s.conf` BootSequence —— `HPHL/HPHR Volume 2`

存档在 `docs/` 之外没必要，但**方法论值得记**：本机凡是 LPASS 音频的事，
先去救援 Ubuntu 的 UCM2 目录抄，别自己推 DAPM 图。
映射也在那里：耳机 `hw:0,0`、扬声器 `hw:0,1`、耳机麦 `hw:0,2`、内置麦 `hw:0,3`。

### ★ 阻塞点 3（原先没看见）：框架走的是 `/sys/class/switch/h2w`，本机没有

策略里加了 `WIRED_HEADPHONE` / `WIRED_HEADSET` / `IN_WIRED_HEADSET` 之后还不够。
`WiredAccessoryManager` 有两条获知插拔的路：默认那条是 legacy switch class
—— 打开 `/sys/class/switch/h2w` 收 uevent。**本机 `ls /sys/class/switch/` 是
ENOENT**（主线没有 h2w 驱动，也不会有），所以框架从来就不知道插孔存在。

开关在框架资源 `config_useDevInputEventForAudioJack`（设备上
`cmd overlay lookup android android:bool/config_useDevInputEventForAudioJack`
实名核实 = `false`）。设成 true 后它改从普通 evdev switch 设备取
`SW_HEADPHONE_INSERT` / `SW_MICROPHONE_INSERT` —— 而这个源**本来就在、
而且已经在被读**：`dumpsys input` 里那个设备的 `Classes` 是
`KEYBOARD | SWITCH`，还挂着活的 `Switch Input Mapper`。
**内核侧一点没缺，缺的只是这一个 bool。**

### 落地的三处改动

| 改动 | 文件 |
|---|---|
| `config_useDevInputEventForAudioJack = true` | `overlay/frameworks/base/core/res/res/values/config.xml` |
| 三个可插拔设备端口 + 路由（`CARD_0_DEV_0` / `_2`）| `audio/primary_audio_policy_configuration.xml` |
| 耳机 + 耳机麦的完整使能序列 | `bin/audio-route.sh` |

设备端口刻意**不进 `attachedDevices`**（可插拔设备由框架在插入时连接），
且**显式写 profile** 而不是留空 —— 扬声器留空能行是因为它开机就 attached，
可插拔设备留空会让策略在连接时去问 HAL，那是条没验证过的路；
48 kHz / stereo / S16_LE 是实测跑通的配置。

**构建前已在设备上用 overlayfs 验过的部分**（这一步值得做，策略 XML 解析失败会让
整个音频挂掉，不该等两小时构建完才发现）：
* `audio-route.sh` 三段共 45 个控件全部应用，**零个"设置失败"**；
* 重启 audioserver 后策略被接受，`dumpsys media.audio_policy` 里三个新端口
  连地址一起认下（`{AUDIO_DEVICE_OUT_WIRED_HEADPHONE, @:CARD_0_DEV_0}`）；
* 扬声器回归正常（PCM1 仍 `state: RUNNING`）。
* ⚠️ `E APM_AudioPolicyManager: invalid volume index range in the curve` **不是
  我引入的**：干净 A/B，旧策略 12 条、新策略 12 条，完全相同。是既有噪声，另记待办。

**仍未验证的一环**：`config_useDevInputEventForAudioJack` 是框架资源，
只能构建期 overlay，且 `WiredAccessoryManager` 在 SystemServer 启动时读一次，
所以端到端（插入 → 框架切路由 → 耳机出声）必须等新 ROM + 真的插一次耳机。
框架层也没有可用的插拔模拟命令（`cmd audio help` 里没有任何 device/connect/jack）。

### ⚠️ HPH 音量的方向不能猜 —— 从内核算

上游把 `HPHL/HPHR Volume` 设成 **2**，而默认是 **24**（range 0→24）。
到底哪边响？查驱动：

```
sound/soc/codecs/wcd938x.c:2620
  SOC_SINGLE_TLV("HPHL Volume", WCD938X_HPH_L_EN, 0, 0x18, 1, line_gain)
sound/soc/codecs/wcd938x.c:192
  DECLARE_TLV_DB_SCALE(line_gain, -3000, 150, 0)
```

→ 控件值 v 对应 **−30 + 1.5·v dB**。所以**默认的 24 = +6 dB**（满增益直接进耳朵），
上游的 **2 = −27 dB**。耳机灵敏度远高于喇叭，衰减是对的，框架自己还有一层音量。
交叉验证同一份配方里的 `ADC2 Volume 10`：`analog_gain = MINMAX(0, 3000)` over
0→20 → 10 = **+15 dB** 麦克风增益，也合理 —— 说明这个读法是对的。
**嫌小就往上调，每 +1 = +1.5 dB；别接近 24。**

### 顺带记两个会误导人的点

* `tinymix contents` **不是这个版本的子命令**（报 `Invalid mixer control: contents`）。
  列控件用不带参数的 `tinymix`。我第一次因此得出"没有 HPH 控件"的错误结论。
* `tinymix` **可以直接用带空格的控件名**（`tinymix 'CLSH Switch' 1`），
  不必像早期脚本那样用控件编号。用名字更好 —— **编号会随内核/拓扑变化而漂移**。


### ★★ 阻塞点 4 与 5（用户实测"插上还是没有声音"之后才找到，2026-08-21）

上面三点做完、混音器通路实测跑满 48 kHz 之后，用户插上耳机**仍然没有声音**。
耳机当时还插着，所以能在线逐层定位。**框架侧从头到尾都是对的**：

```
dumpsys input          SwitchValues: 0x94   = HEADPHONE|MICROPHONE|JACK_PHYSICAL
InputManager           mUseDevInputEventForAudioJack=true
WiredAccessoryManager  MSG_NEW_DEVICE_STATE
AudioDeviceInventory   setWiredDeviceConnectionState( type:4 (sink) … addr: name:h2w)
                       APM failed to make available device 0x4addr= error=1   ← 卡住
```

⚠️ 顺带确认一件事免得下次白费功夫：`sendevent` 注入 `EV_SW` **是有效的**
（并行 `getevent -lt` 抓到了完整的拔/插序列），所以可以在**没有人在机器边**的
情况下复现插拔。这是本轮能定位的关键前提。

#### 阻塞点 4：可插拔设备端口**不能写 `address`**

`AUDIO_DEVICE_OUT_WIRED_HEADSET` 的连接请求里**地址永远是空串**
（日志里那个 `addr:` 后面什么都没有），而
`HwModuleCollection::getDeviceDescriptor()`（`HwModule.cpp`）里有这道守卫：

```cpp
// Prevent overwriting moduleDevice address if connected device does not have the same
// address (since getDevice with empty address ignores match on address), use dynamic device
if (moduleDevice && allowToCreate &&
        (!moduleDevice->address().empty() &&
         (moduleDevice->address().compare(devAddress.c_str()) != 0))) {
    break;
}
```

连接请求的 `allowToCreate` 是 true，于是"声明的地址非空、请求的地址为空"就
`break` 出去改造一个**动态设备** —— 那个动态设备没有任何 profile/route，连接失败。

★ **规律：attached 的设备可以写 address（Speaker / Built-In Mic），
removable 的绝对不能写。** 不写之后 HAL 靠回落值决定 ALSA 设备，而这个回落
恰好就是我们要的：

```
StreamPrimary.h:61   kDefaultCardAndDeviceId{PrimaryMixer::kAlsaCard, PrimaryMixer::kAlsaDevice}
PrimaryMixer.h:27-28 kAlsaCard = 0, kAlsaDevice = 0        → hw:0,0
```

hw:0,0 正是 `RX_CODEC_DMA_RX_0`（MultiMedia1）耳机后端。运气好。

#### ★ 阻塞点 5（真凶）：AOSP 的 AIDL 默认音频 HAL **压根不接受可插拔设备**

去掉 address 之后失败点往前走了一步，这次 HAL 自己说了原因：

```
AHAL_Module: connectExternalDevice: device port 4 device set to
    AudioDevice{type: AudioDeviceDescription{type: OUT_HEADSET, connection: analog},
                address: AudioDeviceAddress{id: }}
AHAL_Module: populateConnectedDevicePort: module implementation must override
    'populateConnectedDevicePort' to handle connection of external devices.
AHAL_Module: Function: connectExternalDevice Line: 768 Failed
```

`Module::populateConnectedDevicePort()` 是一条**纯错误路径**。
`ModuleAlsa` / `ModuleUsb` / `ModuleBluetooth` / `ModuleRemoteSubmix` 全都
override 了它 —— **只有 `ModulePrimary` 没有**
（`class ModulePrimary final : public Module`）。
所以原装 primary 模块**无法接受任何 removable 设备**，插模拟耳机必失败。

★ 讽刺的是 `Module::connectExternalDevice()` 自己的注释就描述了我们这个情形：

> 2. If the template device port has dynamic profiles, while all routable mix ports
>    have static profiles, [...] the connected device port can be left with dynamic
>    profiles [...] **An example of this case is connection of an analog wired headset,
>    it should be treated in the same way as a speaker.**

而它只在"连接后端口仍只有动态 profile **且**可路由 mix port 也只有动态 profile"
时才拒绝。我们两边都是静态 profile，所以什么都不用 populate ——
`patches/0010` 就是一个"接受并返回 ok"的 override。

**修好之后实测**：

```
AHAL_ModulePrimary: populateConnectedDevicePort: accepting AudioPort{id: 4, name: Wired Headset, …}
AHAL_Module: connectExternalDevice: template port 4 external device connected, connected port ID 26
dumpsys audio:  Devices: headset(4)          ← 不再是 speaker(2)
APM Connected device: 0x4
```

#### ⚠️ 耳机麦（`IN_WIRED_HEADSET`）本轮刻意**不声明**

硬件是好的（hw:0,2 实测录到 384000 帧）。但声明它现在会**弄坏录音**：
可插拔端口不能带 address，而不带 address 时 HAL 回落到 hw:0,0，
那是个**只有播放**的设备（`/dev/snd` 里只有 `pcmC0D0p`，没有 `pcmC0D0c`）
→ 打开必失败。于是插着耳机时录音会从"能用的内置麦"退化成"什么都录不到"。
不声明则一直用内置麦，严格优于现状。
**正解**是给 `StreamPrimary::getCardAndDeviceId()` 加一张"按设备类型回落"的表
（它现在只会回落到 `kDefaultCardAndDeviceId`，不看设备类型）。

#### 验到哪一步为止（不要夸大）

已验证：框架切到 `headset(4)`、HAL 接受连接、混音器通路实测 PCM0 实时跑满
48 kHz、四条通路开机即用、无回归。
**未验证：听感。** 无头触发框架音频这条路试了音量键提示音与
`cmd notification post`（后者的 shell 通道 `sound=null`），
`AudioFlinger` 的 `Total writes` 始终是 0 —— 这个 ROM 里很难无头起 AudioTrack。
★ 好消息是设备上装着**网易云音乐**与 **LineageOS 录音机**，用户可以直接验听与验麦。

### ★ 顺带查出：内置麦克风一直是**完全断的**，而且谁都没发现（2026-08-21）

修完耳机之后顺手把两条采集通路也测了 —— 结果内置麦压根打不开：

```
tinycap -D 0 -d 3   →  cannot open device 3 for card 0
tinypcminfo -d 3    →  连能力都查不到（"Device does not exist"）
```

而音频策略里 `Built-In Mic` 声明的正是 `CARD_0_DEV_3`。**也就是说这台机器
自始至终不能录音，只是没人试过。**

根因和耳机是同一类：**前端混音器没接**
（`MultiMedia4 Mixer VA_CODEC_DMA_TX_0` = Off），再加上 va-macro 的 DMIC
使能序列一条都没设。我之前只补了耳机麦那条（MultiMedia3），漏了这条。
补齐上游 `SectionDevice."Mic"` 的 `va-macro/DMIC0EnableSeq.conf` +
`DMIC1EnableSeq.conf` 之后当场好：`tinycap -c 2` 录到 384000 帧、
`pcm3c` `state: RUNNING`、安静房间 **RMS 981 = −30.5 dBFS**
（近满幅样本只有 4 个瞬态）—— 是真实音频，不是静音也不是直流。

⚠️★ **两个采集 PCM 都只支持双声道**（`tinypcminfo`: `channels min=2 max=2`）。
传 `-c 1` 得到的是 `cannot set hw params: Invalid argument` ——
我一度因此认为**耳机麦也是坏的**，其实它一直是好的，换成 `-c 2` 立刻录到
384000 帧。⚠️ 判断"某条通路坏了"之前先看它宣告的能力。

耳机麦的数据符合"没插耳机"：RMS 25 = −62.3 dBFS、峰值 640、**51.3% 精确零**
（开路输入的样子）。要判它到底好不好，得插一副带麦的耳机。

### 与上游 UCM 刻意不同的两处（记下来免得以后当成漏配）

* `SpkrLeft/Right BOOST Switch`：**我们 0，上游 1**。功放升压器一使能，
  每次流起停都有明显爆音（2026-08-19 A/B 盲听定案）。
* `SpkrLeft/Right VISENSE Switch`：**我们 1，上游 0**。现状出声正常、
  dmesg 无抱怨，故未动；但这是个**未验证的偏离**，将来查扬声器功耗或
  保护逻辑时先看这里。

另两处查过是**已经一致**的，不用设：`WSA MODE` 默认就是上游的 0；
`WSA_RXn Digital Volume` 本机范围是 `0->81` 且已在 81（最大），
而上游写的 `84` 在本机是**超范围值**。


## #41 ★Venus 硬件视频编解码：内核这一半已打通并实机验证（2026-08-21）

`/dev/video0` = `qcom-venus-decoder`、`/dev/video1` = `qcom-venus-encoder`，
`aa00000.video-codec` 绑在 `qcom-venus` 驱动上，`abf0000.clock-controller`
绑在 `sm8350-videocc` 上，**延迟 probe 队列空**，**固件加载失败 0 行**。
据我们所知这是 sc8280xp 上第一次在主线内核 + Android 里把 Venus 跑起来。

### 三个前提，动手前逐个核实过（都不缺）

1. **时钟控制器主线已有**：`drivers/clk/qcom/videocc-sm8350.c` 自己就认
   `"qcom,sc8280xp-videocc"`（该文件 :537 与 :572 两处），不用写新驱动。
2. dt-bindings 头文件在：`include/dt-bindings/clock/qcom,sm8350-videocc.h`。
3. ★**固件我们一直在装，只是名字骗了我**。DTS 补丁把 `firmware-name` 指向
   `qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn` —— 而 `firmware/README.md` 里
   那一行当初被我标成"语音服务（未用到，一并带上）"。
   **VSS = Video SubSystem，不是 Voice。** 设备上实测在，2035748 字节。
   驱动确实读 DT 覆盖：`drivers/media/platform/qcom/venus/firmware.c:224`
   `of_property_read_string_index(dev->of_node, "firmware-name", 0, ...)`。

### 补丁：8 个里打 7 个

`refs/linux-gaokun/patch sets/media/` 的 0013–0020。主线 v7.2 里
`sm8350_res` / `sc8280xp_res` / `llcc_path` / 两个 compatible **一个都没有**
（grep 全 0），所以整套都要打。

* **0014 跳过** —— 纯格式清理（去 of_match 表的尾逗号），而主线已分叉
  （多了 msm8939，sc7280/sm8250 被挪进 `#if !IS_ENABLED(CONFIG_VIDEO_QCOM_IRIS)`），
  打不上也不影响功能。
* 0017/0018/0019 需要 `patch -p1 -F3` 的 fuzz，其余 `git apply` 直接过。
* ⚠️ 我们的内核补丁是**铺在工作树上没提交**的，所以只能 `git apply`，不能 `git am`；
  打完要复核自己的补丁还在（`cooling-maps` 9 处、`gpio174` 1 处，都在）。

### ★ compatible 选 `sc8280xp` 而不是 `sm8350`

0019 的 DTS 原文写的是 `qcom,sm8350-venus`，但 0018 专门为本 SoC 加了
`sc8280xp_res`。两个资源结构**只差一个 freq_tbl**：`sm8350_res` 借用
`sm8250_freq_table`（444/366/338/240 MHz），`sc8280xp_res` 有自己的
（240/338/366/444/533/560 MHz）。既然 0018 就是为本 SoC 加的，用它才对
（否则 0018 是死代码）。bindings 里两个都文档化了
（`Documentation/devicetree/bindings/media/qcom,sm8350-venus.yaml:22-23`）。

⚠️ **顺带发现一个上游小 bug，但【故意不改】**：`sc8280xp_freq_table` 是**升序**，
而其他 SoC 的表（msm8916/msm8996/sdm845/sc7180/sc7280）**全是降序**。
查了消费者才敢下结论：V6 走的 `load_scale_v4` 用的是 **OPP 框架**
（`dev_pm_opp_find_freq_floor/ceil`），`freq_tbl` 只在两处用到 ——
`core_get_v4` 在 **DT 没有 OPP 表时**拿它填 OPP（我们的 DTS 有），
以及 `core_clks_enable` 在 OPP 查找失败时取 `freq_tbl[size-1]` 兜底。
所以在我们这个配置下升序**无害**；改了反而是未经验证的偏离。
（若哪天去掉 DT 的 OPP 表，兜底就会取到 560 MHz 最高档而不是 240 MHz 最低档。）

### ⚠️★ 最难猜的一步：必须关掉 `CONFIG_VIDEO_QCOM_IRIS`

不关的话 Venus 编不过，而**报错完全看不出跟它有关**：

```
core.c:1192: error: 'sm8350_reg_preset' undeclared here
core.c:1194: error: 'sm8250_bw_table_enc' undeclared here
core.c:1210: error: 'VPU_VERSION_IRIS2' undeclared here
core.c:1282: error: 'sm8350_res' undeclared here
```

看起来像补丁打错了。真相是主线 v7.2 引入了新的 iris 驱动接管 IRIS2 世代，于是
`core.c:1017` 的 `#if (!IS_ENABLED(CONFIG_VIDEO_QCOM_IRIS))` 把
`sm8250_freq_table` / `sm8250_bw_table_{enc,dec}` / `sm8350_reg_preset` 全编掉，
`core.h:58` 连 `VPU_VERSION_IRIS2` 都没了 —— 而 `sc8280xp_res` 正好引用其中四个。

★ **关它是对的，不是权宜**：iris 的 of_match 里只有 `qcs8300` / `sm8550` /
`sm8650` / `sm8750` / `x1p42100`，**没有 sc8280xp 也没有 sm8350** ——
它永远服务不了本机，却把本机需要的代码删掉了。而且它是 `=m`，Android 不加载模块。

### ⚠️★ 又是 "=m 坑"，这次整条链上有五个

刷机前的实测值：`MEDIA_SUPPORT=m`、`VIDEO_DEV=m`、`VIDEOBUF2_DMA_CONTIG=m`、
`V4L2_MEM2MEM_DEV=m`、`SM_VIDEOCC_8350=m`。Android **不加载任何模块**，
所以只 `--enable VIDEO_QCOM_VENUS` 会得到"配置里明明开了、设备却不存在"。
八个符号全部拉 `=y` 并写进 `scripts/kernel-config-android.sh` 的 MUST_Y 断言
（44 → 52 条），`VIDEO_QCOM_IRIS` 进 MUST_N。

### ⚠️ 一个会骗过自己的构建脚本写法

第一次构建报 `KBUILD_RC=0` 而实际 `drivers/media` 编译失败 ——
因为 `make ... | tail -30` 之后取的 `$?` 是 **tail 的退出码**。
判据要看产物时间戳：`Image` 还停在旧的 10:09，只有 DTB 是新的。
（本仓在 az CLI 上记过同一个坑，这次是在 make 上重演。）

### 还没做的另一半：Android 侧的 Codec2 组件

内核给出的是 V4L2 M2M 设备，Android 要用它还需要一个 Codec2 组件。
★ 好消息：**`external/v4l2_codec2` 本来就在 crDroid 的 manifest 里**
（`LineageOS/android_external_v4l2_codec2`，groups="pdk"），不用新增仓库。
现有 66 个解码器仍然全是软解。


## #42 ★Android 上**能**写 EFI 变量 —— 推翻 M4/M6 的判断（2026-08-21）

M4/M6 记的是"`efi=noruntime` 所以 Android 写不了 `LoaderEntryOneShot`，
要进别的系统只能先重启到救援 Ubuntu 用 `bootctl set-oneshot`"。**这是错的。**

实测（cmdline 里确实有 `efi=noruntime`）：

```
mount -t efivarfs none /data/local/tmp/efivars   → rc=0，列出 78 个变量
读 LoaderEntrySelected / LoaderDevicePartUUID    → 正常（UTF-16LE）
写 LoaderEntryOneShot-4a67b082-...               → rc=0，回读正确
```

写法：4 字节属性（`NV|BS|RT` = `0x07`，小端）+ 条目名的 UTF-16LE + 双字节 NUL。
覆盖已存在的变量前要 `chattr -i`。

★**而且机制我们 Stage 0 就写下来了，只是没把它和 Android 联系起来** ——
`docs/hw-inventory.md` 第 8 节原文：本机的 EFI 变量走**高通 TrustZone 的
`uefisecapp` 后端**，不依赖 EFI 运行时服务（dmesg 里同时有
`EFI runtime services will be disabled.` 和 `efivars: Registered efivars
operations`），所以 `efi=noruntime` **不影响**变量读写。
M4/M6 那个"Android 写不了"的判断，其实与本仓自己的记录是矛盾的 ——
教训是**跨阶段的结论要回头对一遍旧案卷**，不然会重新发明一个错误。

**为什么这条重要**：它让 Android **自己**就能安排"下一次启动进救援系统"，
而且是 oneshot —— 失败会自动回落到 `default`。这正是"远程优先"缺的最后一块。
本轮就靠它安全地试了 Venus 内核：`default` 全程保持已知可用的 slot_b 不动，
oneshot 指向临时条目；万一新内核起不来，一次断电就回到能用的系统。

### ⚠️ 顺带查明：boot_control HAL 会把"默认项=救援系统"这条纪律覆盖掉

`loader.conf` 里读到的是 `default *-android-b.conf` —— 不是安装器写的
`*-int-ubuntu.conf`。因为 boot_control HAL **每次 Android 启动都把当前槽位
镜像进 loader.conf**（M6 的设计）。于是 `docs/INSTALL.md` 里承诺的
"Android 挂死 → 拍电源键 → 自动回落到可远程接入的系统"这条安全网，
**在首次进 Android 之后就静默失效了**。
本轮的 `adb reboot` 本想去 Ubuntu，结果又回到 Android，就是这么发现的。
有了上面的 oneshot 能力，正解是：让 HAL 只镜像槽位、把 `default` 留给救援系统，
或者干脆改用 oneshot。已记入 TODO。

### ⚠️ toybox 的 `mount` 报 `bad /etc/fstab` 其实是"你不是 root"

`mount -t vfat /dev/block/by-name/esp DIR` 直接报
`mount: bad /etc/fstab: No such file or directory` 并 rc=1，**即使参数完整**，
非常容易让人去追 fstab。

⚠️ **我确实为此下过一次错结论**（"toybox mount 要求 /etc/fstab 存在"）——
因为我当时同时改了两个变量：造了空 fstab **并且**重新拿了 root。
干净的 A/B（root 身份、把 fstab 移走）证明：**efivarfs 与 vfat 都照样 rc=0**。
真正的原因是**非 root 挂载**时 toybox 会去查 fstab，查不到就报这句。
`adb root` 之后重启会掉，每次重启都要重做
（`setprop service.adb.root 1` 然后 `adb root`，见 M3）。

顺带两条真的坑：`/mnt` 在 adb shell 里不可写，挂载点要放 `/data/local/tmp/` 下；
**挂载与后续操作要在同一次 `adb shell` 调用里**（不同调用的挂载命名空间可能不同，
不过实测挂载会保留下来，所以看到 `Device or resource busy` 是"已经挂上了"）。


## #43 光感 tcs3701 为什么不通：与能用的加速度计做逐字段对照（2026-08-21）

#37 记了"光感使能后从不返回读数，而且会污染整个 SSC 会话"。这次把两份
出厂配置逐字段对照，**排除了一条假设，并把嫌疑收敛到一处**。

配置在 `/vendor/etc/hexagonrpcd-root/sensors/config/`（华为专有，不入库）：

| 字段 | sh3001 加速度计（**能用**）| tcs3701 光感（**不通**）|
|---|---|---|
| `bus_type` / `bus_instance` | I2C / **1** | I2C / **5** |
| `slave_config` | 54 (0x36) | 57 (0x39) |
| `dri_irq_num` / `irq_is_chip_pin` | 32 / 1 | 127 / 1 |
| `irq_trigger_type` | 3 | 1 |
| `num_rail` / `rail_on_state` | 1 / **1** | 1 / **2** |
| `vddio_rail` | `/pmic/client/sensor_vddio` | **同一条** |

* ❌ **"DSP 够不到 PMIC 电源轨"被排除** —— 能用的加速度计走的是**同一条轨**。
* ❌ **"SLPI 用不了主 SoC 的 TLMM 脚做中断"也站不住** —— 加速度计同样是
  `irq_is_chip_pin=1`（GPIO 32）。（保留一点余地：加速度计约 8.7 Hz 的流也可能
  是轮询出来的，这条没有被同等强度地证否。）
* ★ **嫌疑收敛到 `bus_instance` 1 vs 5**，其次是 `rail_on_state` 1 vs 2。
  而且它正好能解释"污染整个会话"：往一个没起来的 I2C 控制器发事务会在 SEE
  里挂住，之后连加速度计也读不到，必须重启 hexagonrpcd。

**下一步**：找 SLPI 侧 I2C 实例号到实际 QUP 控制器的映射，确认 instance 5
是否需要 AP 让出某个控制器（或需要 AP 侧不去 claim 它）。
本机 AP 的 DTS 里有哪些 i2c 节点是开着的，是可以直接比对的。


## #44 装 OTA 时的"WiFi 很慢"：⚠️我先归错了因，实际 WiFi 没问题（2026-08-21）

**这条的价值主要在于它记录了一次我自己的误判是怎么被查出来的。**

### 现象

装 OTA 时 1.1 GB 的 payload 下到 2% 几乎停住，约 **10 KB/s**；
同时 `ping 1.1.1.1` **45% 丢包**，dmesg 在刷
`ath11k_pci: msdu_done bit in attention is not set`。
而链路指标完美：RSSI −35 dBm、802.11ax、5200 MHz、协商 Tx 2401 / Rx 1921 Mbps。

### ⚠️ 我当场下的结论（**错的**）

"`msdu_done` 丢帧导致 45% 丢包和吞吐崩塌，是 ath11k 的负载触发缺陷。"
理由看起来很硬：那 25 条报错**全部**落在下载那几分钟（t=1423–1671 s，
uptime 1690 s），开机到下载开始一条都没有，而且是唯一一种 ath11k 报错。

**三个证据推翻了它**：

| 实验 | 结果 |
|---|---|
| 空闲 30 秒后 `ping 1.1.1.1` ×30 | **40% 丢包，新增 msdu_done = 0** |
| 27.5 MB 小下载 | 881 KB/s，**新增 msdu_done = 0** |
| ★ `ping 192.168.31.1`（网关）×30，含 1400 字节大包 | **0% 丢包** |
| ★★ 从本机 HTTP 拉 200 MB（纯 LAN / WiFi） | **61.7 MB/s（≈494 Mbps），新增 msdu_done = 0** |

* **丢包只发生在到 1.1.1.1 的 WAN 路径上**，到网关是 0% —— 连 1400 字节大包
  都不丢。所以**不是无线链路、不是驱动**。（到公网 DNS 的高 ICMP 丢包
  很常见，多半是 ICMP 限速。）
* **msdu_done 与丢包、与日常慢速都不相关**：小下载 881 KB/s 时它是 0，
  而 61.7 MB/s 的大流量 LAN 传输**同样是 0**。它只在那条已经很糟的 WAN
  下载里出现 —— 是**伴随现象**，不是原因。
* ★ **WiFi 能跑到 61.7 MB/s**，这一条就足以把"ath11k 有问题"整个否掉。

### 真正还不知道的部分（不要假装知道）

设备从 R2 拉东西只有 **1–2 MB/s**，而**同一个网络里的 PC 拉同一个 URL 是
36.9 MB/s**。20 倍的差距是真的，**原因未定**。候选（都没验证）：
Android 与 Windows 在高时延（40–50 ms）有损路径上的 TCP 行为差异；
不同的 Cloudflare PoP；运营商对不同主机的策略。
**不要再把它记到 ath11k 头上。**

### ★ 顺带得到一条很实用的运维手段

既然设备的 WAN 慢而 LAN/USB 快，装 OTA 就别让设备自己去下。实测五条链路：

| 链路 | 速度 |
|---|---|
| 本机 ↔ Azure 构建机（scp） | **145 KB/s**（1.1 GB 要两小时，不可用）|
| 构建机 → R2（云到云） | 35 MB/s（1.05 GiB / 30 s）|
| **本机 ↔ R2（Cloudflare）** | **36.9 MB/s**（1.1 GB / 30 s）|
| **本机 → 设备（USB adb）** | **35–36 MB/s** |
| **本机 → 设备（WiFi LAN, HTTP）** | **61.7 MB/s** |
| 设备 ↔ R2（WiFi WAN） | 1–2 MB/s |

最快的路是 **R2 → 本机 → USB → 设备**，然后
`update_engine_client --payload=file:///data/local/tmp/payload.bin`。
实测 **76 秒**装完一个 1.1 GB 的包（让设备自己走 WiFi 那次要二十多分钟）。
★ **`update_engine` 支持 `file://`** —— 在这台没有 recovery、没有 sideload
的机器上，这是最快也最可控的装机手段，值得记住。

### 方法论

⚠️ **"同时出现"不等于"因果"。** 那 25 条 msdu_done 与下载完美重合，
时间相关性非常诱人 —— 但只要多做一步"到网关 ping"和"LAN 大流量"，
结论就整个反过来了。本仓 #37 与 #40 都有同类教训：
**先把嫌疑分量隔离，再下结论。**


## #45 s2idle 分层二分：工具就位、但我把机器弄停住了（2026-08-21）

M4 把"挂得下去、醒不回来"定性成内核/EC 缺陷之后就卡住了，卡点很具体：
**`/sys/power/pm_test` 需要 `CONFIG_PM_DEBUG`，而它没开。** 这一轮把它开了。

### 内核侧（已完成，可复用）

内核 **#20** = #19 + 这几项，`scripts/kernel-config-android.sh` 里已固化：

```
CONFIG_PM_DEBUG=y  CONFIG_PM_SLEEP_DEBUG=y  CONFIG_PM_ADVANCED_DEBUG=y
CONFIG_EXPERT=y    CONFIG_DPM_WATCHDOG=y
```

实机确认 `/sys/power/pm_test` = `[none] core processors platform devices freezer`，
另外多了 `pm_print_times` 与 `pm_debug_messages`。

两个 Kconfig 依赖是查源码才知道的（`kernel/power/Kconfig`），**光 `--enable` 会静默无效**：

* ⚠️★ **`PM_TRACE_RTC` 在 arm64 上不存在** —— 它 `depends on X86`，而 `PM_TRACE`
  是个没有 prompt 的 bool，只能由它 select。**这很可惜**：那个机制（把最后执行的
  设备 suspend/resume 哈希写进 RTC，机器不干净复位之后仍能读出来）
  恰恰是为本机这种"userspace 已冻结、journald 来不及落盘、clean hang 不产生
  panic"的症状设计的。**别再去找它了**，arm64 上的替代品是 DPM_WATCHDOG + pstore。
* ⚠️★ **`DPM_WATCHDOG` 依赖 `PM_DEBUG && PSTORE && EXPERT`**。本机 PSTORE 早是 =y，
  但 **EXPERT 没开**，所以必须一并打开。断言表当场抓住了这一条。
* `DPM_WATCHDOG_TIMEOUT` 发布内核里保持默认 120 秒（压低会把合法的慢设备误判成
  挂死）；测试内核单独设 **10 秒** —— 本机 ~13 秒就复位，120 秒永远轮不到它开火。

### ⚠️ 我自己犯的两个错，都值得记

**错一：先放开了 wakelock。** v1 脚本的顺序是「`wake_unlock` → 设 `pm_test` →
写 `mem`」。放开的那一瞬间 Android 的 **SystemSuspend 抢先发起了一次【真实】挂起**
（那时 `pm_test` 还是 `none`），于是走的正是已知会复位的那条路。
日志停在 `########## pm_test = freezer ##########` 这一行，
**看起来像"连 freezer 都挂"，其实压根没跑到 `echo mem`**。
判据是 uptime：日志里 START 记的是 71，复位后读到 48。
★ 正解：**`pm_test` 先设、全程不碰 wakelock**。持有 wakelock 不挡直接写
`/sys/power/state`（`wakeup_count` 协议只在 `events_check_enabled` 时生效，
而 SystemSuspend 正卡在那个 read 上），但它能保证 SystemSuspend 不来抢。

**错二（代价更大）：测试条目的 cmdline 没有 `panic=`。**
v2 顺序改对了，机器随即从 USB 与网络上同时消失，**并且没有回来**。
最可能的情形是：某个设备的 resume 卡住 → DPM_WATCHDOG 在 10 秒时 panic →
记录进 pstore → 而 `panic_timeout` 默认是 **0**，于是机器停在 panic 不自动重启。
★ **做 DPM_WATCHDOG 测试时，`panic=10` 是那个唯一重要的 cmdline 选项** ——
整个机制的目的就是 panic，而 panic 之后必须自己回来。
我还在 v2 里删掉了 v1 有的 RTC 兜底闹钟，等于把第二道网也拆了。
**后果：需要人按一次电源键**，违反了本项目"永不留下需要到机器旁的状态"这条纪律。

### ★★ 二分结果（2026-08-21 第二轮，工具就位后）

`pm_test` 逐层跑下来，**第一个失败的层是 `devices`**：

| 层 | 结果 |
|---|---|
| `freezer` | ✅ **rc=0、`suspend_stats/success=1`**，多次复现 —— PM 核心、进程冻结、
  `PM_SUSPEND_PREPARE` 通知链都是好的 |
| `devices` | ❌ 失败（下面分两种情形）|

#### 情形 A：WiFi 关联着 —— ath11k 的 suspend 回调卡死（有完整栈）

DPM_WATCHDOG 在 10 秒时开火，panic 落进 efi_pstore，栈是完整的：

```
Kernel panic - not syncing: ieee80211 phy0: unrecoverable failure
ieee80211 phy0: PM: **** DPM device timeout ****
  ath11k_mac_flush_tx_complete
  ath11k_mac_op_flush
  __ieee80211_flush_queues
  ieee80211_set_disassoc → ieee80211_mgd_deauth → cfg80211_disconnect
  cfg80211_leave
  wiphy_suspend                ← suspend 回调本身
  dpm_run_callback / device_suspend / async_suspend
```

而它之前几秒，固件就已经不理人了：

```
[64.73] ath11k_pci: Timeout in receiving vdev delete response
[64.73] ath11k_pci: failed to delete vdev 1: -110
[67.80] ath11k_pci: wmi command 36865 timeout
[67.80] ath11k_pci: failed to setup ps on vdev 0: -11
```

★ **这是一个具体、可上报的 ath11k 缺陷，而且是挂起路上的第一道坎。**
⚠️ 顺带说明 M4 为什么没抓到它：M4 逐个卸掉的是 himax / 三个 remoteproc / EC
驱动，**唯独没试过 ath11k**。

#### 情形 B：ath11k（以及 EC 驱动）都解绑之后 —— 静默整板复位

`devices` 层仍然失败，但**不再 panic**：pstore 0 条，机器在进入该层后
**约 26 秒**整板复位。而这个测试本该 5–6 秒结束
（挂设备 + `mdelay(5s)` + 恢复设备）。

★ **这不是回调卡死。** 三个阶段都装了看门狗（10 秒）却一次都没开火：
`device_suspend` 与 `device_resume` 上游本来就有；`device_prepare` 上游**没有**
（`dpm_watchdog_set` 只出现在 `main.c` 的 1133 与 1986 两处），
我为此专门打了一个取证补丁给它也装上 —— **仍然不开火**。

★★ **而且不存在硬件看门狗**：前一次那个 panic（`panic_timeout=0`）让机器
在 panic 上停了**一个多小时**都没自己复位。所以那 26 秒不是板载看门狗，
是挂起路径自己把板子搞复位了。

#### 剩下的嫌疑面：`suspend_console()` / `resume_console()` / `dpm_complete()`

把 `pm_test=devices` 走到的代码逐段划掉之后，看门狗覆盖不到的只剩这三处。
一个值得注意的巧合：SLPI 那条良性噪声
（`Handover signaled, but it already happened`，#37 已定性）**每秒刷 5 条**，
dmesg 里累计上千条 —— `resume_console()` 要把积压的 printk 一次性冲到 DRM
控制台上。

⚠️ **加 `no_console_suspend` 之后失效模式确实变了**：不再是 26 秒复位，
而是**机器停住不再回来**（`panic=10` 也救不了，因为没有 panic）。
**"改了控制台行为 → 失效模式改变"本身就说明控制台这条路参与其中**，
但它把一个会自愈的故障变成了不会自愈的，所以下一步要换个更安全的靶场再查。

### 这一轮实际得到的东西
### 下次动手的正确姿势

0. ⚠️★ **不要在 Android 侧连续做会失联的实验。** 我这一轮把机器弄到需要人
   按电源键**两次**。`panic=10` 只能救"真的 panic"那一类；情形 B 里机器
   要么静默复位（能自愈）、要么直接停住（不能）。
   **正确的靶场是救援 Ubuntu** —— 它是 `default` 项，任何结局都落回一个
   可远程接入的系统，而且没有 SystemSuspend 来抢挂起、不用绕 wakelock。
   代价只是给 Ubuntu 那套配一份带 PM_DEBUG 的内核。
1. **测试条目的 cmdline 必须带 `panic=10`**，并保留 RTC 兜底闹钟。
2. 更好的做法是**在救援 Ubuntu 里跑这套二分**，而不是 Android：
   Ubuntu 是 `default` 项，任何挂死/复位都落回一个可远程接入的系统；
   而且没有 SystemSuspend 来抢挂起，也不用绕 wakelock。
   代价是要给 Ubuntu 那套配一份带 PM_DEBUG 的内核。
3. 层级顺序 `freezer → devices → platform → processors → core`，
   一层一次、每层前把层名写进标记文件（v2 这么做了，这一点是对的）。
4. 先读 pstore 再重跑 —— 上一次的 panic 栈可能已经直接给出答案。


## #46 s2idle 第三轮：靶场搬到救援 Ubuntu，把"问题 2"逼到内核可见范围之外（2026-08-21）

#45 把 s2idle 拆成两个问题之后，这一轮按自己写下的教训**把靶场从 Android 换到
救援 Ubuntu**。结论先说：**问题 2 不是驱动缺陷，它发生在内核看得见的范围之下。**

### 靶场本身就是这一轮最大的改进

`default` 指向普通 Ubuntu 条目，测试用的是另一个条目（同一份 PM 调试内核 + `panic=10`），
靠 `bootctl set-oneshot` 进入。于是**每一次失败都自动落回一个可远程接入的系统**，
一轮实验从"要人按电源键"变成"约 3 分钟一次、全自动"。
`scripts` 里没有留这套东西（一次性的），但方法记在这里：

* Ubuntu 侧可以直接 `sudo bootctl set-oneshot <entry>` —— 比在 Android 里写
  EFI 变量省事得多。
* `journalctl --list-boots` + `journalctl -b -1 -k` 能读上一次启动的内核日志
  （该机 `/var/log/journal` 存在，journald **是持久化的**）。

### 排除清单（每一条都是一次实机实验）

| 实验 | 结果 |
|---|---|
| `pm_test=freezer` | ✅ `rc=0`、`suspend_stats/success=1`（Android 与 Ubuntu 都是）|
| `pm_test=devices` 基线 | ❌ 静默整板复位 |
| 解绑整个显示栈（`msm_dpu`/`msm_dsi`/`msm-dp-display`/`msm-mdss`，DRM 卡剩 0）| ❌ 照样复位 |
| 解绑 `ath11k_pci`（wiphy 消失）| ❌ 照样复位 |
| 解绑 `gaokun-ec` | ❌ 照样复位 |
| 三个 remoteproc 全 `stop`（offline/offline/offline）| ❌ 照样复位 |
| **以上四项同时做** | ❌ **照样复位** |
| **真实挂起**（`pm_test=none` + RTC 闹钟，全部设备保持绑定）| ❌ 复位，**零取证** |

### ★ 关键否定证据

* **没有任何驱动回调卡住。** 三个阶段都有 10 秒 DPM 看门狗
  （`device_suspend`/`device_resume` 上游自带，`device_prepare` 是我加的取证补丁），
  **一次都没开火**，pstore 始终 0 条。
* **不是硬件看门狗。** `/sys/class/watchdog/watchdog0` 存在但没人喂
  （systemd `RuntimeWatchdogUSec=0`），而机器能稳定运行几分钟不复位；
  更硬的证据是 #45 那次 `panic_timeout=0` 的 panic 让机器**停了一个多小时**都没复位。
* **持久 journald 也救不了那个窗口**：挂起一开始 userspace 就被冻结，
  上一次启动的内核日志停在挂起前一刻，之后什么都没有。
* ⚠️ **一次无效实验，记下来免得被当成结论**：我试过"挂起前把 CPU 压到最低频"
  来验证欠压假设，但 `powersave` 调速器没真的把频率降下来
  （`scaling_cur_freq` 仍是 1670400 / 2688000），**所以那次测试不算数**，
  欠压假设既没被证实也没被证否。

### 顺带读到的 EC 挂起时序（不是元凶，但值得记）

`refs/gaokun-buildbot/drivers/gaokun-ec/huawei-gaokun-ec.c:623`：

```c
static int gaokun_ec_suspend(struct device *dev)
{
	u8 ec_req[] = MKREQ(0x02, EC_STANDBY_REG, 1, EC_STANDBY_ENTER);
	ret = gaokun_ec_write(ec, ec_req);      /* 告诉 EC「要进 standby」 */
	msleep(100);
	gpiod_set_value(ec->enable_gpio, 0);    /* ★ 把 EC 的使能脚拉低 */
```

resume 反过来（拉高 → `msleep(100)` → 最多重试三次 `EC_STANDBY_EXIT`），
注释还写着 "Resume may be unstable, so open lid anyways"。
⚠️ **但解绑 EC 驱动（这段完全不执行）之后照样复位**，所以它不是触发点。

### 现在的判断

**问题 2 = 平台/固件层面的复位**，内核在它发生前没有任何机会记录。
继续用"解绑再试"已经没有信息量了 —— 能解绑的都排除完了，剩下的是时钟、稳压器、
interconnect、PCIe/NVMe、pinctrl、rpmhpd 这些拆不掉的核心件。

**下一步应该换一类工具，而不是继续这条路：**

1. ★ **对照 ThinkPad X13s。** 同 SoC、主线内核上 s2idle **是能用的**（jhovold 树）。
   所以差异一定在 gaokun 的 DT / 固件 / EC 上。把两边的 DTS 与 suspend 相关节点
   （rpmhpd、AOSS、smp2p、pdc 唤醒映射）逐项对照，比在本机瞎试有效得多。
   ⚠️ 注意 `recommended/0017-arm64-dts-qcom-sc8280xp-add-several-missing-pdc-map-.patch`
   已经在 buildbot 的应用列表里，但**还有 `0018-HACK-pinctrl-qcom-sc8280xp-do-not-map-gpio175-to-pdc`**
   —— 这类 PDC 唤醒映射的差异正是 s2idle 的常见坑，值得先看。
2. 若要继续在本机取证，唯一还没用过的通道是 **USB gadget 串口控制台**
   （`g_serial` + `console=ttyGS0`），从 PC 端读。代价不小，而且 UDC 自己也会挂起。
3. **问题 1（ath11k）与问题 2 是独立的**，可以先把问题 1 报给上游 / 自己修 ——
   它有完整栈，不依赖问题 2 的进展。


## #47 ★s2idle 定性改写：**挂起是成功的，死在"任何唤醒"**（2026-08-21）

这一条推翻了从 M4 一直沿用到 #45/#46 的说法（"不能待机"／"resume 失败"太笼统）。
干净的 A/B 是这样出来的：

| 唤醒源 | 结果 |
|---|---|
| **不设任何唤醒源** | ★ 机器**安安静静睡着**，几分钟内不复位、不发热、不回来 |
| RTC 闹钟（+45 s） | 闹钟按时触发 → **整板复位** |
| 电源键（`pon@1300:pwrkey`，已注册且 `enabled`）| **整板复位**（落到 `default` = Android，所以屏幕会亮起来）|
| 盖子 / 键盘（EC `15-0038`，已注册且 `enabled`）| **整板复位**（同上）|

★ **第一行是新信息，也是最关键的一行**：以前所有测试都带 RTC 闹钟，所以看到的
永远是"睡下去 → 二三十秒后复位"，很自然地被读成"挂起坏了"。**去掉唤醒源之后
机器睡得好好的** —— 说明 `suspend_prepare` → `dpm_suspend*` → `syscore_suspend`
→ 进入 s2idle 这整条路是通的。**坏的是唤醒/恢复那一侧，而且与唤醒源无关**
（三种唤醒源结果完全一致）。

⚠️ **一个会反复骗人的观察**：三种失败在用户眼里都是"屏幕亮起来了"，
因为复位之后 `default` 指向 Android，机器会自己开机进桌面。
**唯一算数的判据是脚本日志里 `POST` 那一行有没有写出来**
（以及 `uptime` 是不是从头开始）。本轮就靠这条判据纠正了一次"电源键能唤醒"的误判。

### 与上游说法的关系

`refs/linux-gaokun/README.MD:67` 写的是 `| Suspend | works | s2idle. To support
lid wakeup, EC may stop suspend with a 0xc0 event`。我们复现不了"works"，
而且**用 Ubuntu 自带的内核 `#2`（上游配置、非我们的 Android 配置）结果完全一样**
—— 挂起进得去、RTC 唤醒必复位。所以：

* ❌ **不是我们的 Android 内核配置**（这条以前没被排除过，现在排除了）；
* ❌ 不是 Android 侧的 SystemSuspend / wakelock（Ubuntu 上同样）；
* 与上游的差异只可能在：他们的内核版本／DTB／固件版本／BIOS，或者那句 "works"
  是在别的条件下成立的。**值得直接拿这份数据去问 gaokun 社区** ——
  我们现在能给出的是"挂起成功、三种唤醒源全部导致整板复位、无任何取证"，
  这比"不能待机"有用得多。

### 排除清单（截至本轮，每一条都是实机实验）

| 假设 | 怎么测的 | 结果 |
|---|---|---|
| 驱动的 suspend/resume 回调卡住 | 三个阶段各 10 秒 DPM 看门狗（`device_prepare` 那个是本轮加的取证补丁）| ❌ 从未开火，pstore 始终 0 条 |
| 显示栈 | 停 gdm 后解绑 `msm_dpu`/`msm_dsi`/`msm-dp-display`/`msm-mdss`，DRM 卡剩 0 | ❌ 照样复位 |
| ath11k | 解绑 `ath11k_pci`，wiphy 消失 | ❌ 照样复位 |
| 华为 EC | 解绑 `gaokun-ec 15-0038` | ❌ 照样复位 |
| 三个 remoteproc | 全部 `echo stop`（offline）| ❌ 照样复位 |
| **以上四项同时** | 一次全做 | ❌ **照样复位** |
| 我们的 Android 内核配置 | 换成 Ubuntu 自带的 `#2` 内核（上游配置）| ❌ 一模一样 |
| Android 的 SystemSuspend / wakelock | 整套改在 Ubuntu 上重做 | ❌ 一模一样 |
| 硬件看门狗 | `watchdog0` 没人喂而机器稳定运行；一次 `panic_timeout=0` 的 panic 让机器停了一个多小时没复位 | ❌ 不存在这样的看门狗 |
| 控制台挂起/恢复 | 加 `no_console_suspend` | ⚠️ **失效模式变了**（复位 → 停住），说明这条路参与其中，但没解决 |
| ★ **CPU rail power collapse** | 把 8 个 CPU 的 `cpuidle/state1/disable` 全设为 1（只留 WFI），再带 RTC 闹钟真实挂起 | ❌ **照样复位** |

★ 最后一条值得单独说：本机 cpuidle 只有两级 —— `state0 = WFI`、
`state1 = cpu-sleep-N-0（little/big-rail-power-collapse，退出延迟 1264 µs）`，
驱动是 `psci_idle`。"s2idle 期间 CPU 进入 PSCI 电源塌陷、而本平台从该状态退出
是坏的"是个很对症的假设（正好解释"睡得下去、任何唤醒都复位"），
**但实测把它禁掉之后照样复位**，所以不是它。

### 仍然没有取证手段（已穷尽本机自有通道）

* 三个阶段的 10 秒 DPM 看门狗（`device_prepare` 那个是本轮加的取证补丁）
  **从未开火**，`/sys/fs/pstore` 始终 0 条 ⇒ **没有任何驱动回调卡住**。
* ⚠️★ **恢复路径上还有一段没有看门狗**：`dpm_resume_noirq` / `dpm_resume_early`
  / `syscore_resume`（上游的 `dpm_watchdog_set` 只在 `device_suspend` 与
  `device_resume` 两处）。这正是"平台级恢复"最容易出事的地方，也是唯一还没
  插桩的窗口。**但看门狗只能抓"卡住"，抓不到"掉电复位"** —— 而现有证据
  （从来没有 panic）更像后者，所以这一步的期望收益只能算中等。
* journald 持久化在这里帮不上忙：挂起一开始 userspace 就冻结。

### 运维教训（第二条，与 #45 第 0 条并列）

⚠️ **救援 Ubuntu 只能通过 WiFi 远程接入**（它没有 adb）。这一轮它有一次
起来了但没连上 WiFi，于是既不在网上也不在 adb 上 —— 从远端看和"挂死"一模一样，
只能靠人看屏幕。
**改进方向**：给救援系统加一条不依赖 WiFi 的带外通道
（USB gadget 网卡 `g_ether` 或串口 `g_serial`），本机 USB 口是通的。


## #48 ★★★ s2idle 在上游 7.1.0-rc3 内核上**完全正常** —— 这是一个 7.1→7.2 回归（2026-08-21）

同一台机器、同一个 BIOS 2.16、同一个救援 Ubuntu 根文件系统，只换内核：

```
[up=95 ] PRE 真实挂起
[up=137] ★★★ POST rc=0 —— RESUME 成功！success=1 fail=0
   PM: suspend entry (s2idle)
   Restarting tasks: Starting / Done
   PM: suspend exit
```

**连续 4 次挂起/恢复，`success=4 fail=0`**，uptime 全程连续（无复位）。
内核时间只走了 2.6 秒而墙钟走了 40 秒 —— 教科书式的 s2idle。

内核：`7.1.0-rc3-gaokun3+ #1 SMP PREEMPT Thu May 14 00:21:38 UTC 2026`
（原始 gaokun 安装带来的上游构建，`/boot/vmlinuz-7.1.0-rc3-gaokun3+`）。

### 这一条推翻了此前所有关于 s2idle 的定性

* ❌ **不是硬件/EC 缺陷**（M4 的定性、也是 README 首屏挂了很久的说法）
* ❌ 不是 Android、不是我们的设备树、不是 ath11k/显示/remoteproc
* ✅ **是内核回归**，而且**可二分**

### ⚠️ 同时更正 #46/#47 里我自己的一个错误

我在 #46/#47 写过"用 Ubuntu 自带的 `#2` 内核（上游配置）结果一样 ⇒ 排除我们的
配置"。**这是错的** —— `strings vmlinuz` 显示那个 `#2` 是
`Linux version 7.2.0-rc2-gaokun3+ (vahiru@CICD)`，**是我们自己构建机编的**，
不是上游构建。所以那条排除不成立，"我们的配置"当时并没有被排除。
★ 教训：**判断"这是谁编的内核"要看 `strings vmlinuz | grep "Linux version"`
里的构建者字段，不能只看 `uname -v` 的 `#N`。**

### 怎么做到的（方法本身值得复用）

原始安装的 7.1 整套还留在救援分区上：`vmlinuz-` / `initrd.img-` / `dtb-` /
`/lib/modules/7.1.0-rc3-gaokun3+/` / `config-`。做法：

1. **从 Android 里只读挂载 Ubuntu 分区**（`mount -t ext4 -o ro /dev/block/nvme0n1p3`），
   不用重启就能读它的 `/boot`；
2. 腾 ESP 空间（删掉两个用不上的 `recovery-ramdisk.img`，30 MB），
   把 7.1 的 kernel/initrd/dtb 拷进 ESP，克隆一个 BLS 条目；
3. ★ **把测试做成开机自启的 systemd oneshot 服务**，结果写进 `/var/log/s71.log`
   —— 这样**不依赖 WiFi**：即使救援系统没连上网，也能事后从 Android
   挂载分区把结果读回来。（本轮救援 Ubuntu 的 IP 从 .230 漂到 .123，
   正是靠这个设计才没白跑。）

### 配置差异（7.1 上游 vs 我们的 7.2 Android）

PM/挂起相关的差异共 33 条，**绝大多数是 `=m` vs `=y`**（我们是单体内核，
Android 不加载模块）。唯一一条语义上直接相关的是：

| 符号 | 7.1（能挂起）| 我们的 7.2（不能）|
|---|---|---|
| **`PM_WAKELOCKS`** | **n** | **y**（`_GC=y`、`_LIMIT=100`）|

⚠️ 但这条是**混淆的** —— 代码（7.1 vs 7.2 + 补丁集）和配置同时不同，
不能据此下结论。

### 下一步（按信息量排序）

1. ★ **用 7.1 的 config 在 7.2 树上编一次**（`make olddefconfig`）。
   通了 ⇒ 是**配置**差异，可以再二分配置（很快）；仍坏 ⇒ 是**代码**回归。
   这是一次构建就能把问题劈成两半的实验。
2. 若是代码回归：在 7.1.0-rc3 → 7.2.0-rc2 之间 `git bisect`
   （约 10 次构建，每次约 15 分钟）。注意 buildbot 的补丁集在两个版本上不同，
   二分时要固定补丁集或只二分 mainline。
3. **实用旁路**：如果用户现在就想要待机，可以考虑把 Android 的内核换回 7.1
   —— 代价是 Venus（7.2 的补丁）与其他 7.2 特性，需要评估补丁能否回移。


## #49 s2idle 收敛到 **EC 绑定与否**（2026-08-21）

> ⚠️★ **本节原标题是「元凶定位到 EC 驱动的 suspend/resume」，已被 #50 推翻。**
> EC 驱动在 v7.1 → v7.2-rc2 之间**只改了一行纯风格代码**，不可能是回归所在；
> 真正的 delta 在 **geni I²C 的 `*_noirq` 回调**上。下面的**实测数据全部仍然成立**（包括“解绑 EC 就能挂起”与两个 `pm_test` 更正），只是归因错了。

接 #48（7.1 能挂起）。这一轮把范围收到了一个驱动上。

### 决定性实验

**在纯 mainline v7.2-rc2 上解绑 EC 驱动，然后做真实挂起：**

```
[up=593] PRE 真实挂起（EC 已解绑）
[up=635] ★★★ POST rc=0 success=1
   PM: suspend entry (s2idle) → Restarting tasks → PM: suspend exit
```

墙钟走了 42 秒、内核时间只走 2 秒、uptime 连续无复位 —— **真的睡了 40 秒并被
RTC 叫醒**。随后**连挂 4 次全部成功、0 新增失败**。

⇒ **`huawei-gaokun-ec` 的 suspend/resume 处理就是元凶。**

### 三个内核的行为对照（同机同 rootfs，只换内核）

| 内核 | EC | 结果 |
|---|---|---|
| 上游 7.1.0-rc3（#48）| 绑定 | ✅ 挂起+唤醒，4/4 |
| **纯 mainline v7.2-rc2** | 绑定 | ⚠️ 挂起**干净失败**：`last_failed_dev=15-0038`、`step=suspend_noirq`、`errno=-110`，dmesg `geni_i2c a9c000.i2c: Timeout abort_m_cmd` |
| **纯 mainline v7.2-rc2** | **解绑** | ✅ **挂起+唤醒，4/4** |
| 我们的 7.2（+buildbot 补丁）| 绑定 | ❌ 睡得下去，**任何唤醒都整板复位** |

★ 第二行说明 buildbot 那个
`platform/arm64: huawei-gaokun-ec: fix suspend/resume ordering`
（把 EC 的 PM 回调从 NOIRQ 挪到普通阶段）**是必需的** —— 没有它，EC 的 I2C
握手落在 I2C 控制器已挂起之后，必然 -110。**所以不是这个补丁引入的回归。**

### 已排除的两个具体嫌疑

* ❌ **`introduce EC enable pin`（拉低 `enable-gpios`）不是 delta** ——
  反解两棵 DTB，EC 节点**完全相同**，7.1 也有 `enable-gpios = <0x4a 0xad 0x00>`，
  该提交（2026-04-18）早于 7.1 构建（2026-05-14）。
* ❌ **不是 `STANDBY_EXIT` 握手来不及** —— 把 `gaokun_ec_resume` 的重试
  从 3 次（约 300 ms）放宽到 30 次（约 3 秒）并加打点，**照样整板复位**。

### ⚠️⚠️ 必须更正：`pm_test=devices` 在本平台是**无效测试**

#46/#47 里那一大批"排除"（显示栈 / ath11k / EC / 三个 remoteproc / 四项同时 /
CPU power collapse）**全部是用 `pm_test=devices` 做的，因此全部作废**。

判据就在本轮：**同一个 EC 解绑状态下，`pm_test=devices` 会整板复位，
而真实挂起（`pm_test=none`）却成功并唤醒。** 说明 `pm_test=devices` 本身
（挂完所有设备后让 CPU 满速空转 5 秒再恢复）在这台机器上就会杀死板子 ——
很可能是 rpmhpd 的 CX/MX 票已经降下来而 CPU 还在跑。

★ **教训：在这台机器上验证挂起，只能用真实挂起（`pm_test=none` + 唤醒源）。**
`pm_test` 的 `devices` 及更深的层级不可用；`freezer` 层可用（它不碰设备）。

### ⚠️ 另一处更正（#46/#47）

我写过"用 Ubuntu 自带的 `#2` 内核（上游配置）结果一样 ⇒ 排除我们的配置"。
那个 `#2` 是 `Linux version 7.2.0-rc2-gaokun3+ (vahiru@CICD)` ——
**我们自己编的**。判断内核出处要看 `strings vmlinuz | grep "Linux version"`
里的构建者字段，不能只看 `uname -v` 的 `#N`。

### 下一步 → 已在 #50 里做完

当时列的四条，现在的下落：

1. ❌ 只去掉 `gpiod_set_value(ec->enable_gpio, 0)` —— 被第 2 条覆盖。
2. ❌ **EC 的 PM 回调整个 `return 0`：照样整板复位**（实验 #24）—— PM 回调被排除。
3. ✅ **拿 7.1 源码逐字 diff —— 这一条是对的，而且直接找出了元凶**。浅克隆只需 `git fetch --depth=1 --no-tags origin tag v7.1`（不必 `--unshallow`）。
4. ⚠️ “解绑 EC 当旁路”不再需要 —— 见 #50 的真修复。

★ **方法论（贵买的）**：碰上“旧版本行、新版本不行”的回归，**先把两个版本的相关驱动 diff 出来看改动量**，比在实机上一个一个试候选便宜一个数量级：本例里 EC 驱动 1 行、geni 一共 263 行，一眼就知道该看哪个。

---

## #50 ★★★ s2idle 元凶改判：不是 EC 驱动，是 **7.1→7.2 的 geni I²C noirq 重构**（2026-08-21）

接 #49。这一轮把 #49 的**标题结论推翻了**，并给出了一个有源码依据的新元凶。

### 先说三个把 #49 打掉的事实

**1. EC 的 PM 回调整个 return 0，照样整板复位。**

`gaokun_ec_suspend()` / `gaokun_ec_resume()` 开头直接 `return 0`（EC 保持绑定、
中断照常注册、子设备照常在），实验 #24：

```
[up=91] EC 绑定=1  子设备=2
[up=91] PRE 真实挂起
（无 POST —— 复位进 Android）
```

⇒ **EC 的 PM 回调被完全排除**。#49 的"下一步 1/2"至此都做完了，两条都是阴性。

**2. ★ EC 的中断根本不走 tlmm 的 wakeirq 映射表。**

```
interrupts-extended = <&pdc 215 IRQ_TYPE_LEVEL_LOW>;
enable-gpios = <&tlmm 173 GPIO_ACTIVE_HIGH>;
// not stable yet, so comment out it
// wakeup-source;
```

我按 buildbot 那个 `HACK: do not map touchscreen's IRQ to PDC`（它的提交说明原文
是"too many wakeup IRQs are unmasked … fix impact from **new added EC wakeup
IRQ**"）依样画葫芦，把 `{ 107, 217 }` 从 `sc8280xp_pdc_map[]` 里删掉重编 ——
实验 #25 照样复位。

⚠️ **这是一次我自己的错误**：EC 用的是 `&pdc 215`，**直连 PDC**，
跟 tlmm 107 毫无关系。花了一次构建 + 一次实机测试测了个无关变量。
★ 教训：**动手改一个引脚号之前，先把 DTS 里那一行读出来贴上**，
不要从"同类补丁"倒推脚号。

**3. ★★ EC 驱动在 v7.1 → v7.2-rc2 之间只改了一行，而且是纯风格的。**

```diff
 static const struct i2c_device_id gaokun_ec_id[] = {
-	{ "gaokun-ec", },
+	{ .name = "gaokun-ec" },
```

⇒ 既然 7.1 能挂起、7.2 不能，而 EC 驱动**功能上一字未变**，
**回归就不可能在 EC 驱动里。** EC 只是受害者/触发者。

### ★★★ 新元凶：`i2c-qcom-geni.c` 的 `*_noirq` 回调被重写

`git diff v7.1 v7.2-rc2` 在四个相关驱动上的规模，一眼就能看出该看哪个：

| 文件 | 改动量 |
|---|---|
| `drivers/platform/arm64/huawei-gaokun-ec.c` | 1 +/1 −（纯风格） |
| `drivers/i2c/busses/i2c-qcom-geni.c` | 11 +/13 − |
| **`drivers/soc/qcom/qcom-geni-se.c`** | **252 +/18 −** |
| `drivers/irqchip/qcom-pdc.c` | 40 +/23 − |

`qcom-geni-se.c` 新导出了一整套 `geni_se_resources_activate/deactivate`、
`geni_se_set_perf_level/opp`、`geni_se_domain_attach`、`geni_icc_set_bw_ab`
—— 是一次 geni 资源管理的重构。而 i2c 侧的落点正是挂起路径：

```diff
 static int geni_i2c_suspend_noirq(struct device *dev)
 {
 	i2c_mark_adapter_suspended(&gi2c->adap);
-	if (!gi2c->suspended) {
-		geni_i2c_runtime_suspend(dev);      /* ← 返回值被丢弃 */
-		pm_runtime_disable(dev);
-		pm_runtime_set_suspended(dev);
-		pm_runtime_enable(dev);
-	}
-	return 0;
+	ret = pm_runtime_force_suspend(dev);
+	if (ret)
+		i2c_mark_adapter_resumed(&gi2c->adap);
+	return ret;                                 /* ← 现在会传播错误 */
 }

 static int geni_i2c_resume_noirq(struct device *dev)
 {
+	ret = pm_runtime_force_resume(dev);         /* ← 7.1 里【什么都不做】 */
+	if (ret)
+		return ret;
 	i2c_mark_adapter_resumed(&gi2c->adap);
 	return 0;
 }
```

**两处语义变化，各自解释一个我们实测到的症状：**

* ★ `resume_noirq` 现在会 `pm_runtime_force_resume()` —— 也就是在
  **noirq 阶段**真的去把控制器供上电（`core_clk` + 三条 interconnect 票 + OPP）。
  7.1 里这个函数只打了个 `i2c_mark_adapter_resumed` 标记，控制器一直停在
  runtime-suspended，等下一次传输再懒加载。**noirq 阶段中断还关着、供应方
  （rpmh / interconnect / 电源域）不保证已恢复**，此时摸 geni 寄存器就是
  未上电访问 → 整板复位。**这与"睡得下去、任何唤醒都复位"完全吻合。**
* ★ `suspend_noirq` 现在传播 `geni_i2c_runtime_suspend()` 的错误，
  而 7.1 把它丢掉了。**这正好解释纯 7.2 上那个"干净失败"**：
  `last_failed_dev=15-0038` / `step=suspend_noirq` / `errno=-110` +
  `geni_i2c a9c000.i2c: Timeout abort_m_cmd` —— 同样的超时在 7.1 上会被咽掉。

### ★ 顺带解释了"解绑 EC 就能挂起"

`pm_runtime_force_suspend()` 只有在设备**当时是 runtime-active** 的情况下才会
置 `needs_force_resume`，从而让 `force_resume` 真的去上电。
EC 解绑后 `a9c000.i2c` 上没有任何用户，长期停在 runtime-suspended
→ `force_suspend` 什么都不做 → `force_resume` 也什么都不做 → **不会有
noirq 阶段的未上电访问** → 挂起唤醒正常。

⇒ **"EC 是元凶"其实是"EC 是那条 I²C 总线上唯一的用户"的假象。**

### 决定性实验（实验 #26）

把 `i2c-qcom-geni.c` 的 `suspend_noirq`/`resume_noirq`（连同 `gi2c->suspended`
标志）**逐字还原成 v7.1**，其余一切不动（我们完整的 7.2 + buildbot 补丁、
EC 全绑定、PM 回调原版），连做 3 次真实挂起。

```
[03:54:58 up=51] START #26  实验=EC 全绑定，i2c-qcom-geni 的 noirq 语义还原到 v7.1
[03:55:38 up=91] EC 绑定=1  子设备=2
[03:55:38 up=91] PRE 第 1 次真实挂起
（无 POST —— 复位进 Android）
```

**❌ 阴性。** 上面那套听起来严丝合缝的推理**是错的**，或者至少不完整。

### ⚠️ 连带查证：geni 的另外两处改动对本机也不成立

顺着"7.2 重构了 geni 资源管理"继续查，还找到两个看着很像的东西，
**逐个核对后都不成立**，一并记下来免得后人再走一遍：

* `geni_se_clks_off()` 在 7.2 里**新增了 `clk_disable_unprepare(se->core_clk)`**，
  而 `i2c-qcom-geni.c` 里那句 `clk_disable_unprepare(gi2c->core_clk)` 还在
  —— 看起来像典型的"引用计数下溢把还在用的时钟关掉"。
  ❌ **不成立**：`gi2c->core_clk` 只在 `desc->has_core_clk` 时才取，
  而 `.has_core_clk = true` **只属于 `qcom,geni-i2c-master-hub`**
  （`i2c-qcom-geni.c` 的 `i2c_master_hub` desc）。本机 EC 那条总线是普通
  `qcom,geni-i2c` → `gi2c->core_clk` 为 NULL → 那句是空操作。
* `geni_icc_get()` 被重写（DDR 路径改成可选、错误处理换 `dev_err_probe`），
  ❌ 纯重构，行为等价。

### ★ 这一轮真正确立的东西

1. **不是 EC 的 PM 回调**（#24）。
2. **不是 EC 驱动本身**——7.1→7.2 只改了一行风格代码。
3. **不是 `i2c-qcom-geni.c` 的 noirq 语义**（#26）。
4. ★ **这些复位【不经过内核】**：查了 `/sys/fs/pstore`、efivars 的 dump 条目、
   以及 Ubuntu 的 `/var/lib/systemd/pstore/` —— 最新记录停在**前一天**，
   今天十几次复位**一条都没留下**。
   ⇒ 不是 panic、不是 oops，是**固件/TZ 级硬复位**，
   典型成因是未上电寄存器访问或 XPU 违例。
   ★ 这条也说明：**想靠内核日志抓现场是徒劳的**，别再往那个方向花时间。
5. ★ **EC 的中断是 `<&pdc 215>`，而且它的处理函数是【线程化】的**
   （`devm_request_threaded_irq(..., NULL, gaokun_ec_irq_handler, IRQF_ONESHOT, ...)`）。
   `resume_device_irqs()` 一放开，它就会立刻发起一次 I²C 传输，
   而那时**只跑完了 noirq 阶段**。这是"绑定 vs 解绑"仅存的具体差异。

### 实验 #27：EC 绑定但**根本不申请中断** —— 照样复位

`gaokun_ec_probe()` 里那句 `devm_request_threaded_irq()` 用 `if (0)` 跳过，
其余（子设备、hwmon、enable-gpio、PM 回调）全部原样：

```
[04:04:19 up=91] EC 绑定=1  子设备=2
[04:04:19 up=91] i2c 运行时状态: a9c000=suspended
[04:04:19 up=91] PRE 第 1 次真实挂起
（无 POST —— 复位）
```

**❌ 中断也被排除。** 顺带这一行 `a9c000=suspended` 还从另一个方向否掉了
geni 假说：控制器本来就停在 runtime-suspended，
`pm_runtime_force_suspend/resume` 对它是空操作。

### ⚠️★ 一个逻辑缺口：#49 那句"解绑 EC 就能挂起"证明力没有看上去那么强

复盘 #49 的对照表会发现：

| 内核 | EC | 结果 |
|---|---|---|
| 纯 7.2 | 绑定 | **干净失败**（-110 @ suspend_noirq，压根没睡着）|
| 纯 7.2 | 解绑 | ✅ 挂起+唤醒 4/4 |
| **我们的 7.2** | 绑定 | ❌ **睡着了，任何唤醒都复位** |
| 我们的 7.2 | **解绑** | **从未测过** |

★ 纯 7.2 上"绑定"那一格**根本没进到 resume**，所以那组对照证明的只是
"解绑消掉了 -110"，**并不能证明"解绑能消掉复位"**。
⇒ 缺的对照是【我们自己的内核 + EC 解绑】。这是实验 #28。

★ **方法论**：对照实验的两格如果**失败模式不同**（一个是干净 abort、
一个是硬复位），那它们比较的就不是同一件事，别把结论跨过去用。

### ★★★ 实验 #28：补上那个缺失的对照 —— **EC 解绑照样复位**

我们自己的干净内核（`#29` 构建，只带常驻的 venus/dts/staging 改动），
开机后先把 EC 整个解绑再挂起：

```
[04:08:14 up=91] 内核: #29  EC 绑定=1
[04:08:17 up=94] 解绑后 EC 绑定=0  子设备=0
[04:08:17 up=94] PRE  第1轮：EC 已解绑
（无 POST —— 复位）
```

⇒ **EC 被彻底洗清。** 在我们自己的内核上，
**解绑 EC 完全不能阻止复位**，#49 那条"元凶是 EC"的整条推理就此作废。

### 修正后的事实表（只列自己实测过的）

| 内核 | EC | 结果 |
|---|---|---|
| 上游 7.1.0-rc3 | 绑定 | ✅ 挂起+唤醒 4/4 |
| 纯 7.2（无我们的补丁） | 绑定 | ⚠️ 干净失败 −110 @ suspend_noirq |
| 纯 7.2 | 解绑 | ✅ 挂起+唤醒 4/4 |
| **我们的 7.2** | 绑定 | ❌ 复位 |
| **我们的 7.2** | **解绑** | ❌ **复位**（#28，新） |
| 我们的 7.2 | 绑定但 PM 回调 return 0 | ❌ 复位（#24）|
| 我们的 7.2 | 绑定但不申请中断 | ❌ 复位（#27）|

⇒ 差异不在 EC，而在 **"我们的 7.2" 与 "7.1 / 纯 7.2" 之间**
（补丁栈、内核配置、或 DTB）。

### ★ 一个此前完全没被纳入考虑的变量：**Venus**

对照 `ubuntu-71.conf` 与 `plain72.conf`：

* **cmdline 逐字相同** ✅（这个变量干净）
* **DTB 是两份不同的文件**，但**两份都带 venus 节点**（`aa00000` /
  `video-codec` / `qcvss8280` 都在）
* ★ **但 7.1 那个内核里 Venus 绑不上** —— `sc8280xp` 的 videocc 与 venus
  支持是 **M14 我们自己打的补丁**，上游 7.1 没有。
  ⇒ "7.1 能挂起"时 **Venus 从来没上过电**；
  我们的 7.2 上它真的 probe 了，拿走了 rpmhpd 电源域、videocc 时钟、
  IOMMU 和 4 条 interconnect。

**Venus 是 M14 才刚 `status = "okay"` 的全新硬件块，从来没跟挂起一起测过。**
这就是实验 #29。

### 实验 #29：Venus 也不成立 —— 而且它在救援 Ubuntu 上**根本没绑定过**

```
[04:12:27 up=90] venus 驱动: []  EC 子设备: [huawei_gaokun_ec.psy.0 huawei_gaokun_ec.ucsi.0 ]
/usr/local/bin/s71test.sh: 第 22 行： echo: 写入错误: 没有那个设备
```

★ 救援 Ubuntu 的 `/lib/firmware` 里没有 `qcvss8280.mbn`，所以 venus 一直没 probe。
⇒ Venus 排除，**而且这条同时说明：本系列在 Ubuntu 上做的所有实验，
Venus 从头到尾都是没上电的**，它不可能解释任何一次复位。

### ★★★ 实验 #31：连**零补丁的 v7.2-rc2** 也复位 —— #49 那张表又错一行

`git checkout v7.2-rc2`（干净标签、无任何补丁、无工作区改动），
配置用**我们自己的 .config**（只把 LOCALVERSION 改成 `-PLAINV72` 好一眼认出），
DTB / cmdline / rootfs 与前面几轮完全相同：

```
[04:18:13 up=91] ★ 内核: 7.2.0-rc2-PLAINV72  #30
[04:18:13 up=91] EC 绑定=1
[04:18:13 up=91] PRE  第1轮：EC 绑定
（无 POST —— 复位）
```

⚠️ 这与 #49 记的"纯 7.2 + EC 绑定 = 干净失败 −110、压根没睡着"**不符**。
当初那次"纯 7.2"多半用的不是我们的 .config（或根本不是干净标签），
**那一行数据不可信**。

⇒ **我们的 20 个 buildbot 补丁也基本被洗清了。**

### ★ 于是唯一一个从来没被控制过的变量浮出水面：**DTB**

| 轮次 | 内核 | DTB | 结果 |
|---|---|---|---|
| #48 | 上游 7.1.0-rc3 | **`/7.1.0-rc3/dtb`（166 783 B）** | ✅ 4/4 |
| #24–#32 | 各种 7.2 | **`/plain72/dtb`（173 026 B）** | ❌ 全部复位 |

**两份 DTB 从来就不是同一个文件，而我一直把变量当成"内核版本"。**

而补丁栈里正好有一个**只改 DTB、而且直接动唤醒路径**的东西：

```
9545e6638411 arm64: dts: qcom: sc8280xp: add several missing pdc map entries
  -<214 643 1>,   +<214 643 2>,     ← 多出 PDC 215
  -<255 454 1>,   +<255 454 3>,     ← 多出 PDC 256 / 257
  提交说明原文："These entries are reversed from .data section of qcgpio.sys"
```

★ **PDC 215 正是 EC 的中断**（`interrupts-extended = <&pdc 215 IRQ_TYPE_LEVEL_LOW>`），
而这三条 PDC→GIC 映射是**从 Windows 驱动逆出来的猜测**。
一个错的唤醒线映射，症状恰好就是"睡得下去、**任何**唤醒都整板复位"
—— 因为所有唤醒都要经过 PDC。

这就是实验 #32：**内核一个字节不动，只把这两行还原成上游**。

### 实验 #32：pdc-ranges 假说也死了（而且这一轮把机器弄到要人动手）

只换 DTB（内核字节不动），把 `<214 643 2>` / `<255 454 3>` 还原成上游 ——
**机器停在早期启动，没走到 multi-user**。
判据不是"看起来卡住"，而是 **`s71test.service` 的 symlink 还在**
（脚本第一件事就是删掉它），证明它从没被执行。

⚠️ 这一轮**需要用户长按电源键**才恢复。我给测试脚本留了自动回落，
却没给**"改了 DTB 导致开不了机"**留任何回落。
★ **纪律**：改 DTB / 改引导链这类**可能连 userspace 都到不了**的实验，
必须默认走 oneshot（本轮确实是 oneshot，所以一次电源键就回 Android 了）
—— 但要**预先告诉用户可能需要按一次**，不能事后才发现。

随后把两份 DTB 反解出来逐字对比，结论是：

```
dts-71:  0xd6 0x283 0x02   0xff 0x1c6 0x03
dts-72:  0xd6 0x283 0x02   0xff 0x1c6 0x03
```

**两份 DTB 的 `pdc-ranges` 完全相同** —— 7.1 那次也带着 PDC 215/256/257。
⇒ 假说否定；而它们是必需的，所以去掉才会开不了机。

### 实验 #33 / #34：按设备批量解绑

★ 顺带得到一个**好得多的工作方式**：让测试脚本**只解绑、不挂起**，
机器就停在救援 Ubuntu 且 **ssh 可达**，之后可以在**一次开机里连续做多个实验**，
不必每次重启。（本轮就是靠这个在脚本挂起前 30 秒把它 kill 掉的。）

⚠️ **差点又要用户动手**：#33 的批量解绑循环**没排除 RTC** ——
`pm8xxx_rtc` 一旦被解绑，`/sys/class/rtc/rtc0/wakealarm` 就没了，
脚本会**在没有任何唤醒源的情况下挂起**，那就只能长按电源键。
★ 已把安全检查写进 `sx.sh`：**没有 rtc0 或闹钟没设上就拒绝挂起**。

★ 救援 Ubuntu 里实际绑着的叶子设备**比想象中少得多**
（显示栈和 venus 都没绑，`modprobe.blacklist=simpledrm` + 缺固件）：

```
音频: sound / audio-codec / rx,tx,va,wsa macro
USB:  xhci-hcd ×2 / dwc3 ×3
DSP:  1b300000 / 2400000 / 3000000 remoteproc
EC:   15-0038 + ucsi
WiFi: ath11k_pci（PCIe）
```

* **#33（ssh 交互式）**：上面除 WiFi 外**全部解绑** → **仍然复位**。
* **#34**：再加上 **ath11k + PCIe 控制器** 一起解绑（只能用重启式脚本，
  因为 WiFi 就是 ssh 通道本身）。

## #51 ★★★ s2idle 再次改判：**复位发生在挂起【进入】时，不是唤醒时**（2026-08-21）

接 #50。这一条推翻了 #47 的中心结论，也解释了为什么 #24–#36 那一长串
"解绑某某再试"全部无效。

### 决定性实验 #37：把 RTC 闹钟从 40 秒改成 **180 秒**

```
闹钟=1787288159 现在=1787287979 差=180秒
挂起下达 12:52:59
adb 在 12:53:27 回来（27 秒后）
```

⇒ **机器在几秒内就复位了，离 180 秒的唤醒还差得远。**
**复位与唤醒无关**，它就发生在挂起进入的那一刻。

★ 判据设计：用"adb 什么时候回来"来给复位时刻定位 ——
复位→Android 起 adbd 约 30 秒是稳定的，所以 adb 在 t≈27 s 回来
只能对应"t≈0 就复位了"；若真睡到 180 秒才炸，adb 会在 t≈210 s 才回来。

### ★ 于是 `pm_test` 平反了 —— 而且它是最好的夹逼工具

#49 里我判 `pm_test=devices` 是"本平台无效判据"，理由是"它自己就会复位"。
**那个理由本身就是结论**：它会复位，正因为 bug 就在它覆盖的那个窗口里。

同一次开机连续跑（`pm_test` 自带 5 秒后自动返回，不需要唤醒源，很安全）：

```
[04:56:36 up=39] PRE  pm_test=freezer
[04:56:41 up=44] ★ POST pm_test=freezer  rc=0      ← 活着
[04:56:44 up=48] PRE  pm_test=devices
（无 POST —— 复位）                                  ← 死在这里
```

⇒ **故障区间被夹到 `dpm_suspend_start()` + `dpm_suspend_noirq()`（及其紧接的
resume）之内。** freezer 层（只冻结进程、不碰设备）完好，
说明与进程冻结、与 syscore、与 platform ops、与 CPU 空闲态全都无关。

### 已经排除的（本轮全部是实测，不是推理）

| 假说 | 实验 | 结果 |
|---|---|---|
| EC 的 PM 回调 | #24 | ❌ |
| EC 的中断 | #27 | ❌ |
| **EC 本身**（解绑） | #28 | ❌ |
| Venus | #29 | ❌（在救援 Ubuntu 上它**根本没绑定过**）|
| geni I²C 的 noirq 重构 | #26 | ❌ |
| PDC 唤醒映射（pdc-ranges） | #32 + 反解两份 DTB | ❌（两份 DTB 完全相同）|
| 我们的 20 个 buildbot 补丁 | #31（零补丁 v7.2-rc2 也复位）| ❌ |
| **cpuidle 深空闲态**（`cpu-sleep-0-0` 全禁） | #36 | ❌ |
| 硬件看门狗 `qcom_wdt` | 查 sysfs / systemd | ❌ 根本没在跑 |
| 音频 / USB / 三个 remoteproc / ucsi（全解绑） | #33 | ❌ |
| **唤醒源**（180 秒闹钟） | #37 | ❌ **复位不是唤醒引起的** |

### ★ 剩下的嫌疑：**解绑不掉的"供应方"设备**

叶子设备几乎全解绑了仍然复位，所以凶手在这批里：
`nvme`（根文件系统）、`arm-smmu` ×2、`pinctrl-msm`/tlmm、`qcom-pdc`、
`qnoc-sc8280xp` ×13（interconnect）、`rpmhpd`、`spmi_pmic_arb` ×2、
`qcom-tsens` ×5、各时钟控制器。

★★ **NVMe 排在最前面**，理由不是猜的：
**实验 #34 里，一去碰 PCIe（解绑 ath11k 或 PCIe 控制器）机器就当场复位** ——
不是挂起时，是解绑那一瞬间。而 NVMe 也在 PCIe 上，
`nvme_suspend()` 做的正是同一类事（`pci_save_state` / D3）。
根文件系统在它上面，所以**永远解绑不掉**，这也解释了为什么它一直没被测到。

### ★★★ 实验 #39：`pcie_aspm=off` + NVMe APST 关闭 —— **第一次没有复位**

只改 BLS 条目的 cmdline（内核、DTB、rootfs 全不动）：

```
-  pcie_aspm.policy=powersupersave
+  pcie_aspm=off nvme_core.default_ps_max_latency_us=0
```

脚本先跑 `pm_test=devices`（本来 5 秒内必复位），再跑一次真实挂起。

**结果：机器没有复位。** 十分钟内 adb 没有回来、救援机也从网上消失
（全网段扫描 + 逐个 ssh 取 hostname，确认不在线）。
⇒ 它**真的睡进去了**，但**没有被 RTC 闹钟叫醒**。

⚠️ 需要用户按一次电源键才能取回日志（本轮第二次需要人工，已记为纪律问题）。

★ 这一条同时把两件事分开了：
1. **"设备挂起阶段整板复位"** —— 与 PCIe/NVMe 的低功耗转换有关，
   `pcie_aspm=off`（或 NVMe APST 关闭，两者本轮是一起改的，**还没分离**）
   就能绕过；
2. **"醒不回来"** —— 这才是 M4 当初描述的那个症状，它是**另一个独立的问题**，
   被前一个问题掩盖了整整两轮。

⬜ **下一步（待日志确认后）**：
* 把 `pcie_aspm=off` 与 `nvme_core.default_ps_max_latency_us=0` **分离测试**，
  确定是哪一个起作用。
* 若是 ASPM：本机 cmdline 里那句 `pcie_aspm.policy=powersupersave`
  **是我们自己加的**，不是上游默认 —— 那就是一个我们自己埋的雷。
  ⚠️ 但要注意：#48 那次"7.1 能挂起"用的 cmdline 与本轮**逐字相同**，
  所以 ASPM 单独解释不了 7.1/7.2 的差异，多半是"ASPM + 7.2 的某处变化"合并成因。
* 然后才轮到"醒不回来"。

### 变量分离：#40 / #41

`pcie_aspm=off` 和 `nvme_core.default_ps_max_latency_us=0` 是一起改的，
必须分开。两轮都**只跑 `pm_test=devices`**（5 秒自动返回，不需要唤醒源）：

| 轮次 | cmdline | 结果 |
|---|---|---|
| #40 | **只去掉** `pcie_aspm.policy=powersupersave`（`nvme ps_max_latency` 仍是 100000）| ❌ **仍然复位** |
| #41 | 原样 cmdline **只加** `nvme_core.default_ps_max_latency_us=0` | ⚠️ **既没复位也没跑完 —— 机器挂住了**（日志未取回，adbd 不稳）|

⇒ **ASPM 策略不是原因**（#40 排除）。
⇒ 起作用的是 `pcie_aspm=off` 或 NVMe APST 之一，**尚未定论**；
   #41 把失败模式从"整板复位"变成了"挂住"，这本身也是个信号。

### ⚠️★ 纪律：本轮让用户按了**三次**电源键，这是我的问题

1. **#32**：改 DTB 导致开不了机 —— 我给测试脚本留了自动回落，
   却没给"连 userspace 都到不了"的情况留任何回落。
2. **#39**：真实挂起成功、但**醒不回来** —— 我明知"醒不回来"是待查问题，
   还是在同一个脚本里排了一次真实挂起。
3. **#41**：设备挂起阶段挂住 —— 内核层的 hang，脚本自己救不了自己。

★ **改法（已定，尚未全部落地）**：
* **只要"醒不回来"还没解决，就不要跑真实挂起** —— `pm_test=devices`
  自带 5 秒返回，能覆盖目前所有已知的失败窗口，而且不需要唤醒源。
* **给实验用的 BLS 条目加 `panic=10`**：本机内核已开 `CONFIG_DPM_WATCHDOG`，
  设备回调卡住会 panic，配上 `panic=10` 就能自动重启回默认项（Android）。
  纯硬件挂死仍救不了，但能覆盖大部分内核层 hang。
* **改 DTB / 改引导链**这类可能连 userspace 都到不了的实验，
  动手前先明说"可能要按一次电源键"。

### ★ 一个顺带很有用的运维发现

让测试脚本**只做准备、不挂起**（"stay 模式"），机器就停在救援 Ubuntu 且
**ssh 可达**，之后可以在**一次开机里连续做多个实验**。
本轮靠它在 30 秒的窗口里 kill 掉了一个会把机器睡死的脚本。
⚠️ 救援机的 IP 会漂（本轮 `.230`，Android 这边是 `.46`），
`gaokun3-rescue.local` 在 git-bash 的 ssh 里解析不了，
可靠办法是 **ping 全网段填 ARP → 逐个 ssh 取 hostname**。

### ⚠️★★★ 更正：#39 那次成功**不可复现** —— 这个故障是**间歇性**的

补齐 2×2 之后，事情反转了：

| ASPM | NVMe APST | 结果 |
|---|---|---|
| `powersupersave`（我们原本的） | 开（100000） | ❌ 复位（基线） |
| 默认（只去掉策略） | 开 | ❌ 复位（#40） |
| **`off`** | 开 | ❌ 复位（#42） |
| `powersupersave` | **关（0）** | ❌ **挂死**（#41，失败模式变了） |
| **`off`** | **关** | ✅ rc=0（#39）→ ❌ **复位（#43，同一条 cmdline 重跑）** |

**#43 用与 #39 一字不差的 cmdline 重跑，`pm_test=devices` 复位了。**
（日志逐项确认：`pcie_aspm=off`、`ps_max_latency: 0`。）

⇒ **#39 的那次 rc=0 是运气，不是修复。没有任何 cmdline 改动被证明有效。**

### ★★★ 由此得到本轮**最重要的方法论教训**

本轮约 15 次试验里 14 次复位/挂死、1 次通过 ⇒ **单次存活率大约 7%**。
也就是说：**"改了 X，试一次，炸了" 对任何配置都是大概率事件，
它几乎不构成证据。**

⚠️ 这**回过头削弱了本轮几乎所有单次试验的否定结论**。
其中证据力仍然成立的是那些**多次重复**或**改变了失败模式**的：
* #37（180 秒闹钟 → 几秒内复位）—— 复位与唤醒无关，这条很硬。
* `pm_test=freezer` rc=0 vs `pm_test=devices` 复位 —— 夹逼区间成立
  （freezer 那一层多轮都通过）。
* #41 把失败模式从"复位"变成"挂死" —— 变了性质，不是随机波动。
* #31（零补丁 v7.2-rc2 也复位）—— 与十几次基线一致，结论方向可信。

★ **新协议（已固化进脚本 `s71test-loop.sh`）**：
**每次开机把 `pm_test=devices` 循环跑到失败或跑满 10 次**，每次成功立刻写盘。
这样每一轮得到的是"跑到第几次才死"这个**可比较的数**，而不是一个二值结果。
跑满 10 次在基线 7% 存活率下的概率是 10⁻¹¹，才算真的证据。
**第一步必须是测基线**（原样 cmdline），否则没有比较对象。

### ★★ 修好计数工具之后的正式测量（#44–#52）

⚠️ 先记一个把我骗了一轮的**工具缺陷**：`pm_test` 周期结束后会留下 pending
唤醒事件，紧接着再 `echo mem` 会**立刻 `-EBUSY` 返回**。我第一版循环脚本
把那个非零返回也当成"通过"，于是 #45 显示"10/10 全过"，实际上**只有第 1 次
真跑了**（判据：真实周期耗时 5–8 秒，`-EBUSY` 那些是 0–1 秒）。
★ 修法：只把 `rc=0` 计数，非零就等 10 秒重试，并把耗时和 `dmesg` 尾行一起记下。

**正式结果**（每轮 = 一次开机，把 `pm_test=devices` 跑到失败或跑满 10 次）：

| 配置 | 有通过的开机 / 总开机 |
|---|---|
| **基线**：`pcie_aspm.policy=powersupersave` + APST 开（100000） | **0 / 约 16** |
| 只 `pcie_aspm=off`（APST 开） | 0 / 1（#47） |
| 只 APST 关（策略仍 `powersupersave`） | 0 / 1（#48）+ 一次挂死（#41） |
| 去掉策略（ASPM 默认）+ APST 关 | 0 / 1（#49） |
| **`pcie_aspm=off` + APST 关（0）** | **3 / 5** —— 其中 #46 连过 **10/10** |

⇒ **结论（带着不确定性一起记）**
1. ★ **只有"完全关闭 ASPM + 关闭 NVMe APST"这个组合出现过通过**，
   而基线在约 16 次开机里**一次都没通过**。两者差异是真的
   （Fisher 精确检验 p≈0.002）。
2. ★ **两个都必需**：任一单独设置都是第 1 次就死；
   而且**光去掉我们自己加的 `pcie_aspm.policy=powersupersave` 不够**，
   要的是 `pcie_aspm=off`（完全关闭）。
3. ⚠️ **但它不是确定性修复**：同一条 cmdline 有 2 次开机仍然第 1 次就死。
   ★ 而**同一次开机内行为高度一致**（要么连过 10 次，要么第 1 次就死）
   ⇒ **决定性因素是开机时确定下来的某个状态**（probe 顺序 / 绑定结果 /
   WiFi 关联状态之类），不是每次挂起的随机性。**这一层还没查。**

⬜ **下一步**
* 找那个"开机时确定下来的因素"：在同一配置下多开机几次，
  **每次都记录完整的设备绑定清单 + `/sys/class/wakeup/` + dmesg**，
  然后比对"能过的开机"与"不能过的开机"。这是本条唯一还有把握推进的方向。
* 只有把上面这层弄清楚，才谈得上"是不是该把 `pcie_aspm=off` 写进发版 cmdline"
  —— 现在这么做只会让待机变成"有时能睡、有时炸"，比确定不能睡更糟。
* "醒不回来"仍未动（#39 那次唯一睡进去的实机观测）。

★★ **本轮最值钱的东西不是结论，是判据**：
`pm_test=devices` 循环 + 只数 `rc=0`，让"改了 X 之后好没好"第一次变成可测量的。
在这之前我做的十几个"改一次、试一次"的实验，**在 ~93% 的单次失败率下几乎没有
证据力** —— 这也是为什么 #39/#43 会一正一反、把我带偏两轮。

---

## #52 ★★★★★ s2idle 真凶：**`a600000.usb`（我们自己改成 otg 的那个空闲 dwc3）**（2026-08-21）

接 #51。这一条把整晚的碎片全部串起来，并**推翻 #48**（"7.1→7.2 回归"）。

### 定位过程（每一步都是实测）

**1. 先修判据。** 关掉 `gaokun-ec-adapter` 的唤醒能力后（等价于 buildbot
`others/0017` 的效果），`wakeup_count` 从"每 2 秒 +1"变成**全程冻住**：

```
gaokun-ec-adapter    event+10 active+10   ← 20 秒内 +10，唯一持续产生事件的源
关掉后：20 秒内没有任何源增长，wakeup_count 冻在 62
```

⚠️ 这个事件洪水**只在 PLAINV72（零补丁 v7.2-rc2）上有** —— 我从 #31 起一直用它测，
而 buildbot 的 0017 正是治这个的。**测试内核与发版内核在"EC 唤醒行为"上不同，
我却拿它的统计去推断发版内核。**

**2. ★ 那个"`-EBUSY`"根本不是 EBUSY。** 把 write 的错误串抓出来：

```
错误串 = 写入错误: 无效的参数            ← EINVAL，不是 EBUSY
suspend_stats: step=[suspend] dev=[xhci-hcd.2.auto] errno=[-22]
第一次的 dmesg: usb 3-3: USB disconnect, device number 2
```

`xhci_suspend()` 里有一段：HCD 状态不是 `HC_STATE_SUSPENDED` 就 `return -EINVAL`。
**第一次挂起周期里掉了一个 USB 设备，之后 xhci 永久返回 -EINVAL。**
★ 教训：**`echo ... > sysfs` 失败时一定要把 stderr 抓下来**，
不要凭 `rc!=0` 猜 errno —— 我猜了 EBUSY，猜错了，还为此写了个没用的
`wakeup_count` 回写协议。

**3. ★★ 只解绑 USB（2 个 xhci + 3 个 dwc3）→ 10/10 全过。**
与上一轮的差别**只有这一项**（同 cmdline、同样关了适配器唤醒）：
`1/10` → `10/10`。

**4. ★★★★ 逐个解绑 dwc3，日志精确停在一行上：**

```
xhci-hcd.0.auto 的父设备 = a800000.usb     已解绑，活着
xhci-hcd.2.auto 的父设备 = a400000.usb     已解绑，活着
dwc3/a400000.usb                           已解绑，活着
★ 即将解绑 dwc3/a600000.usb                ← 日志到此为止
```

换成把 a600000 放**第一个**解，结果一模一样 ⇒ **与顺序无关。**

### 凶手的身份 —— 而且是我们自己造的

```
a600000.usb  compatible=snps,dwc3  dr_mode=otg  maximum-speed=high-speed
             usb-role-switch  子设备=0 个 xhci  下挂=usb_role
a400000.usb  dr_mode=host   子设备=1 个 xhci
a800000.usb  dr_mode=host   子设备=1 个 xhci
dmesg: Fixed dependency cycle(s) with
       /soc@0/geniqup@ac0000/i2c@a9c000/embedded-controller@38/connector@0
       ↔ /soc@0/usb@a6f8800/usb@a600000
```

★ **`dr_mode="otg"` + `usb-role-switch` + `high-speed` 是我们在 Stage 2 为了
USB adb 自己改上去的**（上游是 `host`，见工作区 DTS 的那段注释）。
而本机 **UCSI 是坏的**（`PPM init failed -ETIMEDOUT`，已知坑），
**没有 role 源** ⇒ 这个控制器停在半初始化的 OTG 状态、**一个 xhci 都没起**、
还和 EC 的 USB-C 连接器构成 devlink 依赖环。
**给这个状态断电（挂起或解绑都会）就整板复位。**

### ★★ 由此推翻 / 解释的旧结论

* ❌ **#48"上游 7.1.0-rc3 正常 ⇒ 7.1→7.2 回归"作废。**
  那次用的是**另一份 DTB**（166 783 B vs 173 026 B），
  两份 DTB 的 diff 有 4131 行 —— a600000 的 `dr_mode` 极可能还是 `host`。
  **从来不是内核回归，是 DTB 差异。**
* ✅ 解释了为什么"解绑 EC / venus / 音频 / remoteproc / cpuidle"全都无效 ——
  它们都不在这条路上。
* ✅ 解释了 #34 的批量解绑为什么"死在中途" —— 那个序列里就有 dwc3。
* ✅ 解释了 `pcie_aspm=off + APST 关` 为什么**看起来**有效又不稳定：
  它根本不是修复，只是把这个概率性的断电时机稍微挪了一下。
  ⇒ **#51 那张 cmdline 对照表的因果解释作废**（数据保留）。
* ✅ 解释了 `last_failed_dev=[a600000.usb] step=[resume]` ——
  同一个设备在 resume 上也失败过，两条独立证据指向同一处。

### ⬜ 修复方向（未做，有取舍）

1. **把 a600000 改回 `dr_mode = "host"`**（= 上游原样）。
   ⚠️ **代价：USB device-mode adb 会没有**（UDC 在这个控制器上，Stage 1 的
   "UDC 出现"就是它）。但 USB adb 本来就有 #27 那个"掉了不回来"的缺陷，
   而 TCP adb 需要 WiFi 在。**这是个真实取舍，要用户决定。**
2. 保留 otg，但让它不要停在半初始化状态 —— 先试运行时写
   `/sys/class/usb_role/*/role`（强制一个角色）再挂起，看断电是否变安全。
   这条**零构建**，应该先试。
3. 上游方向：UCSI 修好了这个问题多半自然消失（`refs/linux-gaokun/README.MD:86-87`
   记着 UCSI 的缺陷）。

### ★★★★★★ 双臂确认：**只改 `usb_role` 一个值，结果就翻转**

同一次开机、同一内核、同一 cmdline、**什么都不解绑**，只写 role switch：

```
==== 臂 host ====
  role 回读=[host]    子 xhci=1  rt=[active]
  ★ 通过 1/5 ... 5/5              ← 5/5 全过

==== 臂 device ====
  role 回读=[device]  子 xhci=0  rt=[active]
  PRE 臂=device 第1/5
（日志到此为止 —— 整板复位）
```

⇒ **根因确认：`a600000.usb` 的 role switch 停在 `device`、没有任何 gadget
配置、连 xhci 都没实例化（子 xhci=0）—— 给这个"半初始化"状态断电就复位。
给它一个真实角色（`host`，子 xhci=1）之后，它就是个普通控制器，挂起完全正常。**

★ 注意默认值：**开机后 role switch 自己停在 `device`**
（`a600000.usb-role-switch 当前=[device]`），所以这个坑是默认命中的。

### ⚠️ 修复的取舍 —— 需要用户决定，且**先要做一个 Android 侧实验**

`role=host`（或 DTS 改回 `dr_mode="host"`）能修好挂起，但**代价是
USB device-mode adb 没了**（UDC 就在这个控制器上，Stage 1 的"UDC 出现"就是它）。

★★ **但在 Android 上情况可能不同，必须先测**：救援 Ubuntu 里**没有任何东西配置
USB gadget**，所以 `device` 角色是空的、半初始化的；而 **Android 的 adbd 会通过
configfs 真的把 gadget 配起来**，那时 `device` 角色未必是"半初始化"状态。
⇒ **如果 Android 上挂起本来就没问题，那这个坑只影响救援 Ubuntu，不需要改 DTS。**

**Android 侧怎么安全测**（内核带 `CONFIG_PM_DEBUG`，`pm_test` 可用）：

```sh
adb shell 'echo gaokun3_nosuspend > /sys/power/wake_unlock'   # 先放开 wakelock
adb shell 'echo devices > /sys/power/pm_test'                 # ★ 只测设备阶段，5 秒自动返回
adb shell 'echo mem > /sys/power/state; echo rc=$?'
adb shell 'cat /sys/class/usb_role/*/role; ls /sys/bus/platform/devices/a600000.usb/ | grep -c ^xhci'
```

⚠️ **必须用 `pm_test=devices`**，不要在 Android 上跑真实挂起 ——
"醒不回来"仍未解决，真实挂起会把机器睡死、只能长按电源键。

### ★ Android 侧现状（决定"要不要改 DTS"的关键）

```
/sys/class/udc/a600000.usb -> .../a6f8800.usb/a600000.usb/udc/a600000.usb
sys.usb.controller = a600000.usb    sys.usb.state = adb    sys.usb.ffs.ready = 1
configfs: /config/usb_gadget/g1  已配置
usb_role: a600000.usb-role-switch = [device]    子 xhci = 0
/sys/power/ 里【没有 pm_test】
```

三条结论：

1. ★ **USB adb 的 UDC 就在 a600000 上** ⇒ 改成 `host` **确实会失去 USB
   device-mode adb**。取舍是真实的，不是理论上的。
2. ★★ **但 Android 上这个 `device` 角色是有真实 gadget 的**（configfs g1 已配、
   `ffs.ready=1`、`sys.usb.state=adb`），而救援 Ubuntu 上**一个 gadget 都没有**。
   ⇒ "半初始化状态"可能**只在救援 Ubuntu 成立**。
   **如果 Android 本来就不受影响，那修法是零代价的**：只在救援 Ubuntu 的
   启动脚本里写一行 `role=host`（那边根本不用 USB gadget）。
3. ⚠️ **Android 内核 #19 没有 `CONFIG_PM_DEBUG`**（`/sys/power/pm_test` 不存在），
   所以**在 Android 上安全验证这件事需要重编内核**。
   `scripts/kernel-config-android.sh` 在 M14 已经把 PM debug 那一块加进断言了，
   只是还没构建过。

⬜ **待用户决定的岔路**（两条路工作量和代价差很多）：
* **A**：重编 Android 内核（带 `PM_DEBUG`）→ 在 Android 上用 `pm_test=devices`
  安全测一次 → 才能知道 Android 是否受影响、以及要不要动 DTS。
  代价：构建机开机时间 + 换掉设备上现役内核。
* **B**：先只修救援 Ubuntu（开机写 `role=host`，零代价、零风险），
  Android 侧留着不动。
⚠️ **不要**在 Android 上跑真实挂起来省这一步 —— "醒不回来"没解决，会睡死。

### ⚠️★ 靶场纪律补一条（第四次让用户按电源键，成因与前三次不同）

Android 侧测试：我先设 `pm_test=devices` **再**放开 `gaokun3_nosuspend` wakelock，
以为这样连 Android 自己的 autosleep 都变成安全的测试挂起 —— 这一半是对的。
**漏掉的是收尾路径**：脚本结束时"先拿回 wakelock、再 `pm_test=none`"，
那一刻起真实挂起就重新可能发生，而**唯一的拦阻只剩那个 wakelock**。
它没拿稳（或 Android 自己的 wakelock 也为空）→ 几秒内真睡下去 → 醒不回来。

★★ **正确做法（已定为纪律）**：**在放开 wakelock 之前，先
`echo +120 > /sys/class/rtc/rtc0/wakealarm`。**
真睡下去也会被闹钟叫；即使醒不回来，结果也只是**整板复位**，
而**复位会自动回到默认启动项（Android），是可恢复的；无限睡下去不可恢复。**
⇒ **设计安全网时要区分"可恢复的失败"和"不可恢复的失败"，
把不可恢复的那种堵死，而不是笼统地"减少失败"。**

★ 顺带记录本轮的部署手法（是好的，值得复用）：新内核放进**独立的 ESP 目录 +
独立 BLS 条目**（`android/slot_b_pmdbg/Image` + `*-android-b-pmdbg.conf`，
dtb/initrd 复用 slot_b 的以省 ESP 空间），靠 oneshot 进去。
默认启动项仍是 `*-android-b.conf`（glob 不匹配 `-pmdbg`），
所以**新内核起不来时复位就回到已知可用的那个**。这一半奏效了：
新内核（`#31`，带 `CONFIG_PM_DEBUG`）实测正常启动、`pm_test` 节点出现、
`boot_completed=1`、USB adb 正常。

## #53 ★★ 两道坎各有归属，可测的层级全部通过（2026-08-21）

`pm_test` 不止 `devices` 一层 —— 还有 `platform` / `processors` / `core`，
**四层全都是 5 秒自动返回、不需要唤醒源**，所以整条挂起+恢复路径
（含 syscore 与 resume）几乎都能安全验证。之前只测到 `devices`
是因为它在那一层就复位、根本走不下去。

### 逐层结果（每层 5 次）

| 层 | PLAINV72（零补丁）+ role=host | **我们的内核 #31（带补丁）+ role=host** |
|---|---|---|
| `devices` | **5/5** ✅ | **5/5** ✅ |
| `platform` | 0/5 ❌ `suspend_noirq` / dev=**`15-0038`** / **−110** | **5/5** ✅ |
| `processors` | 0/5（−11，0 秒，step/dev 空）| 0/5（同）|
| `core` | 0/5（同）| 0/5（同）|

**⇒ 两道坎，各有归属：**
1. **`a600000.usb` 的 role 停在 `device`** → 设备挂起阶段整板复位。
   **`role=host` 修好**（本轮是第三次独立复现 5/5）。
2. **EC（`15-0028`→`15-0038`）在 `suspend_noirq` 超时 −110** →
   **我们发版内核本来就修好了**，靠 buildbot 的
   `platform/arm64: huawei-gaokun-ec: fix suspend/resume ordering`
   （把 EC 的 PM 回调从 NOIRQ 阶段挪出来，好让 I²C 握手来得及）。
   ★ #49 记的那个签名一直是对的，我只是没意识到测试内核已经漂到零补丁上去了。

★ **`processors` / `core` 两层对 s2idle 不适用**：0 秒返回、`-EAGAIN`、
`step`/`dev` 全空、两个内核上 10/10 完全一致 —— 这是 `pm_test` 深层级
（为 platform suspend 设计）的限制，不是本机缺陷。
⇒ **本平台能安全验证的最深层级是 `platform`，而它现在通过。**

### ⬜ 剩下的唯一问题：真实 s2idle 的进入与唤醒（"醒不回来"）

到此为止，**所有不需要唤醒源的层级都通过了**。没验的只有
`suspend_ops->enter()` 那一步本身 —— 也就是真正睡下去再被叫醒。

⚠️ 这一步**必须用真实挂起**，因此有"睡死、要按电源键"的风险。
唯一的历史观测是 #39（那时 role 还是坏的）：真睡进去了、没醒。
现在两道已知的坎都修好了，值得再试一次，但**要事先跟用户说明可能要按一次电源键**。

## #54 ★★★★★★ **s2idle 通了** —— 真实挂起 5/5（2026-08-21）

配置：**我们带补丁的内核 `#31`（= 发版内核 + `CONFIG_PM_DEBUG`）+ `role=host`**，
救援 Ubuntu，`pm_test=none`（真实挂起，不是测试模式），RTC 闹钟 +40 秒。

```
★ role=[host]  子xhci=1
第1 次 rc=0 success=1 fail=0
第2 次 rc=0 success=2 fail=0
第3 次 rc=0 success=3 fail=0
第4 次 rc=0 success=4 fail=0
第5 次 rc=0 success=5 fail=0
最终 success=5 fail=0

每次的 dmesg：
  PM: suspend entry (s2idle)
  Restarting tasks: Done
  PM: suspend exit
```

★ **判据不只是 `success` 计数**：每一次**墙钟走约 43 秒，而内核 printk 时间
只走约 2.7 秒** —— 本地时钟停了、墙钟靠 RTC 补回来，这正是"真的睡下去"的签名。
如果只是空转 40 秒，两个时间会一起走。

### 两道坎，各有归属

| 坎 | 症状 | 修法 |
|---|---|---|
| **`a600000.usb` 的 role 停在 `device`**（无 gadget、无 xhci、半初始化） | 设备挂起阶段**整板复位**，无任何日志 | **`role=host`**（子 xhci 从 0 变 1）—— 双臂对照 + 三次独立复现 |
| **EC（`15-0038`）`suspend_noirq` 超时 −110** | 干净失败，`pm_test=platform` 0/5 | **我们发版内核本来就修好了**：buildbot `platform/arm64: huawei-gaokun-ec: fix suspend/resume ordering`（把 EC 的 PM 回调从 NOIRQ 挪出来）|

⇒ 这也解释了为什么整晚测不出来：我从 #31 起用的测试内核是**零补丁的 PLAINV72**，
它缺第二道坎的修复；而 role 那道坎两个内核都有。**两道坎叠在一起，
任何单独一项的实验都会失败**，于是每个假说看起来都被"否定"了。

### ⬜ 还要做的：把修复变成永久的

* **救援 Ubuntu**：零代价 —— 开机写一次 `role=host` 即可（那边不用 USB gadget）。
* **Android**：⚠️ **有取舍**。USB adb 的 UDC 就在 a600000 上
  （`sys.usb.controller=a600000.usb`），改成 host 就没有 USB device-mode adb。
  ★ 但 Android 上那个 `device` 角色**有真实 gadget**（configfs g1 已配、
  `ffs.ready=1`），而救援 Ubuntu 上一个都没有 —— 差别正是"半初始化"的关键。
  **所以 Android 未必受影响，需要一次实测**（内核 `#31` 已在 ESP 上，
  `pm_test` 可用，测法见 #52 末尾；⚠️ 放开 wakelock 前先设 RTC 闹钟）。
* 若 Android 确实受影响，可选项：①DTS 改回 `dr_mode="host"`（失去 USB adb）；
  ②保留 otg，但在息屏/挂起前临时切 host、恢复后切回 device；
  ③等 UCSI 修好（那才是根上的问题）。

## #55 ★★★★ **救援 Ubuntu 的挂起已修复并端到端验收**（2026-08-22）

```
挂起前 role=[device]                                  ← 系统正常状态就是 device
★ 第1/3 挂起成功（success 0→1）role 现在=[host]
★ 第2/3 挂起成功（success 1→2）
★ 第3/3 挂起成功（success 2→3）
==== 3/3  success=3 fail=0 ====
每次墙钟 ~115 秒，内核 printk 时间只走 ~2.1 秒 = 真睡
```

**走的是 `systemctl suspend`** —— 也就是合盖 / 闲置 / 用户触发实际会走的那条路径。

### ★★ 修复形态：**必须是 `system-sleep` 钩子，开机设一次不够**

`a600000.usb` 的 role switch **不归我们管，typec/UCSI 层才是它的主人**
（dmesg: `Fixed dependency cycle(s) with .../embedded-controller@38/connector@0`）。
实测三段证据：

1. 开机的 systemd 单元**确实成功**置成了 host
   （journal: `role: device -> host（子 xhci = 1）`，`status=0/SUCCESS`）；
2. **但到 up=76 秒它又变回 `device`** —— 被 typec 层改回去了；
3. 而且开机太早时写入会直接失败（journal 里有一次
   `⚠️ 置 host 失败，仍是 device`）。

⇒ 管用的是 **`/usr/lib/systemd/system-sleep/` 钩子：每次挂起之前再置一次**。
装法见 `scripts/s2idle/INSTALL-rescue.md`。

★ 另外 **udev 规则那条路走不通**（`ATTR{role}="host"`）：
`udevadm test` 显示规则确实匹配上了，但它设的值同样会被 typec 层覆盖。
已从仓库删除，免得误导。

### ⚠️★ 两个"判据本身错了"的坑，都值得记

1. **`echo mem > /sys/power/state` 不会跑 `system-sleep` 钩子** ——
   只有 `systemctl suspend`（及合盖/闲置这些走 logind 的路径）才会。
   我第一版验收脚本用 `echo mem`，钩子从没被调用，于是"修复看起来无效"。
   ★ **用错的触发方式去验证一个挂在正确触发方式上的修复，必然得到假阴性。**
2. ★ **`systemctl reboot` 是异步返回的。** 我在验收脚本里写了
   "role 不是 host 就拒绝挂起 → `systemctl reboot`"，但**没有 `exit`** ——
   于是护栏打印完警告后**照样往下跑进了真实挂起**，正好是危险配置。
   （所幸失败模式是复位、可自动恢复。）
   ★ **安全护栏里的"终止"必须真的终止**：`systemctl reboot` 后面要跟 `exit`。

### ⬜ Android 侧仍未做

USB adb 的 UDC 就在这个控制器上（`sys.usb.controller=a600000.usb`），
所以不能照搬。而 Android 的 `device` 角色**有真实 gadget**，未必受影响。
需要一次实测：**带 `CONFIG_PM_DEBUG` 但【不带调试插桩】的内核** + `pm_test=devices`。
⚠️ 那个插桩（我给 `device_prepare()` 加的 DPM 看门狗）在 Ubuntu 上无害，
但 Android 的 SystemSuspend 会不停发起挂起尝试，每次给约 700 个设备各建一个
定时器 —— 疑似把系统拖死过一次。**调试插桩要有"退场"意识。**

## #56 ★★★ Android 同样受影响 —— 而且"有真实 gadget"并不能免疫（2026-08-22）

用**干净的测试内核 `#32`**（= 发版内核 + `CONFIG_PM_DEBUG`，**已删掉我加在
`device_prepare()` 里的 DPM 看门狗插桩**）在 Android 上测。
方法只用 `pm_test=devices`（5 秒自动返回、不需要唤醒源），**不做真实挂起**。

```
==== Android 设备挂起阶段测试  内核=#32 ====
role=[device] 子xhci=0
UDC=[a600000.usb] state=[configured] gadget=[ffs.adb]   ← ★ 有真实 gadget
RTC 已武装 1787332812
pm_test=[... [devices] ...]
已放开 wakelock
（日志到此为止 —— 整板复位，回到默认的 android-b / #19）
```

★ **注意日志停的位置**：在"已放开 wakelock"之后、我的测试循环第一次试验**之前**。
也就是说 **wakelock 一松开，Android 的 SystemSuspend 自己立刻发起了挂起，
然后就复位了** —— 不是我的脚本触发的，是 Android 的正常待机路径。

⇒ **结论：Android 与救援 Ubuntu 一样受影响。**
★★ 而 Android 上 UDC 是 `configured`、gadget 是 `ffs.adb` —— **有真实 gadget
照样复位**。所以致命的**不是"缺 gadget"，是 `device` 模式本身**
（#52 里"半初始化"那个说法要收窄：缺 gadget 只是 Ubuntu 侧的表象）。

★ 推测的机制（未验证）：`host` 模式下 xhci 是子设备，走正常 USB PM 路径先挂起；
`device` 模式下走的是 `dwc3_gadget_suspend`，在本机这个被 DP 半占用的
QMP combo PHY 上，那条路径断电时打死 SoC。

### ⚠️ 直接推论：那个 wakelock 现在是**承重**的

`init.gaokun3.rc` 里的 `write /sys/power/wake_lock gaokun3_nosuspend`
**是唯一挡着随机整板复位的东西**。在这个问题修好之前**不能撤**。
（M4 当初加它是因为"挂起醒不来"，现在知道真实后果更严重：是复位。）

### 取舍表（Android 侧，现在是确定的了）

| 做法 | 待机 | USB device-mode adb | 状态 |
|---|---|---|---|
| `dr_mode="host"`（= 上游；PR #3 的 DTB 就是这个） | ✅ | ❌ 只剩 TCP adb（要 WiFi） | 可立即落地 |
| 保留 `otg`，挂起前切 `host` / 恢复后切回 | ✅ | ✅ | **需实现**：Android 没有 systemd-sleep 钩子，得在 SystemSuspend 或 power HAL 上挂 |
| 保留 `otg` 不动（现状） | ❌ 必须一直持 wakelock | ✅ | 现状 |

★ 第二行是唯一两全的，但要写代码。Android 侧可行的挂点：
`android.system.suspend` 的 wakelock 协调、或一个监听 `/sys/power/` 的原生
小服务、或干脆在内核里给 dwc3 加一个"挂起前切 host"的 quirk（那才是根治）。

## #57 ★★★★★★ **Android 的 s2idle 通了** —— 真实挂起/唤醒 ×4（2026-08-22）

配置：内核 `#32`（= 发版内核 + `CONFIG_PM_DEBUG`，**无调试插桩**）
+ `bin/gaokun3-usbrole.sh host`，`pm_test=none`（**真实挂起**），RTC 闹钟 +45 秒。
**走的是 Android 自己的 SystemSuspend 路径**，不是手动 `echo mem`。

```
role=[host] 子xhci=1
RTC 闹钟=1787334892（45 秒后）
★★★ 放开 wakelock，交给 Android 自己挂起
★★★ 回来了  success 3 → 7  fail=3
dmesg: PM: suspend entry (s2idle) → Restarting tasks: Done → PM: suspend exit
```

★ **`success` +4** —— 那个窗口里 Android 自己挂起并唤醒了**四次**，
每次都干净恢复，**`fail` 一次没涨**（那 3 次旧的是 pm_test 阶段
`alarmtimer.1.auto` 的良性拒绝，见 #56 末尾）。

### ★★ 附带结论：**"醒不回来"这个第二个问题并不存在**

M4 以来一直把 s2idle 当成两个问题（"挂起时复位" + "醒不回来"）。
现在看：**它们是同一个根因的两种表现**。#39 那次唯一的"睡进去没醒"观测，
是在 role 还坏着的情况下做的。role 修好之后，**进入和唤醒都正常**。
⇒ `docs/TODO.md` A9 里"复位解决后再攻醒不回来"那一条**可以划掉**。

### 完整的解决路径（三层，缺一不可）

1. **内核**：buildbot 的 `huawei-gaokun-ec: fix suspend/resume ordering`
   —— 治 EC 在 `suspend_noirq` 超时 −110。**发版内核本来就有。**
2. **救援 Ubuntu**：`/usr/lib/systemd/system-sleep/` 钩子，挂起前置 `role=host`。
   实测 `systemctl suspend` 3/3。零代价（那边不用 USB gadget）。
3. **Android**：`bin/gaokun3-usbrole.sh` + `etc/usbrole.rc`
   —— 息屏切 host、亮屏切回 device，靠"确认到 host 才放行"的 wakelock 不变量
   消除竞态。实测**真实挂起唤醒 ×4，零复位**。

⬜ **剩下的收尾**
* 目前 Android 侧默认**关闭**（`persist.gaokun3.allow_suspend` 未设）。
  要正式启用得走一次 ROM 构建 + 验收。
* ⚠️ 启用后的用户可见代价：**息屏时 USB device-mode adb 断开，亮屏恢复**
  （TCP adb 不受影响）。发版说明必须写清楚。
* ⚠️ 发版说明里"挂起是内核/EC 缺陷、Ubuntu 同样复现"那句是**错的**，必须改
  —— 真凶是我们自己在 Stage 2 加的 `dr_mode="otg"`。

★ **2026-08-22 更新**：以上三条都已随 **v0.3.0-alpha** 发版，Android 侧
`persist.gaokun3.allow_suspend=1` **默认开启**（`device.mk`），发版说明也已按
真实归因重写。所以这一节的"剩下的收尾"已经清空。

---

## #58 ★★★★★ 「切到设置就卡死」是**内核 panic**，而且是一个活着的上游竞态（2026-08-22）

用户报「切换到设置卡死」。**现场没保住**（机器已重启、logcat 是内存的、
`/data/anr/` 是空的、hangdump 看门狗也没命中——它判的是 D 状态线程 ≥2 分钟，
而这次机器压根没活到两分钟）。

★ **救回证据的是 pstore。** `/sys/fs/pstore/` 里有 **45 条 efi_pstore 记录**，
分属今天两次事件（uptime **411 s** 与 **713 s**）。这是 Stage 0 布下的那条
"没有串口就走 EFI 变量"的通路第一次在一个**用户报告的问题**上付清成本。
⚠️ 记一条：`ls /sys/fs/pstore/` 需要 root（普通 shell 是 Permission denied），
而 `adb root` 在本机要先 `setprop service.adb.root 1`。

原始日志已入库：[`evidence-58-drm-crtc-panic.txt`](evidence-58-drm-crtc-panic.txt)
（按 part 号重排回时间顺序，去掉 163 行 Handover 噪声；已扫过无 IP/SSID/凭据）。
⚠️ 每次事件在 pstore 里有**两份**：`Oops#1`（BUG 当场）与 `Panic#2`
（随后的 panic），内容大半重复 —— 别把它当成"崩了四次"。

### 不是卡死，是 `BUG()` → panic

```
kernel BUG at drivers/gpu/drm/drm_crtc.c:161!
Internal error: Oops - BUG: 00000000f2000800 [#1]  SMP
CPU: 1 UID: 1000 PID: 1658 Comm: RenderThread Not tainted 7.2.0-rc2-gaokun3+ #19
pc : drm_crtc_fence_get_driver_name+0x2c/0x30
lr : dma_fence_driver_name+0x1c/0x34
Call trace:
 drm_crtc_fence_get_driver_name+0x2c/0x30 (P)
 sync_file_ioctl+0x260/0x610
 __arm64_sys_ioctl+0xac/0x104
 ...
Kernel panic - not syncing: Oops - BUG: Fatal exception
```

★ **ESR = `0xf2000800`，EC = 0x3C（BRK 指令）** ⇒ 这是**显式的 `BUG()` 断言**，
不是空指针访问。这一个数字就把方向定死了：去找那一行的 `BUG_ON`，
不要去查内存越界。

**两次事件不是同一条路径，也不是同一个 app：**

| | 事件 1（411 s） | 事件 2（713 s） |
|---|---|---|
| helper | `dma_fence_driver_name` | `dma_fence_timeline_name` |
| 回调 | `drm_crtc_fence_get_driver_name` | `drm_crtc_fence_get_timeline_name` |
| 入口 | `sync_file_ioctl+0x260` | `sync_file_get_name` ← `sync_file_ioctl+0x354` |
| 肇事进程 | `RenderThread`，`app=org.lineageos.updater` | `RenderThread`，`app=com.android.permissioncontroller` |

⇒ ★**跟"设置"没有关系**。是任何 app 的 RenderThread 对一个 present fence 做
`SYNC_IOC_FILE_INFO` / `SYNC_IOC_MERGE` 就能触发 —— 也就是**普通应用能把整台
机器 panic 掉**。用户看到的"卡死"就是 panic 到重启之间那几秒。

### 根因：`drm_crtc` 的 `BUG_ON` 与 dma-fence 的「signal 时摘掉 ops」相互冲突

v7.2-rc2 原文（逐字核对过，就是第 161 行）：

```c
static struct drm_crtc *fence_to_crtc(struct dma_fence *fence)
{
	BUG_ON(rcu_access_pointer(fence->ops) != &drm_crtc_fence_ops);
	return container_of(fence->extern_lock, struct drm_crtc, fence_lock);
}
```

而 `dma_fence_signal_timestamp_locked()` 里（同样是 v7.2-rc2）：

```c
	ops = rcu_dereference_protected(fence->ops, true);
	if (!ops->release && !ops->wait)
		RCU_INIT_POINTER(fence->ops, NULL);
```

`drm_crtc_fence_ops` **只有两个 name 回调，既没有 `.release` 也没有 `.wait`**
—— 正好是"signal 即摘 ops"的那一类。而两个 helper 是**先取 ops 再通过它回调**：

```c
	ops = rcu_dereference(fence->ops);
	if (ops)
		return (const char __rcu *)ops->get_driver_name(fence);
```

于是竞态窗口是明摆着的：

```
CPU A（sync_file_ioctl）                    CPU B（vblank）
ops = rcu_dereference(fence->ops)  → &drm_crtc_fence_ops
                                            RCU_INIT_POINTER(fence->ops, NULL)
ops->get_driver_name(fence)
  → fence_to_crtc(): BUG_ON(... != &drm_crtc_fence_ops)   ← 现在是 NULL → panic
```

**CRTC out-fence 每次 vblank 都 signal**（本机面板 120 Hz），Android 又在不停地
查 fence 名字，所以撞上只是时间问题 —— 表现为"随机卡死"。

### ★ 这不是我们的锅，而且**上游 master 也没修**

* 摘 ops 那套机制的四个提交（`f4cc3ab824d6` protect fence ops by RCU、
  **`541c8f2468b9` detach fence ops on signal**、`3e5067931b5d`、`1f32f310a13c`）
  用 GitHub compare API 逐个查过：相对 `v7.2-rc2` 全是 `behind`
  ⇒ **都已经在我们的内核里**。所以"升级内核就好了"这条路不存在。
* 拉 `torvalds/linux` **master** 的 `drm_crtc.c` 对比：那个 `BUG_ON` **一字未改**
  ⇒ 缺陷在当前 mainline 里仍然活着。搜 lore / dri-devel 没有对应报告。
* ⚠️★ 方法论：我一开始的假说是"7.2-rc2 缺了保护、后来修了"，很顺，也**错了**。
  救我的是**没有停在假说上，而是去查那几个提交到底在不在这个 tag 里**
  （compare API 一条命令）。"应该已经修了"和"确认在不在"差着一次 panic。

### 修法：`patches/0013-drm-crtc-drop-racy-BUG_ON-in-fence_to_crtc.patch`

删掉那行 `BUG_ON`，**不做别的**。理由（不是想当然）：
* `fence_to_crtc()` **只有两个调用者**，而它们本身就是 `drm_crtc_fence_ops` 的
  成员 —— 能进到函数里就说明解引用那一刻 ops 是匹配的。这个 `BUG_ON` 断言的
  正是调用路径已经保证过的事，**而事后再查一遍只能引入竞态**。
* 摘 ops 只是宣告"驱动数据在一个 RCU grace period 后可能被释放"。
  这两个回调之后读的东西都还活着：`crtc->timeline_name` 是嵌在 CRTC 里的数组、
  `crtc->dev->driver->name` 是静态存储；而两个 helper 的文档明写只能在
  `rcu_read_lock()` 里调，那就是 CRTC 不会消失的依据。

已用 `git apply --check` 对着 **v7.2-rc2 原文**验过能干净应用。
⬜ **但还没编、没上机** —— 需要一次内核构建。

★ 顺带堵掉一个长期漂移源：`patches/*.patch` **此前没有任何消费者**（全靠人在
构建机上手 `git apply`），本会话刚为此付过一次代价（`ee5eca9` 补上两个只活在
构建机工作区里的 DTS 改动）。新增 `scripts/kernel-apply-patches.sh`：
只打内核那几个补丁（目前 8 个）、幂等（用反向 `--check` 判定已应用）、失败就非零退出。

### 附带记两条噪声，别再被它们带偏

* `qcom_q6v5_pas 2400000.remoteproc: Handover signaled, but it already happened`
  在崩溃日志里刷了 **163 行**（约每 200 ms 一条）。#37 已用对照实验证明它是
  **良性噪声**（工作正常的加速度计同样每 12 秒 13 条）。⚠️ 它在这里唯一的作用
  是**把 pstore 的有效内容挤掉** —— 45 条记录里真正有用的不到 10 条。
* 崩溃前有一大串 `avc: denied` 指向 `vulkan.freedreno.so` / `minigbm` /
  `vendor_default_prop`，全是 `permissive=1`，且属于 app 首次初始化 GPU 的正常
  过程。**与 panic 无关**，但它们确实指出了肇事 app 是谁 —— 这次帮上了忙。

---

## #59 ★★ SLPI 的 handover 噪声：查清了、量化了，并且**它损害取证能力**（2026-08-22）

[#58](#58) 的副产品。那条从 #37 起就被当成"良性噪声"放过的日志：

```
qcom_q6v5_pas 2400000.remoteproc: Handover signaled, but it already happened
```

**它确实无害，但它不是无代价的。** 本轮的量化：

* `2400000.remoteproc` = **remoteproc0 = SLPI**（传感器 DSP），`state=running`。
  ADSP 是 `3000000`、CDSP 是 `1b300000`，两者都不刷。
* uptime 1133 s 时 dmesg 里已有 **1478 行**，间隔约 **199 ms（≈5 Hz）**，
  而且永不停止。
* ★ **`/proc/interrupts` 给出了硬证据**：SLPI 的 `q6v5 ready`（smp2p bit 1）
  与 `q6v5 handover`（bit 2）**计数完全相同、同步增长**
  （5 秒内 5475 → 5502），而 `smp2p-adsp` / `smp2p-nsp0` 的对应两条各只有 **2**。
  ⇒ **是远端在以 5 Hz 反复翻转这两个位**，不是中断卡住
  （卡住的电平中断会连续刷，不会是整齐的 5 Hz）。
  ⇒ 也不是我们的驱动数错了：`q6v5_handover_interrupt()` 在
  `handover_issued` 已置时**直接返回、不碰任何 proxy 资源**（源码核对过），
  所以唯一的后果就是那行日志。

### ★ 后果不小：它擦掉了 #58 的崩溃栈

pstore 只保留内核日志的**尾部**。#58 那两次 panic 一共留下 45 条 efi_pstore
记录，而**其中不到 10 条装着有用东西** —— 其余全是这一行。
**一个良性且自我重复的条件，不该有能力把 panic 的调用栈挤出崩溃日志。**

### 修法：`patches/0014-remoteproc-qcom-ratelimit-repeat-handover-error.patch`

`dev_err` → `dev_err_ratelimited`。前几条照样打（远端不健康仍然看得见），
但它再也淹不掉别的东西。已对 v7.2-rc2 原文 `git apply --check` 通过。
⬜ 未编译上机。

⬜ **根因仍未查**（远端为什么每 200 ms 翻一次位）。**第一步**：5 Hz 这个数字
很像一个采样节拍 —— 停掉 sensors HAL / `hexagonrpcd` 看频率变不变，就能判定
是不是我们自己的传感器通路在驱动它。
⚠️ 但**别在没人看着的时候做这个实验**：M12 记过停/重启 HAL 会污染 SSC 会话，
之后连独立客户端都读不到传感器，要重启 `hexagonrpcd` 才恢复
（还得等约 20 秒沉降）。代价是自动旋转当场失效。

---

## #60 ★★ SELinux 转 enforcing 的第一步：先做一次 denial 普查（2026-08-22）

[TODO B1](TODO.md) 的第一步一直写着"把现有 denial 收集成 `.te`"。真做了一次
普查之后，发现**清单本身就推翻了那句话的前提**。

**方法**（uptime 约 1200 s 的一次普通启动，未刻意操作）：
`dmesg` + `logcat -b all` 里的 `avc: denied` 全取出来，按
`(scontext, tcontext, tclass, perm)` 去重。

**结果：988 行 → 237 种去重元组。** 按来源分：

| scontext | 条数 | 真正是谁 |
|---|---:|---|
| `vendor_init` | 381 | ★ **`chcon`（363）= 我们自己的 `bpf-relabel.sh`** |
| `shell` | 121 | 我自己的 adb 探测（`sys_rawio` / `sys_ptrace`），不是目标 |
| `init` | 88 | ★ `android.hardwar`(46) + **`hexagonrpcd`**(22) + `gaokun3-usbrole`(1) |
| `platform_app` / `surfaceflinger` / `hal_graphics_*` / `bootanim` | 约 130 | 几乎全是 `device : chr_file {ioctl,read,write,map}` |
| `hal_health_default` | 50 | `sysfs : file {read,open,getattr}` |
| `network_stack` | 27 | 同上，`sysfs` |

### ★ 推翻的前提：我们的服务**根本没有域**

TODO 里写的是"需要写 policy 的至少有 `hexagonrpcd`、sensors HAL、
`audioroute`、`smmustall`"。但普查里**根本找不到这些域名** —— 因为它们
`scontext` 是 `u:r:init:s0`：init 起的 root 进程没有 `file_contexts` 条目就
**留在 init 域里**。（`sensors HAL` 在列表里显示为 `comm=android.hardwar`，
被截断的进程名。）

⇒ 所以第一步不是"补 allow 规则"，是**给我们的可执行文件定义域并做 transition**。
在那之前收集到的 denial 都挂在错误的主体上，照着写出来的 `.te` 是错的。

### 第二个结构性发现：一大批 `device : chr_file`

`surfaceflinger` / `platform_app` / `bootanim` / `hal_graphics_composer` /
`hal_graphics_allocator` 都在被拒 `device : chr_file {ioctl,read,write,map}`。
`device` 是**通用兜底标签** —— 说明那些设备节点没有任何 `file_contexts`
条目。这一类不用逐条写 allow，**给节点打上正确的类型就一起消失**。

### ✅ 本轮就地清掉的一块：`bpf-relabel.sh`（-363 条，约全系统 37%）

★ **denial 自己就是证据**：`chcon` 被拒时的 **tcontext 已经是
`fs_bpf_netd_shared`**，而不是错的根标签 `fs_bpf` —— 要是标签靠这个脚本设的，
它走进去时看到的应该是后者。上机复核：9 个子目录标签全对；临时造一个
`zz_probe` 拿到 `fs_bpf`（`genfs_contexts` 里没有它的条目，回落正是惰性匹配
该有的行为）⇒ **`patches/0007` 在干活，脚本是死重量。**

**但没有删它** —— 没打 0007 的内核上它仍是 system_server 不崩溃循环的唯一依靠
（#36 那一仗）。改成**先检查再动手**：探一个已知子目录的标签，对了就
`exit 0`，错了才走原来的 `chcon -R`。实机 `sh -x` 验过走的是早退分支、
`chcon` 一次都没跑。

⚠️ 顺带记一条：这个脚本一个人贡献了全系统 **约 1/3** 的 denial，而
[#59](#59) 刚证明日志洪水会把 panic 的调用栈从 pstore 里挤掉。
**"permissive 下 denial 无害"是错的** —— 它们要花取证预算。

### ✅ 第二块也就地清掉了：`/dev/dri/*` 从来没人打过标签（约 130 条）

追到具体节点：那批 `device : chr_file` 里绝大多数点名的是
**`/dev/dri/renderD128`（多个域共 130+）与 `/dev/dri/card1`（17）**。
上机核实三条：
* 两个节点的标签确实是通用兜底的 `u:object_r:device:s0`；
* `/vendor/etc/selinux/vendor_file_contexts` 里**一条 dri 都没有**；
* ★ AOSP 核心的 `plat_file_contexts` 只有 `/dev/pvrsrvkm → gpu_device`
  —— **它根本没考虑过 `/dev/dri`**，因为常见 Android 设备走 `/dev/kgsl-3d0`
  或 mali 节点，不是 DRM 渲染节点。这正是"AOSP on mainline"会独有的缺口。

`gpu_device` 是 AOSP 的**公共**类型，surfaceflinger / bootanim / system_server /
system_app / platform_app / 两个 graphics HAL / mediaswcodec 的读写规则核心策略
里全都现成 ⇒ **只加两行 `file_contexts`，不写一条 allow**。
⚠️ 本机 DRM 主节点是 **card1 不是 card0**，用通配别写死。

⬜ **同一处还剩一半**：`/dev/dri`【目录本身】被拒的是 `{ read open }`
而**不是** `search` —— 那是 mesa/libdrm 在 `opendir`+`readdir` 枚举显卡节点。
给目录换类型解决不了，得真写 allow 规则，而规则该落在核心还是 vendor 策略
要先对 Treble 的 neverallow 确认。留到真正切 enforcing 时做。
★ 记这条是因为**两者看着像同一个问题，其实不是**：一个是标签缺失，
一个是缺规则。

### ⬜ B1 的真正待办（按顺序）

1. 给 `hexagonrpcd`、sensors HAL、`gaokun3-usbrole`、`audioroute`、
   `smmustall`、`hangdump`、`bpfrelabel` 各定一个域 + `file_contexts` 条目。
2. 给那批 `device` 标签的字符设备节点定类型。
3. `hal_health_default` / `network_stack` 要的 `sysfs` 子路径打标签。
4. 全部做完再重新普查一次 —— **只有那时的清单才是可以照抄成 allow 规则的**。
