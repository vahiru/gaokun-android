<!--
  维护说明（不显示）：
  * 与docs/INSTALL.md内容对应，改一份就改另一份。
  * 安装器界面上的文字以live/installer-flutter/lib/l10n/app_zh.arb为准；开机菜单条目只有英文，原样保留。
  * 安装器的风险提示页（riskExpBody）引用了本文"在真机上测过什么"一节，改标题时要一并改app_zh.arb和app_en.arb。
  * scripts/release.sh会把本文件上传到R2的relnotes/INSTALL.md，系统更新里"无法更新（版本不受支持）"的说明链接指向那里。
-->
# 安装

[**English → INSTALL.md**](INSTALL.md) · [常见问题](FAQ.zh-CN.md) · [已知限制](known-limitations.zh-CN.md)

本文介绍如何在华为MateBook E Go上安装本系统，以及安装之后的系统更新、开机菜单、救援系统和清除数据。

> ### ⚠️ 安装前须知
> 除双系统模式外，安装会清空内置硬盘，Windows和其中的文件将一并删除，且无法撤销。本项目不提供出厂系统的镜像，之后如需恢复Windows，请使用华为的恢复介质或Windows安装U盘重新安装。
>
> 请在开始前备份重要数据，并确保自己有能力处理机器无法开机的情况。

## 在真机上测过什么

安装器的各项功能都在测试盘上验证过。各条安装路径在真实机器上的使用情况如下：

| 安装路径 | 实测情况 |
|---|---|
| 重新安装Android，保留用户数据 | 已在实机上完成，系统镜像分别取自安装介质和联网下载 |
| 重新安装Android，清除数据 | 仅在测试盘上验证 |
| 清除整个磁盘 | 仅在测试盘上验证 |
| 保留现有系统（双系统） | 已有用户在实机上完成安装 |
| 手动调整磁盘 | 仅在测试盘上验证 |
| 从U盘启动安装器 | 已有用户在实机上完成安装 |
| 从Windows启动安装器 | 已有用户在实机上完成安装 |
| 命令行安装器 | 仅在测试盘上验证 |

"测试盘"是在电脑上模拟的一块磁盘，安装器每一步写入的结果都会被检查。它能发现很多问题，但无法完全代替真实的硬盘和固件。如果你走过表中尚未实测的路径，无论成功与否，都欢迎[提交测试报告](https://github.com/vahiru/gaokun-android/issues/new/choose)。

## 准备工作

| 项目 | 要求 |
|---|---|
| 机型 | 华为MateBook E Go，2022款和2023款均可。安装器会检查型号，两款在系统信息中的型号都是GK-W7X |
| BIOS版本 | 不限 |
| Secure Boot | 必须关闭。系统内核未经签名，安装器检测到Secure Boot开启时会停止 |
| 安装介质 | 一个4 GB或更大的U盘。机器上还有Windows时，也可以不用U盘，直接从Windows启动安装器 |
| 网络 | 安装介质中没有附带系统镜像时，安装器需要通过Wi-Fi下载，约1.3 GB |
| 键盘盖 | 图形安装器可以用手指操作，并带有屏幕键盘；使用命令行安装器、在开机菜单中选择条目时需要键盘盖 |

不需要另一台电脑，也不需要adb或本仓库的代码。系统运行所需的固件已经包含在发布的镜像中。

另外请注意，本系统的用户数据分区没有加密，拿到机器的人可以读取其中的全部数据，详见[已知限制](known-limitations.zh-CN.md)。

### 如果机器上有Windows

开始安装之前，请在Windows中依次完成以下几步：

1. **关闭快速启动**：控制面板 → 电源选项 → 选择电源按钮的功能，取消勾选"启用快速启动"。之后请用"关机"而不是"休眠"退出Windows。处于休眠状态的Windows分区无法安全地调整大小，安装器检测到时会拒绝操作。
2. **处理BitLocker**：如果启用了BitLocker或设备加密，请先确认能取得恢复密钥（<https://aka.ms/myrecoverykey>），再暂停BitLocker。关闭Secure Boot会使Windows要求输入恢复密钥，因此这一步必须在关闭Secure Boot之前完成。
3. **关闭Secure Boot**：设置 → 系统 → 恢复 → 高级启动 → 立即重新启动，然后选择疑难解答 → 高级选项 → UEFI固件设置，在固件设置中关闭Secure Boot。

从Windows启动安装器时，脚本会按上述顺序引导你完成前两步。在本机上暂停BitLocker后能否顺利重新启用，尚未经实测。

## 下载

| 文件 | 用途 | 下载位置 |
|---|---|---|
| `gaokun3-installer-<版本>-usb.img.xz` | 图形安装器的U盘镜像，附带系统镜像，无需联网即可安装 | [v1.0.0-rc.2发布页](https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.2) |
| `gaokun3-installer-<版本>-windows.zip` | 从Windows启动图形安装器，安装时联网下载最新版系统 | 同上 |
| `boot.img`、`super.img.zst`、`install-artifacts.sha256` | 系统镜像，仅在命令行安装和离线安装时需要 | 各版本的发布页，目前最新为[v1.0.0-rc.3](https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.3) |
| `rescue.squashfs`、`initramfs.img` | 救援系统，命令行安装时可选 | [v1.0.0-rc.2发布页](https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.2) |

目前的图形安装器版本是0.2.0-rc.2。v1.0.0-rc.3没有重新制作安装器，U盘镜像附带的是rc.2系统，安装完成后请在设置中更新到rc.3。各文件的校验值见同一发布页上的`gaokun3-installer-<版本>-SHA256SUMS`和`install-artifacts.sha256`。

### 从镜像站下载

GitHub下载缓慢或无法连接时（例如在中国大陆），可以从镜像站下载：

- 图形安装器：`https://ota.072172.xyz/installer/<安装器版本>/<文件名>`
- 系统镜像：`https://ota.072172.xyz/install/<build>/<文件名>`

`<build>`是该版本OTA包去掉`.zip`后的文件名，当前版本的文件名可以在<https://ota.072172.xyz/ota/gaokun3.json>的`filename`字段中查到。以v1.0.0-rc.3为例：

```
https://ota.072172.xyz/installer/0.2.0-rc.2/gaokun3-installer-0.2.0-rc.2-usb.img.xz
https://ota.072172.xyz/install/crDroidAndroid-17.0-20261030-gaokun3-v13.0/boot.img
https://ota.072172.xyz/install/crDroidAndroid-17.0-20261030-gaokun3-v13.0/super.img.zst
https://ota.072172.xyz/install/crDroidAndroid-17.0-20261030-gaokun3-v13.0/install-artifacts.sha256
```

## 使用图形安装器

### 第一步：启动安装器

#### 方式一：从Windows启动（无需U盘）

1. 解压`gaokun3-installer-<版本>-windows.zip`，双击其中的`gaokun3-setup.cmd`，它会请求管理员权限。脚本会检查机型、启动模式和Secure Boot，在改动硬盘之前要求输入`YES`确认。
2. 脚本会从D盘缩出安装器所需的少量空间（约0.5–2 GB），把安装器放进去，并设置为仅下一次开机进入安装器。给Android的空间稍后在安装器中划分。如果快速启动仍然开着，脚本会提出替你关闭；如果D盘启用了BitLocker或设备加密，由于安装器无法缩小加密的分区，脚本会询问是否现在就为Android腾出空间。
3. 重启后进入安装器。先选择"缩小现有分区腾出空间"并选中D盘，再选择"保留现有系统"；如果第2步已经为Android腾出了空间，直接选择"保留现有系统"即可。安装器缩小D盘之后，Windows下次启动时会先检查一次磁盘，这是正常现象。

如需离线安装，请在第1步之前，把`boot.img`、`super.img.zst`和`install-artifacts.sha256`放进`gaokun3-setup.cmd`旁边名为`payload`的文件夹，安装器会直接使用这些文件，不再联网下载。

如果在安装之前改变了主意，运行`gaokun3-setup.cmd -Uninstall`即可删除安装器的分区和启动项，并把D盘恢复原状。如果重启后直接回到了Windows、没有进入安装器，请加上`-UseFallbackPath`参数再运行一次脚本。

#### 方式二：从U盘启动

把`gaokun3-installer-<版本>-usb.img.xz`写入U盘。balenaEtcher和Rufus可以直接读取`.xz`文件；在Linux上可以运行：

```sh
xz -dc gaokun3-installer-<版本>-usb.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

插上U盘，开机时在固件的启动菜单中选择它。

### 第二步：选择安装方式

| 安装方式 | 说明 |
|---|---|
| 保留现有系统（双系统） | 只在空闲空间中新建分区，不改动现有分区；在EFI分区中放入启动器，以后每次开机显示系统选择菜单。空闲空间不够时，可以先用"缩小现有分区腾出空间"从NTFS或ext4分区中腾出空间 |
| 清除整个磁盘 | 删除硬盘上的全部内容，按[分区布局](#分区布局)重新分区 |
| 重新安装Android | 硬盘上已有Android时可用。保留现有分区，重新写入系统，适合修复系统或更换版本。默认清除用户数据，可以在下一步选择"保留用户数据"；换到更旧的版本时，保留的数据可能导致无法开机 |
| 手动调整磁盘 | 删除、新建、格式化、缩小或扩大分区，每一步单独确认并立即执行 |

开始写入硬盘之前，安装器会显示一页风险提示，需要读到底、等待一段时间并勾选确认后才能继续；清除整个磁盘，或在装有Windows的硬盘上手动调整分区时，还需要输入`ERASE`。如果Windows分区启用了BitLocker，并且这次安装需要替换EFI分区上的默认启动程序，还需要先确认已经取得恢复密钥。

安装双系统时，确认页上可以选择开机默认进入哪个系统，默认为Android，详见[双系统](#双系统)。

如果安装介质附带救援系统，可以勾选"同时安装救援系统"，详见[救援系统](#救援系统)。

### 安装失败时

请使用安装器的"保存日志"功能，把日志保存到另一个FAT或exFAT格式的U盘上。也可以保存到安装介质本身，但它所在的EFI分区在Windows和macOS上默认不可见。报告问题时请附上整个日志文件夹。

## 使用命令行安装器（进阶）

命令行安装器只能清除整个磁盘后安装，与图形安装器的"清除整个磁盘"使用同一套安装流程。请在图形安装器的终端中运行它：

1. 用U盘启动图形安装器。如需下载系统镜像，先在安装器中连接Wi-Fi。
2. 在安装器第一页点击"退出到终端"，或按Ctrl+Alt+F2（Ctrl+Alt+F1可以切回），以`root`登录，无需密码。
3. 下载系统镜像并运行安装器：

   ```sh
   mkdir -p /tmp/rel && cd /tmp/rel
   B=crDroidAndroid-17.0-20261030-gaokun3-v13.0      # 要安装的版本，见"下载"一节
   for f in boot.img super.img.zst install-artifacts.sha256; do
       curl -fLO "https://ota.072172.xyz/install/$B/$f" || echo "FAILED: $f"
   done
   /usr/share/gaokun3/install-gaokun3.sh /tmp/rel
   ```

下载的文件不需要解压。安装器会先检查机器，列出将被删除的分区和将要创建的分区布局，等你输入`ERASE`确认之后才会写入硬盘。默认的目标硬盘是`/dev/nvme0n1`，可以用`DISK=`指定其他硬盘。如需同时安装救援系统，请把`rescue.squashfs`和`initramfs.img`放进同一目录。

> 这一流程尚未在实机上运行过。

也可以在其他arm64 Linux中运行本仓库的`scripts/install-gaokun3.sh`，但目前还没有通用的Linux发行版被证实能在本机上启动。

### 分区布局

清除整个磁盘后，硬盘上的分区如下：

| 分区 | 大小 | 用途 |
|---|---|---|
| `esp` | 300 MiB | 开机菜单和各槽位的内核 |
| `misc` | 4 MiB | 记录A/B槽位的状态 |
| `metadata` | 32 MiB | Android的元数据 |
| `super` | 12 GiB | 系统分区，A/B两份 |
| `boot_a`、`boot_b` | 各64 MiB | 内核和启动镜像，A/B两份 |
| `gk3rescue` | 1 GiB | 救援系统（可选） |
| `userdata` | 剩余全部空间 | 用户数据 |

## 首次开机

首次开机大约需要两到三分钟，之后按开机向导完成设置即可，不需要运行额外的脚本。

### 此设备未经Play保护机制认证

本系统不在Google的认证设备名单中，Play商店会提示"此设备未经Play保护机制认证"，部分应用在登记之前会拒绝安装。登记是免费的，每台设备只需要做一次：

1. 在平板上登录Google账号。
2. 读取设备的Android ID。目前需要在电脑上通过adb运行本仓库中的脚本：

   ```sh
   bash scripts/google/gsf-android-id.sh
   ```

   这个ID存放在Google Play服务的私有目录中，读取需要root权限。请先安装ReSukiSU管理器，为Shell授予root权限，脚本会自动使用它。adb的设置方法见[常见问题](FAQ.zh-CN.md)。
3. 用同一个Google账号打开<https://www.google.com/android/uncertified/>，填入这个ID并登记。
4. 等待几分钟，然后清除Google Play商店的存储数据（也可以运行`adb shell pm clear com.android.vending`）。

恢复出厂设置或清除Google Play服务的数据之后，ID会改变，需要重新登记。

登记之后，Play Integrity仍然无法通过。它是部分银行和支付应用使用的更严格的检测，要求锁定的bootloader和Google签名的系统，而本机必须关闭Secure Boot才能运行本系统。依赖Play Integrity的应用在本系统上无法使用。

## 系统更新

在"设置 → 系统 → 系统更新"中检查并安装更新，内核也随系统一起更新。更新会写入另一个槽位，写入期间可以照常使用，重启后生效。网络不畅时，也可以通过其他途径下载OTA包，放到平板的内部存储中，在同一界面中使用"安装本地更新"功能。

如果更新后的系统反复无法完成启动，开机时会自动退回上一版本，并在进入系统后通知你。如果系统卡住而没有自动重启，请长按电源键强制关机后再开机。

> **更新后无法退回上一版本。**新版本首次成功启动后，上一版本的系统即被合并覆盖。开机菜单中仍会列出另一个槽位的条目，但选择它只会白白失败一次开机。大版本更新（例如从Android 16到17）还会在首次启动时升级用户数据，旧版本即使能启动，也可能无法使用这些数据。如需退回旧版本，只能用图形安装器重新安装，并且很可能需要清除数据。更新前请备份重要数据。

## 开机菜单

每次开机时会显示开机菜单，默认停留5秒，不做选择则进入默认系统。可以用机身音量上/下键选择、电源键确认，或连接键盘盖用方向键和回车操作。菜单中主要有以下几项：

| 条目 | 说明 |
|---|---|
| `Android` | 默认启动的Android |
| `Android fastboot / boot menu` | 进入fastboot模式，见[fastboot](#fastboot) |
| `gaokun3 rescue` | 救援系统，见[救援系统](#救援系统) |
| `Windows Boot Manager` | 双系统时出现，进入Windows |

菜单中还有按槽位列出的Android条目和上一版启动器的条目，用于排查问题，平时不需要选择。

### 救援系统

本机没有可用的Android recovery，救援系统承担了它的作用。救援系统是一个精简的Linux系统，与图形安装器是同一套程序，可以重新安装Android，也提供终端和SSH。它存放在单独的1 GiB分区中，运行期间的改动在重启后不会保留。

安装时勾选"同时安装救援系统"即可一并安装。从内置硬盘上的安装器安装时，这一项默认不勾选，因为那份安装器本身就在开机菜单中，同样可以用来重新安装。

Android卡死时，系统不会自动进入救援系统。请长按电源键强制关机，再次开机后在开机菜单中选择`gaokun3 rescue`。在救援系统中直接重启，即可回到Android。

如需通过SSH远程登录救援系统，请在安装之前把自己的SSH公钥存为安装器U盘上的`gaokun3/authorized_keys`，安装器会把它连同已连接的Wi-Fi一起复制到救援分区。发布的镜像中不含任何人的公钥，没有放入公钥时，救援系统只能在机器前使用。

### fastboot

在开机菜单中选择`Android fastboot / boot menu`，或在电脑上运行`adb reboot bootloader`，即可进入fastboot模式。屏幕上会显示`FASTBOOT MODE`、当前槽位和USB连接状态。

- 数据线需要插在**靠近电源键**的USB-C口，另一个口不支持fastboot。
- 在装有Android SDK Platform Tools的电脑上，可以使用`fastboot devices`、`fastboot getvar all`、`fastboot oem device-info`（查看硬盘、分区、A/B和启动入口的状态）、`fastboot reboot`和`fastboot reboot bootloader`。
- 在平板上可以用音量键移动选项，用电源键确认。

刷写功能（`fastboot flash`）尚未在实机上测试，用电源键确认选项也尚未经实测。需要重新写入系统时，请使用图形安装器。

### 双系统

双系统时，开机默认进入的系统可以在安装器的确认页上选择，之后可以在Android的"设置 → 系统 → 启动选项"中修改。如果选择默认进入Windows，安装完成后的前两次开机仍会进入Android，以便完成首次设置。Android的"启动选项"中还可以直接重启到Windows。这些功能尚未经过系统的测试。

重装Windows时建立的100 MiB EFI分区容量不足，安装器不支持这种分区布局。

从Windows启动安装器时，脚本最后还会安装一组Windows伴随工具，位于开始菜单的gaokun3文件夹中，可以重启到Android、设置默认启动的系统、修复被Windows更新覆盖的启动项，或卸载Android。从U盘安装的用户，可以在Windows中运行U盘上的`gaokun3-windows\gaokun3-setup.cmd`来安装它。伴随工具目前是预览版，各项功能尚未经过系统的测试。

## 清除数据（恢复出厂设置）

设置中的"清除所有数据（恢复出厂设置）"会在重启后由fastboot模式执行，但这一流程尚未在实机上测试，请暂时不要依赖它。

需要清除数据时（包括出售或送修之前），请启动图形安装器（开机菜单中的`gaokun3 rescue`，或安装器U盘），选择"重新安装Android"，并且不要勾选"保留用户数据"。

## 无法开机时

| 现象 | 处理方法 |
|---|---|
| 开机后黑屏，没有出现开机菜单 | 确认Secure Boot已经关闭 |
| 更新后选择了另一个槽位的条目，机器随即重启 | 正常现象，旧版本已被合并覆盖，见[系统更新](#系统更新) |
| 固件的启动菜单里看不到U盘，或华为官方恢复无法启动 | 早期版本会把内核崩溃记录写进固件存储，存储被占满后会出现这种情况，主板并没有损坏。从1.0.0-rc.3起不再写入，已有的记录需要按[issue #56](https://github.com/vahiru/gaokun-android/issues/56)手动清理一次 |
| Android无法启动 | 进入救援系统，用图形安装器重新安装Android。可以先尝试保留用户数据 |

## 报告问题

请通过[issue表单](https://github.com/vahiru/gaokun-android/issues/new/choose)提交问题，提交前请先阅读[issues.md](../issues.md)。日志比文字描述更有帮助：Android能够启动时，最好附上`adb bugreport`生成的压缩包。其中包含Wi-Fi名称、账号名、已安装的应用等个人信息，公开上传之前请先检查。各类日志的获取方法见[常见问题](FAQ.zh-CN.md)。
