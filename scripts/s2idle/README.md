# `s2idle`挂起/恢复压力测试

本目录的`android-ath11k-s2loop.sh`在Android中循环执行挂起与恢复，检查ath11k固件每次都能重新加载、Wi-Fi每次都能恢复连接。`s2idle`与AOSS深睡（CX collapse）的架构与修复详见[`../../docs/dev/deep-sleep.md`](../../docs/dev/deep-sleep.md)，执行时机与通过判据见[`../../docs/dev/release-checklist.md`](../../docs/dev/release-checklist.md)。

## 一、用法

脚本需以root身份在设备上运行：

```sh
setsid nohup sh /data/local/tmp/android-ath11k-s2loop.sh <pm_test次数> <真实s2idle次数> <同网段可ping通的主机> &
```

- 前两个参数默认为10和3：先执行`pm_test=platform`循环，再执行带RTC唤醒闹钟（20秒）的真实`s2idle`。
- 第三个参数必须给出，请填写确认能ping通的同网段主机，不要按主路由表推测网关地址：Android的默认路由位于策略路由表中。
- 测试期间脚本会停止USB端口HAL（`vendor.usb-gaokun3`）并将`a600000.usb`固定为`host`角色，结束后恢复为`device`并重新启动该服务，因此USB adb在测试期间会断开，需通过`setsid nohup`启动。
- 结果写入`/data/local/tmp/mhi-test/log`，每轮的`dmesg`与`logcat`保存在同一目录。

环境变量：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `WOW` | `0` | `0`测试断电挂起（每轮应重新加载固件并重新关联）；`1`用于WoWLAN已开启时的测试（脚本不会自行开启WoWLAN；挂起时芯片不断电、连接不应断开），固件拒绝进入WoW的次数单独记为`declined`，不计为失败 |
| `PACE` | `75` | 每轮的最短间隔（秒）。Android Wi-Fi框架会限制自动重连频率，间隔过短时后续轮次会因不再重连而误判失败；只检查内核侧时可设为`0` |

## 二、判据与注意事项

1. **每轮通过条件**：挂起返回0；60秒内能ping通对端；本轮标记之后的内核日志中没有页分配失败、MHI上电失败、DPM超时、WoW或HIF挂起/恢复失败、固件崩溃等错误。`WOW=0`时还要求本轮重新关联并至少重新加载一次固件；`WOW=1`时连接断开（`deauthenticating`）也计为错误。
2. **必须使用`pm_test=platform`**：`devices`档不执行`suspend_late`/`resume_early`，ath11k既不断电也不重新加载固件，Wi-Fi必然无法恢复，测不到需要验证的路径。
3. **`-EBUSY`重试**：上一轮结束后可能残留待处理的唤醒事件，此时再次写入`/sys/power/state`会立即返回`-EBUSY`。脚本只在返回0时计入成功，非零时等待5秒后重试，每轮最多尝试6次。
4. **USB角色**：USB端口HAL按对端决定port0的数据角色，连接电脑或充电器时，`host`角色下约6秒内没有下游设备就会切回`device`，因此脚本在测试期间停止该服务。每轮挂起前脚本还会核对角色，不是`host`时中止测试。
