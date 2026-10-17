# 华为 MateBook E Go 跑 Android（SC8280XP / `gaokun3`）

**crDroid 16.0（Android 16），跑在主线 Linux 内核上，Adreno 690 硬件 Vulkan。**

高通从来没给 8cx 系列发过 Android BSP，只有 Windows 和 Linux 驱动。所以这里
没有可以扒 vendor blob 的原厂 Android ROM，机器上也没有 Android 那一套常规设施：
没有 `fastboot`、没有 Android bootloader、没有 recovery 分区、没有串口 —— 只有
UEFI。这不是一次常规移植 —— 它是 **AOSP on mainline**，每一个 HAL 都建在上游驱动之上。

> ### ⚠️ Alpha 阶段，先读这段
> 最新版：**v0.7.1-alpha**。游戏跑得不错，待机与唤醒也正常。
> **但仍然没有可用的 recovery，"恢复出厂设置"也不起作用** —— 见[已知限制](#已知限制)。
> 命令行安装器（以及图形安装器的"清空整块硬盘"）会**清空内置硬盘，Windows 一起没**；
> 只有图形安装器的双系统模式（预览）会保留 Windows。出厂状态的镜像哪里都没有。
> 你需要有能力救一台开不了机的机器。不提供任何担保。
>
> 装之前还要知道：**`/data` 没有加密**、**内核里内置了 root**、镜像用的是
> **Android 公开的测试密钥签名**，而且到 v0.7.1 为止**网络 adb 不要授权就能连**。
> 详见[已知限制](#已知限制)；常见问题（抓日志、开机菜单、卖机前清数据）见 [FAQ](docs/FAQ.zh-CN.md)。

[**English → README.md**](README.md)

**交流：**[Telegram](https://t.me/gaokunAndroid) · QQ 群 **920133252**

---

## 现状

以 v0.7.1-alpha 为准。标 ✅ 的每一条都是实机测出来的，不是推断；没测过的都写明了。
证据在 [`docs/`](docs/) 里，案卷编号（#NN）在 [`docs/stage4-findings.md`](docs/stage4-findings.md)。

| 项目 | 状态 | 说明 |
|---|:--:|---|
| 引导（UEFI + systemd-boot，内置盘） | ✅ | 不需要 U 盘。A/B 槽位 + Virtual A/B；系统更新（含内核）在设置里装 |
| 屏幕 1600×2560 @ 120 Hz | ✅ | 框架默认值把渲染钉在 60，已覆盖；实测 vsync 周期 8.33 ms |
| GPU —— Adreno 690 硬件 Vulkan | ✅ | Mesa 26.0.3 `turnip`；22 分钟浸泡零 SMMU fault |
| 触摸屏 | ⚠️ | 可用 —— Himax HX83121A，需要 `patches/` 里的 gpio174 补丁（[#26](docs/stage4-findings.md#26)）。**v0.6.2 起手感是实机量出来的**：① 默认预设里的跳点检测阈值实际上是一条 1.0 m/s 的限速线，快滑时驱动一个点都不上报，一次甩动被切成十几次触摸 —— 判据改成对照预测位置（[#114](docs/stage4-findings.md#114)）；② 坐标 `fuzz` 从 8 改为 0：实测质心抖动不到半个坐标单位，而 fuzz=8 抹掉了四分之一的真实运动数据（[#116](docs/stage4-findings.md#116)）；③ 按下延迟 25→17 ms；④ 上报触点面积与压力，供读取它们的应用使用（本机 Android 自带的手掌误触抑制是关着的，用不上这些数据）。驱动本身修了 6 个缺陷并加了逐级计数器与原始电容帧导出（[#115](docs/stage4-findings.md#115)）。⚠️ 手掌压屏会碎成多个触点（不影响点击） |
| 磁吸键盘 + 触控板 | ✅ | USB HID `12d1:10b8`；可以在"设置 › 系统"里关掉（"磁吸键盘"） |
| 键盘盖当合盖 | ⚠️ | EC 会上报合盖开关，但 Android 不处理：合上键盘盖不会熄屏，要等息屏超时 |
| Wi-Fi | ✅ | ath11k / WCN6855。局域网拉 200 MB 实测 61.7 MB/s（[#44](docs/stage4-findings.md#44)）。从远处服务器（约 300 ms）下载时，单连接原先封顶约 3.5 MB/s —— 是 Android 默认 TCP 缓冲区的限制；v0.7.0 调大，实测 3.5→9.5 MB/s（[#119](docs/stage4-findings.md#119)）。WPA3-SAE 能连（[#107](docs/stage4-findings.md#107)）；有的 WPA2/WPA3 混合模式路由器会拒绝 Android 自动把 WPA2 升级成 WPA3，v0.7.1 起关掉了这个升级。[issue #2](https://github.com/vahiru/gaokun-android/issues/2)（纯 WPA3，在中兴路由器上关联后被踢）这里复现不出来。⚠️ MAC 地址每次开机都变 |
| Wi-Fi 热点 | ⚠️ | v0.7.1 起可用（[#11](https://github.com/vahiru/gaokun-android/issues/11)）—— 热点开得起来；还没测过真有手机连上来。⚠️ 一开热点平板自己就断开 Wi-Fi（还没配置 Wi-Fi + 热点同时工作），所以没有上行可以分享 |
| 蓝牙 | ⚠️ | 可用 —— `hci_qca`，adapter `ON`，开机后零崩溃。⚠️ 蓝牙耳机放音（A2DP）**没在实机上验证过**。音频策略里没有蓝牙 SCO 通路，所以**通话时用不了蓝牙耳机的麦克风**。长期运行后可能与音频一起死锁（[#38](docs/stage4-findings.md#38)） |
| 扬声器 | ⚠️ | 可用 —— WSA883x 走 audioreach。增益分配原先是错的、一直在削波（−6 dBFS 素材上 THD −20 dB）；v0.6.0 起数字级压在单位增益、响度由 PA 出，干净约 20 dB（[#86](docs/stage4-findings.md#86)）。可选的"扬声器增强（实验性）"在"设置 › 声音"里，默认关（v0.7.0，感谢 @mashen11）。v0.7.1 起播放由声卡硬件定拍，负载下不再丢块，音游不再错位（[#130](docs/stage4-findings.md#130)）。⚠️ 长期运行后音频可能死锁，与蓝牙一起（[#38](docs/stage4-findings.md#38)） |
| 耳机口 | ✅ | **已修复，用户实机确认出声。** 三处阻塞：rx-macro 内部的插值器链从来没接上（于是后端拒绝打开，**内核一行日志都不打**）；音频策略里没声明有线输出；框架去看 `/sys/class/switch/h2w`，主线上根本没这个东西（[#40](docs/stage4-findings.md#40)） |
| 麦克风 | ⚠️ | **v0.7.0 起内置麦克风能录音**（[PR #10](https://github.com/vahiru/gaokun-android/pull/10)，感谢 @mashen11；[#127](docs/stage4-findings.md#127)）。它此前从来没工作过：前端混音器与 DMIC 序列从没设过、HAL 覆盖了麦克风的 address、策略列着硬件开不了的采样率、管道节流约每 8 块丢 1 块。⚠️ **有线耳机的麦克风用不上** —— 插四段耳机通话、录音时用的仍是内置麦 |
| USB 音频 | ❌ | USB 耳机、USB 声卡不走（内核驱动在，音频 HAL 没有 USB 模块） |
| 电池、充电 | ⚠️ | 华为 EC 驱动。⚠️ 接低功率电源（电脑 USB 口、小功率手机充电器）时，可能显示"正在充电"而电量照掉，Android 也就不会做低电量关机 —— 到 0% 直接断电。电池温度读数恒为 0 |
| **游戏** | ✅ | 原神画质极高流畅。GPU 空闲 270 MHz、峰值 690 MHz、最高 50 °C。v0.7.1 冒烟测试：明日方舟、三角洲行动、卡拉彼丘、Phigros、Arcaea 都能跑。⚠️ 英雄联盟手游启动后马上退出（有人报告，还没有日志） |
| CPU 温控降频 | ✅ | 主线 DTS **根本没有** CPU 的 cooling map —— 已由 [`patches/0009`](patches/) 在设备树里根治 |
| **待机 / 挂起** | ✅ | **2026-08-22 修复 —— 而且真凶是我们自己，不是内核**（[#52](docs/stage4-findings.md#52)、[#57](docs/stage4-findings.md#57)）。真实挂起/唤醒，零复位。v0.7.1 修掉了长时间开机后唤醒卡死（[#16](https://github.com/vahiru/gaokun-android/issues/16)、[#131](docs/stage4-findings.md#131)）。插着电脑的 USB 时，息屏但不睡，所以 USB adb 一直在。还没量过：整夜待机掉多少电 |
| 传感器（加速度计+陀螺仪）| ✅ | **自动旋转可用，用户实机确认方向正确。** 为本机写的 sensors HAL 把真实的加速度计、陀螺仪读数喂给 SensorService，框架据此融合出 Game Rotation Vector / Gravity / Linear Acceleration。出厂安装矩阵全零（校准数据随 Windows 一起没了），但传感器坐标系与面板方向本来就一致，不需要纠正。本机**没有磁力计**（所以没有指南针）。光感在总线上有应答，但一激活就让传感器 DSP 崩溃，所以一直关着 —— 没有自动亮度（[#121](docs/stage4-findings.md#121)）。⚠️ 那颗 DSP 一旦崩溃重启，传感器就全丢了，要重启才回来（[#121](docs/stage4-findings.md#121)） |
| 硬件视频解码 | ✅ | **v0.7.0 起是 `qcom-iris`**（原来是 `qcom-venus`）：H.264、HEVC、VP9 走 `c2.v4l2.*.decoder`，中途停止、拖动、重播、中途变分辨率都过了，为此修了上游驱动几处（[#128](docs/stage4-findings.md#128)）。VP8 走软解。最早打通解码时修的两个 `external/v4l2_codec2` 可移植性 bug 见 [#41](docs/stage4-findings.md#41) |
| 硬件视频编码 | ❌ | 故意关着：`v4l2_codec2` 的编码组件不会把交给它的 RGBX 帧转成硬件要的 NV12，开着的话应用会直接失败而不是回退。应用用的是软件编码器 |
| 摄像头 | ✅ | **前后摄都能用**（v0.6.1 起）。前摄 Hynix hi846，后摄 **OmniVision OV13B10** —— 是从华为 Windows 驱动包里解出上电序列才认出来的（[#106](docs/stage4-findings.md#106)）。链路：主线 `camss` → libcamera simple 流水线 + 软件 ISP → libyuv → 为本机写的 AIDL HAL。闪光灯可用；后摄有**自动对焦**（v0.7.0，感谢 @mashen11）；照片按应用要求的方向旋转。"每隔一次就拍不了"的电源域缺陷已根治（[`patches/0031`](patches/)、[#105](docs/stage4-findings.md#105)）。⚠️ 最高 15 fps，没有变焦、没有曝光补偿。画质没调：没有色彩矫正矩阵、暗处噪点多、闪光片过曝；暗处对焦慢。**录像没在实机上验证过** |
| USB-C | ⚠️ | UCSI 起得来，两个连接器都注册了（[#112](docs/stage4-findings.md#112)）。数据角色跟着对面实际是什么走 —— 接电脑时我们是设备；接 hub 或 U 盘时应当切成主机，这是按设计做的、还没拿真硬件试过（[`patches/0048`](patches/)，v0.7.0）—— v0.7.1 起 Android 的 USB 服务也起来了，应用能用 USB 设备（[#13](https://github.com/vahiru/gaokun-android/issues/13)）。⚠️ **回插之后（在待机后出现过）这个口可能坏掉，要重启才恢复** —— 按代码推断，那之后到重启为止整机也不再待机。能和电脑传文件（MTP/PTP：到设置 > 已连接的设备 > USB 里选，USB 2.0 的速度）。⚠️ **U 盘不会挂载**（Android 侧还没有可移动存储的配置）。DP 外接显示没测过 |
| 指纹 | ❌ | 进行中：华为签名的指纹 TA 已经能在本机加载进安全世界（[#125](docs/stage4-findings.md#125)）；驱动和 HAL 还没有 |
| 手写笔（M-Pencil）、TPM | ❌ | 不支持 |
| Root | ⚠️ | 每个内核都**内置了** KernelSU（ReSukiSU 分支），关不掉。不装 ReSukiSU 管理器 App 时处于休眠；装了之后，只有你在管理器里批准的应用才能拿到 root。检测 root 或未锁定启动链的应用可能拒绝运行（[详情](docs/known-limitations.zh-CN.md#默认带-rootkernelsu--resukisu)） |
| DRM（Widevine） | ❌ | 系统里完全没有 DRM 模块：Netflix、Disney+、Prime Video 这类放不了正片（[详情](docs/known-limitations.zh-CN.md#无法播放受-drm-保护的视频没有-widevine)） |
| SELinux | ⚠️ | `permissive`。为转 enforcing 已经做了七轮策略；enforcing 试跑时主要功能都正常，相机也在内（[#129](docs/stage4-findings.md#129)） |

## 硬件

| | |
|---|---|
| SoC | 高通骁龙 8cx Gen 3（SC8280XP） |
| 型号 | HUAWEI GK-W7X，2022 款，CSOT 面板 —— 唯一构建并测试过的型号 |
| BIOS | 任何版本都行。（本页以前写着"只能 2.16、不要升 2.17"；这条限制 2026-09-25 已撤销 —— 已经有人验证过不依赖 BIOS 版本。**请不要去降级 BIOS。**） |
| GPU | Adreno 690 |
| 屏幕 | Himax HX83121A，MIPI-DSI，1600×2560 —— 与 Galaxy Tab S7 FE 同款面板 |
| Wi-Fi / 蓝牙 | WCN6855 |
| 存储 | NVMe |
| 固件 | UEFI，必须关闭 Secure Boot |

---

## 安装

从 [**Releases**](https://github.com/vahiru/gaokun-android/releases) 取最新版，按
[`docs/INSTALL.zh-CN.md`](docs/INSTALL.zh-CN.md)操作（英文版 [`docs/INSTALL.md`](docs/INSTALL.md)）。任何 BIOS 版本都行；必须关闭
Secure Boot。安装器有两个：

| | 图形安装器（预览） | 命令行安装器 |
|---|---|---|
| 能做什么 | **装在 Windows 旁边**（双系统）、清空整块硬盘、原地**重新安装 Android**（默认清除数据，也可保留）、手动调整分区 | 清空整块硬盘再装 |
| 从哪里跑 | 它自带的小 Linux：U 盘，或者 —— 不用 U 盘 —— 在 Windows 里用 `gaokun3-setup.cmd` 启动 | arm64 Linux live U 盘 + 本仓库的 checkout（通用 Ubuntu/Debian 镜像能不能在本机起来，没验证过） |
| 系统镜像 | 介质上带着，或者连 Wi-Fi 下载 | 发布页的 `super.img.zst` + `boot.img` + `install-artifacts.sha256` |
| 文件 | [v0.7.0-alpha 发布页](https://github.com/vahiru/gaokun-android/releases/tag/v0.7.0-alpha)上的 `gaokun3-installer-0.1.0-preview-*` | `scripts/install-gaokun3.sh` |
| 实机测过 | 只有"重新安装 Android、保留数据"。双系统、清空整盘、调整分区只在测试盘上跑过；Windows 脚本只在虚拟机里跑过；U 盘镜像还没真正启动过 | 最初的安装方式。2026-09-24 起它用的是图形安装器的后端，下面这套布局目前只在测试盘上建过，没在真机上建过 |

清空整盘后的布局：

| 分区 | 大小 | 用途 |
|---|---|---|
| `esp` | 300 MiB | systemd-boot，外加它实际加载的内核 / DTB / ramdisk，每个槽位一个目录 |
| `misc` | 4 MiB | A/B 槽位状态 |
| `metadata` | 32 MiB | Android metadata |
| `super` | 12 GiB | system / system_ext / product / vendor，A/B |
| `boot_a`、`boot_b` | 各 64 MiB | Android boot 镜像；OTA 写的是它们，ESP 上的那份由它们解出来 |
| `gk3rescue` | 1 GiB | 可选的救援系统 —— 见下 |
| `userdata` | 剩余全部 | `/data` |

**救援系统。** 本机没有能用的 Android recovery，也没有串口，所以能从开机菜单启动的
小 Linux 就是修机器的办法。安装器把它放进单独的 1 GiB 分区，在开机菜单里是**非默认**
的一项（每次开机菜单停 15 秒）：Android 卡死时长按电源键，在菜单里选救援那一项。
不会自动回落到它。图形安装器会装上它；命令行安装器只在找到救援镜像时才装 ——
发布页不带这个镜像，所以用通用 live U 盘装出来的只有 Android。

> 本页以前写的是"约 25 GiB 的 Ubuntu 救援分区、默认启动项"。那一套 2026-09-24
> 已经撤掉，见 [`docs/INSTALL.zh-CN.md`](docs/INSTALL.zh-CN.md#关于救援系统)。

**SSH 登录救援系统**要用你自己的公钥，安装之前先放到安装器 U 盘上 —— 发布的镜像里不带任何人的公钥。
见 [`docs/INSTALL.zh-CN.md`](docs/INSTALL.zh-CN.md#关于救援系统)。

**国内下载：** GitHub 的下载服务器慢或者连不上时，系统镜像（`boot.img`、`super.img.zst`、
`install-artifacts.sha256`）有镜像站：`https://ota.072172.xyz/install/<build>/<文件名>`，
`<build>` 是该版 OTA 包去掉 `.zip` 的文件名 —— 见 [`docs/INSTALL.zh-CN.md`](docs/INSTALL.zh-CN.md#下载)。
图形安装器自己就是从这个镜像站下载的；安装器本身的文件目前只在 GitHub 上。

**更新：** v0.2.x 起都在设置里更新 —— 系统内的更新程序装进非活动槽位，下次重启生效。

---

## 已知限制

完整清单和应对办法见 [`docs/known-limitations.md`](docs/known-limitations.md)；
每一版改了什么见最新的发版说明（[v0.7.1-alpha](docs/relnotes/v0.7.1-alpha.md)）。简要来说：

* **安全与隐私。** `/data` **没有加密** —— 机器到了别人手里，上面的数据全都能读，
  锁屏密码挡不住。每个内核都**内置了 root**（ReSukiSU，KernelSU 的分支）；不装它的
  管理器 App 时处于休眠，但检测 root 或未锁定启动链的应用可能拒绝运行。镜像和更新用
  **Android 公开的测试密钥**签名，任何人都能签出本机会接受的更新包或系统应用。
  到 v0.7.1 为止，**Wi-Fi 上的 adb（5555 端口）是开着的、不弹授权框** —— 同一网络里
  任何人都能拿到 root shell；发布版会把它关掉（计划在 1.0）。SELinux 是 `permissive`。
* **没有可用的 recovery**，所以设置里的"清除所有数据"不起作用。要清空设备，用图形
  安装器的"重新安装 Android"。
* **Google。** 发布镜像带着 Google 应用（MindTheGapps），没有不带的版本。登记之前
  Play 商店会说设备未经认证（[INSTALL 的这一节](docs/INSTALL.zh-CN.md#此设备未经-play-保护机制认证)），
  Play Integrity 过不了。没有 Widevine，Netflix 这类付费流媒体放不了。没有 GPS，
  网络定位靠 Google 服务。没有预装中文输入法。
* **不支持：** 指纹（进行中）、手写笔、TPM、U 盘、USB 音频、
  硬件视频编码、蓝牙耳机麦克风、有线耳机麦克风、自动亮度。
* **已知缺陷：** 长期运行后音频与蓝牙可能死锁（看门狗会把证据存到
  `/data/vendor/gaokun3/hangdump-*`，请附上）；插电脑时平板可能反过来给电脑供电、
  两边互相看不到（重插一次）；回插后 USB-C 口可能坏掉、要重启；
  手掌压屏碎成多个触点；开热点会断 Wi-Fi；英雄联盟手游启动即退；低功率电源下可能
  显示"正在充电"而电量在掉。
* **专有组件。** 发布镜像里带着华为固件和华为 Histen 音频库，它们**不在**本仓库里。

---

## 构建

需要一台 Linux，至少 64 GB 内存（AOSP 官方建议；我们从不低于这个配置构建 ——
[`docs/build-machine.md`](docs/build-machine.md)），约 400 GB 磁盘。

```sh
repo init -u https://github.com/crdroidandroid/android.git -b 16.0 --git-lfs
cp <本仓库>/manifests/local_manifest_gaokun3.xml .repo/local_manifests/
repo sync -c -j"$(nproc)"

cp -a <本仓库>/device/huawei/gaokun3 device/huawei/gaokun3
python3 <本仓库>/scripts/crdroid-tree-fixes.py .   # 为什么要改，脚本里写了
source build/envsetup.sh
lunch lineage_gaokun3-bp4a-userdebug
m bacon superimage
```

* `bacon` 和 `superimage` 要在**同一次** `m` 里编：分两次会得到两个构建戳，OTA 包和
  `super.img` 互不相认（[`scripts/release.sh`](scripts/release.sh)）。
* 编 **`userdebug`**，不要编 `user`：`user` 构建会强制 SELinux enforcing，而本机的
  策略还撑不起 enforcing 开机。
* 默认编出来的是**发布构建**（变体仍是 `userdebug`）：adb 默认关、要用户自己打开，每台电脑都要授权；
  不监听 TCP 5555；`ro.debuggable` 为 0；镜像里没有 adb 公钥。
  `GAOKUN3_DEV_BUILD=1 m bacon superimage` 编出的是**开发构建**，带着以前那几样开发便利
  （adb 免授权、TCP 5555、`ro.debuggable=1`、烤进你的公钥）—— 绝不要拿它发布。
  开关在 [`lineage_gaokun3.mk`](device/huawei/gaokun3/lineage_gaokun3.mk)，v0.7.1-alpha 之后才有。
* 有几样构建输入**不在**本仓库里，各目录的 README 写了怎么准备：`firmware/`（华为
  固件，从你自己的机器上取 —— [`firmware/README.md`](device/huawei/gaokun3/firmware/README.md)）、
  `hexagonrpcd-root/`（传感器 DSP 用的文件）、`prebuilt-boot/`（内核，见下）、
  `effects/prebuilt/`（Histen 库；缺了构建照样通过，只是悄悄没了扬声器增强）。
  开发构建还要 `adb_keys`（你自己的 adb 公钥：`cp ~/.android/adbkey.pub device/huawei/gaokun3/adb_keys`）；
  发布构建不用它。

内核单独构建，分几层：主线 **v7.2-rc2**（`8cdeaa50eae8dad34885515f62559ee83e7e8dda`），
上面是 [`linux-gaokun-buildbot`](https://github.com/KawaiiHachimi/linux-gaokun-buildbot) 的补丁，
再打本仓 [`patches/`](patches/) 里的补丁（`scripts/kernel-apply-patches.sh <内核树>`，幂等）
和 ReSukiSU（`scripts/kernel-setup-resukisu.sh <内核树>`）。某一版内核到底用了哪些源码，
看那一版 release 附件里的 `kernel-source.txt`（v0.7.1-alpha 发布时还没有这个附件，见
[`docs/relnotes/v0.7.1-alpha-sources.md`](docs/relnotes/v0.7.1-alpha-sources.md)）。
`scripts/clone-refs.sh` 拉的是 buildbot 的 `main` 分支，只供参考，不代表发版用的就是它。
Android 相关的配置断言在 [`scripts/kernel-config-android.sh`](scripts/kernel-config-android.sh)；`vmlinuz.efi` 与
DTB 怎么编、放哪里，见
[`prebuilt-boot/README.md`](device/huawei/gaokun3/prebuilt-boot/README.md)。

---

## 仓库结构

| 路径 | 内容 |
|---|---|
| `device/huawei/gaokun3/` | 设备树 |
| `patches/` | 未进上游的内核、Mesa 与 AOSP 补丁 |
| `scripts/` | 构建、发布、部署、取证、安装工具（`scripts/live/` 构建图形安装器用的 Linux） |
| `live/installer-flutter/` | 图形安装器（Flutter） |
| `tools/` | 调试与点亮工具（指纹、相机等） |
| `docs/` | **工程案卷。** 每一条结论都带证据。从索引 [`docs/README.md`](docs/README.md) 找起 |
| `manifests/` | `repo` local manifest |

`docs/` 不是附属品。这个平台的任何信息都不存在于任何 wiki、也不在任何模型的
训练数据里，所以那些 findings 文件本身就是主要产出：它们记录了测到了什么、
哪些判断后来被证明是错的、哪些早先的结论被推翻了。**被推翻的有好几条。**

---

## 招人 / Help wanted

完整的待办 —— 每条都写了具体的第一步，搁置的也写了理由 —— 在
[`docs/TODO.md`](docs/TODO.md)。下面是挑出来、值得花一个周末的几条，大致由易到难：

1. **GPU SMMU 中断修复。** SMMU 拉的是 SPI 675/680，设备树声明的是 678/679，
   所以 context fault 永远到不了 CPU。改 DTB 应该就能彻底丢掉
   `smmu-nostall.sh` 那个轮询 workaround（[`docs/stage5-freedreno.md`](docs/stage5-freedreno.md) D6）。
2. **手掌误触。** 手掌压屏会碎成多个触点；三种按阈值的办法都实测过、全都不行，
   下一步是在触摸驱动里做跨帧的形态判据。驱动已经上报触点面积；触摸 IDC 文件还没写。
   [#116](docs/stage4-findings.md#116)、[`scripts/touch/README.md`](scripts/touch/README.md)。
3. **硬件视频【编码】。** 故意关着（见状态表）。要让它工作，得教 `v4l2_codec2` 的
   编码组件把 RGBX 转成 NV12 —— `device/huawei/gaokun3/device.mk` 里的注释列出了
   复测时要改的三处。
4. **SELinux 转 enforcing。** 已经做了七轮，enforcing 试跑能用；剩下的是 enforcing
   下的真实待机、enforcing → enforcing 的 OTA、enforcing 下的全新安装（[#129](docs/stage4-findings.md#129)）。
5. **环境光传感器。** 一激活 SLPI 的 sensor_process 就整个崩溃；Windows 用的是同一套
   配置，所以差别在 DSP 自己写的注册表里（[#121](docs/stage4-findings.md#121)）。
   加速度计与陀螺仪那一套已经跑通 —— 据我们所知是 SC8280XP 上头一次，ThinkPad X13s
   也没有 —— 协议整理在 [`docs/sensors-ssc-protocol.md`](docs/sensors-ssc-protocol.md)，
   想在自己机器上做可以直接拿。
6. **相机画质。** 剩下的是画质，不是链路：没有色彩矫正矩阵（要色卡）、软件 ISP
   自己没有降噪、闪光片高光过曝。具体下一步见 [`docs/TODO.md`](docs/TODO.md) T3。

恢复出厂和 `fastboot` 现在走统一启动入口（设计 [`docs/boot-entry-design.md`](docs/boot-entry-design.md)，
代码 [`tools/gk3boot/`](tools/gk3boot/README.md)）。它在 1.0 的开发构建里、还没进任何发布版，
经它恢复出厂也还没在真机上测过 —— 想碰 recovery 的话，请先来聊一下。

**如果你手上有 MateBook E Go 想帮忙测**，下面这些从没在实机上试过：蓝牙耳机、
USB-C 接外接显示器、手机连本机热点、v0.7.1 针对 WPA2/WPA3 混合模式路由器的改动、
图形安装器在真盘上的双系统与清空整盘、Windows 脚本在真 Windows 上跑。欢迎开 issue
—— **报告哪里坏了和交补丁一样有用**。请附上你的 BIOS 版本和 SKU。

---

## 社区

| | |
|---|---|
| **Telegram** | [t.me/gaokunAndroid](https://t.me/gaokunAndroid) |
| **QQ 群** | **920133252** |
| Issues | [GitHub issues](https://github.com/vahiru/gaokun-android/issues) —— 需要留痕的事走这里 |

群里适合问"这样是不是正常的"；**只要能复现，就开个 issue**，
否则聊天记录一刷就没了。

---

## 致谢

* **gaokun Linux 社区** ——
  [linux-gaokun](https://github.com/right-0903/linux-gaokun)、
  [linux-gaokun-buildbot](https://github.com/KawaiiHachimi/linux-gaokun-buildbot)、
  [EGoTouchRev](https://github.com/chiyuki0325/EGoTouchRev-Linux)
  —— 内核、EC 驱动、触摸逆向，这个移植是站在他们肩上的。
* **[aospm](https://github.com/aospm)**，让人看到"AOSP 跑在主线内核上"这条路
  本身是走得通的。
* **Johan Hovold** 以及所有把 SC8280XP 支持推上主线的人。
* **crDroid** 与 **LineageOS**。
* **Mesa** —— `freedreno` 和 `turnip`。
* **@mashen11** —— 麦克风、扬声器增强、相机自动对焦。

## 许可

GNU General Public License v3.0 或更新版本，见 [`LICENSE`](LICENSE) 与
[`NOTICE`](NOTICE)。少数从 AOSP 改造而来的文件保留它们自己的 Apache-2.0 头
（Apache-2.0 单向兼容 GPL-3，可以并入）。[`patches/`](patches/) 下的内核补丁
**保持 GPL-2.0-only** —— 它们是 Linux 的衍生作品，不能改许可；Mesa 补丁沿用上游的 MIT。
