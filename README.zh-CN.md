# Gaokun Android

适用于华为MateBook E Go（gaokun3）的非官方crDroid 13移植。

[**English → README.md**](README.md)

<p align="center">
  <a href="https://t.me/gaokunAndroid"><img alt="Telegram频道：gaokunAndroid" src="https://img.shields.io/badge/Telegram-gaokunAndroid-26A5E4?logo=telegram&logoColor=white"></a>
  <a href="https://qm.qq.com/q/3VIV2KY6o0"><img alt="QQ群：920133252" src="https://img.shields.io/badge/QQ%E7%BE%A4-920133252-12B7F5?logo=qq&logoColor=white"></a>
  <a href="https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.3"><img alt="最新版本：1.0.0-rc.3" src="https://img.shields.io/badge/Latest-1.0.0--rc.3-yellow"></a>
</p>

## 现状

> ### ⚠️ 安装前须知
> 安装本系统可能导致**丢失硬盘上的全部数据**，请在安装前确保有能力处理无法开机的情况并**备份重要数据**。
>
> 更多已知问题详见[已知限制](#已知限制)；常见问题（抓日志、开机菜单、卖机前清数据）见[FAQ](docs/FAQ.zh-CN.md)。

以下列出各主要功能在当前版本的工作状况，请阅读[各部分的详细文档](docs/)了解详情。

- ✅：功能正常
- ⚠️：功能可用，但存在已知问题
- ❌：不可用

| 项目 | 状态 | 说明 |
|---|:--:|---|
| 启动与系统更新 | ✅ | 支持A/B无缝系统更新 |
| 恢复出厂设置 | ⚠️ | 还未上机验证，如要清空数据请用图形安装器重装 |
| 屏幕 | ✅ | 120 Hz，使用出厂校准的色彩，支持广色域和HDR |
| GPU | ✅ | 支持硬件加速与Vulkan |
| 触摸屏 | ✅ | 可在设置里切换日常与游戏模式，日常模式下能消除书写等场景下的手掌误触 |
| 手写笔（M-Pencil） | ⚠️ | 支持二代和三代M-Pencil，实现了压感、悬停、倾斜和侧边双击功能，笔迹平滑强度可调节，三代M-Pencil尚未实测日常使用效果 |
| 磁吸键盘和触控板 | ✅ | |
| Wi-Fi | ✅ | |
| Wi-Fi热点 | ⚠️ | 可以开启；还未实际测试使用其他设备经热点上网 |
| 蓝牙 | ⚠️ | 通话时无法使用蓝牙耳机的麦克风 |
| 扬声器 | ✅ | 可在设置里开启低音增强或华为Histen音效 |
| 麦克风 | ✅ | 支持波束成形，有可按场景开关的回声消除和降噪 |
| 耳机 | ⚠️ | 有线耳机上的麦克风还未实测 |
| USB音频 | ⚠️ | USB耳机、USB声卡尚未实测 |
| 电池与充电 | ⚠️ | 由于基础功率较高，在连接输出功率较低的电源使用时可能出现掉电，并在电量低时在没有警告的情况下强制关机 |
| 待机 | ✅ | 待机功耗约0.5W，满电待机续航约三天 |
| 消息推送 | ⚠️ | 休眠时，FCM可唤醒系统并及时推送消息；微信、QQ等不使用FCM的应用的消息要在系统下次唤醒时才能收到，通常延迟一两分钟 |
| 传感器 | ✅ | 加速度计及环境光传感器正常工作，支持自动旋转和自动亮度，无磁力计硬件 |
| 游戏 | ⚠️ | 原神最高画质运行流畅，明日方舟、三角洲行动等均可运行；英雄联盟手游可以启动，**游玩使用ACE等反作弊框架的游戏可能导致封号** |
| 视频播放 | ✅ | 支持硬件解码，包括10-bit和HDR视频 |
| 视频编码 | ❌ | 因编码组件的适配尚未完成，硬件编码暂时不可用，录屏和录像使用软件编码 |
| 摄像头 | ⚠️ | 使用软件ISP。前后摄可以拍照和录像，后摄支持自动对焦；曝光补偿暂不支持，暗处噪点较多，录制720p@30fps以上规格的视频可能导致相机App崩溃 |
| USB-C | ⚠️ | 支持与电脑传输文件、共享网络，可以连接U盘和外接显示器；偶尔开机后USB-C口无法正常工作，重启后恢复，原因尚在排查 |
| 指纹 | ❌ | 驱动和系统服务尚在开发，暂时不可用 |
| 定位 | ❌ | 无GPS硬件，卫星定位无法使用 |
| TPM | ❌ | 不支持 |
| 视频版权保护（DRM） | ❌ | 系统未集成Widevine，Netflix等付费流媒体的内容无法播放 |
| Root | ⚠️ | 系统内置KernelSU，部分检测root的应用可能拒绝运行 |
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

从[**Releases**](https://github.com/vahiru/gaokun-android/releases)取最新版，详细安装操作请见[`docs/INSTALL.zh-CN.md`](docs/INSTALL.zh-CN.md)（英文版[`docs/INSTALL.md`](docs/INSTALL.md)）。安装前**必须关闭Secure Boot**。

目前有两种安装器可供选择：

| | 图形安装器（预览版） | 命令行安装器 |
|---|---|---|
| 支持的安装方式 | 与Windows共存（双系统）；清空整块硬盘后安装；保留分区重新安装Android（默认清除数据，可选保留）；手动调整分区 | 清空整块硬盘后安装 |
| 运行环境 | 自带的Linux系统，可从U盘启动，也可在Windows中运行`gaokun3-setup.cmd`启动，无需U盘 | arm64 Linux Live U盘，另需下载本仓库的代码；通用的Ubuntu/Debian镜像能否在本机启动，未经验证 |
| 系统镜像来源 | 随安装介质附带，或联网下载 | 从发布页手动下载`super.img.zst`、`boot.img`和`install-artifacts.sha256` |
| 获取方式 | 在[v1.0.0-rc.2发布页](https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.2)下载`gaokun3-installer-0.2.0-rc.2-*`文件（v1.0.0-rc.3未重新制作安装器，U盘镜像内置的是rc.2系统，安装完成后请在设置中更新到rc.3） | 本仓库中的`scripts/install-gaokun3.sh` |
| 实机测试情况 | "清空整块硬盘后安装"尚未经过实机测试；安装成功后仍然可能发生报错 | 与图形安装器共用同一套安装流程；下文的分区布局目前仅在测试盘上创建过 |

清空全盘安装的默认分区布局：

| 分区 | 大小 | 用途 |
|---|---|---|
| `esp` | 300 MiB | 开机菜单和各槽位的内核 |
| `misc` | 4 MiB | 记录A/B槽位的状态 |
| `metadata` | 32 MiB | Android的元数据 |
| `super` | 12 GiB | 系统分区，A/B两份 |
| `boot_a`、`boot_b` | 各64 MiB | 内核和启动镜像，A/B两份 |
| `gk3rescue` | 1 GiB | 救援系统（可选） |
| `userdata` | 剩余全部空间 | 用户数据 |

**救援系统：**本机没有可用的Android recovery，救援系统是出问题时的修复手段。它是一个精简的Linux系统，与图形安装器是同一套程序，可以重新安装Android，存放在单独的1 GiB分区中。图形安装器可以一并安装它（安装时勾选"同时安装救援系统"选项）；命令行安装器只在提供了救援系统镜像时才会安装。

每次开机时会显示15秒的启动菜单，可以从中进入救援系统；Android卡死时，系统不会自动进入救援系统。需要时请长按电源键强制关机，再次开机后在菜单中选择救援系统；选择菜单项需要连接键盘盖。如需通过网络（SSH）登录救援系统，请在安装前把自己的SSH公钥放到安装器U盘上，具体做法见[安装说明](docs/INSTALL.zh-CN.md#救援系统)。

**中国境内下载：**GitHub的下载服务器慢或者连不上时，系统镜像（`boot.img`、`super.img.zst`）可从镜像站：`https://ota.072172.xyz/install/<build>/<文件名>`下载，`<build>`即该版OTA包去掉`.zip`的文件名，详见[`docs/INSTALL.zh-CN.md`](docs/INSTALL.zh-CN.md#下载)。

**更新：**在"设置 → 系统 → 系统更新"界面进行OTA更新，OTA更新包会从上述镜像站下载。网络不畅时也可通过其他途径下载OTA包，在此界面使用"安装本地更新"功能安装放置在平板内部存储空间中的OTA包。

## 已知限制

下表列出使用中需要留意的限制。完整说明见[`docs/known-limitations.zh-CN.md`](docs/known-limitations.zh-CN.md)。

| 项目 | 说明 | 建议 |
|---|---|---|
| 数据加密 | 用户数据分区未加密，他人拿到机器即可读取全部数据，锁屏密码无法阻止 | 不要存放敏感资料；出售、送修前用图形安装器重装并清除数据 |
| Root | 内核内置KernelSU，无法关闭；未安装管理器时处于休眠状态。部分检测root的应用可能拒绝运行；在管理器中开启adb root后，已授权USB调试的电脑通过adb即可获得root权限 | 不需要时不要开启adb root；只授权信任的电脑，不使用时关闭USB调试和无线调试 |
| 系统签名 | 系统和更新包使用Android公开的测试密钥签名，任何人都能制作本机会接受的更新包和系统应用 | 只通过设置中的系统更新或本项目的发布页更新，不要安装来源不明的"系统组件" |
| 启动安全 | 必须关闭Secure Boot，启动链未上锁，系统分区不做完整性校验 | 这是在本机运行Android的前提，无法规避 |
| 恢复出厂设置 | 设置中的"清除所有数据"未经实测，请勿依赖 | 用图形安装器的"重新安装Android"清除数据 |
| 版本回退 | 新版本首次成功启动后，上一版即被合并覆盖，无法回退；大版本更新还会升级用户数据 | 更新前备份重要数据 |
| Google认证 | 本机未通过Play保护机制认证，需手动登记后Play商店才能正常使用；Play Integrity无法通过，依赖它的应用（如Google钱包、部分银行应用）无法使用 | 按[安装说明](docs/INSTALL.zh-CN.md#此设备未经play保护机制认证)登记一次 |
| 国内使用 | 系统只附带Google应用，未预装国内应用商店；无法访问Google时，Play商店、Google服务和网络定位均无法使用 | 从应用官网或应用商店网页版下载安装包 |
| 内核版本号 | 为通过部分应用的环境检测，应用读取到的内核版本号是伪装的 | 从下一个版本起，可在"设置 → 系统 → 内核版本伪装"中关闭或修改 |
| 音频与蓝牙 | 有用户报告长时间运行后声音和蓝牙同时失效，原因尚在排查 | 重启即可恢复；反馈时请附上`/data/vendor/gaokun3/hangdump-*`（需要root权限读取） |
| 连接电脑 | 连接部分电脑时，平板可能反过来为电脑供电，双方都无法识别对方 | 重新插拔一次，或换用电脑上的另一个接口 |
| USB-C接口 | 插拔设备后（多见于待机之后），USB-C口可能无法正常工作，直到重启 | 重启；在此之前通过Wi-Fi使用adb和传输文件 |
| 低功率电源 | 连接电脑USB口或小功率充电器时，可能显示"正在充电"而电量仍在下降，电量耗尽时会直接断电 | 使用功率足够的充电器，留意电量百分比，及时保存工作 |

## 参与贡献

欢迎通过以下方式参与：

- **测试与反馈**：[现状](#现状)中标注"未经实测"的功能，如果你测试过，无论结果如何，都欢迎[提交issue](https://github.com/vahiru/gaokun-android/issues/new/choose)告诉我们。报告问题前请先阅读[issues.md](issues.md)。
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
* **@dezige131**：图形安装器的修复。
* 帮忙测试的各位用户。

## 许可

本项目以GPL-3.0或更新版本授权，见[`LICENSE`](LICENSE)。部分文件沿用其来源项目的许可，例如内核补丁为GPL-2.0-only，详见[`NOTICE`](NOTICE)。
