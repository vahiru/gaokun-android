# 统一启动入口与执行端预置目录（`prebuilt-gk3boot/`）

本目录通过`.gitignore`忽略所有二进制文件，仅将本说明文件纳入版本控制。统一启动入口（`gk3boot.efi`）与fastboot执行端（`fastboot.img`）的完整设计与实现规范见[`tools/gk3boot/README.md`](../../../../tools/gk3boot/README.md)。

## 一、目录结构与文件要求

```text
gk3boot.efi    tools/gk3boot/build/efi/gk3boot.efi（aarch64 UEFI PE镜像，约100 KB）
version        单行文本：构建时的BOOT_VERSION（仅限[A-Za-z0-9._+-]，不超过64字符，且不可为"log"）
fastboot.img   可选：执行端initramfs（gzip压缩cpio，2–4 MiB），来自tools/gk3boot/build/fastboot/fastboot.img
```

- **可选打包机制**：[`device.mk`](../device.mk)通过`wildcard`检查本目录。当`gk3boot.efi`与`version`同时存在时，安装至`/vendor/boot/gk3boot/{gk3boot.efi,version}`；若`fastboot.img`一并存在，则同步安装至`/vendor/boot/gk3boot/fastboot.img`。文件缺失时不影响ROM编译。
- **版本号约束（`version`）**：
  - ESP上的部署目录命名为`EFI/gk3boot/<version>/`，`boot_control` HAL与OTA `postinstall`均依据`version`判断ESP上的启动入口是否已更新。
  - `version`内容必须与`gk3boot.efi`内部嵌入的版本串（`gk3boot-<version>`）完全一致，`scripts/sync-device-tree.sh`会在同步时提取二进制字符串进行比对。
  - `fastboot.img`与`gk3boot.efi`共用同一个`version`，同步部署至同一`EFI/gk3boot/<version>/`目录并随版本轮换或回收。**只要更新了`gk3boot.efi`或`fastboot.img`中的任意一项（包括增删`fastboot.img`），都必须递增`version`**，以确保旧版本能正常保留至上一版回退槽（`gk3prev`）。
  - 请勿放入带`.dirty`后缀（工作区存在未提交改动时构建）的产物。

## 二、构建与更新步骤

在具备ARM64容器支持（Docker）的宿主机上执行：

```sh
# 1. 构建 gk3boot.efi 并运行 QEMU 端到端测试
bash scripts/gk3boot/test-boot.sh
cp tools/gk3boot/build/efi/gk3boot.efi device/huawei/gaokun3/prebuilt-gk3boot/
echo 0.2.0-e5.g<提交号> > device/huawei/gaokun3/prebuilt-gk3boot/version
shasum -a 256 device/huawei/gaokun3/prebuilt-gk3boot/gk3boot.efi

# 2. 构建执行端 initramfs（fastboot.img）并验证（可选）
bash scripts/gk3boot/build-fastboot-img.sh
cp tools/gk3boot/build/fastboot/fastboot.img device/huawei/gaokun3/prebuilt-gk3boot/
shasum -a 256 device/huawei/gaokun3/prebuilt-gk3boot/fastboot.img
```

## 三、ESP部署模式与条目格式

打包进`/vendor/boot/gk3boot/`的产物由`boot_control` HAL（开机完成时）与[`gaokun3-ota-postinstall.sh`](../bin/gaokun3-ota-postinstall.sh)（OTA升级时）根据属性`persist.vendor.gaokun3.gk3boot`（[`device.mk`](../device.mk)默认设为`action`）部署至ESP：

| 属性值 | `boot_control` HAL（[`Gk3Boot.cpp`](../boot_control/Gk3Boot.cpp)，开机完成时） | OTA `postinstall`（[`gaokun3-ota-postinstall.sh`](../bin/gaokun3-ota-postinstall.sh)） |
|---|---|---|
| `off` / 未设置 | 清理全部`gk3boot-*`与`gk3prev-*`条目（含`gk3boot-tools.conf`）及`EFI/gk3boot/<ver>/`目录（保留`log/`） | 移除启动条目，目录留待新槽位HAL回收 |
| `observe` | 部署本槽位版本，设`gk3.observe=1`（仅计算决策、不修改`misc`）；不生成`gk3boot-tools.conf` | 若ESP尚无入口则直接部署`+3`条目；若已有旧版则仅铺设目录与`.staged`标记 |
| `action` | 部署本槽位版本，设`gk3.observe=0`（扣减`tries_remaining`、自动回滚、写入GK3记录）；存在`fastboot.img`时一并生成`gk3boot-tools.conf` | 同上，并在首次部署或版本一致时同步生成`gk3boot-tools.conf` |

ESP上常态最多保留两个版本目录（当前生效版本与`gk3prev`上一版本）；OTA写入新槽位后、新槽位首次开机完成前最多暂存三个版本（含`.staged`），待HAL激活新版本时自动回收最旧目录。

生成的BLS启动项格式如下：

```text
# gk3boot-android-<slot>.conf
title      Android                          （observe模式为"Android (gk3boot observe)"，上一版为"Android (previous loader)"）
version    gk3boot-<ver>
sort-key   0gk3                             （上一版为0gk3prev，均排在直连条目zandroid<x>之前）
efi        /EFI/gk3boot/<ver>/gk3boot.efi
options    gk3.observe=<0|1> gk3.hint=<x>

# gk3boot-tools.conf（菜单手动进入fastboot执行端）
title      Android fastboot / boot menu
version    gk3boot-<ver>
sort-key   0gk3tools
efi        /EFI/gk3boot/<ver>/gk3boot.efi
options    gk3.action=fastboot
```
