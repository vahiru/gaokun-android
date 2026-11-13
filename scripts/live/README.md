# `scripts/live/`：救援系统与LiveCD构建

本目录构建`gaokun3`的救援系统与图形安装器LiveCD镜像。两者共用同一套基于**Debian 13（trixie）arm64**的只读`squashfs` + `tmpfs` overlay内存根文件系统：

| Profile | 包含内容 | 部署位置 |
|---|---|---|
| `rescue` | 无图形界面：`sshd`、Wi-Fi连接服务及分区/文件系统维护工具 | 内置NVMe磁盘的`gk3rescue`小分区（约1 GiB） |
| `live` | 在`rescue`基础上集成图形安装器（[`installer/live/`](../../installer/live/README.md) + `cage` Wayland合成器） | USB启动盘或免U盘双系统安装的`GK3LIVE`分区 |

## 一、构建命令

所有构建步骤均在arm64容器内执行，无需连接Android编译服务器：

```sh
bash scripts/live/build-flutter.sh                                    # 编译Flutter图形安装器 → out/installer-flutter-linux-arm64/
bash scripts/live/build-live.sh --boot-img <boot.img>                 # 构建LiveCD镜像 → out/live/gaokun3-live.img
bash scripts/live/build-live.sh --release <发布目录> --payload        # 打包内置离线ROM安装载荷的完整LiveCD
bash scripts/live/build-live.sh --profile rescue --boot-img <boot.img> --ssh-key ~/.ssh/id_ed25519.pub
```

`build-live.sh`在容器内依次执行：
1. `build-rootfs.sh`（使用`mmdebstrap`构建Debian rootfs，锁文件保存在`packages-<profile>.lock`，并在打包`mksquashfs`前校验`sgdisk`、`resize2fs`、`simg2img`、`wpa_supplicant`与`ath11k`固件均已就位）。
2. `build-initramfs.sh`（生成查找并挂载`squashfs`的`initramfs.img`）。
3. 从传入的Android `boot.img`提取共用内核`vmlinuz.efi`与`gaokun3.dtb`。
4. `build-usb.sh`（组装可启动GPT镜像）。

发布打包脚本：

```sh
bash scripts/live/release-installer.sh --boot-img <boot.img> --rom <ROM版本> --firmware <目录> [--upload <tag>]
```

## 二、关键架构设计

1. **与Android共用同一内核镜像**：
   救援系统与Android使用完全相同的`vmlinuz.efi`与`gaokun3.dtb`，仅通过不同的`initramfs.img`和内核命令行区分。为此在[`scripts/kernel-config-android.sh`](../kernel-config-android.sh)中内建了三项配置：
   - `CONFIG_SQUASHFS=y`（挂载只读根文件系统）。
   - `CONFIG_NTFS3_FS=y`（双系统安装前只读挂载Windows分区，检查Windows是否处于休眠，同时显式禁用旧版`CONFIG_NTFS_FS`）。
   - `CONFIG_NLS_UTF8=y`（支持FAT32上的非ASCII文件名）。
2. **`initramfs`动态扫描介质**：
   `initramfs-init`不依赖固定的分区标签或UUID，而是先扫描可移动介质、再扫描内置NVMe分区，寻找包含`/gaokun3/rescue.squashfs`（或`live.squashfs`）的分区挂载启动。
3. **启动失败自动重启回退**：
   由于设备无外露串口，若`initramfs`找不到根文件系统且未传入`gk3.debug`参数，会在打印诊断信息60秒后自动执行`reboot -f`，由启动加载器超时回退至默认系统（Android或Windows），避免停在无人值守的错误shell中。
4. **无线凭据与公钥外部化**：
   `/etc/init.d/gk3-wifi`优先读取启动介质上的`gaokun3/wpa_supplicant.conf`与`gaokun3/authorized_keys`，使公开发布的Live镜像不含任何私人凭据，且用户可在FAT分区上直接预置Wi-Fi配置。

## 三、安装器后端（`installer-lib.sh`）与测试套件

命令行安装器（[`scripts/install-gaokun3.sh`](../install-gaokun3.sh)）与Flutter图形安装器共用同一份后端库`scripts/live/installer-lib.sh`（并行推进的Rust重写版见[`docs/dev/installer-rust-design.md`](../../docs/dev/installer-rust-design.md)）。后端配套三个辅助工具（安装于Live镜像的`/usr/share/gaokun3/`）：

| 工具 | 作用 |
|---|---|
| `gk3-unsparse.py` | 从标准输入流式展开Android sparse镜像并直接写入目标分区（无需像`simg2img`那样要求输入文件支持`lseek`而额外生成12 GiB临时文件） |
| `gk3-bootimg.py` | 解析`boot.img`（v2）并拆出`Image`、`gaokun3.dtb`、`ramdisk.img`与`cmdline.txt` |
| `gk3-misc` | 初始化`misc`分区的A/B槽位状态与`GK3R`记录（源码位于[`tools/gk3boot/misc/gk3-misc.c`](../../tools/gk3boot/misc/gk3-misc.c)） |

运行后端自动化测试：

```sh
bash scripts/live/test-plan.sh          # 分区规划计算校验（不重叠、不越界）
bash scripts/live/test-unsparse.sh      # sparse流式展开与截断拒绝测试
bash scripts/live/test-boot-android.sh  # 救援系统一键重启回Android的UEFI变量写入测试

# 容器内端到端磁盘安装测试（使用loop与dm-error设备）：
bash scripts/live/test-in-container.sh scripts/live/test-apply.sh
```
