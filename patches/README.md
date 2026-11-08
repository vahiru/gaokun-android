# 补丁目录说明（`patches/`）

本目录按**目标源码树与子系统**分级存放所有树外补丁。每个叶子子目录使用独立的本地四位序号（`0001..NNNN`），新增补丁只需在对应子目录末尾顺延编号并在消费脚本中登记，无需跨子系统抢占全局序号。

## 目录结构与消费者

| 目录 | 目标源码树 / 子系统 | 消费脚本 / 工具 | 说明 |
|---|---|---|---|
| [`kernel/core/`](kernel/core/) | Linux内核（核心、架构、Android基础支撑、通用驱动） | [`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)（`KPATCHES`） | `efi-pstore`、`bpffs`、`ashmem`、`xt_quota2`、39位地址窗口、SELinux netlink、内核版本上报、FastRPC等 |
| [`kernel/pcie-pm/`](kernel/pcie-pm/) | Linux内核（PCIe、NVMe、cpuidle、系统休眠与唤醒） | [`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)（`KPATCHES`） | SC8280XP PCIe L2/D3深度待机、并行恢复、链路重试、NVMe死盘panic、PCIe `WAKE#`与唤醒中断管理 |
| [`kernel/wifi/`](kernel/wifi/) | Linux内核（`ath11k`、`mac80211`、`bus/mhi`） | [`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)（`KPATCHES`） | MHI固件跨断电保留、WoWLAN挂起/恢复、固件崩溃恢复与退避、扫描与唤醒保持、RX BA会话重建 |
| [`kernel/ec-usb/`](kernel/ec-usb/) | Linux内核（华为高鲲EC、电池、UCSI、USB Gadget、xHCI） | [`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)（`KPATCHES`） | EC盖子状态与挂起中断、电池状态位与温度解码、UCSI端口/DP HPD/方向/重试、`android_usb`、configfs UDC等待 |
| [`kernel/drm/`](kernel/drm/) | Linux内核（DRM核心、MSM DPU、DSI PHY、Himax面板、MSM GPU） | [`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)（`KPATCHES`） | 双DSI绑定PLL初始化、面板硬件ID引脚识别（CSOT/BOE）、DPU PCC色域转换与出厂3D LUT校准、8 bpc抖动、10-bit平面、线性背光、GPU上下文地址空间延迟创建的空指针与竞争修正（回移）、GPU队列优先级换算修正与抢占记录挂起重置（回移）、A690 GPU抢占 |
| [`kernel/iris/`](kernel/iris/) | Linux内核（`media/iris`视频硬件解码） | [`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)（`KPATCHES`） | `qcom-iris`断电中断并发保护、Venus兼容`G_FMT`/flush/seek/EOS行为、看门狗恢复、裁剪保持、10-bit P010解码 |
| [`kernel/camera/`](kernel/camera/) | Linux内核（`camcc`、`camss`、`ov13b10`、`hi846`） | [`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)（`KPATCHES`） | `camcc-sc8280xp` GDSC等待值与共享RCG、`camss`缺席传感器容错、后摄OV13B10板级上电序列、前后摄帧长与曝光上限更新 |
| [`kernel/audio/`](kernel/audio/) | Linux内核（ASoC `sc8280xp`、LPASS macro、WSA883x、SoundWire、Q6APM） | [`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)（`KPATCHES`） | LPASS编解码器PM clock运行时电源管理、SoundWire末端口分配、WSA883x动态DRE与增益上限、VA启动消咔哒与高通恢复、四路麦采集 |
| [`kernel/input/`](kernel/input/) | Linux内核（`himax-spi`触摸屏、HX83121A手写笔从片、`usbhid`） | [`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)（`KPATCHES`） | 触摸跳点检测、SPI读重试、连通域与掌压/手部位置图、点击锁定与一欧滤波、边缘补偿、级联手写笔从片驱动、键盘/笔HID恢复`SET_IDLE` |
| [`kernel/dts/`](kernel/dts/) | Linux内核（`sc8280xp.dtsi`与`sc8280xp-huawei-gaokun3.dts`等设备树） | [`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)（`KPATCHES`） | 温控cooling-maps、USB0 OTG、前后摄与闪光灯/隐私灯、启用`iris`/`videocc`、CMA低于4 GiB、手写笔从片、常开稳压器、DMIC降频、CDSP `dma-coherent`等 |
| [`kernel/fingerprint/`](kernel/fingerprint/) | Linux内核（QSEECOM与指纹传输驱动） | [`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)（`KPATCHES`） | QSEECOM TA加载/卸载与listener服务、`/dev/gaokun3_fpcmd`受限指纹传输驱动（FocalTech FTE7001与Goodix GDIX5125，`CONFIG_GAOKUN3_FPCMD`默认关闭） |
| [`android/hardware-interfaces/`](android/hardware-interfaces/) | AOSP `hardware/interfaces` | [`scripts/crdroid-tree-fixes.py`](../scripts/crdroid-tree-fixes.py) | Audio AIDL primary HAL外设连接、麦克风地址、数据驱动采集、ALSA硬件定拍播放、低延迟块与period数、播放/采集厂商钩子、APEX命名空间 |
| [`android/drm-hwcomposer/`](android/drm-hwcomposer/) | AOSP `external/drm_hwcomposer` | [`scripts/crdroid-tree-fixes.py`](../scripts/crdroid-tree-fixes.py) | 10-bit客户端目标帧缓冲、内置屏sRGB/Display P3切换、压暗图层走GPU、内置屏HDR10/HLG声明 |
| [`android/v4l2-codec2/`](android/v4l2-codec2/) | AOSP `external/v4l2_codec2` | [`scripts/crdroid-tree-fixes.py`](../scripts/crdroid-tree-fixes.py) | HEVC CSD合并入首帧、字节缓冲模式使用dmabuf ID、HDR静态元数据传递 |
| [`android/tinyalsa/`](android/tinyalsa/) | AOSP `external/tinyalsa_new` | [`scripts/crdroid-tree-fixes.py`](../scripts/crdroid-tree-fixes.py) | `sw_params`从refined `hw_params`推导 |
| [`android/frameworks-base/`](android/frameworks-base/) | AOSP `frameworks/base` | 候选/按需应用 | SystemUI指纹锁定状态处理 |
| [`mesa/`](mesa/) | AOSP `external/mesa3d`（Turnip Vulkan驱动） | [`scripts/mesa-a17/regen.sh`](../scripts/mesa-a17/regen.sh) | Android Native Buffer空内存重绑定容错、`VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`（P010）支持、Mesa 26.1上游构建与依赖回移（`0003..0011`）、A6xx抢占（`0012`上游VSC修正，`0013`） |
| [`libcamera/`](libcamera/) | 上游`libcamera`（替换`external/libcamera`） | [`scripts/camera/setup-libcamera-tree.sh`](../scripts/camera/setup-libcamera-tree.sh) | 详见[`libcamera/README.md`](libcamera/README.md) |
| [`resukisu/`](resukisu/) | 内核树内`KernelSU/`子模块 | [`scripts/kernel-setup-resukisu.sh`](../scripts/kernel-setup-resukisu.sh) | Linux 7.x内核API适配与内置路径调整 |

## 编号与维护规范

1. **子目录内局部编号**：每个子目录内部从`0001`起连续编号，文件名格式为`NNNN-<subsystem-or-topic>.patch`。
2. **头部说明与溯源标签（Git Trailers）**：所有补丁采用标准英文mbox头部格式（`From:` / `Subject: [PATCH] ...` / `---`），并在`---`分隔线前附带以下两个Trailer字段（遵循DEP-3与Yocto/OpenEmbedded规范）：
   - `Origin:`：`local`（本仓库编写）、`backport, <commit / MR / URL>`（自上游主线、维护者队列、MR或ACK回移）或`other, <URL>`（移植或参考自第三方社区树）。
   - `Upstream-Status:`：
     - `Backport [<ref>]`：上游已合入或在审的回移补丁，基线升级包含后即可移除。
     - `Pending`：通用的驱动修复、DTS节点或功能补丁，具备向上游投递的价值。
     - `Inappropriate [<reason>]`：仅限本地维护的补丁（如`[Android-specific]`、`[firmware workaround]`、`[device-specific policy]`等），不向上游主线投递。
3. **内核补丁正交性**：[`kernel/`](kernel/)下各子系统目录修改的内核源文件互不重叠；同一子系统内存在上下文依赖的补丁按本地序号先后排列，由[`scripts/kernel-apply-patches.sh`](../scripts/kernel-apply-patches.sh)的`KPATCHES`数组显式定义应用顺序。
4. **验证命令**：
   - 核对内核树与配方逐字节一致：`bash scripts/kernel-apply-patches.sh <内核树路径> --verify`
   - 核对文档链接完整性：`python3 -I scripts/check-doc-links.py`
