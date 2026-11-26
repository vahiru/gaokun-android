# Android系统移植与子系统集成（crDroid 13 / Android 17）

本文档汇总`gaokun3`从UEFI引导进入Android用户态的启动链路、内核配置强约束、核心HAL（电源/温控/无线/蓝牙）、SELinux策略及内置`ReSukiSU`集成方案。

## 一、UEFI启动链路与动态分区布局

### 1. 双阶段引导流程

`gaokun3`不采用传统高通手机的ABL `boot.img`直接引导，而是由UEFI加载`systemd-boot`（`\EFI\BOOT\BOOTAA64.EFI`），再由默认BLS条目启动统一启动入口`gk3boot.efi`（详见[`tools/gk3boot/README.md`](../../tools/gk3boot/README.md)）：

```
UEFI 2.16 (关闭Secure Boot)
  └──► \EFI\BOOT\BOOTAA64.EFI (systemd-boot)
         ├── 默认入口：\EFI\gk3boot\<ver>\gk3boot.efi
         │      └── 读NVMe上的misc与boot_{a,b}裸分区（Boot Image v2：kernel / ramdisk / dtb / cmdline），经Linux EFI Stub启动
         └── 直连回退：\<machine-id>\android\slot_{a,b}\{Image,ramdisk.img,gaokun3.dtb}
```

1. **第一阶段`init`（Ramdisk）**：
   - 内核挂载ramdisk并执行`/init`（即`first_stage_init`）。
   - 解析内核命令行中的`androidboot.slot_suffix=_a`（或`_b`）与ramdisk根目录的`/fstab.gaokun3`（vendor副本位于`/vendor/etc/fstab.gaokun3`），在NVMe磁盘上定位`super`与`userdata`分区。
   - 通过`dm-linear`从`super`镜像解析并映射逻辑分区`system_a`、`system_ext_a`、`product_a`、`vendor_a`，挂载`/system`并切换根目录（`switch_root`）进入第二阶段`/system/bin/init`。
2. **内核命令行来源**：
   - `device/huawei/gaokun3/BoardConfig.mk`中的`BOARD_KERNEL_CMDLINE`写入AOSP产出的`boot.img`头部。
   - `gk3boot.efi`启动Android时直接以`boot_x`头部的cmdline为基底并追加槽位与状态参数；OTA `postinstall`与`gk3-fastbootd`刷写`boot_x`时也会同步将`boot.img`解出的cmdline写入ESP直连回退条目（`\loader\entries\<machine-id>-android-{a,b}.conf`）。
3. **Virtual A/B与回滚注意事项**：
   - 通过`update_engine`执行OTA更新后，`super`分区的LP元数据仅保留目标槽位的一套逻辑分区（可通过`lpdump`核实）。`bootctl is-slot-bootable`仅读取`misc`分区标志位，不能代表`super`内仍存在旧槽逻辑分区。

## 二、Android必选内核配置（`scripts/kernel-config-android.sh`）

主线`defconfig`默认面向通用Linux发行版，大量驱动配为`=m`且缺少Android专属内核特性。由于本项目的Android镜像不部署`/vendor/lib/modules`模块树，基础`defconfig`配合`scripts/kernel-config-android.sh`确保所有板级驱动与Android子系统内建（`=y`）：

| 类别 | 关键Kconfig项与补丁 | 缺失时的典型故障 |
|---|---|---|
| IPC与内存 | `ANDROID_BINDER_IPC=y`、`ANDROID_BINDERFS=y`、`ASHMEM=y`（`patches/kernel/core/0004`恢复`ashmem`）、`PSI=y`、`TRANSPARENT_HUGEPAGE_MADVISE=y`、`LRU_GEN_ENABLED=y`、`UCLAMP_BUCKETS_COUNT=20` | `servicemanager`/`lmkd`无法启动，或依赖`/dev/ashmem`的应用崩溃；uclamp桶数不足时Power HAL的非焦点窗口限频无法生效 |
| BPF与文件系统 | `BPF_SYSCALL=y`、`CGROUP_BPF=y`、`FS_VERITY=y`、`F2FS_FS=y`、`DM_SNAPSHOT=y` | `bpfloader`失败触发`init`重启，或Virtual A/B快照映射失败 |
| `netd`网络栈 | `NETFILTER_XT_MATCH_QUOTA2=y`（`patches/kernel/core/0005`移植AOSP `xt_quota2`）、`IP_NF_RAW=y`、`IP6_NF_RAW=y`、`NETFILTER_XT_TARGET_CT=y`、`NETFILTER_XT_MATCH_OWNER=y`、`NETFILTER_XT_MATCH_BPF=y`、`NET_CLS_BPF=y`、`NET_SCH_INGRESS=y` | `netd`在`BandwidthController::enableBandwidthControl()`初始化时报错退出，触发60秒rescue party重启循环 |
| 远端处理器与QMI | `QCOM_Q6V5_PAS=y`、`QCOM_SYSMON=y`、`QCOM_FASTRPC=y`、`QCOM_PD_MAPPER=y`、`QRTR=y`、`QRTR_SMD=y`、`QRTR_MHI=y` | ADSP/CDSP/SLPI与Wi-Fi QMI无法建立通信 |
| 板级外设驱动 | `DRM_MSM=y`、`TOUCHSCREEN_HIMAX_HX83121A_SPI=y`、`TOUCHSCREEN_HIMAX_HX83121A_PEN=y`、`ATH11K_PCI=y`、`CFG80211_CERTIFICATION_ONUS=y`、`ATH_REG_DYNAMIC_USER_REG_HINTS=y`、`BT_HCIUART_QCA=y`、`EC_HUAWEI_GAOKUN=y`、`TYPEC_UCSI=y`、`QCOM_SPMI_ADC_TM5=y`、`HUAWEI_GAOKUN3_CAMERA_PROBER=y`、`VIDEO_S5K4H7=y`、`VIDEO_S5K3L6=y`、`GPIO_SHARED_PROXY=y`、`GAOKUN3_FPCMD=y`、`INPUT_JOYSTICK=y` | 屏幕、触控/手写笔、Wi-Fi及国家码、蓝牙、EC电池/Type-C、板载温控区、前后摄模组探测、侧边指纹或有线游戏手柄不可用 |

## 三、核心HAL与系统服务集成

### 1. 自研温控HAL（`device/huawei/gaokun3/thermal/`）

- **为何必须替换AOSP示例实现**：AOSP默认的`android.hardware.thermal-service.example`上报硬编码的模拟温度，并将`SKIN`与`BATTERY`的`SHUTDOWN`阈值设为`36.0 °C`；而`gaokun3`正常空载运行时热区温度即在`36–38 °C`之间，若不排除该mock APEX，系统会在开机数分钟内由`ThermalManagerService`触发强制关机。
- **实现机制**：`android.hardware.thermal-service.gaokun3`读取`/sys/class/thermal/thermal_zone*`下的CPU、GPU、内存、PMIC与板载热敏电阻温区。CPU/GPU温区按裸片温度分级，105 °C时由Android有序关机。板载热敏电阻`sys-therm1`–`sys-therm4`各作为一个`SKIN`温度上报，阈值以各自当前最低的被动降频点t为准：`LIGHT`为t-2，`MODERATE`为t（内核从这里开始降频），`SEVERE`为t+4（降频已接近最低保留性能），`CRITICAL`为t+8，`EMERGENCY`为t+14，`SHUTDOWN`为95 °C；等级降低要等温度比当前等级的阈值低2 °C。`sys-therm3`最低的降频点只限制GPU，它的等级跟随GPU降频点。框架取最高的`SKIN`等级作为整机状态，并逐路计算温度余量；在长时间高负载下整机状态会达到`MODERATE`或`SEVERE`，JobScheduler随之推迟后台任务。内核没有这些温区时，`SKIN`退回CPU/GPU温区的最大值。这几路都不是直接测得的外壳温度。
- **板载温控与性能模式**：内核（`patches/kernel/dts/0031`）按本机Windows ACPI表的开机默认值，在`sys-therm1`–`sys-therm4`上限制CPU与GPU。降频的加深与减轻也沿用Windows的被动降温公式（`patches/kernel/core/0011`与`dts/0032`，系数取自DSDT）：每个采样周期（多为5秒）按`_TC1`×（温度较上一周期的变化）+`_TC2`×（温度−降频点）的正负号加深或减轻一档，所以温度停在降频点以上时降频会继续加深，直到温度回落或达到最低保留性能。Windows按这个值的大小决定性能下调的幅度，这里与Linux的ACPI温控驱动一样只取正负号。几个温区共用同一组降温设备，同时高于降频点时每个周期各加深一档；Windows则按各温区分别计算，取其中最严格的一个。Windows运行时还会按EC上报的状态和华为的性能模式改写这些降频点，HAL采用其中的两部分：「设置 → 电池」里的性能模式开关（`persist.sys.gaokun3.perf_mode`）开启时，`sys-therm1`–`sys-therm3`使用华为性能模式的数值，用电池时也可以开启；关闭时，接电源（`gaokun-ec-adapter`的`online`为1）使用EC事件中按推测对应接电源的一套数值，未接电源使用开机默认值。Windows用电池时的数值更保守（例如`sys-therm2`为44 °C），这里没有采用。降频仍由内核thermal core完成，HAL只改写降频点，各档数值见`thermal/Thermal.cpp`。
- **过热时限制亮度**：Windows在`sys_therm2`–`sys_therm4`的温控区（包括`sys_therm3`上只限制GPU的那一个）降频时会同时降低屏幕背光，下限因温控区而异（`sys_therm2`为56%）。显示服务只能跟随一路传感器，所以温控HAL另报一路`DISPLAY`类型的`panel`，等级取`sys-therm2`–`sys-therm4`中最高的等级；三路各自在当前最低的降频点报告`MODERATE`，所以与Windows一样，任一路开始降频时亮度即受限。显示配置（`etc/displayconfig/display_port_0.xml`）按`panel`的等级限制最高亮度：`MODERATE`时约为最高亮度的78%，`SEVERE`及以上统一按`sys_therm2`的下限取约56%；当前亮度低于上限时不受影响，屏幕上的HDR内容也不受此上限限制。`panel`不计入整机温控状态，也不改变CPU与GPU的降频。内核没有板载温区时不上报`panel`，亮度不受限制。

### 2. 电源、电池与导航栏

- **Power HAL**：使用Lineage的`android.hardware.power-service.lineage-libperfmgr`，配置为`device/huawei/gaokun3/power/powerhint.json`：触摸、启动应用时提高top-app的uclamp下限，省电模式限制CPU频率上限，并为HWUI等的ADPF会话调整线程的uclamp；RT任务的uclamp默认值由HAL改为0；接电源时，温控HAL通过Power HAL的扩展接口开启`AC_POWER`模式，给CPU设频率下限；用电池时只给小核设较低的下限，并限制后台进程与离顶层较远的自由窗口的频率。取值依据见[power/README.md](../../device/huawei/gaokun3/power/README.md)。
- **电池与充电器状态**：由内核`ec_huawei_gaokun`驱动暴露`/sys/class/power_supply/gaokun-ec-battery`与`gaokun-ec-adapter`，对接`android.hardware.health-service.example`（锁屏充电信息补丁`patches/android/system-core/0001`按已识别电池节点读取）。
- **导航栏与侧滑返回手势**：平板设备必须在framework overlay（`overlay/frameworks/base/core/res/res/values/config.xml`）中声明`config_showNavigationBar = true`。在Android中，全屏边缘侧滑返回（Edge Back Gesture）的输入监听器由`NavigationBar` / `Taskbar`组件负责创建；若未启用该项，系统既无底部任务栏也无法响应侧滑返回手势。
- **游戏兼容、内存与待机**：游戏兼容性、内存与调度调优详见[gaming.md](gaming.md)，深度待机（`s2idle`）详见[deep-sleep.md](deep-sleep.md)。

## 四、无线网络（Wi-Fi 6E）与蓝牙（Bluetooth 5.2）

### 1. Wi-Fi（WCN6855 / `ath11k_pci`）

- **固件与初始化**：PCIe设备`0006:01:00.0`（WCN6855 hw2.1，上报chip_id 0x12）加载`/vendor/firmware/ath11k/WCN6855/hw2.1/`下的`amss.bin`、`m3.bin`、`board-2.bin`与`regdb.bin`。
- **板级数据（射频校准）**：`ath11k`以PCI ID、`qmi-chip-id=18`、`qmi-board-id=255`和设备树的`qcom,calibration-variant`组成名称，在`board-2.bin`中查找板级数据，找不到时去掉变体名再查一次。设备树使用`HW_GK3`（`patches/kernel/dts/0025`），这份数据在`linux-firmware`中也用于联想X13s的`qmi-chip-id=2`条目（X13s的`qmi-chip-id=18`条目与`NTM_TW220`共用另一份数据）。`linux-firmware`只在`qmi-chip-id=2`下登记了它，构建前需运行`scripts/wifi/ath11k-board-alias.py`补充`qmi-chip-id=18`的名称（见[`firmware/README.md`](../../device/huawei/gaokun3/firmware/README.md)）。三份候选数据的实测对比如下：
  - **`HW_GK3`**：固件按周围AP确定国家，2.4 GHz第12、13信道可被动扫描；5 GHz信号与吞吐量和`NTM_TW220`相同。
  - **`NTM_TW220`（此前使用）**：国家固定为US，无论设置哪个国家，2.4 GHz都只开放到第11信道（规则上限为2472 MHz），第12、13信道上的网络扫描不到。
  - **不指定变体（通用数据）**：信号减弱约22 dB，吞吐量只有前两者的几分之一。

  `HW_GK3`原本登记在`qmi-chip-id=2`的芯片上，用于本机时只对比过2.4 GHz和5 GHz，6 GHz尚未实测。
- **国家码**：固件默认根据周围AP通告的国家（802.11d）确定国家码。Android框架另有自己的国家码（本机没有基带，由扫描结果推断，见`Gaokun3WifiOverlay`），经以下路径下发到网卡：
  1. 框架调用`ISupplicantStaIface.setCountryCode`，wpa_supplicant将其转为私有驱动命令`COUNTRY XX`，由私有命令库[`wifi/wpa_supplicant_8_lib`](../../device/huawei/gaokun3/wifi/wpa_supplicant_8_lib/)处理，以`NL80211_CMD_REQ_SET_REG`发给内核。
  2. cfg80211把请求交给`ath11k_reg_notifier`，由它把国家码发给固件。内核未开启`CONFIG_ATH_REG_DYNAMIC_USER_REG_HINTS`时，该函数直接返回，国家码不会送达固件。
  3. 固件按新的国家重新下发信道表。`config_wifiStaDynamicCountryCodeUpdateSupported`使框架在连接期间也会下发：当前信道仍允许时连接保持，否则cfg80211在60秒的宽限期后断开连接。

  cfg80211同时需要无线监管数据库（[`wifi/regdb`](../../device/huawei/gaokun3/wifi/regdb/README.md)，安装至ramdisk的`/lib/firmware/`），否则下发国家码后会反复等待CRDA超时约30秒。

  需要注意的行为：
  - 框架下发与网卡当前不同的国家码后，本次开机内`ath11k`不再进行802.11d扫描，此后国家码只随框架下发而改变；与网卡当前相同的国家码不会产生影响，固件仍按802.11d确定国家。Wi-Fi开启期间固件崩溃时，驱动会在恢复后重新下发保存的国家码。
  - 没有基带时，框架按扫描结果推断国家码（`WifiCountryCode.findCountryCodeFromScanResults`）：同一批扫描结果中，信号不低于-85 dBm且带国家信息的条目必须全部通告同一个国家，并且至少有2条（US为3条；按BSSID计数，同一台路由器的两个频段分别计入），框架才会采用并下发。周围AP通告的国家不一致时（例如边境附近），框架不会更新国家码，直到某次扫描结果重新一致。
  - cfg80211为self-managed的网卡设置了`REGULATORY_COUNTRY_IE_IGNORE`，连接或断开AP本身不会改变监管域，AP通告的国家只经固件的802.11d扫描和框架的推断起作用。`ath11k`在关联AP后停止802.11d扫描，断开后重新开始，所以在框架下发不同的国家码之前，连接期间网卡的国家不随周围AP变化。
  - 内核未开启`CONFIG_ATH_REG_DYNAMIC_USER_REG_HINTS`时，框架下发国家码后全局监管域仍会改变并广播事件，`cmd wifi get-country-code`会显示新的国家，但网卡不变。排查时以`iw reg get`中`phy#N (self-managed)`一节为准。
- **持久化MAC地址生成**：因主线`ath11k`未从UEFI/NV读取出厂Wi-Fi MAC，系统基于`/sys/devices/soc0/serial_number`通过FNV-1a 64确定性派生稳定的本地管理MAC地址：开机阶段由`device/huawei/gaokun3/bin/gaokun3-wlan-ap.sh`在`wlan0`启用前写入，`patches/android/hardware-interfaces/0013`在Wi-Fi HAL侧返回同一派生出厂MAC，并配合`Gaokun3WifiOverlay`中的`config_wifiSaveFactoryMacToWifiConfigStore=false`，避免每次重启MAC变化导致路由器DHCP租约重置。
- **WPA3（SAE-H2E）与多频段**：支持2.4 GHz / 5 GHz / 6 GHz（802.11ax 160 MHz）及WPA2-PSK与WPA3-SAE（`sae_pwe=1` H2E）。
- **CMA内存预留与休眠恢复（`patches/kernel/dts/0011`）**：`ath11k`在每次挂起/恢复（或模块重初始化）时向DMA子系统申请高阶连续物理内存。设备树将`linux,cma`配置为4 GiB以下（`alloc-ranges = <0x0 0x80000000 0x0 0x80000000>`）的128 MiB `shared-dma-pool`（`reusable`），确保系统长时间运行内存碎片化后`ath11k`仍能从低4 GiB CMA池稳定分配DMA缓冲。

### 2. 蓝牙（WCN6855 UART / `HCI_CHANNEL_USER`）

- **内核HCI初始化**：蓝牙控制器作为内核`serdev`挂载在UART `988000.serial`（`988000.serial:0.0`，不经用户态tty节点），由内核`CONFIG_BT_HCIUART_QCA=y`自动加载`/vendor/firmware/qca/wcnhpbtfw21.tlv`与`wcnhpnv21g.bin`并注册Linux标准`hci0`接口。
- **Android AIDL桥接**：
  - 高通手机常用的vendor `libbt-vendor`假定用户态直接操作裸tty节点下载固件，会与主线内核的`btqca`驱动冲突。
  - 本项目直接使用AOSP自带的`android.hardware.bluetooth-service.default`（`hardware/interfaces/bluetooth/aidl/default/BluetoothHci.cpp`），通过Linux内核提供的`AF_BLUETOOTH`（`BTPROTO_HCI`、`HCI_CHANNEL_USER`）独占打开`hci0`，将其桥接为Android标准的`android.hardware.bluetooth.IBluetoothHci` AIDL接口。

## 五、SELinux策略与块设备标签（`device/huawei/gaokun3/sepolicy/`）

设备侧SELinux策略位于`device/huawei/gaokun3/sepolicy/`，遵循以下原则：

1. **自定义服务与节点域隔离**：
   - 为自研HAL与服务配置独立安全上下文：`hal_camera_default`、`hal_sensors_default`、`hal_thermal_default`、`hal_power_default`、`hal_fingerprint_default`、`hal_usb_default`、`hexagonrpcd`等。
   - 补齐AOSP默认策略未覆盖的主线内核设备节点：`/dev/media[0-9]+`与`/dev/v4l-subdev[0-9]+`标为`video_device`；`/dev/fastrpc-[a-z]+`标为`gaokun3_fastrpc_device`（`ueventd.gaokun3.rc`仅开放`/dev/fastrpc-sdsp`与`/dev/fastrpc-cdsp`）；`/dev/dri/renderD128`标为`gpu_device`。
2. **块设备按GPT分区名匹配而非分区号**：
   - 用户通过双系统安装器（Windows收缩分区后追加）与全盘安装创建的NVMe分区号（`nvme0n1pX`）并不相同。
   - 因此`file_contexts`中对`esp`、`EFI_system_partition`、`misc`、`metadata`、`boot_a`、`boot_b`、`super`、`userdata`的标签统一绑定在`/dev/block/by-name/<name>`符号链接路径上（本机NVMe位于`1c20000.pcie/pci0002:00`），不得硬编码`nvme0n1p*`编号。

## 六、内置Root方案（`ReSukiSU`）

内核通过`patches/resukisu/`集成`ReSukiSU`（基于KernelSU架构的内核态Root实现）：

- **构建要求**：主线Linux 7.2+内核移除了`strncpy()`，`patches/resukisu/0002`为`ReSukiSU`恢复兼容实现`ksu_compat_strncpy()`（详见[`patches/resukisu/README.md`](../../patches/resukisu/README.md)）。
- **使用说明**：内核内置ReSukiSU，检测root的应用可能拒绝运行。

