# Android系统移植与子系统集成（crDroid 12 / Android 16）

本文档汇总`gaokun3`从UEFI引导进入Android用户态的启动链路、内核配置强约束、核心HAL（电源/温控/无线/蓝牙）、SELinux策略及内置`ReSukiSU`集成方案。

---

## 一、UEFI启动链路与动态分区布局

### 1. 双阶段引导流程

`gaokun3`不采用传统高通手机的ABL `boot.img`直接引导，而是通过UEFI加载器（`gk3boot`或`systemd-boot`，详见[`tools/gk3boot/README.md`](../../tools/gk3boot/README.md)）从ESP分区加载三件套：

```
UEFI 2.16 (关闭Secure Boot)
  └──► \EFI\BOOT\BOOTAA64.EFI (gk3boot)
         ├── Kernel:    \EFI\gk3boot\slot_{a,b}\Image (Linux 7.2 arm64 EFI stub)
         ├── Initramfs: \EFI\gk3boot\slot_{a,b}\initramfs.img (First-stage init + /lib/firmware)
         └── DTB:       \EFI\gk3boot\slot_{a,b}\gaokun3.dtb
```

1. **第一阶段`init`（Ramdisk）**：
   - 内核挂载`initramfs.img`并执行`/init`（即`first_stage_init`）。
   - 解析内核命令行中的`androidboot.slot_suffix=_a`（或`_b`）与ramdisk内的`/system/etc/fstab.gaokun3`，在NVMe磁盘上定位`super`与`userdata`分区。
   - 通过`dm-linear`从`super`镜像解析并映射逻辑分区`system_a`、`system_ext_a`、`product_a`、`vendor_a`，挂载`/system`并切换根目录（`switch_root`）进入第二阶段`/system/bin/init`。
2. **内核命令行来源注意**：
   - `device/huawei/gaokun3/BoardConfig.mk`中的`BOARD_KERNEL_CMDLINE`仅写入AOSP产出的`boot.img`头部，而UEFI加载器直接从ESP启动裸`Image`，**不会读取`boot.img`头部的cmdline**。
   - 实际生效的内核启动参数由`gk3boot`内置默认值或ESP上的启动条目配置（`\loader\entries\*.conf`）提供。
3. **Virtual A/B与回滚注意事项**：
   - 通过`update_engine`执行OTA更新后，`super`分区的LP元数据仅保留目标槽位的一套逻辑分区（可通过`lpdump`核实）。`bootctl is-slot-bootable`仅读取`misc`分区标志位，不能代表`super`内仍存在旧槽逻辑分区。

---

## 二、Android必选内核配置（`scripts/kernel-config-android.sh`）

主线`defconfig`默认面向通用Linux发行版，大量驱动配为`=m`且缺少Android专属内核特性。由于本项目的Android镜像不部署`/vendor/lib/modules`模块树，`scripts/kernel-config-android.sh`强制将所有板级驱动与Android子系统设为内建（`=y`）：

| 类别 | 关键Kconfig项与补丁 | 缺失时的典型故障 |
|---|---|---|
| IPC与内存 | `ANDROID_BINDER_IPC=y`、`ANDROID_BINDERFS=y`、`ASHMEM=y`（`patches/0017`恢复`ashmem`）、`PSI=y` | `servicemanager`/`lmkd`无法启动，或依赖`/dev/ashmem`的应用崩溃 |
| BPF与文件系统 | `BPF_SYSCALL=y`、`CGROUP_BPF=y`、`FS_VERITY=y`、`UNICODE=y`、`F2FS_FS=y`、`DM_SNAPSHOT=y` | `bpfloader`失败触发`init`重启；`/data/media`不区分大小写挂载失败 |
| `netd`网络栈 | `NETFILTER_XT_MATCH_QUOTA2=y`（`patches/0016`移植AOSP `xt_quota2`）、`IP_NF_RAW=y`、`IP6_NF_RAW=y`、`NETFILTER_XT_TARGET_CT=y`、`NETFILTER_XT_MATCH_OWNER=y`、`NETFILTER_XT_MATCH_BPF=y`、`CLS_BPF=y`、`NET_SCH_INGRESS=y` | `netd`在`BandwidthController::enableBandwidthControl()`初始化时报错退出，触发60秒rescue party重启循环 |
| 远端处理器与QMI | `QCOM_Q6V5_PAS=y`、`QCOM_SYSMON=y`、`QCOM_FASTRPC=y`、`QCOM_PD_MAPPER=y`、`QRTR=y`、`QRTR_SMD=y`、`QRTR_MHI=y` | ADSP/CDSP/SLPI与Wi-Fi QMI无法建立通信 |
| 板级外设驱动 | `DRM_MSM=y`、`HID_HIMAX_83121A=y`、`ATH11K_PCI=y`、`BT_HCIUART_QCA=y`、`EC_HUAWEI_GAOKUN=y`、`QCOM_PMIC_GLINK=y`、`TYPEC_UCSI=y` | 屏幕、触控、Wi-Fi、蓝牙、电池状态与USB角色切换不可用 |

---

## 三、核心HAL与系统服务集成

### 1. 自研温控HAL（`device/huawei/gaokun3/thermal/`）

- **为何必须替换AOSP示例实现**：AOSP默认的`android.hardware.thermal-service.example`上报硬编码的模拟温度，并将`SKIN`与`BATTERY`的`SHUTDOWN`阈值设为`36.0 °C`；而`gaokun3`正常空载运行时热区温度即在`36–38 °C`之间，若不排除该mock APEX，系统会在开机数分钟内由`ThermalManagerService`触发强制关机。
- **实现机制**：`android.hardware.thermal-service.gaokun3`读取`/sys/class/thermal/thermal_zone*`下的8核CPU、GPU、内存、机身表面（skin）及4路PMIC真实温度传感器，配置合理的节流（throttling）与关机阈值（CPU/GPU关机阈值105–115 °C，机身表面关机阈值65 °C）。

### 2. 电源、电池与导航栏

- **电池与充电器状态**：由内核`ec_huawei_gaokun`驱动暴露`/sys/class/power_supply/gaokun-battery`与`gaokun-psy-ec`，对接`android.hardware.health-service.example`。
- **导航栏与侧滑返回手势**：平板设备必须在framework overlay（`overlay/frameworks/base/core/res/res/values/config.xml`）中声明`config_showNavigationBar = true`。在Android中，全屏边缘侧滑返回（Edge Back Gesture）的输入监听器由`NavigationBar` / `Taskbar`组件负责创建；若未启用该项，系统既无底部任务栏也无法响应侧滑返回手势。
- **游戏模式与高刷调度**：CPU/GPU调频、SurfaceFlinger合成锁与触控采样率优化详见[gaming.md](gaming.md)，深度休眠（S3 mem sleep）详见[deep-sleep.md](deep-sleep.md)。

---

## 四、无线网络（Wi-Fi 6E）与蓝牙（Bluetooth 5.2）

### 1. Wi-Fi（WCN6855 / `ath11k_pci`）

- **固件与初始化**：PCIe设备`0002:01:00.0`加载`/vendor/firmware/ath11k/WCN6855/hw2.1/`下的`amss.bin`、`m3.bin`、`board-2.bin`与`regdb.bin`（`board-id`未收录时自动回退至通用`bus=pci,qmi-chip-id=2,qmi-board-id=255`条目）。
- **持久化MAC地址生成**：因主线`ath11k`未从UEFI/NV读取出厂Wi-Fi MAC，`init.gaokun3.rc`在开机时运行`wpa_supplicant_mac_gen`脚本，在`/data/vendor/wifi/wlan_mac.bin`生成并持久化一个本地管理MAC地址，避免每次重启MAC变化导致路由器DHCP租约重置。
- **WPA3（SAE-H2E）与多频段**：支持2.4 GHz / 5 GHz / 6 GHz（802.11ax 160 MHz）及WPA2-PSK与WPA3-SAE（`sae_pwe=1` H2E）。
- **CMA内存预留与休眠恢复（`patches/0071`）**：`ath11k`在每次挂起/恢复（或模块重初始化）时向DMA子系统申请高阶连续物理内存（order-9，2 MiB×32段）。设备树在`0xec000000`处配置512 MiB `shared-dma-pool`（`reusable`），确保系统长时间运行内存碎片化后`ath11k`仍能从CMA池稳定分配DMA缓冲。

### 2. 蓝牙（WCN6855 UART / `bt_linux_hci`）

- **内核HCI初始化**：蓝牙控制器挂载在UART `988000.serial`（`ttyHS1`），由内核`CONFIG_BT_HCIUART_QCA=y`自动加载`/vendor/firmware/qca/wcnhpbtfw21.tlv`与`wcnhpnv21g.bin`并注册Linux标准`hci0`接口。
- **Android AIDL桥接（`device/huawei/gaokun3/bt-hci/`）**：
  - 高通手机常用的vendor `libbt-vendor`假定用户态直接操作裸tty节点下载固件，会与主线内核的`btqca`驱动冲突。
  - 本项目采用`android.hardware.bluetooth-service.btlinux`，通过Linux内核提供的`AF_BLUETOOTH`（`BTPROTO_HCI`、`HCI_CHANNEL_USER`）独占打开`hci0`，将其桥接为Android标准的`android.hardware.bluetooth.IBluetoothHci` AIDL接口。

---

## 五、SELinux策略与块设备标签（`device/huawei/gaokun3/sepolicy/`）

设备侧SELinux策略位于`device/huawei/gaokun3/sepolicy/`，遵循以下原则：

1. **自定义服务与节点域隔离**：
   - 为自研HAL配置独立安全上下文：`hal_camera_default`、`hal_sensors_default`、`hal_thermal_default`、`hal_bluetooth_btlinux`、`hexagonrpcd`。
   - 补齐AOSP默认策略未覆盖的主线内核设备节点：`/dev/media[0-9]+`与`/dev/v4l-subdev[0-9]+`标为`video_device`；`/dev/fastrpc-adsp`與`/dev/fastrpc-sdsp`标为`qdsp_device`；`/dev/dri/renderD128`标为`gpu_device`。
2. **块设备按GPT分区名匹配而非分区号**：
   - 用户通过双系统安装器（Windows收缩分区后追加）与全盘安装创建的NVMe分区号（`nvme0n1pX`）并不相同。
   - 因此`file_contexts`中对`super`、`userdata`、`metadata`、`misc`、`esp`的标签统一绑定在`/dev/block/platform/soc@0/1c08000.pcie/pci0000:00/.../by-name/<name>`符号链接路径上，不得硬编码`nvme0n1p*`编号。

---

## 六、内置Root方案（`ReSukiSU`）

内核通过`patches/0100`–`0103`集成`ReSukiSU`（基于KernelSU架构的内核态Root实现）：

- **构建要求**：`ReSukiSU`依赖C11复合字面量与声明混排语法，`patches/0102`为其Makefile移除内核顶层的`-Wvla`与`-Wdeclaration-after-statement`警告升级。
- **使用说明**：内核内置ReSukiSU，检测root的应用可能拒绝运行。

