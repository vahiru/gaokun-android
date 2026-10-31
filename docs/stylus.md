# 手写笔（华为 M-Pencil）

跟踪：issue [#26](https://github.com/vahiru/gaokun-android/issues/26)（此前的讨论见 #21）。

## 功能与适用范围

支持二代 M-Pencil（CD54 系列）：压感、悬停、倾斜；笔身侧边双击在笔与橡皮擦（Android 的 `TOOL_TYPE_ERASER`）之间切换；
系统休眠恢复后照常可用；电量显示在"设置 → 已连接的设备 → 触控笔"，电量低时系统提醒充电。

一代（CD52）使用另一种协议（HPP2），不支持。三代未验证。

## 数据通路

笔的数据来自两处，由守护进程 `gk3pend` 合成一支 uinput 触控笔。坐标与触摸屏是同一套像素坐标，但以 1/16 像素为单位上报
（`ABS_X` 0–25599、`ABS_Y` 0–40959）。

**触摸从片：位置、悬停、倾斜。** 屏幕的触摸控制器是一对 HX83121A：主片在 spi6，由 himax-spi 驱动，负责手指；
感应笔的级联从片在 spi20（ACPI 中的 THPB/HIMX0002），中断在 GPIO 39，没有复位线。初始化时写两个寄存器
（`raw_out_sel = 0xF6`、rawdata 地址 `0x5AA5`），之后从片在中断上给出 339 字节的帧，其中笔尖（TX1）与笔尖上方的
环形电极（TX2）各有一组锚点和 9×9 网格。位置与悬停取自 TX1，倾斜取自 TX1 与 TX2 的偏移。

**笔 MCU：压感、侧键、电量。** 平板内的 USB 设备 `12d1:10b8` 是笔的接收端：

- 接口 0 的 hidraw，报文 `0x55`：压感，每包 4 个 u16 槽。笔一拿起报文就开始发送，`gk3pend` 用它提前唤醒从片。
- 接口 1 的厂商 bulk 通道（OUT `0x02`、IN `0x85`）：侧边双击（事件 `0x2F`）、电量（`0x08`，百分比）、充电状态（`0x09`）、
  笔的型号（`0x00`）与固件版本（`0x03`）。主机要先发 `0x7101`、`0x7701` 查询才会收到事件，每个事件都要回 ACK
  （`0x8001`），否则后续事件会堵住。协议见 EGoTouchRev 的 `penevt/BTMCU_PROTOCOL.md`。

## 组成

| 部分 | 位置 | 说明 |
|---|---|---|
| 从片驱动 | `patches/0079` | `drivers/input/touchscreen/himax_hx83121a_pen.c`（`CONFIG_TOUCHSCREEN_HIMAX_HX83121A_PEN`，`kernel-config-android.sh` 里 =y），帧经 `/dev/gk3_pen` 交给 `gk3pend` |
| 设备树 | `patches/0080` | 启用 `&spi20` 与引脚，挂上从片节点 `pen@0` |
| 启动参数 | `BoardConfig.mk` | `usbcore.quirks=12d1:10b8:b`，见下 |
| 守护进程 | `device/huawei/gaokun3/stylus/` | `gk3pend`，服务 `vendor.gk3pend`（user system，group uhid usb） |
| 权限 | `ueventd.gaokun3.rc`、`sepolicy/gk3pend.te` | `/dev/gk3_pen`、hidraw、uinput、usbfs，以及读设备树中面板的 compatible |
| 电量 | 驱动 + `sepolicy/genfs_contexts`、`system_server.te`、`hal_health_default.te` | power supply `m-pencil`，见下 |

## 设计要点

- **解算放在用户态。** 压感、侧键和电量只能经 USB 从笔 MCU 取得，驱动只负责把从片的帧交出来，由 `gk3pend` 一处合成。
  解算沿用 EGoTouchRev（原厂 TSA 的逆向）的 HPP3 步骤，常数的来历写在 `stylus/solver.h` 的注释里。
- **位置滤波与亚像素。** 解算出的位置每帧有零点几个像素的抖动，慢速书写时相邻两帧只差 1–2 个像素，放大后线条呈锯齿。
  `gk3pend` 对位置做一欧滤波（One Euro filter）：截止频率随速度升高（默认 1 Hz + 每 mm/s 0.5 Hz，速度本身按 5 Hz 低通），
  慢时多平滑，快时几乎不增加延迟；笔进入感应范围和每次落笔时重新开始，笔画的起点就是落笔处。坐标以 1/16 像素上报，
  Android 按浮点数换算到屏幕坐标，亚像素部分能传到应用。原厂坐标后处理链里按速度调强度的 IIR 由它替代；
  直线拟合（`LinearFilterProcess`）没有采用：写字时很少触发，却会把慢速画的缓弧拉成一段段直线。
- **从片的复位。** 从片没有复位线，主片每次复位（熄屏亮屏、himax-spi 自恢复）都会让它丢掉配置。驱动在帧头异常或连续读
  失败时重新初始化，并用一个跟随面板的看门狗，在亮屏却收不到帧时重新初始化。
- **省电。** 没有笔时从片每秒仍中断约 125 次。超过 `idle_after_ms` 没见到笔，驱动就关掉中断，改为每 `idle_poll_ms` 读一帧；
  `gk3pend` 收到笔 MCU 的报文时写一下 `/dev/gk3_pen`，让驱动立即恢复中断。
- **休眠恢复。** 系统休眠恢复后，笔 MCU 不再转发压感，只有 USB 端口复位能恢复，所以启动参数里加了
  `usbcore.quirks=12d1:10b8:b`（`USB_QUIRK_RESET_RESUME`）。复位后 MCU 要重新收到查询才会发事件，`gk3pend` 检测到
  休眠恢复后会重新握手。
- **电量。** 驱动注册的 power supply `m-pencil` 作用域为设备，health HAL 会跳过它。它故意不挂父设备：EventHub 为输入设备
  找电池时，从设备的 sysfs 节点往上找最近的 `power_supply` 目录，而 uinput 设备位于 `/sys/devices/virtual`，所以这块电池
  会被认作触控笔的电池。读取它的是 system_server 里的 InputManager，而核心策略只允许 health HAL 读 `sysfs_batteryinfo`，
  所以节点使用单独的类型 `sysfs_gaokun3_pen_battery`。
- **列间距。** 传感器的列不是等距的：BOE 屏两端各 10 列的宽度为标称值的 33/32，中间 40 列为 63/64。`gk3pend` 读设备树中
  面板的 compatible（面板驱动按主板的 ID 引脚更新过，见 `hw-inventory.md` 的「面板」），只在 BOE 屏上按这个间距换算，CSOT 屏仍按等距计算。原厂的间距表来自 CSOT 屏，与 BOE 屏方向相反，
  尚未在 CSOT 屏上验证，所以没有采用。
- **列的读出时差。** 从片的采样是等间隔的 240 Hz，但偶数列比奇数列晚约 1 ms 读出，行没有这个差别。Y 由峰值两侧的列算出，X 由峰值所在的列算出，两者的奇偶正好相反，所以笔移动时 X、Y 随列的
  奇偶反向摆动，幅度和速度成正比。横线没有 Y 向速度，竖线的误差顺着线走，只有斜线上看得出，是周期一到两列的规则波。
  `pos_skew()` 在算出位置后减去"速度 × 时差"，时差为 X `+294 sin(π·列)` µs、Y `-531 sin(π·列)` µs，只用已有的帧，不增加延迟。
  系数是在 BOE 屏上拟合的，所以只在 BOE 屏上启用。帧的时间戳只是驱动读取的时刻，不能当采样间隔用：它在 3.0 ms 和 5.35 ms 之间
  交替，而空档之后的第一帧其实紧接着上一帧采样，只是读得晚，缺的位移在下一帧。所以速度按采样时隙计算，空档算到下一帧上；`seq`
  跳变时帧是真丢了，算在当前帧；笔重新进入范围时时序不明，前三帧基本不修正。
- **没有刷新的读数。** 单元偶尔保留上一帧的读数，有些单元特别频繁。笔慢时没有影响，笔快时这样的帧约三成偏离路径超过 0.3 mm，
  笔迹容易在固定的位置折一下。`stale_frame()` 发现中心 3×3 里有单元的读数和上一帧完全相同（十字 ≥ 100，四角 ≥ 300：四角碰巧相等的
  更多，影响也更小）就跳过这一帧；不连续跳两帧，免得一个卡死的单元把笔定住。

## 已知限制

- **防误触**：笔在感应范围内（悬停或落笔）时，Android 会屏蔽同一窗口里的手指触摸；笔离开感应范围后，手掌碰到屏幕
  仍会被当作触摸。要解决需要在 himax-spi 中识别手掌（DISP-7 / T1）。
- **边缘**：离边约 2 mm 以内的位置会被压向边线。
- **斜线的细小起伏**：放大看用尺子慢慢画的斜线，还有约 ±0.1 mm 的起伏。这是列方向的格内误差，随握笔的倾斜变化，所以单一的
  修正表校正不了，没有处理。
- **CSOT 屏**：列间距、边缘处理和列的读出时差都没有在 CSOT 屏上验证，时差修正在 CSOT 屏上默认关闭；跳帧默认开启，它的阈值
  也是在 BOE 屏上定的。
- **三代笔**：未验证；压感若超过 4095 会被截断。

## 调试

- 日志：`gk3pend` 的输出在内核日志里（userdebug），包括启动参数、电量变化、休眠恢复后的重新握手、笔的型号与固件版本。
- 从片：`/sys/kernel/debug/gk3_pen/stats`（帧数、错误、重新初始化、空闲进出次数）；驱动参数在
  `/sys/module/himax_hx83121a_pen/parameters/`，`idle_after_ms=0` 关闭空闲轮询。
- 电量：`/sys/class/power_supply/m-pencil/{present,capacity,status}`；`dumpsys input` 中笔设备带 `BATTERY` 类别，
  BatteryController 一节有它的状态。
- 休眠恢复：`/sys/bus/usb/devices/3-3/quirks` 应为 `0x2`。
- 位置：`stop vendor.gk3pend` 后以 root 手动运行 `/vendor/bin/gk3pend`。`-pf MIN,BETA,DCUT` 设位置滤波（单位依次是 mHz、
  每 mm/s 的 mHz、mHz），`-pf 0` 关掉；`-sk X,Y` 设列时差修正（微秒，`0,0` 关掉，指定后 CSOT 屏也生效），`-ns` 不跳过读数没有
  刷新的帧；`-rec FILE` 把每一帧原始数据（含压感、接触、倾斜）追加到文件，供离线分析。
  调完 `start vendor.gk3pend` 恢复。
