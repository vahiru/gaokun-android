# 给 AI agent 的 bug 报告指南

> 这份文档是写给 **AI agent** 的（[CONTRIBUTING.md](CONTRIBUTING.md)「报告问题」）。如果你是用户，把它交给你的 agent，
> 让它照着收集信息、整理报告；最后由你自己检查并提交。

你在帮一位用户给 **gaokun-android**（华为 MateBook E Go 上的 Android 16 / crDroid，跑在主线 Linux 内核上）提交 bug 报告。
报告要让维护者**不用再追问**就能定位问题。下面按顺序做。

## 规矩

* **只报事实。** 命令输出原样贴，不改写、不截掉看起来"无关"的部分。你对原因的判断可以写，但单独放在"其他补充"里、标明是推测。
* **不要在用户的机器上做有风险的事。** 收集信息只用下面列出的**只读**命令。不要：重启或关机（除非用户自己决定）、恢复出厂、
  改分区或 EFI 分区、写 `/dev/mem`、unbind / rebind 驱动、`setenforce`、改系统属性、装卸应用。这台机器没有串口，
  卡死了只能靠用户按电源键。
* **提交前让用户过目。** 不要替用户直接提交 issue；把整理好的内容给用户看，由用户确认后提交。
* **隐私**：见下面「第 5 步」。

## 第 0 步：先确认是不是已知问题

1. 读最新发版说明里的 *Known issues*：<https://github.com/vahiru/gaokun-android/releases>（候选版是 pre-release，也要看）。
2. 读 [docs/known-limitations.md](docs/known-limitations.md)（[中文](docs/known-limitations.zh-CN.md)）——长期限制和不支持的功能都在这里。
3. 在 <https://github.com/vahiru/gaokun-android/issues> 搜索现有 issue，**包括已关闭的**。
4. 已经有人报过：在那个 issue 下补充你的版本、复现情况和日志，不要另开新的。

## 第 1 步：连上设备

* 按 [FAQ「Setting up adb」](docs/FAQ.md#setting-up-adb)打开 USB 调试。**只有电源键旁边那个 USB-C 口能做 USB 调试**；
  息屏时 USB 调试会断，收集期间让屏幕亮着。也可以用无线调试（FAQ 同一节）。
* 需要 root 的命令用 `adb shell su -c '…'`。第一次要在 ReSukiSU 管理器里给 **Shell** 授权（让用户去点）。
* Android 起不来：跳到「第 4 步」里的"Android 起不来"。

## 第 2 步：版本与机器

```sh
adb shell 'getprop ro.build.date.utc; getprop ro.build.version.incremental; getprop ro.build.fingerprint; getprop ro.vendor.gaokun3.version; uname -a; getprop ro.boot.slot_suffix'
adb shell 'cat /sys/class/dmi/id/product_name /sys/class/dmi/id/product_sku /sys/class/dmi/id/bios_version'
```

* `ro.build.date.utc`（10 位数字）是**构建戳**，最能说明装的是哪一版；每份发版说明里都写着它。
* ⚠️ 1.0.0-rc.2 和 rc.3 上，`uname` 显示的 `6.6.30-android15-…` 是**伪装的版本号**（见 known-limitations 第一节）；之后的版本
  只有应用读到伪装的版本，`adb shell` 里的 `uname` 是真实的，但应用的崩溃记录里仍是伪装的版本。真实内核版本一律以 `adb shell su -c 'dmesg | head -1'` 为准；报告里两样都贴上。
* 问用户：这个版本是怎么装上去的（图形安装器的哪种模式 / 系统更新 / 别的），从哪个版本升上来的。

## 第 3 步：问清现象

* 现象与预期，各一两句。
* 复现步骤：尽量从冷启动开始编号写，应用名、设置项、命令原样写。
* 频率：每次 / 经常 / 一次 / 长时间运行后 / 待机唤醒后。
* 出问题的大概时间（设备时钟），方便在日志里定位。

## 第 4 步：收日志（按问题类型）

**所有问题都要：**

```sh
adb logcat -b all -d > logcat.txt          # 出问题后尽快抓
adb shell su -c dmesg > dmesg.txt          # 内核日志，要 root
```

**按类型再加：**

| 问题 | 再收什么 |
|---|---|
| 意外重启、死机后重启 | 抓崩溃时内核留下的记录：`adb exec-out "su -c 'tar -C /sys/fs -cf - pstore'" > pstore.tar`（2026-10-30 修复起 efi_pstore 默认关，一般是空的；旧版本抓完要按 FAQ 清掉 NVRAM 里的 `dump-type*`） |
| 应用崩溃 / 闪退 | `adb logcat -b crash -d > crash.txt`；`adb shell su -c 'ls -t /data/tombstones/'`，取最新的那个：`adb exec-out "su -c 'cat /data/tombstones/<文件名>'" > tombstone.txt` |
| 卡死、声音或蓝牙不动了 | 看门狗留的取证：`adb exec-out "su -c 'cd /data/vendor/gaokun3 && tar -cf - hangdump-*'" > hangdump.tar` |
| USB-C（插电脑不识别、口坏了、充电方向反了） | 在仓库里跑 `bash scripts/usb/ucsi-snapshot.sh <adb 序列号> "<插的是什么>"`，每换一种插法跑一次 |
| 游戏 / App 画面、性能 | `adb shell getprop ro.hardware.vulkan`（如果是 `pastel`，用的是软件渲染兜底，性能数字没意义）；游戏内画质设置 |
| 触摸 / 手写笔 | `adb shell getevent -pl > getevent.txt` |
| Android 起不来 | 按 [FAQ「Collecting logs when the machine won't boot Android」](docs/FAQ.md#collecting-logs-when-the-machine-wont-boot-android)：开机菜单选 `gaokun3 rescue`，诊断信息在 `/media/gk3/gaokun3/diag/`；**重启前**把 `/sys/fs/pstore/` 与 `/var/lib/systemd/pstore/` 拷到那个目录 |
| 安装失败 | 用 *Install / boot failure* 表单；写清安装方式、模式、系统镜像从哪来、安装器版本，以及屏幕上的报错原文 |

`adb bugreport` 能一次全收，但里面个人信息很多。优先用上面这些针对性的命令；用户坚持要附 bugreport 时，先做第 5 步。

## 第 5 步：隐私检查

公开上传前，把下面这些从附件和正文里删掉或打码，并告诉用户你删了什么：

* Wi-Fi 网络名（SSID）、密码、IP 地址以外的网络标识；
* 账号名、邮箱、手机号；
* 设备序列号（`ro.serialno`）、Android ID；
* 已安装应用列表里用户不想公开的部分；
* 照片、文件名里的个人信息。

不确定的，问用户。

## 第 6 步：选表单、填写

在 <https://github.com/vahiru/gaokun-android/issues/new/choose> 选：

| 表单 | 什么时候用 |
|---|---|
| 🐞 Bug report / 缺陷报告 | Android 能起来，但某个功能不对 |
| 🧨 Install / boot failure / 安装与开机失败 | 装不上、起不来、更新后起不来 |
| 🔬 Hardware feature report / 硬件功能报告 | 测了某个硬件功能（能用也报，"能用"同样有价值） |
| 🎮 Game & app compatibility / 游戏与 App 兼容 | 某个游戏或 App 的兼容性、画面、性能 |
| ✨ Feature request · 📚 Documentation correction · 💬 Other | 功能申请、文档纠错、其他 |

* *Release* 写装的发布 tag（例如 `v1.0.0-rc.2`），自己编的写"self-built"。
* *Build stamp* 一栏贴第 2 步的输出。
* 长日志作为**文件附件**上传，不要贴进正文；拍屏只能作补充，不能代替文字日志。
* 标题一句话说清"在什么情况下、什么东西、出了什么错"。

## 第 7 步：交给用户确认

把整理好的报告（标题、各栏内容、附件清单、删掉了哪些隐私信息）给用户看。用户确认后再提交。
