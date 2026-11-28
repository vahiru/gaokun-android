# Gaokun Android

适用于华为MateBook E Go（gaokun3）的非官方crDroid 13移植。

[**English → README.md**](README.md)

<p align="center">
  <a href="https://t.me/gaokunAndroid"><img alt="Telegram频道：gaokunAndroid" src="https://img.shields.io/badge/Telegram-gaokunAndroid-26A5E4?logo=telegram&logoColor=white"></a>
  <a href="https://qm.qq.com/q/3VIV2KY6o0"><img alt="QQ群：920133252" src="https://img.shields.io/badge/QQ%E7%BE%A4-920133252-12B7F5?logo=qq&logoColor=white"></a>
  <a href="https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.5"><img alt="最新版本：1.0.0-rc.5" src="https://img.shields.io/badge/Latest-1.0.0--rc.5-yellow"></a>
</p>

## 现状

> ### ⚠️ 安装前须知
> 安装本系统可能导致**丢失硬盘上的全部数据**，请在安装前确保有能力处理无法开机的情况并**备份重要数据**。
>
> 更多已知问题详见[已知限制](#已知限制)；常见问题（获取日志、开机菜单、出售或送修前清除数据）见[常见问题](docs/FAQ.zh-CN.md)。

以下列出各主要功能在当前版本的工作状况，请阅读[各部分的详细文档](docs/)了解详情。

- ✅：功能正常
- ⚠️：功能可用，但存在已知问题
- ❌：不可用

| 项目 | 状态 | 说明 |
|---|:--:|---|
| 启动与系统更新 | ✅ | 支持A/B无缝系统更新 |
| 恢复出厂设置 | ⚠️ | 还未上机验证，如要清空数据请用Live安装器重装 |
| 屏幕 | ✅ | 120 Hz，使用出厂校准的色彩，支持广色域、HDR与实验性平行视界 |
| GPU | ✅ | 支持硬件加速与Vulkan |
| 触摸屏 | ✅ | 可在“设置 → 显示”中开启防误触模式，减少使用手写笔书写等场景下的手掌误触 |
| 手写笔（M-Pencil） | ✅ | 兼容二代和三代M-Pencil，支持压感、悬停、倾斜和侧边双击功能，笔迹平滑强度可调节，支持4096级压感 |
| 磁吸键盘和触控板 | ✅ | |
| Wi-Fi | ✅ | |
| Wi-Fi热点 | ✅ | |
| WLAN直连 | ✅ | |
| 蓝牙 | ✅ | |
| 扬声器 | ✅ | 可在设置中开启低音增强或华为Histen音效 |
| 麦克风 | ✅ | 支持波束成形，有可按场景开关的回声消除和降噪 |
| 耳机 | ⚠️ | 有线耳机上的麦克风还未实测 |
| 电池与充电 | ✅ | |
| 待机 | ✅ | 待机功耗约0.5W，满电待机续航约三天 |
| 消息推送 | ⚠️ | 休眠时，FCM可唤醒系统并及时推送消息；微信、QQ等不使用FCM的应用的消息要在系统下次唤醒时才能收到，通常延迟一两分钟 |
| 传感器 | ✅ | 加速度计、陀螺仪与环境光传感器正常工作，支持自动旋转和自动亮度，无磁力计硬件 |
| 游戏 | ⚠️ | 原神最高画质运行流畅，明日方舟、三角洲行动等均可运行；英雄联盟手游可以启动；支持游戏手柄。**游玩使用ACE等反作弊框架的游戏可能导致封号** |
| 温控和CPU调度 | ✅ | |
| 视频播放 | ✅ | 支持H.264、HEVC和VP9硬件解码 |
| 视频编码 | ✅ | 支持H.264与HEVC硬件编码 |
| 摄像头 | ✅ | |
| USB-C | ⚠️ | 目前仅左侧USB-C接口支持device role，右侧接口始终保持host模式 |
| 指纹 | ⚠️ | 支持录入、锁屏解锁和应用内验证（兼容Goodix与FocalTech传感器）；微信支付、支付宝等应用的指纹支付无法使用 |
| 定位 | ❌ | 无GPS硬件 |
| TPM | ❌ | 不支持 |
| 视频版权保护（DRM） | ❌ | 系统未集成Widevine，Netflix等付费流媒体的内容无法播放 |
| Root | ⚠️ | 内核内置ReSukiSU |
| 中文输入法 | ✅ | 预装fcitx5 |

## 硬件

| 组件 | 规格 |
|---|---|
| SoC | 高通骁龙8cx Gen 3（SC8280XP） |
| 型号 | HUAWEI GK-W7X |
| GPU | Adreno 690 |
| 屏幕 | PPC357DB1-4（由CSOT或BOE供应），驱动芯片Himax HX83121A，1600×2560，120 Hz |
| Wi-Fi / 蓝牙 | WCN6855 |
| 存储 | NVMe |

## 安装

从[**Releases**](https://github.com/vahiru/gaokun-android/releases)页面下载最新版，详细安装操作请见[`docs/INSTALL.zh-CN.md`](docs/INSTALL.zh-CN.md)（英文版[`docs/INSTALL.md`](docs/INSTALL.md)）。安装前**必须关闭Secure Boot**。

目前有两种安装器可供选择：

| | Windows安装器 | Live安装器 |
|---|---|---|
| 支持的操作 | 安装到系统磁盘的未分配空间（与Windows双系统）；卸载Android；制作独立安装介质（Live安装器） | 保留现有系统（双系统）；清除整个磁盘；重新安装Android（默认清除数据，可选保留）；删除分区；删除安装器分区 |
| 运行方式 | 在机器上的Windows中运行 | 从U盘或本机磁盘上的安装器分区启动 |
| 系统安装文件 | 联网下载最新版本，或使用已下载的本地文件 | 随安装介质附带，安装时不联网 |
| 获取方式 | [Releases](https://github.com/vahiru/gaokun-android/releases)页面中的`Windows installer`发布 | [Releases](https://github.com/vahiru/gaokun-android/releases)页面中的`Live installer`发布 |
| 最新版本 | [Windows安装器0.2.1](https://github.com/vahiru/gaokun-android/releases/tag/windows-installer-v0.2.1) | [Live安装器0.2.0（v1.0.0-rc.5）](https://github.com/vahiru/gaokun-android/releases/tag/live-installer-v0.2.0-for-v1.0.0-rc.5) |

清除整个磁盘安装的默认分区布局：

| 分区 | 大小 | 用途 |
|---|---|---|
| `esp` | 300 MiB | 开机菜单和各槽位的内核 |
| `misc` | 4 MiB | 记录A/B槽位的状态 |
| `metadata` | 32 MiB | Android的元数据 |
| `boot_a`、`boot_b` | 各64 MiB | 内核和启动镜像，A/B两份 |
| `super` | 12 GiB | 系统分区，A/B两份 |
| `userdata` | 剩余全部空间 | 用户数据 |

**修复：**本机没有可用的Android recovery。Android无法启动时，请用Live安装器重新安装Android，可以选择保留用户数据。

**中国境内下载：**GitHub的下载服务器慢或者连不上时，系统镜像（`boot.img`、`super.img.zst`）可从镜像站：`https://ota.072172.xyz/install/<build>/<文件名>`下载，`<build>`即该版OTA包去掉`.zip`的文件名，详见[`docs/INSTALL.zh-CN.md`](docs/INSTALL.zh-CN.md#下载)。使用Windows安装器时，可以直接把下载源切换为“R2镜像源”。

**更新：**在“设置 → 系统 → 系统更新”界面进行OTA更新，OTA更新包会从上述镜像站下载。网络不畅时也可通过其他途径下载OTA包，在此界面使用“安装本地更新”功能安装放置在平板内部存储空间中的OTA包。

## 已知限制

下表列出使用中需要留意的限制。完整说明见[`docs/known-limitations.zh-CN.md`](docs/known-limitations.zh-CN.md)。

| 项目 | 说明 | 建议 |
|---|---|---|
| 数据加密 | `/data`未加密，他人拿到机器即可读取全部数据 | 不要存放敏感资料 |
| Root | 内核内置ReSukiSU，安装管理器后部分检测root的应用可能拒绝运行 | 请自行权衡 |
| 启动安全 | 必须关闭Secure Boot | 这是在本机运行第三方操作系统的前提，无法规避 |
| Google认证 | 本机未通过Play保护机制认证，需手动登记后Play商店才能正常使用 | 按[安装说明](docs/INSTALL.zh-CN.md#此设备未经play保护机制认证)登记一次 |
| 国内使用 | 系统只附带Google应用，未预装国内应用商店 | 请使用酷安等第三方应用商店 |

## 参与贡献

欢迎通过以下方式参与：

- **测试与反馈**：[现状](#现状)中标注“还未实测”或“还未上机验证”的功能，如果你测试过，无论结果如何，都欢迎[提交issue](https://github.com/vahiru/gaokun-android/issues/new/choose)告诉我们。报告问题前请先阅读[issues.md](issues.md)。
- **参与开发**：待办事项和每项的下一步见[`docs/TODO.md`](docs/TODO.md)。

提交issue或PR之前，请先阅读[CONTRIBUTING.md](CONTRIBUTING.md)。

## 社区

| | |
|---|---|
| **Telegram** | [t.me/gaokunAndroid](https://t.me/gaokunAndroid) |
| **QQ群** | **920133252** |
| Issues | [GitHub issues](https://github.com/vahiru/gaokun-android/issues) |

## 致谢

本项目建立在许多人的工作之上，在此致谢：

* **gaokun Linux社区**：[linux-gaokun](https://github.com/right-0903/linux-gaokun)、[linux-gaokun-buildbot](https://github.com/KawaiiHachimi/linux-gaokun-buildbot)、[EGoTouchRev](https://github.com/chiyuki0325/EGoTouchRev-Linux)、[matebook-e-go-linux](https://github.com/whitelewi1-ctrl/matebook-e-go-linux)、[matebook-e-go-boot-works](https://github.com/matalama80td3l/matebook-e-go-boot-works)，提供了内核、EC驱动、面板驱动和触摸逆向等基础。
* **[uup-drivers-sc8280xp](https://github.com/matebook-e-go/uup-drivers-sc8280xp)**：整理了华为的Windows驱动包，本项目使用的固件、相机调校和麦克风阵列参数均取自这里。
* **[aospm](https://github.com/aospm)**：证明了在主线内核上运行AOSP的可行性。
* **Johan Hovold**及所有为SC8280XP主线支持做出贡献的开发者。
* **crDroid**与**LineageOS**。
* **Mesa**的`freedreno`和`turnip`驱动。
* **@mashen11**：麦克风、扬声器增强和相机自动对焦。
* **@Rivarise**：CSOT屏手写笔的调校与测试。
* **@dezige131**：相机硬件ISP像素通路的最初实现，以及图形安装器的修复。
* 帮忙测试的各位用户。

## 许可

本项目以GPL-3.0或更新版本授权，见[`LICENSE`](LICENSE)。部分文件沿用其来源项目的许可，例如内核补丁为GPL-2.0-only，详见[`NOTICE`](NOTICE)。
