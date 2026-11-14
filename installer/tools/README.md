# `installer/tools/`：LiveCD的构建与测试

本目录构建`gaokun3`的LiveCD：基于**Debian 13（trixie）arm64**的只读`squashfs` + `tmpfs` overlay内存根文件系统，开机后在`cage`中运行图形安装器（[`installer/live/`](../live/README.md)），后端是`gk3l`（[`installer/backend/`](../backend/)）。同一份镜像既可写入USB启动盘，也可放入内置盘上的安装器分区`GK3LIVE`。LiveCD中没有网络与`sshd`，安装所用的系统文件只从安装介质的`gaokun3/payload/`读取。

## 一、构建

所有步骤都在arm64容器内原生执行（Apple Silicon的Mac或arm64的Linux主机）：

```sh
bash installer/tools/build/build-flutter.sh                                         # 编译图形安装器 → out/installer-flutter-linux-arm64/
bash installer/tools/build/build-live.sh --boot-img <boot.img> --firmware <目录>     # 构建LiveCD → out/live/gaokun3-live.iso
bash installer/tools/build/build-live.sh --release <发布目录> --firmware <目录> --payload   # 同时放入系统文件
```

`--firmware`指向形如`/vendor/firmware`的目录（GPU的zap shader为华为专有固件，Debian中没有）。正式构建由GitHub Actions完成（[`.github/workflows/installer-livecd.yml`](../../.github/workflows/installer-livecd.yml)，推送`livecd-v*`标签触发），输入均按sha256固定。

`build-live.sh`在容器内依次执行：
1. `build/build-rootfs.sh`：用`mmdebstrap`按[`pkgs.txt`](build/pkgs.txt)构建根文件系统，叠加`build/overlay/`，静态编译`gk3l`，放入图形安装器与GPU固件；打包`squashfs`前逐项检查（命令、单元是否启用、动态库能否解析、字体、freedreno）。所装软件包的版本记入[`packages-live.lock`](build/packages-live.lock)。
2. `build/build-initramfs.sh`：静态`busybox`加[`initramfs-init`](build/initramfs-init)，生成`initramfs.img`。
3. `build/gk3-bootimg.py`：从Android的`boot.img`拆出共用的内核`Image`、`gaokun3.dtb`与内核参数。
4. `build/build-iso.sh`：组装混合ISO。ISO9660文件树（Rock Ridge加Joliet）是U盘的完整内容，可以在Windows中打开ISO，把`EFI`、`loader`、`gaokun3`三个目录拖进格式化为FAT32的U盘；附加的EFI系统分区装着除`live.squashfs`以外的启动文件，ISO整个写入U盘后固件从它启动，initramfs再从ISO9660分区读取`live.squashfs`。后一种用法要求内核开启`CONFIG_ISO9660_FS`，构建时从`boot.img`中提取内核配置检查，未开启时拒绝构建（测试构建可加`--allow-no-iso9660`）。

## 二、关键设计

1. **与Android共用同一内核**：
   LiveCD与Android使用完全相同的内核与`gaokun3.dtb`，只以`initramfs.img`和内核参数区分。为此[`scripts/kernel-config-android.sh`](../../scripts/kernel-config-android.sh)内建了：
   - `CONFIG_SQUASHFS=y`（挂载只读根文件系统）。
   - `CONFIG_NTFS3_FS=y`（双系统安装前只读挂载Windows分区，检查Windows是否处于休眠，同时显式禁用旧版`CONFIG_NTFS_FS`）。
   - `CONFIG_NLS_UTF8=y`（支持FAT32上的非ASCII文件名）。
   - `CONFIG_ISO9660_FS=y`（ISO整个写入U盘后，从ISO9660分区读取`live.squashfs`）。rc.3及之前的内核尚未开启，带上它的ROM发布之前，正式构建会被拒绝。
2. **`initramfs`动态查找介质**：
   `initramfs-init`不依赖固定的分区标签或UUID，而是先扫描可移动介质、再扫描内置盘，寻找包含`/gaokun3/live.squashfs`的分区。
   介质位于内置盘（免U盘安装的安装器分区`GK3LIVE`）时，先把`gaokun3/`整个复制进内存（tmpfs）再卸下该分区，内存副本同样挂在`/media/gk3`；安装器因此可以在清除整个磁盘时保留该分区、安装完成后将其删除并入`/data`（见[`installer/backend/src/linux/live.h`](../../installer/backend/src/linux/live.h)）。内存不足（`gaokun3/`的大小加2 GiB）或复制失败时照旧直接挂载；内核参数`gk3.toram=0|1`可强制关闭或开启。介质信息记入`/run/gaokun3/medium`（`/run`在initramfs中挂好后整体移入新根），`gk3l`据此识别介质。
3. **启动失败自动重启**：
   设备没有外露串口。若`initramfs`找不到根文件系统且未传入`gk3.debug`，会在打印诊断信息60秒后执行`reboot -f`，由启动管理器超时回到默认系统，避免停在无人值守的错误shell中。
4. **写入磁盘期间不睡眠、不关机**：
   睡眠相关的systemd target全部屏蔽，logind忽略电源键与合盖，写入磁盘的任务另由`systemd-inhibit`保护。
5. **开机诊断写回介质**：
   `gk3-diag`在开机45秒后把服务状态、挂载、显示输出、输入设备、日志等写入介质的`gaokun3/diag/`；介质已读进内存时，再由`gk3l persist-sync`写回分区本身。

## 三、测试

```sh
# 需要root、loop设备与分区工具的测试，在容器里运行（环境见test-env.Dockerfile）：
bash installer/tools/test/test-in-container.sh installer/backend/test/linux/test-apply.sh   # gk3l的其余容器测试同在该目录
bash installer/tools/test/test-in-container.sh installer/tools/test/test-initramfs.sh            # initramfs读进内存与介质记录
bash installer/tools/test/test-in-container.sh installer/tools/test/gen-fixtures.sh              # 用真实的gk3l录制图形安装器的测试数据

bash installer/tools/test/test-boot-container.sh   # 以systemd为PID 1启动构建好的squashfs，检查各单元
bash installer/tools/test/test-render.sh           # 在无头cage中运行图形安装器：截图与内存占用
```
