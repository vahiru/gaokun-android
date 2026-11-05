# 免U盘安装与Windows伴随工具

本目录提供在Windows 11 ARM下免U盘部署`gaokun3`图形安装器、以及在双系统环境下管理启动项与卸载Android的PowerShell脚本与测试套件。安装器Linux侧实现见[`scripts/live/README.md`](../live/README.md)，双系统启动入口规范见[`tools/gk3boot/README.md`](../../tools/gk3boot/README.md)。

| 文件 | 用途 |
|---|---|
| `gaokun3-setup.cmd` | 用户双击入口：以管理员身份运行同目录的`gaokun3-setup.ps1`并透传参数 |
| `gaokun3-setup.ps1` | 免U盘部署脚本与常驻Windows伴随工具（部署`GK3LIVE`分区、切换启动项、修复ESP引导、卸载Android） |
| `build-bundle.sh` | 生成Windows安装包目录与zip（由`build-live.sh`调用）→ `out/live/gaokun3-windows{,.zip}` |
| `test-setup.ps1` / `test-setup.sh` | PowerShell容器测试：验证语法、UTF-8 BOM编码、PowerShell 5.1兼容性、纯逻辑及`wpa_supplicant`配置解析 |
| `test-companion.ps1` | 伴随工具单元测试：模拟ESP、固件变量、BitLocker/`manage-bde`、分区cmdlet与计划任务 |
| `test-fixtures.sh` | 在容器内生成3种GPT磁盘镜像，供`-RemoveAndroid`的GPT解析与安全断言测试使用 |

```bash
bash scripts/live/build-live.sh --boot-img ... --firmware out/vendor-firmware   # 构建Live镜像并同步产出Windows安装包
bash scripts/windows/test-setup.sh
```

## 免U盘部署设计要点

- **按需划分安装器分区**：脚本默认仅压缩出安装器自身所需的`GK3LIVE` FAT32分区空间（按安装包大小×1.25 + 128 MiB并按256 MiB向上取整，至少512 MiB，可通过`-LiveMiB`覆盖）；给Android使用的磁盘空间留在进入图形安装器后再行划分。若目标卷启用了BitLocker或设备加密（`VolumeStatus`非`FullyDecrypted`），由于Linux侧无法离线缩小BitLocker卷，脚本会在Windows侧提示一次缩出Android所需空间（默认64 GiB，也可通过`-AndroidGiB <N>`显式指定）。
- **关闭Windows快速启动**：双系统共用ESP，若Windows处于快速启动（混合休眠）状态恢复，可能损坏ESP文件系统。脚本在確認后将`HiberbootEnabled`设为`0`并记录原值（安装Android前执行`-Uninstall`可恢复原值；安装双系统后保持关闭，伴随工具每次开机也会巡检）。
- **独立`GK3LIVE`分区存放Live镜像**：出厂ESP剩余空间有限且`C:`可能启用BitLocker，因此Live镜像写入新建的FAT32分区`GK3LIVE`，ESP仅写入`systemd-boot`与加载条目。
- **一次性引导进入安装器**：默认通过`bcdedit /set {fwbootmgr} bootsequence`设置单次启动（等效于UEFI `BootNext`），不修改常规默认启动项；若固件不支持`bootsequence`，可使用`-UseFallbackPath`接管`\EFI\Boot\bootaa64.efi`（原文件备份为`.before-gaokun3`）。
- **Wi-Fi配置迁移**：自动提取当前连接的Wi-Fi配置写入`GK3LIVE`供Live安装器使用。WPA2使用802.11i预推导的64位十六进制PSK；WPA3（SAE）写入明文密码；`netsh`导出的临时明文文件在`finally`块中清理。
- **编码与语法兼容**：`.ps1`文件采用带BOM的UTF-8与CRLF换行，确保Windows内置PowerShell 5.1正确解析中文且不使用PowerShell 7专有语法（`?:`、`??`、`&&`、`?.`）。

## Windows伴随工具

免U盘安装的最后一步会自动安装Windows伴随工具；通过U盘安装双系统的用户可在进入Windows后双击U盘FAT分区`gaokun3-windows\`目录下的`gaokun3-setup.cmd`安装。

安装后的组件布局：

- `%ProgramFiles%\gaokun3\`：存放管理员只读的`gaokun3-setup.ps1`与`gaokun3-setup.cmd`。
- 计划任务`\gaokun3\`：`BootCheck`（开机、`SYSTEM`权限）、`Notify`（用户登录弹窗提示）、`RebootToAndroid` / `DefaultAndroid` / `DefaultWindows`（固定命令行任务，供开始菜单快捷方式通过`schtasks /Run`无提权触发）。
- 开始菜单`gaokun3`：提供「重启到Android」「开机默认进Android」「开机默认进Windows」「修复Android启动」「检查启动状态」「仅暂停BitLocker」「卸载Android」入口。
- `%ProgramData%\gaokun3\`：保存`companion.json`、`boot-check.json`、`companion.log`及任务执行回执。

### 子命令说明

| 子命令 | 功能说明 |
|---|---|
| 开机自检（`BootCheck`） | ①比对`EFI\Boot\bootaa64.efi`与`EFI\systemd\systemd-bootaa64.efi`的SHA-256，若被Windows更新替换为`bootmgfw.efi`则在登录时弹窗提示一键修复；②规范化`LoaderEntryDefault`（仅允许为空或`auto-windows`/`gk3-windows.conf`，其余非法值自动清除）；③检测BIOS版本变化、移除`GK3LIVE`盘符并确保快速启动保持关闭 |
| `-RepairBoot [-Check]` | 检查或修复被Windows更新覆盖的`bootaa64.efi`：若启用了BitLocker则先调用`Suspend-BitLocker -RebootCount 1`，备份`.before-gaokun3`后拷回`systemd-bootaa64.efi`并校验SHA-256 |
| `-RebootToAndroid` | 读取`loader.conf`中的默认Android条目，通过`SetFirmwareEnvironmentVariableExW`写入`LoaderEntryOneShot`（UTF-16LE + NUL，属性`0x7`）并读回校验，随后执行`shutdown /r /t 0`重启进入Android |
| `-SetDefault android\|windows` | 设置开机默认系统：`windows`将`LoaderEntryDefault`设为`gk3-windows.conf`或`auto-windows`；`android`删除`LoaderEntryDefault`变量 |
| `-SuspendBitLocker` | 暂停BitLocker保护2次重启（供U盘安装路径在进入BIOS关闭Secure Boot前使用） |
| `-RemoveAndroid [-NoExtend]` | 安全卸载Android并恢复纯Windows：先只读校验系统盘GPT分区表（核对分区名、Linux filesystem类型、偏移、唯一性及非NTFS/BitLocker/FAT内容），暂停BitLocker 1次，将`bootmgfw.efi`还原至`EFI\Boot\bootaa64.efi`，清理ESP上的Android目录与Loader变量，删除Android与`GK3LIVE`分区并在相邻时将空间合并回`D:`，最后恢复快速启动设置并卸载伴随工具 |
| `-WindowsPreset on\|off` | 可选特性（默认`off`）：Windows开机时预置`LoaderEntryOneShot`为Windows条目，正常关机时清除，使Windows更新的多次重启始终留在Windows |
| `-Hibernate ask\|off\|keep` | 安装伴随工具时是否关闭Windows休眠（`powercfg /hibernate off`） |

## 验证状态

- **容器与单元测试**：`bash scripts/windows/test-setup.sh`覆盖237项断言（含PowerShell 5.1语法树检查、GPT解析与`sgdisk -i`逐项对照、`wpa_supplicant`配置解析、伴随工具状态机与`-RemoveAndroid`安全边界）。
- **Parallels Windows 11 ARM虚拟机实测**：已验证UTF-8 BOM中文输出、分区大小预检与拒绝、压缩数据卷并创建`GK3LIVE`、写入ESP与`bcdedit` `bootsequence`单次引导、`-Uninstall`无损还原分区与ESP、以及`-UseFallbackPath`备份还原。
- **待真机Windows环境进一步实测项**：
  1. 华为UEFI固件在Windows下通过`SetFirmwareEnvironmentVariableExW`读写`systemd-boot` GUID变量及`bcdedit` `bootsequence`的兼容性（真机尚未安装Windows双系统实测）。
  2. BitLocker在关闭Secure Boot与切换`bootaa64.efi`前后的自动暂停计数表现。
