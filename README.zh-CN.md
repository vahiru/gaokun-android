# 华为 MateBook E Go 跑 Android（SC8280XP / `gaokun3`）

**crDroid 16.0（Android 16），跑在主线 Linux 内核上，Adreno 690 硬件 Vulkan。**

高通从来没给 8cx 系列发过 Android BSP，只有 Windows 和 Linux 驱动。所以这里
没有可以扒 vendor blob 的原厂 ROM，没有 `fastboot`，没有 A/B 槽位，没有
recovery 分区，也没有串口。这不是一次常规移植 —— 它是 **AOSP on mainline**，
每一个 HAL 都建在上游驱动之上。

> ### ⚠️ Alpha 阶段，先读这段
> 游戏跑得不错，而且从 v0.3.0-alpha 起**待机也好了**。
> **但仍然没有可用的 recovery** —— 见[已知问题](#已知问题)。
> 安装会**清空内置硬盘**。你需要有能力救一台开不了机的机器。不提供任何担保。

[**English → README.md**](README.md)

**交流：**[Telegram](https://t.me/gaokunAndroid) · QQ 群 **920133252**

---

## 现状

下面每一条都是实机测出来的，不是推断。证据在 [`docs/`](docs/) 里。

| 项目 | 状态 | 说明 |
|---|:--:|---|
| 引导（UEFI + systemd-boot，内置盘） | ✅ | 不需要 U 盘 |
| 屏幕 1600×2560 @ 120 Hz | ✅ | 框架默认值把渲染钉在 60，已覆盖；实测 vsync 周期 8.33 ms |
| GPU —— Adreno 690 硬件 Vulkan | ✅ | Mesa 26.0.3 `turnip`；22 分钟浸泡零 SMMU fault |
| 触摸屏 | ✅ | Himax HX83121A；需要 `patches/` 里的 gpio174 补丁 |
| 磁吸键盘 + 触控板 | ✅ | USB HID `12d1:10b8` |
| Wi-Fi | ✅ | ath11k / WCN6855 |
| 蓝牙 | ⚠️ | 可用 —— `hci_qca`，adapter `ON`，开机后零崩溃。**但长期运行后可能与音频一起死锁**，见 [#38](docs/stage4-findings.md) |
| 扬声器 | ⚠️ | 可用 —— 用户实机确认出声，WSA883x 走 audioreach。**但长期运行后音频可能死锁**，与蓝牙一起，见 [#38](docs/stage4-findings.md) |
| 耳机口 / 麦克风 | ✅ | **已修复，用户实机确认出声。** 三处阻塞，没有一处玄学：真凶是 rx-macro 内部的插值器链从来没接上（输入 mux 与解调 mux 都停在复位值），DAPM 路径不完整 → 后端拒绝打开，而**内核一行日志都不打**；其次是音频策略里没声明耳机设备；最后是框架去看 `/sys/class/switch/h2w`，主线上根本没这个东西。⚠️ 顺带查出**内置麦克风此前完全是断的、而且谁都没发现**，同样已修（安静房间 RMS −30.5 dBFS）。[#40](docs/stage4-findings.md) |
| 电池、充电、合盖检测 | ✅ | 华为 EC 驱动 |
| **游戏** | ✅ | 原神画质极高流畅。GPU 空闲 270 MHz、峰值 690 MHz、最高 50 °C |
| CPU 温控降频 | ✅ | 主线 DTS **根本没有** CPU 的 cooling map —— 已由 [`patches/0009`](patches/) 在设备树里根治 |
| **待机 / 挂起** | ✅ | **2026-08-22 修复 —— 而且真凶是我们自己，不是内核。** 真实挂起/唤醒，零复位。⚠️ 代价一条：息屏时 USB adb 会断。[#52](docs/stage4-findings.md)、[#57](docs/stage4-findings.md) |
| 传感器（加速度计+陀螺仪）| ✅ | **自动旋转可用，用户实机确认方向正确。** 为本机写的 sensors HAL 已把真实读数喂给 SensorService，框架据此自动融合出 Game Rotation Vector / Gravity / Linear Acceleration。出厂安装矩阵全零（校准数据随 Windows 一起没了），但实测传感器坐标系与面板方向本来就一致，**不需要纠正**。本机**没有磁力计**（所以没有指南针）；光感一使能就会污染整个 DSP 会话，见 [#37](docs/stage4-findings.md) |
| 硬件视频解码 | ⚠️ | **内核这一半已通** —— `/dev/video0` / `/dev/video1` 就是 `qcom-venus-decoder` / `qcom-venus-encoder`，供应方全部解析、固件零报错，据我们所知是 SC8280XP 上的头一次。但 Android 还缺一个跟 V4L2 说话的 Codec2 组件，所以 66 个解码器仍全是软解。[#41](docs/stage4-findings.md) |
| 摄像头 | ❌ | 没开始 |
| USB-C 外接显示 / UCSI | ❌ | UCSI PPM 初始化超时，本机主线的已知缺陷 |
| 指纹、TPM | ❌ | 没有驱动 |
| SELinux | ⚠️ | `permissive` |

### 两件反直觉的事

**主线设备树里原先完全没有 CPU 温控。** `sc8280xp.dtsi` 里总共只有一处
`cooling-maps`，在 `gpu-thermal` 下面。每个 CPU 温区只有一条 110 °C 的
*critical* trip，别的什么都没有 —— CPU 会一路满频跑到内核紧急关机，
中间**没有任何渐进降频**。这台是被动散热的无风扇平板，长时间游戏真的撞得到。

[`patches/0009`](patches/) 在设备树里把它修好了：8 个 per-core 温区各加一条
85 °C 的 passive trip，绑到本簇的 cpufreq cooling device。同一台机器只换 DTB
的实测对比：每个温区绑定的 cooling device 从 0 变 1、trip 点从 1 变 2。
**这个缺口不是本机特有的** —— 任何跑主线的 sc8280xp 机器都值得看一眼。

**待机坏了整整一个阶段，而真凶是我们自己改的一行设备树。**
它看起来像内核或 EC 的缺陷：挂下去几秒后整机复位，而且**在 Ubuntu 上用同一棵
内核复现得一模一样** —— 这恰恰是你会拿来排除 Android 的那种证据。
它确实排除了 Android，也把我们指向了完全错误的一层。

真正的原因是 Stage 2 为了 USB adb 自己加的：我们把第二个 USB 控制器设成
`dr_mode = "otg"` + `usb-role-switch`，而上游就是普通的 `host`。本机的 UCSI
是坏的，没有任何东西会去指派 role，于是控制器停在 `device`、既没有 gadget
也没有 xhci 子设备。给这个"半初始化"状态断电 —— 系统挂起会，单纯解绑驱动也会
—— **整板复位，且不留任何日志**。

有两件事让它拖了这么久。**两道坎叠在一起**（第二道是 EC 的 `suspend_noirq`
超时，而它早就被我们带着的一个补丁修好了），所以每个单变量实验都返回"无效"。
而且**单次失败率约 93%** —— "改一条、试一次、炸了"对任何配置都是大概率事件，
有好几轮是在追噪声。Android 侧的修法是**睡下去之前把 role 切到 `host`、
亮屏时切回 `device`**，这也就是息屏时 USB adb 会断的原因。
[#52](docs/stage4-findings.md)、[#57](docs/stage4-findings.md)

---

## 硬件

| | |
|---|---|
| SoC | 高通骁龙 8cx Gen 3（SC8280XP） |
| 型号 | HUAWEI GK-W7X，2022 款，CSOT 面板 |
| **BIOS** | **2.16 —— 不要升到 2.17。** 两版的触摸 SPI 总线和 GPIO 编号完全不同，上游驱动是按 2.16 开发的 |
| GPU | Adreno 690 |
| 屏幕 | Himax HX83121A，MIPI-DSI，1600×2560 —— 与 Galaxy Tab S7 FE 同款面板 |
| Wi-Fi / 蓝牙 | WCN6855 |
| 存储 | NVMe |
| 固件 | UEFI，必须关闭 Secure Boot |

---

## 安装

从 [**Releases**](../../releases) 取最新版，按
[`docs/INSTALL.md`](docs/INSTALL.md) 操作。

安装会**清空内置硬盘**。它建出来的布局：

| 分区 | 大小 | 用途 |
|---|---|---|
| ESP | 300 MiB | systemd-boot、内核、ramdisk |
| `userdata` | 剩余全部 | `/data` |
| `super` | 12 GiB | system / system_ext / product / vendor |
| `metadata` | 32 MiB | |
| 救援系统 | 约 25 GiB | 一个完整的 Ubuntu，可 SSH |

最后那个分区是**故意留的**。这台机器没有 recovery 分区、没有串口，所以一个
普通的 Linux 安装**就是** recovery 环境。它是默认启动项 —— 于是 Android 挂死时，
拍一下电源键就回到一个能 SSH 进去远程修的系统，**人不用在机器旁边**。

---

## 构建

需要一台 Linux，约 16 GB 内存、400 GB 磁盘。

```sh
repo init -u https://github.com/crdroidandroid/android.git -b 16.0
# 把 manifests/local_manifest_gaokun3.xml 放进 .repo/local_manifests/
repo sync -c -j"$(nproc)"

python3 scripts/crdroid-tree-fixes.py <树路径>     # 为什么要改，脚本里写了
source build/envsetup.sh
lunch lineage_gaokun3-bp4a-userdebug
m
m superimage
```

华为专有固件**不在**本仓库里。获取方法见
[`device/huawei/gaokun3/firmware/README.md`](device/huawei/gaokun3/firmware/README.md)
—— 从你自己的机器上取。

内核单独构建，来自
[`linux-gaokun-buildbot`](https://github.com/KawaiiHachimi/linux-gaokun-buildbot)。
Android 相关的配置断言在
[`scripts/kernel-config-android.sh`](scripts/kernel-config-android.sh)，
额外补丁在 [`patches/`](patches/)。

---

## 仓库结构

| 路径 | 内容 |
|---|---|
| `device/huawei/gaokun3/` | 设备树 |
| `patches/` | 未进上游的内核与 Mesa 补丁 |
| `scripts/` | 构建、部署、取证、安装工具 |
| `docs/` | **工程案卷。** 每一条结论都带证据 |
| `manifests/` | `repo` local manifest |

`docs/` 不是附属品。这个平台的任何信息都不存在于任何 wiki、也不在任何模型的
训练数据里，所以那些 findings 文件本身就是主要产出：它们记录了测到了什么、
哪些判断后来被证明是错的、哪些早先的结论被推翻了。**被推翻的有好几条。**

---

## 已知问题

| 问题 | 位置 |
|---|---|
| **没有可用的 recovery。** 镜像能造能交付，但启动它会让机器进复位循环，所以启动项默认不创建。代价：没有 `adb sideload`、没有 `fastbootd`，设置里的"恢复出厂设置"大概不起作用（它是去请求 bootloader 进 recovery，而 systemd-boot 不读那个请求）| [#39](docs/stage4-findings.md) |
| **音频与蓝牙在长期运行后可能死锁** —— 用户实机报告，尚未复现定位。两者都走同一条到 DSP 的 QRTR/FastRPC 通路，而那条通路上我们已经实测到过会话级卡死 | [#38](docs/stage4-findings.md) |
| 使能环境光传感器不但不返回读数，还会污染整个 DSP 会话，所以没有自动亮度（#37）。加速度计与陀螺仪本身已经跑通并接进框架 | [`docs/stage4-findings.md`](docs/stage4-findings.md) |
| 拔插 USB 后 adb 不重枚举（#27）；现在**息屏时 USB adb 也会断** —— 那正是待机修复在把控制器切到 host。默认开着的 5555 端口 adb over TCP 不受影响 | [`docs/stage4-findings.md`](docs/stage4-findings.md) |
| GPU SMMU 拉的是 SPI 675/680，而 DT 声明 678/679 | [`docs/stage5-freedreno.md`](docs/stage5-freedreno.md) D6 |
| 热管理 HAL 是 AOSP mock，它的 SHUTDOWN 阈值只有 36 °C | [`docs/stage6-crdroid.md`](docs/stage6-crdroid.md) §M4 |

---

## 招人 / Help wanted

都是边界清楚的活，大致由易到难：

1. **硬件视频解码 —— Android 这一半。** 内核那半已经做完：`/dev/video0` 与
   `/dev/video1` 是能用的 Venus 解码器和编码器。缺的是一个跟 V4L2 说话的
   Codec2 组件，所以 66 个解码器仍全是软解。`external/v4l2_codec2` 本来就在
   manifest 里，三个前提也已在设备上核实：它要的 `IComponentStore/default`
   是空的、`media.c2.hal.selection` 已经是 `aidl`；⚠️ 而 poolmask 必须用 BLOB
   的 `0xfc0000`，**不是**它 README 里写的 `0xf50000` —— 那是 ION 的值，
   而本机内核没有 ION。
2. **GPU SMMU 中断修复。** SMMU 拉的是 SPI 675/680，设备树声明的是 678/679，
   所以 context fault 永远到不了 CPU。改 DTB 应该就能彻底丢掉
   `smmu-nostall.sh` 那个轮询 workaround。
3. **写一个真的热管理 HAL**，读 `/sys/class/thermal`。⚠️ **必须同时把 SHUTDOWN
   阈值改掉** —— AOSP mock 报的是 36 °C，`ThermalManagerService` 看到就会
   直接关机。
4. **SELinux 转 enforcing。** 有两个服务需要写策略。
5. **传感器 —— 剩 sepolicy 与一个内核开关。** 传感器本体已经做完：
   加速度计与陀螺仪经 SLPI DSP 进到为本机写的 HAL，**自动旋转可用**。
   据我们所知其他 SC8280XP 设备都没跑通过这一套，ThinkPad X13s 也没有 ——
   协议整理在 [`docs/sensors-ssc-protocol.md`](docs/sensors-ssc-protocol.md)，
   想在自己机器上做可以直接拿。这里剩两个尾巴：HAL 与 `hexagonrpcd`
   都还没写 sepolicy，现在靠 `permissive` 顶着。
   **环境光传感器是另一个更硬的问题**：使能后不但不返回读数，还会污染整个
   SSC 会话，所以没有自动亮度。
6. **摄像头。** 完全没碰。

如果你手上有 MateBook E Go 想帮忙测，欢迎开 issue —— **报告哪里坏了和交补丁
一样有用**。请附上你的 BIOS 版本和 SKU。

---

## 社区

| | |
|---|---|
| **Telegram** | [t.me/gaokunAndroid](https://t.me/gaokunAndroid) |
| **QQ 群** | **920133252** |
| Issues | [GitHub issues](../../issues) —— 需要留痕的事走这里 |

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

## 许可

GNU General Public License v3.0 或更新版本，见 [`LICENSE`](LICENSE) 与
[`NOTICE`](NOTICE)。少数从 AOSP 改造而来的文件保留它们自己的 Apache-2.0 头
（Apache-2.0 单向兼容 GPL-3，可以并入）；而 [`patches/`](patches/) 下的内核补丁
**保持 GPL-2.0-only** —— 它们是 Linux 的衍生作品，不能改许可。
`patches/` 下针对 Linux 内核的补丁按衍生作品适用 GPL-2.0-only；
Mesa 补丁沿用上游的 MIT。
