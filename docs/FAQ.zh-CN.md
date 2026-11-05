<!--
  维护说明（不显示）：
  * 按"遇到了什么问题 → 怎么办"组织。写不出可靠答案的问题，如实写"尚未整理"或"尚未经实测"，不凭印象写。
  * 与docs/FAQ.md内容对应，改一份就改另一份。
  * 设置里的菜单名以设备上显示的为准。
-->
# 常见问题

[**English → FAQ.md**](FAQ.md) · [安装](INSTALL.zh-CN.md) · [已知限制](known-limitations.zh-CN.md)

* [开机与开机菜单](#开机与开机菜单)
* [更新与回退](#更新与回退)
* [获取日志](#获取日志)
* [救援系统与SSH](#救援系统与ssh)
* [清除数据](#清除数据)
* [卸载Android、恢复Windows](#卸载android恢复windows)
* [开发者选项与adb](#开发者选项与adb)

## 开机与开机菜单

### 怎么进入固件设置、从U盘启动？

机器上还有Windows时，可以在Windows中进入固件设置，步骤见[安装说明](INSTALL.zh-CN.md#如果机器上有windows)。在本机开机时进入固件设置和启动菜单的按键，以及不连接键盘盖时的操作方法，目前尚未整理。如果你知道，欢迎在社区群或issue中告诉我们。

从U盘启动时，请先关闭Secure Boot，插上安装器U盘，再在固件的启动菜单中选择它。

### 固件的启动菜单里看不到U盘，华为官方恢复也无法启动

1.0.0-rc.3之前的所有版本，在内核每次崩溃时都会把日志写进主板固件的变量存储，并且不会清理。崩溃次数多了，存储就会被占满，固件无法再建立新的启动项：U盘、读卡器和华为官方恢复镜像都不会出现在启动菜单中，固件升级和Windows修改启动项也可能失败。键盘和鼠标仍然可以正常使用，因此不容易察觉。

**主板并没有损坏，不需要更换。**从1.0.0-rc.3起，系统不再写入这些记录，但已经写入的不会自动消失，需要按以下步骤清理一次。清理时固件需要整理存储，可能要等待一段时间，期间请不要强制关机。

**在Android中清理**（需要先[准备好adb](#准备adb)，并在ReSukiSU管理器中为Shell授予root权限）：

1. 查看是否有残留记录：

   ```sh
   adb shell "su -c 'mkdir -p /data/local/tmp/efivars; mount -t efivarfs none /data/local/tmp/efivars 2>/dev/null; ls /data/local/tmp/efivars | grep -c ^dump-type'"
   ```

   输出`0`说明没有残留，不需要清理。

2. 如果愿意把这些日志提供给我们，可以先保存一份：

   ```sh
   adb exec-out "su -c 'cd /data/local/tmp/efivars && tar -cf - dump-type*'" > efi-dumps.tar
   ```

3. 删除记录：

   ```sh
   adb shell "su -c 'cd /data/local/tmp/efivars && chattr -i dump-type* && rm -f dump-type*; ls | grep -c ^dump-type'"
   ```

   最后输出`0`即清理完成。

**在救援系统或其他Linux中清理**：运行`sudo bash scripts/pstore-ctl.sh clear`，或先对`/sys/firmware/efi/efivars/dump-type*`执行`chattr -i`再删除。只删除`/sys/fs/pstore/`中的文件，在本机上并不会真正删除这些记录。

**在Windows中清理**：报告者提供了一种方法，我们尚未在本机上验证：以管理员权限调用`SetFirmwareEnvironmentVariableEx`，GUID为`{CFC8FC79-BE2E-4DDC-97F0-9F98BFE298A0}`，名称为每个`dump-type0-*`，数据长度为0，属性为7（属性为0时会报错19）。

相关讨论见[issue #56](https://github.com/vahiru/gaokun-android/issues/56)。

### 开机菜单里的各项是什么？

只装了Android时，开机菜单停留15秒；双系统时停留5秒。不做选择则启动默认项。

| 菜单项 | 说明 |
|---|---|
| `Android` | 默认启动的Android。它会自动选择槽位，更新后的系统反复无法启动时自动退回上一版本 |
| `Android (previous loader)` | 上一版的启动程序，供`Android`一项出现问题时使用 |
| `Android fastboot / boot menu` | fastboot模式，见[安装说明](INSTALL.zh-CN.md#fastboot) |
| `crDroid … (gaokun3) — slot _a`<br>`crDroid … (gaokun3) — slot _b` | 直接启动某一个槽位的Android，用于排查问题。更新后，旧槽位的条目将无法启动，见[下文](#更新后出了问题能退回上一版本吗) |
| `gaokun3 rescue (slot a kernel)`<br>`gaokun3 rescue (slot b kernel)` | 救援系统，安装时勾选了"同时安装救援系统"才会出现。两项的区别只是借用哪个槽位的内核 |
| `gaokun3 installer` | 从Windows启动安装器时留下的条目 |
| `Windows Boot Manager` | 双系统时出现，进入Windows |

### 不连接键盘盖能操作开机菜单吗？

连接键盘盖时，用方向键选择、回车确认。不连接键盘盖时能否用音量键和电源键操作开机菜单，尚未经实测。需要在菜单中选择时，请先连接键盘盖。

## 更新与回退

### 更新后出了问题，能退回上一版本吗？

这取决于新版本能否启动：

- **新版本无法完成启动**（例如反复重启）：开机时会自动退回上一版本，进入系统后会收到通知。
- **新版本能够启动，只是某些功能有问题**：无法退回。新版本首次成功启动后，上一版本的系统即被合并覆盖，开机菜单中旧槽位的条目从此无法启动，选择它只会白白重启一次。这时请等待修复版本，或者用图形安装器重新安装旧版本（见下一条）。

刚安装、还没有更新过的机器，另一个槽位中没有系统。

### 能安装旧版本吗？

可以用图形安装器的"重新安装Android"安装任意版本。但新版本可能已经把用户数据升级成旧版本无法识别的格式，保留数据安装旧版本时，系统可能无法启动。安装旧版本时请清除数据（不勾选"保留用户数据"，这也是默认设置），并在安装前备份。

### 开机一直重启或卡住怎么办？

1. 如果是更新之后出现的，先等待几次重启，系统通常会自动退回上一版本。如果卡在某个画面不动，请长按电源键强制关机后再开机。
2. 仍然无法启动时，在开机菜单中选择`gaokun3 rescue`，或从安装器U盘启动，选择"重新安装Android"。勾选"保留用户数据"可以保住数据，安装同一版本时这一做法已在实机上验证过。
3. 救援系统中也可以获取日志，见[无法进入Android时怎么获取日志](#无法进入android时怎么获取日志)。

## 获取日志

报告问题时请写明你做了什么、发生了什么，并附上系统版本和下面的日志。请通过[issue表单](https://github.com/vahiru/gaokun-android/issues/new/choose)提交，也可以在[Telegram](https://t.me/gaokunAndroid)或QQ群**920133252**中反馈。

### 准备adb

1. 在电脑上安装Google的SDK Platform Tools，其中包含`adb`。
2. 在平板上打开开发者选项和USB调试，步骤见[下文](#打开开发者选项)。
3. 用USB线把平板连接到电脑，然后在电脑上运行`adb devices`。数据线请插在**靠近电源键**的USB-C口，另一个口无法用于USB调试。
4. 如果电脑上看不到设备，请拔下数据线重新插一次。连接部分电脑时，双方偶尔会协商错误，变成平板为电脑供电、双方都无法识别对方，见[已知限制](known-limitations.zh-CN.md#连接电脑时平板可能反过来为电脑供电)。
5. 不方便连接数据线时，可以使用[无线调试](#无线调试)。

### 机器能进入Android时

最简单也最完整的方式是：

```sh
adb bugreport gaokun3-bugreport.zip
```

它包含系统日志、内核日志和崩溃记录。其中也包含Wi-Fi名称、账号名、已安装的应用等个人信息，公开上传之前请先检查。

只需要部分日志时：

```sh
adb logcat -b all -d > logcat.txt          # 全部系统日志
adb logcat -b crash -d > crash.txt         # 只有崩溃记录
adb shell dmesg > dmesg.txt                # 内核日志
```

如果`dmesg`提示权限不足，或者需要音频与蓝牙卡死时自动保存的记录，请先在ReSukiSU管理器中为Shell授予root权限，然后运行：

```sh
adb shell su -c dmesg > dmesg.txt
adb exec-out "su -c 'cd /data/vendor/gaokun3 && tar -cf - hangdump-*'" > hangdump.tar
```

系统版本：

```sh
adb shell getprop ro.build.version.incremental
adb shell getprop ro.build.date.utc
```

### 没有电脑时

开发者选项中的"Bug报告"在本机上能否正常使用，尚未经实测。目前请尽量用电脑按上面的方法获取日志。

### 无法进入Android时怎么获取日志

在开机菜单中选择`gaokun3 rescue`，或从安装器U盘启动。

- 救援系统启动45秒后，会自动把诊断信息写入`/media/gk3/gaokun3/diag/`，这个目录在硬盘上，重启后仍然保留。
- 内核日志可以用`journalctl -k`查看。
- 如果已经设置了SSH，可以在电脑上用`scp -r root@<IP>:/media/gk3/gaokun3/diag .`取回，设置方法见下一节。
- 从安装器U盘启动时，`/media/gk3`就是U盘本身。把U盘插到任意一台电脑上，日志就在`gaokun3/diag/`中。

## 救援系统与SSH

### 怎么通过SSH登录救援系统？

救援系统只接受SSH公钥登录。发布的镜像中不含任何人的公钥，需要先放入你自己的公钥，否则只能在机器前操作。

**安装之前**（推荐）：

1. 在电脑上生成一对密钥（已有可跳过）：`ssh-keygen -t ed25519`。
2. 把公钥（`~/.ssh/id_ed25519.pub`的内容）保存为安装器U盘上的`gaokun3/authorized_keys`。U盘上的这个分区是FAT格式，Windows和macOS都可以直接写入。
3. 如果希望救援系统开机后自动连接Wi-Fi，在安装器中连接一次Wi-Fi即可，安装器会把这份配置一并复制过去；也可以在U盘上放一份`gaokun3/wpa_supplicant.conf`。
4. 安装时勾选"同时安装救援系统"。

**已经安装好之后**：

1. 开机时在菜单中选择`gaokun3 rescue`，屏幕上会出现安装器界面。
2. 点击"退出到终端"（或按Ctrl+Alt+F2），以`root`登录。在机器前登录不需要密码。
3. 添加公钥并重启SSH服务：

   ```sh
   cat >> /media/gk3/gaokun3/authorized_keys     # 粘贴公钥后回车，再按Ctrl+D
   systemctl restart ssh
   ```

   这个文件保存在救援分区上，以后每次进入救援系统都有效。

**连接**：在救援系统的终端中用`ip addr`查看IP地址，然后在电脑上运行`ssh root@<IP>`。

## 清除数据

### 怎么恢复出厂设置？出售或送修前怎么清除数据？

设置中的"清除所有数据（恢复出厂设置）"会在重启后由fastboot模式执行，但这一流程尚未在实机上测试，请暂时不要依赖它。

请按以下步骤清除数据：

1. 开机时在菜单中选择`gaokun3 rescue`；没有这一项时，从安装器U盘启动。
2. 选择"重新安装Android"，不要勾选"保留用户数据"（默认即不勾选）。
3. 系统镜像会从U盘读取；U盘中没有时，通过Wi-Fi下载，约1.3 GB。

本机的用户数据分区没有加密（见[已知限制](known-limitations.zh-CN.md)），出售或送修之前请务必先清除。

## 卸载Android、恢复Windows

### 双系统时临时进入Windows

开机时在菜单中选择`Windows Boot Manager`即可。双系统时也可以把Windows设为默认系统，见[安装说明](INSTALL.zh-CN.md#双系统)。

### 双系统时彻底删除Android

从Windows启动安装器时，开始菜单的gaokun3文件夹中有"卸载Android"一项，它会还原启动设置、删除Android的分区并把空间还给D盘。这个工具目前是预览版，尚未在实机Windows上运行过，手动卸载的完整流程也尚未在实机上走过。动手之前请先备份，并在社区群中询问。

安装器改动过以下几处，可供参考：

- **启动程序**：EFI分区上的`EFI/BOOT/BOOTAA64.EFI`被替换为本系统的启动菜单，原文件备份为`EFI/BOOT/BOOTAA64.EFI.before-gaokun3`。还原这个文件后，开机将直接进入Windows。
- **EFI分区上的文件**：一个以machine-id命名、内含`android/`的目录，以及`loader/`下的配置和启动项。
- **分区**：`misc`、`metadata`、`super`、`boot_a`、`boot_b`、`userdata`，可能还有`gk3rescue`。删除之后，可以在Windows中把空间合并回去。

注意，Android安装完成之后，`gaokun3-setup.cmd -Uninstall`只会删除安装器自己的启动项和文件，不会卸载Android。

### 用"清除整个磁盘"安装后，想恢复Windows

这种情况下Windows已经被删除，只能用华为的恢复介质或Windows安装U盘重新安装。这一流程尚未在本机上实测。如果想保留Windows，安装时请选择"保留现有系统"（双系统）。

## 开发者选项与adb

### 打开开发者选项

1. 在"设置 → 关于本机"中连续点按"Build号"七次，按提示输入锁屏密码。
2. 之后可以在"设置 → 系统 → 开发者选项"中找到它。

### USB调试

在开发者选项中打开"USB调试"，用数据线连接电脑。平板上会弹出"是否允许USB调试"的提示，核对电脑的指纹后，勾选"一律允许使用这台计算机进行调试"并点击允许。未经授权的电脑无法连接。

### 无线调试

1. 让平板和电脑连接同一个Wi-Fi。
2. 在开发者选项中打开"无线调试"，点击"使用配对码配对设备"，记下IP地址、端口和配对码。
3. 在电脑上运行`adb pair <IP>:<配对端口>`，输入配对码。每台电脑只需配对一次。
4. 再运行`adb connect <IP>:<端口>`。这里的端口是"无线调试"页面上显示的端口，不是配对端口。

不使用时请关闭无线调试。

### 为什么`adb root`无法使用？

发布版本不支持`adb root`。需要root shell时，请先在ReSukiSU管理器中为Shell授予root权限，再使用`adb shell su`。

> v0.7.1及更早的版本默认在5555端口开启了无需授权的网络adb，同一网络中的任何人都可以直接连接并取得root权限。1.0.0-rc.1起已经关闭。如果仍在使用旧版本，请尽快更新，在此之前不要连接不信任的Wi-Fi。
