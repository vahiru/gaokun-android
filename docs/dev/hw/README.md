# 硬件原始数据与拆包清单

## `bios-2.16/`：BIOS 2.16升级包拆包

拆包脚本、分析脚本和逐模块清单（名字 / GUID / 大小 / sha256 / depex）。固件二进制本身不入库，[`bios-2.16/README.md`](bios-2.16/README.md)记录了从官方安装包逐字节复现的步骤。

## `ov13b10-module-eeprom-0x50.bin`：后摄模组EEPROM（16 KiB）

后摄模组（OV13B10，CCI总线0 = `/dev/i2c-1`）上`0x50`地址EEPROM的完整内容，16位地址、16384字节、前后两半不重复（不是8 KiB镜像）。

**读取方式**：CCI适配器不支持`I2C_RDWR`的组合消息（`i2ctransfer`报`ioctl 707: Operation not supported`），因此采用SMBus两步法：`i2cset -y 1 0x50 <hi> <lo> b`写地址指针，随后用`i2cget -y 1 0x50`（receive byte）逐字节读取，地址自增。该EEPROM挂在与面板VDDI共用的1.8 V轨上，屏幕点亮期间无需单独给相机传感器上电即可应答。

**已确认的字段布局**（其余偏移为厂商私有布局）：

| 偏移 | 内容 |
| --- | --- |
| `0x0000-0x0005` | `16 0b 0f 91 06 00`头 |
| `0x0006-0x0024` | ASCII模组标识`123060401622BF02AXD702Y67000000` |
| `0x0025-0x0037` | 疑似AWB标定数据（`07 68 / 01 1b / 1a 74 …`，形态对应R/G、B/G均值对） |
| `0x0afc-0x0e1e` | 平滑二维表，值域`0x56-0x65`（镜头阴影LSC表形态） |
| `0x2400-0x2e00` | 其他二维标定表 |

用途：后续为libcamera的`ov13b10.yaml`提取AWB标定值与LSC表的原始数据（详见[`../camera.md`](../camera.md)）。
