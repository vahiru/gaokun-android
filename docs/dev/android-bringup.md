# Android系统移植与子系统集成（crDroid 12 / Android 16）

本文档汇总`gaokun3`从UEFI引导进入Android用户态的启动链路、内核配置强约束、核心HAL（电源/温控/无线/蓝牙）、SELinux策略及内置`ReSukiSU`集成方案。

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

## 二、Android必选内核配置（`scripts/kernel-config-android.sh`）

主线`defconfig`默认面向通用Linux发行版，大量驱动配为`=m`且缺少Android专属内核特性。由于本项目的Android镜像不部署`/vendor/lib/modules`模块树，`scripts/kernel-config-android.sh`强制将所有板级驱动与Android子系统设为内建（`=y`）：

| 类别 | 关键Kconfig项与补丁 | 缺失时的典型故障 |
|---|---|---|
| IPC与内存 | `ANDROID_BINDER_IPC=y`、`ANDROID_BINDERFS=y`、`ASHMEM=y`（`patches/kernel/core/0004`恢复`ashmem`）、`PSI=y` | `servicemanager`/`lmkd`无法启动，或依赖`/dev/ashmem`的应用崩溃 |
| BPF与文件系统 | `BPF_SYSCALL=y`、`CGROUP_BPF=y`、`FS_VERITY=y`、`UNICODE=y`、`F2FS_FS=y`、`DM_SNAPSHOT=y` | `bpfloader`失败触发`init`重启；`/data/media`不区分大小写挂载失败 |
| `netd`网络栈 | `NETFILTER_XT_MATCH_QUOTA2=y`（`patches/kernel/core/0005`移植AOSP `xt_quota2`）、`IP_NF_RAW=y`、`IP6_NF_RAW=y`、`NETFILTER_XT_TARGET_CT=y`、`NETFILTER_XT_MATCH_OWNER=y`、`NETFILTER_XT_MATCH_BPF=y`、`CLS_BPF=y`、`NET_SCH_INGRESS=y` | `netd`在`BandwidthController::enableBandwidthControl()`初始化时报错退出，触发60秒rescue party重启循环 |
| 远端处理器与QMI | `QCOM_Q6V5_PAS=y`、`QCOM_SYSMON=y`、`QCOM_FASTRPC=y`、`QCOM_PD_MAPPER=y`、`QRTR=y`、`QRTR_SMD=y`、`QRTR_MHI=y` | ADSP/CDSP/SLPI与Wi-Fi QMI无法建立通信 |
| 板级外设驱动 | `DRM_MSM=y`、`HID_HIMAX_83121A=y`、`ATH11K_PCI=y`、`BT_HCIUART_QCA=y`、`EC_HUAWEI_GAOKUN=y`、`QCOM_PMIC_GLINK=y`、`TYPEC_UCSI=y` | 屏幕、触控、Wi-Fi、蓝牙、电池状态与USB角色切换不可用 |

## 三、核心HAL与系统服务集成

### 1. 自研温控HAL（`device/huawei/gaokun3/thermal/`）

- **为何必须替换AOSP示例实现**：AOSP默认的`android.hardware.thermal-service.example`上报硬编码的模拟温度，并将`SKIN`与`BATTERY`的`SHUTDOWN`阈值设为`36.0 °C`；而`gaokun3`正常空载运行时热区温度即在`36–38 °C`之间，若不排除该mock APEX，系统会在开机数分钟内由`ThermalManagerService`触发强制关机。
- **实现机制**：`android.hardware.thermal-service.gaokun3`读取`/sys/class/thermal/thermal_zone*`下的CPU、GPU、内存、PMIC与板载热敏电阻温区。CPU/GPU温区按裸片温度分级，105 °C时由Android有序关机。板载热敏电阻`sys-therm1`–`sys-therm4`各作为一个`SKIN`温度上报，阈值以各自当前最低的被动降频点t为准：`LIGHT`为t-2，`MODERATE`为t（内核从这里开始降频），`SEVERE`为t+4（降频已接近最低保留性能），`CRITICAL`为t+8，`EMERGENCY`为t+14，`SHUTDOWN`为95 °C；等级降低要等温度比当前等级的阈值低2 °C。`sys-therm3`最低的降频点只限制GPU，它的等级跟随GPU降频点。框架取最高的`SKIN`等级作为整机状态，并逐路计算温度余量；在长时间高负载下整机状态会达到`MODERATE`或`SEVERE`，JobScheduler随之推迟后台任务。内核没有这些温区时，`SKIN`退回CPU/GPU温区的最大值。这几路都不是直接测得的外壳温度。
- **板载温控与性能模式**：内核（`patches/kernel/dts/0031`）按本机Windows ACPI表的开机默认值，在`sys-therm1`–`sys-therm4`上限制CPU与GPU。Windows运行时还会按EC上报的状态和华为的性能模式改写这些降频点，HAL采用其中的两部分：「设置 → 电池」里的性能模式开关（`persist.sys.gaokun3.perf_mode`）开启时，`sys-therm1`–`sys-therm3`使用华为性能模式的数值，用电池时也可以开启；关闭时，接电源（`gaokun-ec-adapter`的`online`为1）使用EC事件中按推测对应接电源的一套数值，未接电源使用开机默认值。Windows用电池时的数值更保守（例如`sys-therm2`为44 °C），这里没有采用。降频仍由内核thermal core完成，HAL只改写降频点，各档数值见`thermal/Thermal.cpp`。
- **过热时限制亮度**：Windows在`sys_therm2`–`sys_therm4`的温控区（包括`sys_therm3`上只限制GPU的那一个）降频时会同时降低屏幕背光，下限因温控区而异（`sys_therm2`为56%）。显示配置（`etc/displayconfig/display_port_0.xml`）据此按`sys-therm2`的`SKIN`等级限制最高亮度：`MODERATE`时约为最高亮度的78%，`SEVERE`及以上约为56%；当前亮度低于上限时不受影响，屏幕上的HDR内容也不受此上限限制。Windows几个会降低背光的温控区中，`sys_therm3`上只限制GPU的那一个降频点最低：用电池时与`sys_therm2`同为58 °C，接电源时为57 °C；性能模式下`sys_therm2`的降频点升至64 °C，而它不变。因此接电源时，本机开始限制亮度的温度比这一降频点高1 °C，性能模式下高6–7 °C。内核没有板载温区时不限制亮度。

### 2. 电源、电池与导航栏

- **Power HAL**：使用Lineage的`android.hardware.power-service.lineage-libperfmgr`，配置为`device/huawei/gaokun3/power/powerhint.json`：触摸、启动应用时提高top-app的uclamp下限，省电模式限制CPU频率上限，并为HWUI等的ADPF会话调整线程的uclamp；RT任务的uclamp默认值由HAL改为0；接电源时，温控HAL通过Power HAL的扩展接口开启`AC_POWER`模式，给CPU设频率下限；用电池时只给小核设较低的下限，并限制后台进程与离顶层较远的自由窗口的频率。取值依据见[power/README.md](../../device/huawei/gaokun3/power/README.md)。
- **电池与充电器状态**：由内核`ec_huawei_gaokun`驱动暴露`/sys/class/power_supply/gaokun-battery`与`gaokun-psy-ec`，对接`android.hardware.health-service.example`。
- **导航栏与侧滑返回手势**：平板设备必须在framework overlay（`overlay/frameworks/base/core/res/res/values/config.xml`）中声明`config_showNavigationBar = true`。在Android中，全屏边缘侧滑返回（Edge Back Gesture）的输入监听器由`NavigationBar` / `Taskbar`组件负责创建；若未启用该项，系统既无底部任务栏也无法响应侧滑返回手势。
- **游戏模式与高刷调度**：CPU/GPU调频、SurfaceFlinger合成锁与触控采样率优化详见[gaming.md](gaming.md)，深度休眠（S3 mem sleep）详见[deep-sleep.md](deep-sleep.md)。

## 四、无线网络（Wi-Fi 6E）与蓝牙（Bluetooth 5.2）

### 1. Wi-Fi（WCN6855 / `ath11k_pci`）

- **固件与初始化**：PCIe设备`0006:01:00.0`（WCN6855 hw2.1，上报chip_id 0x12）加载`/vendor/firmware/ath11k/WCN6855/hw2.1/`下的`amss.bin`、`m3.bin`、`board-2.bin`与`regdb.bin`。
- **板级数据（射频校准）**：`ath11k`以PCI ID、`qmi-chip-id=18`、`qmi-board-id=255`和设备树的`qcom,calibration-variant`组成名称，在`board-2.bin`中查找板级数据，找不到时去掉变体名再查一次。设备树使用`HW_GK3`（`patches/kernel/dts/0025`），这份数据在`linux-firmware`中也用于联想X13s的`qmi-chip-id=2`条目（X13s的`qmi-chip-id=18`条目与`NTM_TW220`共用另一份数据）。`linux-firmware`只在`qmi-chip-id=2`下登记了它，构建前需运行`scripts/wifi/ath11k-board-alias.py`补充`qmi-chip-id=18`的名称（见[`firmware/README.md`](../../device/huawei/gaokun3/firmware/README.md)）。三份候选数据的实测对比如下：
  - **`HW_GK3`**：固件按周围AP确定国家，2.4 GHz第12、13信道可被动扫描；5 GHz信号与吞吐量和`NTM_TW220`相同。
  - **`NTM_TW220`（此前使用）**：国家固定为US，无论设置哪个国家，2.4 GHz都只开放到第11信道（规则上限为2472 MHz），第12、13信道上的网络扫描不到。
  - **不指定变体（通用数据）**：信号减弱约22 dB，吞吐量只有前两者的几分之一。

  `HW_GK3`原本登记在`qmi-chip-id=2`的芯片上，用于本机时只对比过2.4 GHz和5 GHz，6 GHz尚未经实测。
- **持久化MAC地址生成**：因主线`ath11k`未从UEFI/NV读取出厂Wi-Fi MAC，`init.gaokun3.rc`在开机时运行`wpa_supplicant_mac_gen`脚本，在`/data/vendor/wifi/wlan_mac.bin`生成并持久化一个本地管理MAC地址，避免每次重启MAC变化导致路由器DHCP租约重置。
- **WPA3（SAE-H2E）与多频段**：支持2.4 GHz / 5 GHz / 6 GHz（802.11ax 160 MHz）及WPA2-PSK与WPA3-SAE（`sae_pwe=1` H2E）。
- **CMA内存预留与休眠恢复（`patches/kernel/dts/0011`）**：`ath11k`在每次挂起/恢复（或模块重初始化）时向DMA子系统申请高阶连续物理内存（order-9，2 MiB×32段）。设备树在`0xec000000`处配置512 MiB `shared-dma-pool`（`reusable`），确保系统长时间运行内存碎片化后`ath11k`仍能从CMA池稳定分配DMA缓冲。

### 2. 蓝牙（WCN6855 UART / `bt_linux_hci`）

- **内核HCI初始化**：蓝牙控制器挂载在UART `988000.serial`（`ttyHS1`），由内核`CONFIG_BT_HCIUART_QCA=y`自动加载`/vendor/firmware/qca/wcnhpbtfw21.tlv`与`wcnhpnv21g.bin`并注册Linux标准`hci0`接口。
- **Android AIDL桥接（`device/huawei/gaokun3/bt-hci/`）**：
  - 高通手机常用的vendor `libbt-vendor`假定用户态直接操作裸tty节点下载固件，会与主线内核的`btqca`驱动冲突。
  - 本项目采用`android.hardware.bluetooth-service.btlinux`，通过Linux内核提供的`AF_BLUETOOTH`（`BTPROTO_HCI`、`HCI_CHANNEL_USER`）独占打开`hci0`，将其桥接为Android标准的`android.hardware.bluetooth.IBluetoothHci` AIDL接口。

## 五、SELinux策略与块设备标签（`device/huawei/gaokun3/sepolicy/`）

设备侧SELinux策略位于`device/huawei/gaokun3/sepolicy/`，遵循以下原则：

1. **自定义服务与节点域隔离**：
   - 为自研HAL配置独立安全上下文：`hal_camera_default`、`hal_sensors_default`、`hal_thermal_default`、`hal_bluetooth_btlinux`、`hexagonrpcd`。
   - 补齐AOSP默认策略未覆盖的主线内核设备节点：`/dev/media[0-9]+`与`/dev/v4l-subdev[0-9]+`标为`video_device`；`/dev/fastrpc-adsp`與`/dev/fastrpc-sdsp`标为`qdsp_device`；`/dev/dri/renderD128`标为`gpu_device`。
2. **块设备按GPT分区名匹配而非分区号**：
   - 用户通过双系统安装器（Windows收缩分区后追加）与全盘安装创建的NVMe分区号（`nvme0n1pX`）并不相同。
   - 因此`file_contexts`中对`super`、`userdata`、`metadata`、`misc`、`esp`的标签统一绑定在`/dev/block/platform/soc@0/1c08000.pcie/pci0000:00/.../by-name/<name>`符号链接路径上，不得硬编码`nvme0n1p*`编号。

## 六、内置Root方案（`ReSukiSU`）

内核通过`patches/resukisu/`集成`ReSukiSU`（基于KernelSU架构的内核态Root实现）：

- **构建要求**：`ReSukiSU`依赖C11复合字面量与声明混排语法，`patches/resukisu/0002`为其Makefile移除内核顶层的`-Wvla`与`-Wdeclaration-after-statement`警告升级。
- **使用说明**：内核内置ReSukiSU，检测root的应用可能拒绝运行。

