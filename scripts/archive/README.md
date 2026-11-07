# `scripts/archive`：已退役的早期脚本

本目录归档项目早期阶段使用过、现已被正式工具链或补丁替代的一次性脚本，仅保留供查阅历史实现。**请勿在当前设备上运行本目录中的脚本**，对应操作请使用下表所列的现行工具。

| 归档脚本 | 原用途 | 退役原因与现行替代工具 |
|---|---|---|
| `deploy-android.sh` | 从早期U盘Linux将`super`与`ramdisk`部署至内置盘 | 已被[`../install-gaokun3.sh`](../install-gaokun3.sh)、Live图形安装器（[`../live/README.md`](../live/README.md)）及[`../install-ota-local.sh`](../install-ota-local.sh)取代 |
| `deploy-from-ubuntu.sh` | 在救援Ubuntu下从构建机拉取产物并刷写分区 | 硬编码了早期分区号（如`nvme0n1p8`），与现行安装器分区布局不兼容；现使用[`../install-ota-local.sh`](../install-ota-local.sh)或Live安装器 |
| `partition-android.sh` | 早期手动划分Android分区 | 现由[`../install-gaokun3.sh`](../install-gaokun3.sh)或[`../live/installer-lib.sh`](../live/installer-lib.sh)统一完成分区创建 |
| `esp-migrate-to-internal.sh` | 将引导链从外置U盘ESP迁移至内置ESP | 早期一次性迁移脚本 |
| `flash-usb.ps1` | 在Windows下将Ubuntu镜像写入U盘 | 已被[`../live/build-usb.sh`](../live/build-usb.sh)与[`../windows/gaokun3-setup.ps1`](../windows/gaokun3-setup.ps1)取代 |
| `restore-boot-entries.ps1` | 在Windows下恢复U盘ESP启动项 | 已被[`../boot-oneshot.sh`](../boot-oneshot.sh)与[`../windows/gaokun3-setup.ps1`](../windows/gaokun3-setup.ps1)取代 |
| `windows-preflight.ps1` | 在Windows下读取机型、BIOS与Secure Boot状态 | 硬件信息已收录于[`../../docs/dev/hardware.md`](../../docs/dev/hardware.md)；现行Windows侧预检集成在[`../windows/gaokun3-setup.ps1`](../windows/gaokun3-setup.ps1)中 |
| `probe-windows-sensors.sh` | 挂载Windows分区识别传感器型号 | 传感器型号与配置已合入[`../../device/huawei/gaokun3/hexagonrpcd-root/`](../../device/huawei/gaokun3/hexagonrpcd-root/)（详见[`../../docs/dev/sensors-ssc-protocol.md`](../../docs/dev/sensors-ssc-protocol.md)） |
| `sensors-up-android.sh` | 在`CONFIG_QCOM_FASTRPC=m`的旧内核上手动拉起传感器 | 现役内核已将`CONFIG_QCOM_FASTRPC=y`内建，由`init`自动拉起`hexagonrpcd` |
| `kernel-bpffs-genfscon-fix.py` | 修改内核源码使`bpffs`支持延迟SELinux标签分配 | 已固化为内核补丁[`../../patches/kernel/core/0002-bpf-inode-label-bpffs-lazily-for-android-genfscon.patch`](../../patches/kernel/core/0002-bpf-inode-label-bpffs-lazily-for-android-genfscon.patch) |
| `glslang-host-tool.py` | 为`glslang`补充宿主机端`glslangValidator`目标 | Android 17的`external/deqp-deps/glslang`已原生提供`glslangValidator`宿主机目标，不再需要树外修改 |
| `mesa-relocate-abs-paths.py` | 替换Mesa `Android.bp`中的绝对路径 | 早期源码树迁移时的一次性脚本 |
| `lpdump.py` | 读取硬编码分区的LP元数据头 | 已被通用逻辑分区解析工具[`../lpext.py`](../lpext.py)取代 |
