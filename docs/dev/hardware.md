# 硬件清单与平台基线（HUAWEI MateBook E Go / `gaokun3`）

本文汇总HUAWEI MateBook E Go性能版（型号`GK-W7X`、代号`gaokun3`）的硬件拓扑、显示与输入总线、USB控制器分工、固件清单及崩溃日志持久化机制。

---

## 一、平台概览

| 项目 | 规格与内核标识 |
|---|---|
| 机型与代号 | HUAWEI MateBook E Go（`GK-W7X`，SKU `C233`），设备树`qcom,sc8280xp-huawei-gaokun3` |
| SoC | Qualcomm Snapdragon 8cx Gen 3（`SC8280XP`，8核Kryo：4×Prime + 4×Gold） |
| 内存与存储 | 16 GB LPDDR4X，NVMe SSD（PCIe，预留`linux-cma` 512 MiB于`0xec000000`） |
| 显示面板 | 12.35英寸1600×2560 IPS（60 Hz / 120 Hz），MIPI DSI-1直连，Himax HX83121A DDIC |
| 显卡 | Adreno 690（`0x06090000`，8档OPP：270–690 MHz，GMU `a660_gmu.bin`） |
| 无线与蓝牙 | Qualcomm WCN6855（Wi-Fi 6E `ath11k_pci` `0002:01:00.0`，蓝牙5.2 UART `988000.serial` / `hci0`） |
| 音频子系统 | LPASS + SoundWire：WCD9380/WCD9385耳放编解码器 + 双路WSA8830智能功放（4扬声器） + 板载DMIC |
| 相机子系统 | Qualcomm CAMSS（Titan 170）：后摄OmniVision OV13B10（`@0x36` + DW9714 VCM `@0x0c`），前摄Hynix Hi-846（`@0x20`） |
| 传感器中枢 | SLPI DSP（`qcslpi8280.mbn`）+ QMI SSC：ICM42607（IMU）、AK09918（磁力计）、TCS3408（颜色/光感）、SAR、霍尔传感器 |
| 电源与EC | Huawei Matebook E Go EC（I2C `@0x76`，电源适配器`gaokun-psy-ec`、电池`gaokun-battery`、Type-C连接器状态） |
| 指纹传感器 | FocalTech `FTE7001`（侧边电源键集成，SPI接口由TrustZone安全世界独占，通过QSEECOM `fingerprint` TA访问） |

---

## 二、BIOS版本警告：2.16与8cx Gen 2固件不可混用

本项目的设备树与固件解包基线为**BIOS 2.16**（`2023-07-24`）。

切勿刷入互联网上流通的`Gaokun_8C_BIOS_..._2.17.exe`——该包面向**8cx Gen 2（SC8180X）标准版**而非8cx Gen 3（SC8280XP）性能版，两者在ACPI `DSDT`中的引脚分配存在多处硬件冲突：

| 子系统 | BIOS 2.16（SC8280XP `gaokun3`） | 2.17包（SC8180X标准版） | 混用后果 |
|---|---|---|---|
| 触控/手写笔SPI总线 | `SPI7`（`998000.spi`，12 MHz） | `SPI1` | 触控与手写笔无法探测 |
| 触控复位与中断 | Reset `GPIO 99`、IRQ `GPIO 175` | Reset `GPIO 54`、IRQ `GPIO 113` | 复位与中断完全错位 |
| 面板ID检测引脚 | `GPIO 102`（`ID0`）、`GPIO 91`（`ID1`） | `GPIO 178`、`GPIO 179` | 在`gaokun3`上`GPIO 178/179`为左右WSA8830扬声器功放的`powerdown-gpios`，误配会导致面板与扬声器互相干扰 |

BIOS 2.16的UEFI DXE卷结构与显示表参数详见[bios-2.16/README.md](hw/bios-2.16/README.md)。

---

## 三、显示与输入总线

### 1. DRM/KMS与DSI面板

- **DRM驱动**：`msm`（`ae00000.display-subsystem`，DPU 8.1），连接器`DSI-1`（status `connected`，物理尺寸166×265 mm）。
- **显示模式**：
  - `1600x2560@120`：像素时钟313.524 MHz，`htotal=1912`、`vtotal=2736`（默认首选模式）。
  - `1600x2560@60`：像素时钟313.524 MHz，`htotal=1912`、`vtotal=5472`（通过将`vtotal`翻倍保持相同DSI link clock实现无闪切60 Hz，见`patches/0107`）。
- **图层与格式**：DPU提供6个CRTC与25个plane，支持`XR24`/`AR24`/`XB24`/`AB24`/`RG16`/`NV12`/`P010`等格式，均同时支持`LINEAR`与Qualcomm UBWC（`QCOM_COMPRESSED`，modifier `0x0800000000000001`）。
- **背光**：MIPI DCS背光（`/sys/class/backlight/ae96000.dsi.0`，范围`0..4095`）。
- **多供应商面板识别**（`patches/0123`、`0124`）：通过TLMM `GPIO 102`（`ID0`）与`GPIO 91`（`ID1`）在probe时读取硬件编码（`ID0=0, ID1=1` → BOE `0x0F`；`ID0=0, ID1=0` → CSOT `0x0D`），色彩与Gamma校详见[display-color.md](display-color.md)。

### 2. 触控、手写笔与键盘盖

- **触控与手写笔控制器（Himax HX83121A）**：
  - 挂载在`998000.spi`（`spi0.0`），驱动为`himax-hx83121a-spi`（`drivers/hid/hid-himax-83121a.c`）。
  - **关键引脚**：
    - `GPIO 174`：传输模式选择引脚（**低电平=SPI模式**，高电平=I2C HID `@0x4f`模式）。驱动必须在拉低`GPIO 99`触发触控复位并加载`/vendor/firmware/Touch_fw.bin`之前将`GPIO 174`驱动为低电平，否则芯片回退至I2C模式，SPI总线上只能读到全`0x00`或全`0xFF`。
    - `GPIO 99`：触控控制器复位（Touch Reset）。
    - `GPIO 38`：显示面板复位（Display Reset）。
    - `GPIO 175`：触控中断（下降沿触发）。
  - 注册两个独立输入设备：`Himax HX83121A Touch`（多点触控）与`Himax HX83121A Stylus`（MPP手写笔，含距离、倾斜与双侧键，详见[stylus.md](stylus.md)与[palm-rejection.md](palm-rejection.md)）。
- **原装磁吸键盘与触控板**：
  - 通过底部POGO触点挂在内部USB控制器`a400000.usb`（USB2，`12d1:10b8`）：接口0为键盘（`Huawei HID Keyboard Device Keyboard`），接口1为多点触控板（`Huawei HID Keyboard Device Touchpad`）。
- **物理按键**：`gpio-keys`提供音量加/减键；PMIC `pm8941_pwrkey`（`/dev/input/event0`）提供电源键。

---

## 四、USB控制器分工

SC8280XP在`gaokun3`上启用了3个DWC3控制器，各自的角色能力不同：

| 控制器地址 | DT别名 | 物理位置 | 支持角色（`role`） | 说明 |
|---|---|---|---|---|
| `a600000.usb` | `usb_0` | **靠近电源键的机身USB-C口**（Type-C `port0`） | `host` / `device`（OTG） | 唯一支持peripheral（`udc` / USB Gadget / `adb`）的端口，绑定`a600000.usb-role-switch` |
| `a800000.usb` | `usb_1` | **另一侧机身USB-C口**（Type-C `port1`） | 仅`host` | 硬件与设备树配置为纯Host模式，连接电脑不会枚举USB `adb` |
| `a400000.usb` | `usb_2` | 底部磁吸键盘POGO接口 | 仅`host` | 内部多端口控制器，专用于原装键盘盖 |

- **USB角色切换与休眠行为**：
  - `pmic_glink.ucsi`通过`typec_Port0`通知`a600000.usb-role-switch`切换Host/Device角色。
  - 当Type-C端口未连接对端或UCSI报告断开时，控制器切回`host`或`none`并撤下Gadget（`configfs-gadget.g1`）。调试脚本可通过写入`/sys/class/usb_role/a600000.usb-role-switch/role`手动强制`device`模式。

---

## 五、专用固件清单（`/vendor/firmware/`与ramdisk）

以下专有固件从Windows驱动包或本机Windows分区（`/mnt/win/Windows/System32/DriverStore/FileRepository/`）提取，体积较大或受版权限制，不直接检入Git仓库（见`.gitignore`与[firmware/README.md](../../device/huawei/gaokun3/firmware/README.md)）：

| 相对路径（位于`/vendor/firmware/`及ramdisk `/lib/firmware/`） | 子系统 | 用途说明 |
|---|---|---|
| `Touch_fw.bin` | 触控 / 手写笔 | Himax HX83121A 128 KB SRAM固件（每次亮屏恢复时由SPI驱动重载） |
| `qcom/a660_sqe.fw`、`qcom/a660_gmu.bin` | GPU | Adreno 690命令处理器微码与GMU电源管理固件 |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcdxkmsuc8280.mbn` | GPU | Adreno 690受保护上下文切换（Zap Shader）签名固件 |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn` + `adsp*.jsn` | ADSP | 音频、电池/UCSI（`pmic_glink`）远端处理器固件 |
| `qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn` + `cdsp*.jsn` | CDSP | 计算DSP远端处理器固件 |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn` + `slpi*.jsn` | SLPI | 传感器中枢DSP固件（配合`/vendor/etc/sensors/config/`与`sns.reg`使用，见[sensors-ssc-protocol.md](sensors-ssc-protocol.md)） |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn` | 视频解码 | Iris / Venus硬件视频编解码器固件（`VIDEO.VE.6.0-00055-PROD-1`） |
| `qcom/sc8280xp/SC8280XP-HUAWEI-GAOKUN3-tplg.bin` | 音频拓扑 | Audioreach（`q6apm`）声卡拓扑文件 |
| `ath11k/WCN6855/hw2.1/{amss.bin,m3.bin,board-2.bin,regdb.bin}` | Wi-Fi | WCN6855 PCIe无线网卡固件与板级校准数据 |
| `qca/wcnhpbtfw21.tlv`、`qca/wcnhpnv21g.bin` | 蓝牙 | WCN6855蓝牙控制器补丁与NVM配置 |

---

## 六、崩溃日志持久化（`efi_pstore` vs `ramoops`）

在`gaokun3`上调试内核panic或早启动死机时，需注意以下硬件与固件特性：

1. **DRAM在每次复位时均被UEFI重初始化**：
   - 高通XBL/UEFI在每次Warm Reset（含`reboot`及`panic_timeout`触发的自动重启）时都会重新训练并清零DRAM。
   - 因此基于保留内存的`ramoops`在本机上无法跨重启保存任何数据。若在设备树中添加`ramoops`节点，还会因Linux `pstore`仅允许单个活跃后端（`pstore_register()`检查`psinfo != NULL`）而抢占`efi_pstore`槽位，导致真正可用的持久化后端被屏蔽。
2. **使用`efi_pstore`（基于Qualcomm `uefisecapp`）**：
   - 本机内核命令行默认带有`efi=noruntime`，但Qualcomm平台通过`qcom_scm` + `qsee_uefisecapp`（`drivers/firmware/qcom/qcom_qseecom_uefisecapp.c`）将自定义`efivar_ops`直接注册到安全世界，**不依赖标准EFI Runtime Services**。
   - **初始化时序修复（`patches/0001`）**：内置`efi_pstore`在`~0.28s`初始化时检查`efivar_supports_writes()`，而`qsee_uefisecapp`直到`~0.76s`才完成探测并调用`efivars_register()`。`patches/0001`在`drivers/firmware/efi/vars.c`中引入`efivar_ops_nh`通知链与工作队列，当`efivars_register()`稍后上线时自动触发`efi_pstore`补注册，从而使内核panic日志稳定写入`/sys/fs/pstore/dmesg-efi_pstore-*`。

