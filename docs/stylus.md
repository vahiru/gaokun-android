# 手写笔（华为 M-Pencil）

跟踪：issue [#26](https://github.com/vahiru/gaokun-android/issues/26)（此前的讨论见 #21）。

## 功能与适用范围

支持二代 M-Pencil（CD54 系列）：压感、悬停、倾斜；笔身侧边双击在笔与橡皮擦（Android 的 `TOOL_TYPE_ERASER`）之间切换；
系统休眠恢复后照常可用；电量显示在"设置 → 已连接的设备 → 触控笔"，电量低时系统提醒充电。"设置 → 已连接的设备 →
手写笔设置"（Parts 的 `StylusSettingsActivity`）可以调笔迹平滑（四档），把双击改成侧键单击或关掉。

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
| 休眠恢复 | `patches/0169`、`BoardConfig.mk` | `usbhid.quirks=0x12d1:0x10b8:0x21000000`，见下 |
| 守护进程 | `device/huawei/gaokun3/stylus/` | `gk3pend`，服务 `vendor.gk3pend`（user system，group uhid usb） |
| 权限 | `ueventd.gaokun3.rc`、`sepolicy/gk3pend.te` | `/dev/gk3_pen`、hidraw、uinput、usbfs，以及读设备树中面板的 compatible |
| 电量 | 驱动 + `sepolicy/genfs_contexts`、`system_server.te`、`hal_health_default.te` | power supply `m-pencil`，见下 |

## 设计要点

- **解算放在用户态。** 压感、侧键和电量只能经 USB 从笔 MCU 取得，驱动只负责把从片的帧交出来，由 `gk3pend` 一处合成。
  解算沿用 EGoTouchRev（原厂 TSA 的逆向）的 HPP3 步骤，常数的来历写在 `stylus/solver.h` 的注释里。
- **位置滤波与亚像素。** 解算出的位置每帧有零点几个像素的抖动，慢速书写时相邻两帧只差 1–2 个像素，放大后线条呈锯齿。
  `gk3pend` 对位置做一欧滤波（One Euro filter）：截止频率随速度升高（默认 1 Hz + 每 mm/s 0.5 Hz，速度本身按 5 Hz 低通），
  慢时多平滑，快时几乎不增加延迟。设置页的四档平滑把"每 mm/s"那一项设成 0.5、0.3、0.2、0.125 Hz，越小越平滑、笔迹落后得越多
  （写字速度下约 5、6.5、8、10 ms）；笔进入感应范围和每次落笔时重新开始，笔画的起点就是落笔处。坐标以 1/16 像素上报，
  Android 按浮点数换算到屏幕坐标，亚像素部分能传到应用。原厂坐标后处理链里按速度调强度的 IIR 由它替代；
  直线拟合（`LinearFilterProcess`）没有采用：写字时很少触发，却会把慢速画的缓弧拉成一段段直线。
- **从片的复位。** 从片没有复位线，主片每次复位（熄屏亮屏、himax-spi 自恢复）都会让它丢掉配置。驱动在帧头异常或连续读
  失败时重新初始化，并用一个跟随面板的看门狗，在亮屏却收不到帧时重新初始化。
- **省电。** 没有笔时从片每秒仍中断约 125 次。超过 `idle_after_ms` 没见到笔，驱动就关掉中断，改为每 `idle_poll_ms` 读一帧；
  `gk3pend` 收到笔 MCU 的报文时写一下 `/dev/gk3_pen`，让驱动立即恢复中断。
- **休眠恢复。** 笔 MCU 收到 SET_FEATURE(REMOTE_WAKEUP) 后按接口扣住 IN 报文，要等该接口收到 SET_IDLE 或 OUT 才放行。
  接口 2 由 `mt_resume()` 放行；接口 0（压感）绑 hid-generic，恢复时没有任何请求，靠 `patches/0169` 的
  `HID_QUIRK_IDLE_ON_RESUME`；接口 1（侧键、电量等事件）靠 `gk3pend` 检测到休眠恢复后重新握手。
  排查：`/sys/bus/usb/devices/3-3/quirks` 应为 0x0，hidraw 收不到 0x55 时先看 dmesg 里有没有 `SET_IDLE on resume failed`。
- **电量。** 驱动注册的 power supply `m-pencil` 作用域为设备，health HAL 会跳过它。它故意不挂父设备：EventHub 为输入设备
  找电池时，从设备的 sysfs 节点往上找最近的 `power_supply` 目录，而 uinput 设备位于 `/sys/devices/virtual`，所以这块电池
  会被认作触控笔的电池。读取它的是 system_server 里的 InputManager，而核心策略只允许 health HAL 读 `sysfs_batteryinfo`，
  所以节点使用单独的类型 `sysfs_gaokun3_pen_battery`。
- **列间距。** 传感器的列不是等距的：BOE 屏两端各 10 列的宽度为标称值的 33/32，中间 40 列为 63/64（点击靶实测）；CSOT 屏用
  原厂的间距表（EGoTouchRev 的 `ApplyPitchMap`），两端 63/64、中间 129/128，方向与 BOE 屏相反。按等距算时，CSOT 屏左右两侧的
  笔迹会往中间偏，第 10、50 列附近最多约 0.7 mm。`gk3pend` 读设备树中面板的 compatible（面板驱动按主板的 ID 引脚更新过，
  见 `hw-inventory.md` 的「面板」）选表，认不出的面板按等距计算。启动日志的 `column pitch boe|csot|even` 是选中的表。
- **列的读出时差。** 从片的采样是等间隔的 240 Hz，但偶数列比奇数列晚约 1 ms 读出，行没有这个差别。Y 由峰值两侧的列算出，X 由峰值所在的列算出，两者的奇偶正好相反，所以笔移动时 X、Y 随列的
  奇偶反向摆动，幅度和速度成正比。横线没有 Y 向速度，竖线的误差顺着线走，只有斜线上看得出，是周期一到两列的规则波。
  `pos_skew()` 在算出位置后减去"速度 × 时差"，时差为 X `+294 sin(π·列)` µs（强峰区域为 `+600`）、Y `-531 sin(π·列)` µs，只用已有的帧，不增加延迟。
  294、531 是在 BOE 屏上拟合的，一台 CSOT 屏上量到的时差相同，所以两种屏都启用；600 在两种屏上拟合。帧的时间戳只是驱动读取的时刻，不能当采样间隔用：它在 3.0 ms 和 5.35 ms 之间
  交替，而空档之后的第一帧其实紧接着上一帧采样，只是读得晚，缺的位移在下一帧。所以速度按采样时隙计算，空档算到下一帧上；`seq`
  跳变时帧是真丢了，算在当前帧；笔重新进入范围时时序不明，前三帧基本不修正。
- **没有刷新的读数。** 单元偶尔保留上一帧的读数，有些单元特别频繁。笔慢时没有影响，笔快时这样的帧约三成偏离路径超过 0.3 mm，
  笔迹容易在固定的位置折一下。`stale_frame()` 发现中心 3×3 里有单元的读数和上一帧完全相同（十字 ≥ 100，四角 ≥ 300：四角碰巧相等的
  更多，影响也更小）就跳过这一帧；不连续跳两帧，免得一个卡死的单元把笔定住。
- **峰值区域。** 位置由峰值区域在行、列上的投影算出。原来的区域从窗口中心向八个方向扩展，收进所有高于 100 的单元。笔尖
  所在的整行读数都在 150 左右，两种屏上区域通常都横跨整行。CSOT 屏有几行的噪声是同一行一起波动的，也随投影进来，写字时笔迹
  发抖。强峰区域（来自 #63）从窗口的最大值出发，只收四连通、超过最大值一半的单元，区域达到十个单元时退回原来的区域。这时约
  三分之二的帧里 X 只来自峰值所在的一列，不再混进相邻的列，列的读出时差在 X 上更完整，所以 X 的时差系数用 600 µs，两种屏上
  拟合都是约 600；Y 变化不大，仍用 531。BOE 屏上强峰区域让缓曲线更偏离路径，所以只在非 BOE 屏上默认使用。

## 已知限制

- **防误触**：笔在感应范围内（悬停或落笔）时，Android 会屏蔽同一窗口里的手指触摸；笔离开感应范围后，手掌碰到屏幕
  仍会被当作触摸。要解决需要在 himax-spi 中识别手掌（DISP-7 / T1）。
- **边缘**：离边约 2 mm 以内的位置会被压向边线。
- **斜线的细小起伏**：放大看用尺子慢慢画的斜线，还有约 ±0.1 mm 的起伏。这是列方向的格内误差，随握笔的倾斜变化，所以单一的
  修正表校正不了，没有处理。
- **CSOT 屏**：列间距用的是原厂表，没有用点击靶实测过；边缘处理没有在 CSOT 屏上验证。列的读出时差只在一台 CSOT 屏上量过，速度到约 850 mm/s；跳帧的阈值
  是在 BOE 屏上定的。
- **CSOT 屏的行噪声**：一台 CSOT 屏上，第 12–19 行的读数噪声约为第 20–32 行的 2 倍，第 19、20 行之间是一道台阶，写字时这一带的笔迹
  更抖。强峰区域减轻了但没有消除。噪声在原始帧里就有，和 USB 供电无关，来源不明；其他 CSOT 屏是否相同不知道。
- **三代笔**：未验证；压感若超过 4095 会被截断。

## 调试

- 日志：`gk3pend` 的输出在内核日志里（userdebug），包括启动参数、电量变化、休眠恢复后的重新握手、笔的型号与固件版本。
- 从片：`/sys/kernel/debug/gk3_pen/stats`（帧数、错误、重新初始化、空闲进出次数）；驱动参数在
  `/sys/module/himax_hx83121a_pen/parameters/`，`idle_after_ms=0` 关闭空闲轮询。
- 电量：`/sys/class/power_supply/m-pencil/{present,capacity,status}`；`dumpsys input` 中笔设备带 `BATTERY` 类别，
  BatteryController 一节有它的状态。
- 休眠恢复：`/sys/bus/usb/devices/3-3/quirks` 应为 `0x2`。
- 位置：`stop vendor.gk3pend` 后以 root 手动运行 `/vendor/bin/gk3pend`。`-pf MIN,BETA,DCUT` 设位置滤波（单位依次是 mHz、
  每 mm/s 的 mHz、mHz），`-pf 0` 关掉；`-sk X,Y` 设列时差修正（微秒，`0,0` 关掉），`-ns` 不跳过读数没有
  刷新的帧；`-rec FILE` 把每一帧原始数据（含压感、接触、倾斜）追加到文件，供离线分析。
  `-pr` / `-lr` 强制使用强峰区域或原来的区域，不指定时按面板选择；X 的时差系数跟着变（600 / 294），`-sk` 指定时以它为准。
  调完 `start vendor.gk3pend` 恢复。
- 设置页：写 `persist.sys.gaokun3.pen_beta`（500、300、200、125）和 `persist.sys.gaokun3.pen_side`（`eraser`、`button`、`none`），
  `stylus/gk3pend.rc` 按字面值转成 `vendor.gaokun3.pen_*`，`gk3pend` 在任何属性变化后重新读取，下一帧生效，日志打印
  `settings: filter beta …, side …`。命令行带 beta 的 `-pf` 或 `-side` 优先于设置。`-side none` 只忽略双击，电量照常。
