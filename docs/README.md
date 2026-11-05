# 文档索引 · Documentation Index

**Using the ROM?** Start here — these are the pages written for end users, in English and Chinese:

| | English | 中文 |
|---|---|---|
| Install / update / remove | [INSTALL.md](INSTALL.md) | [INSTALL.zh-CN.md](INSTALL.zh-CN.md) |
| Questions & troubleshooting | [FAQ.md](FAQ.md) | [FAQ.zh-CN.md](FAQ.zh-CN.md) |
| Known limitations, unsupported features | [known-limitations.md](known-limitations.md) | [known-limitations.zh-CN.md](known-limitations.zh-CN.md) |
| Release notes | [relnotes/](relnotes/) (latest released: [v1.0.0-rc.3](relnotes/v1.0.0-rc.3.md)) | 同左（已发布的各版为英文；1.0.0正式版起另附`.zh-CN.md`） |

项目总览与硬件支持状态表位于仓库根目录的[README.md](../README.md)与[README.zh-CN.md](../README.zh-CN.md)。

---

## 一、用户与构建者文档（`docs/`顶层）

| 文档 | 内容说明 |
|---|---|
| [INSTALL.md](INSTALL.md) / [INSTALL.zh-CN.md](INSTALL.zh-CN.md) | 图形安装器、命令行安装、免U盘双系统安装、OTA更新、ADB调试、救援系统与fastboot使用指南 |
| [FAQ.md](FAQ.md) / [FAQ.zh-CN.md](FAQ.zh-CN.md) | 常见问题与故障排查：启动菜单、版本回退、抓取日志、恢复出厂设置与卸载 |
| [known-limitations.md](known-limitations.md) / [known-limitations.zh-CN.md](known-limitations.zh-CN.md) | 当前版本的已知限制、硬件不支持项、安全模型与专有固件说明 |
| [BUILDING.zh-CN.md](BUILDING.zh-CN.md) | 从源码编译ROM与主线Linux内核：拉取crDroid树、准备专有固件、发布构建与开发构建的区别 |
| [TODO.md](TODO.md) | 待办事项、待实机核对的改动与后续功能开发清单 |
| [relnotes/](relnotes/) | 历次发布的版本说明、模板（[TEMPLATE.md](relnotes/TEMPLATE.md)）及GPL源码对应清单（[v0.7.1-alpha-sources.md](relnotes/v0.7.1-alpha-sources.md)） |

---

## 二、开发者技术文档（`docs/dev/`）

面向内核、驱动与Android系统开发者的架构说明、硬件参考及协议规范统一放在[`dev/`](dev/)目录下：

### 1. 平台与子系统架构

| 文档 | 内容说明 |
|---|---|
| [dev/hardware.md](dev/hardware.md) | 硬件清单与平台基线：SoC/外设拓扑、BIOS 2.16引脚警告、DRM/DSI显示、USB控制器分工、专有固件清单与`efi_pstore`崩溃日志机制 |
| [dev/android-bringup.md](dev/android-bringup.md) | Android系统移植总览：UEFI双阶段启动链路、必选内核Kconfig项、温控/电源/导航栏HAL、Wi-Fi 6E与蓝牙集成、SELinux策略与内置`ReSukiSU` |
| [dev/gpu-turnip.md](dev/gpu-turnip.md) | Adreno 690 GPU加速架构：Turnip + ANGLE + DPU图形栈、`VkNativeBufferANDROID`延迟绑定修复、GPU SMMU失速死锁处理与P010零拷贝合成 |
| [dev/mesa-a17.md](dev/mesa-a17.md) | Android 17上的Mesa 26.1 Turnip构建管线（`meson2hermetic`与`scripts/mesa-a17/`） |
| [dev/audio-video.md](dev/audio-video.md) | 音频与硬件视频解码：Audioreach/SoundWire拓扑、扬声器增益与防爆音、有线耳机插拔检测、内置麦克风录音、低延迟音频播放及`qcom-iris`视频硬解 |
| [dev/camera.md](dev/camera.md) | 相机子系统：前后摄（Hi-846 / OV13B10）与闪光灯供电拓扑、`titan_top_gdsc`电源域死锁修复、`libcamera`软件ISP标定及自研AIDL Camera HAL |

### 2. 专题设计与协议参考

| 文档 | 内容说明 |
|---|---|
| [dev/display-color.md](dev/display-color.md) | 显示色彩管理：ACPI `DLUT`出厂3D LUT校准提取、BOE/CSOT双面板识别、sRGB与Display P3切换、10-bit抖动与HDR提亮 |
| [dev/stylus.md](dev/stylus.md) | 手写笔（M-Pencil）：Himax SPI与蓝牙双通路、压感/倾斜上报、双击与侧键守护进程 |
| [dev/palm-rejection.md](dev/palm-rejection.md) | 触摸屏防误触：驱动层手掌位置图算法、`MT_TOOL_PALM`撤回机制与调参接口 |
| [dev/deep-sleep.md](dev/deep-sleep.md) | 深度待机（S3 / s2idle）：`qcom_stats`睡眠计数、阻断`cx`/`ddr`下电的投票排查、PCIe电源与Wi-Fi唤醒恢复 |
| [dev/gaming.md](dev/gaming.md) | 游戏兼容性与性能调优：39位虚拟地址空间窗口、着色器缓存、触控采样预设与游戏模式脚本 |
| [dev/sensors-ssc-protocol.md](dev/sensors-ssc-protocol.md) | SLPI传感器中枢（SSC）QMI over QRTR与Protobuf协议规格、`hexagonrpcd`文件服务及`ambient_light`流类型约束 |
| [dev/fingerprint-driver-design.md](dev/fingerprint-driver-design.md) | 侧边指纹（FocalTech `FTE7001`）TrustZone QSEECOM TA加载机制与客户端驱动/HAL架构设计 |
| [dev/installer-rust-design.md](dev/installer-rust-design.md) | 安装器Rust后端（`tools/gk3-installer/`）设计：零静默失败执行模型、分区表安全判据与Shell对拍框架 |
| [dev/release-checklist.md](dev/release-checklist.md) | 候选版与正式版发布回归测试清单（A档自动化 / B档交互 / C档长稳 / D档产物核对） |
| [dev/hw/README.md](dev/hw/README.md) | 硬件原始转储：后摄模组EEPROM镜像与[BIOS 2.16解包工具及清单](dev/hw/bios-2.16/README.md) |
| [dev/fingerprint/README.md](dev/fingerprint/README.md) | 指纹Windows侧专有二进制本地逆向报告目录说明 |
| [dev/upstream/README.md](dev/upstream/README.md) | 待提交至Linux主线的5份内核补丁草稿（`camcc-sc8280xp` / `ov13b10` / `camss`） |

---

## 三、各代码目录内的模块说明

各工具、脚本与HAL子目录自带的`README.md`说明该模块的使用与测试方式：

- 启动入口与Fastboot：[`tools/gk3boot/README.md`](../tools/gk3boot/README.md)、[`device/huawei/gaokun3/prebuilt-gk3boot/README.md`](../device/huawei/gaokun3/prebuilt-gk3boot/README.md)
- LiveCD与图形安装器：[`scripts/live/README.md`](../scripts/live/README.md)、[`live/installer-flutter/README.md`](../live/installer-flutter/README.md)、[`scripts/windows/README.md`](../scripts/windows/README.md)
- 设备树HAL与音频后处理：[`camera/README.md`](../device/huawei/gaokun3/camera/README.md)、[`sensors-hal/README.md`](../device/huawei/gaokun3/sensors-hal/README.md)、[`ssc/README.md`](../device/huawei/gaokun3/ssc/README.md)、[`micarray/README.md`](../device/huawei/gaokun3/micarray/README.md)、[`denoise/README.md`](../device/huawei/gaokun3/denoise/README.md)、[`speakerfx/README.md`](../device/huawei/gaokun3/speakerfx/README.md)、[`effects/README.md`](../device/huawei/gaokun3/effects/README.md)
- 构建输入准备：[`firmware/README.md`](../device/huawei/gaokun3/firmware/README.md)、[`hexagonrpcd-root/README.md`](../device/huawei/gaokun3/hexagonrpcd-root/README.md)、[`prebuilt-boot/README.md`](../device/huawei/gaokun3/prebuilt-boot/README.md)
- 调试与性能脚本：[`scripts/touch/README.md`](../scripts/touch/README.md)、[`scripts/perf/README.md`](../scripts/perf/README.md)、[`scripts/selinux/README.md`](../scripts/selinux/README.md)

检查仓库内所有Markdown相对链接与锚点：

```sh
python3 -I scripts/check-doc-links.py
```
