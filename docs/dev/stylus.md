# 手写笔（华为M-Pencil）

跟踪：issue [#26](https://github.com/vahiru/gaokun-android/issues/26)。

## 功能与适用范围

支持二代和三代M-Pencil：压感、悬停、倾斜；笔身侧边双击在笔与橡皮擦（Android的`TOOL_TYPE_ERASER`）之间切换；系统休眠恢复后照常可用；电量显示在"设置 → 已连接的设备 → 触控笔"，电量低时系统提醒充电。"设置 → 已连接的设备 → 手写笔设置"（Parts的`StylusSettingsActivity`）可以调节四档笔迹平滑，或把侧边双击改为侧键单击、关闭。

一代M-Pencil（CD52）使用另一种协议（HPP2），驱动不支持；三代M-Pencil的日常使用效果尚未经系统实测，压感若超过4095会被截断。

## 数据通路

笔的数据来自两处，由用户态守护进程`gk3pend`合成为一支`uinput`触控笔。坐标与触摸屏是同一套像素坐标，以1/16像素为单位上报（`ABS_X` 0–25599、`ABS_Y` 0–40959）。

* **触摸从片（位置、悬停、倾斜）**：屏幕的触摸控制器是一对HX83121A，主片在`spi6`（由`himax-spi`驱动，负责手指），感应笔的级联从片在`spi20`（ACPI中的`THPB`/`HIMX0002`），中断在GPIO 39，没有独立复位线。初始化写入`raw_out_sel = 0xF6`与rawdata地址`0x5AA5`后，从片在中断上输出339字节的帧，包含笔尖（TX1）与笔尖上方环形电极（TX2）各自的锚点和9×9网格。位置与悬停取自TX1，倾斜取自TX1与TX2的偏移。
* **笔MCU（压感、侧键、电量）**：平板内部USB设备`12d1:10b8`是笔的接收端：
  * 接口0的`hidraw`（报文`0x55`）：压感，每包4个`u16`槽。笔一拿起报文即开始发送，`gk3pend`用它提前唤醒从片。
  * 接口1的厂商bulk通道（OUT `0x02`、IN `0x85`）：侧边双击（事件`0x2F`）、电量（`0x08`）、充电状态（`0x09`）、型号（`0x00`）与固件版本（`0x03`）。主机需先发送`0x7101`、`0x7701`查询才会收到事件，且每个事件都必须回ACK（`0x8001`），否则后续事件会阻塞。协议见EGoTouchRev的`penevt/BTMCU_PROTOCOL.md`。

## 组成

| 部分 | 位置 | 说明 |
|---|---|---|
| 从片驱动 | `patches/0079` | `drivers/input/touchscreen/himax_hx83121a_pen.c`，帧经`/dev/gk3_pen`交给`gk3pend` |
| 设备树 | `patches/0080` | 启用`&spi20`与引脚，挂载从片节点`pen@0` |
| 休眠恢复 | `patches/0169`、`BoardConfig.mk` | `usbhid.quirks=0x12d1:0x10b8:0x21000000`（`HID_QUIRK_IDLE_ON_RESUME`） |
| 守护进程 | `device/huawei/gaokun3/stylus/` | `gk3pend`（服务`vendor.gk3pend`），解算与系数说明见`stylus/solver.h` |
| 权限与电量 | `ueventd.gaokun3.rc`、`sepolicy/gk3pend.te`、`genfs_contexts` | `/dev/gk3_pen`、`hidraw`、`uinput`、`usbfs`及`sysfs_gaokun3_pen_battery` |

## 设计取舍

* **解算放在用户态**：压感、侧键和电量只能经USB从笔MCU取得，因此内核驱动只负责交出从片原始帧，由`gk3pend`在一处合并解算（沿用EGoTouchRev逆向出的HPP3步骤）。
* **一欧滤波（One Euro filter）替代直线拟合**：解算出的原始位置每帧有零点几个像素的抖动，慢速书写时放大呈锯齿。`gk3pend`采用截止频率随速度升高的一欧滤波，慢写时多平滑、快划时几乎不增加延迟，并以1/16像素上报亚像素精度。原厂后处理链中的直线拟合（`LinearFilterProcess`）未采用：写字时很少触发，反而会把慢速画出的缓弧拉成折线。
* **从片跟随复位与空闲降频**：从片没有复位线，主片每次复位（熄屏亮屏、`himax-spi`自恢复）都会使从片丢失配置，因此驱动在帧头异常、连续读失败或亮屏无帧时自动重初始化。无笔超过`idle_after_ms`后关闭中断改为低频轮询，`gk3pend`收到笔MCU报文时写`/dev/gk3_pen`立即恢复中断。
* **USB休眠恢复**：笔MCU在收到`SET_FEATURE(REMOTE_WAKEUP)`后会按接口扣住IN报文，直到该接口收到`SET_IDLE`或OUT传输才放行。接口0（压感）绑定`hid-generic`，靠`patches/0169`新增的`HID_QUIRK_IDLE_ON_RESUME`在恢复时发送`SET_IDLE`；接口1（侧键与电量）由`gk3pend`在检测到休眠恢复后重新发送查询握手。
* **电量节点挂载方式**：驱动注册的`power_supply`节点`m-pencil`故意不挂父设备——Android `EventHub`为虚拟`/sys/devices/virtual`下的`uinput`设备查找电池时，会将无父设备的设备级电池关联为触控笔电池，并由单独的SELinux类型`sysfs_gaokun3_pen_battery`放行`system_server`读取。
* **面板列间距、列读出时差与强峰区域**：
  * **列间距**：BOE屏两端变宽、中间变窄，CSOT屏相反。`gk3pend`读取设备树中面板的`compatible`自动选表（日志打印`column pitch boe|csot|even`）。
  * **列读出时差**：从片偶数列比奇数列晚约1 ms读出，导致斜线出现周期一到两列的规则波浪。`pos_skew()`按采样时隙的速度补偿时差，不增加延迟。
  * **未刷新帧与强峰区域**：`stale_frame()`跳过中心3×3读数未刷新的帧（最多连跳一帧）。CSOT屏部分行存在整行同相波动噪声，默认启用只取四连通高值单元的"强峰区域"以减小行噪声混入；BOE屏上保持原区域算法。

## 已知限制

* **防误触**：笔在感应范围内时Android会屏蔽同窗口的手指触摸；笔离开感应范围但手掌仍搭在屏上时，由触摸驱动在日常模式下的[手掌识别](palm-rejection.md)处理。
* **边缘与斜线细小起伏**：离屏幕边缘约2 mm内的坐标会被压向边线；慢速用尺画斜线仍有约±0.1 mm随倾角变化的格内误差，单一修正表无法消除。
* **CSOT屏行噪声**：在一台CSOT屏上测得第12–19行原始噪声约为其余行的2倍，强峰区域可减轻但无法完全消除。

## 调试

* **日志与统计**：`gk3pend`日志输出在`dmesg`（含面板类型、电量、重握手、设置变更）；从片计数见`/sys/kernel/debug/gk3_pen/stats`，驱动参数在`/sys/module/himax_hx83121a_pen/parameters/`。
* **休眠恢复检查**：`/sys/module/usbhid/parameters/quirks`应含`0x12d1:0x10b8:0x21000000`，`/sys/bus/usb/devices/3-3/quirks`应为`0x0`；`hidraw`无压感报文时检查`dmesg`有无`SET_IDLE on resume failed`。
* **手动调参与录制**：`stop vendor.gk3pend`后以root运行`/vendor/bin/gk3pend`（`-pf`调滤波、`-sk`调列时差、`-ns`禁跳帧、`-pr`/`-lr`切换峰值区域、`-rec FILE`录制原始帧），测试完毕后`start vendor.gk3pend`。
