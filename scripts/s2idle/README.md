# `s2idle`挂起诊断工具集（SC8280XP / 主线Linux 7.2）

本目录包含在Linux环境下排查`s2idle`（Suspend-to-Idle）设备挂起复位与唤醒问题的自动化循环测试脚本。`s2idle`与AOSS深睡（CX collapse）的完整架构与修复详见[`../../docs/dev/deep-sleep.md`](../../docs/dev/deep-sleep.md)。

## 一、测试方法与判据

1. **多轮循环验证**：单次挂起测试受竞态影响可能偶发通过，本目录脚本（`s2loop.sh` / `s2fp.sh`）以连续通过10轮作为稳定判据。
2. **`pm_test=devices`窗口定位**：
   - `echo devices > /sys/power/pm_test`覆盖`dpm_suspend_start()`与`dpm_suspend_noirq()`阶段，并在5秒后自动恢复，无需外部唤醒源，适合无人值守二分定位挂起阶段崩溃。
3. **识别`-EBUSY`假阳性**：
   - `pm_test`周期刚结束时可能残留pending唤醒事件，立即再次执行`echo mem > /sys/power/state`会在0–1秒内返回`-EBUSY`。
   - 真实`pm_test=devices`周期耗时5–8秒；脚本仅在`rc=0`时计入成功次数，非零返回时等待10秒后重试。
4. **真实`s2idle`时间判据**：在真实`s2idle`挂起（带RTC唤醒闹钟）中，除检查返回码外，还需核对墙钟时间（由RTC补偿）显著大于内核`printk`单调时钟增量，以确认本地时钟确实进入停机状态。

## 二、目录文件说明

| 文件 | 用途 |
|---|---|
| `s2fp.sh` | 完整开机指纹采集与`pm_test=devices`自动串链测试脚本（采集绑定驱动、wakeup源、中断、`runtime_status`、PCI链路、genpd与dmesg） |
| `s2cmp.sh` | 对比正常通过10/10与挂起失败开机的系统状态快照差异 |
| `s2loop.sh` | 轻量级`pm_test=devices`循环测试脚本 |
| `sx.sh` | 单次挂起辅助脚本（强制检查`rtc0`与`wakealarm`是否就绪，防止无唤醒源死睡） |
| `s2real.sh` | 结合`role=host`与RTC闹钟的真实`s2idle`挂起测试脚本 |
| `s2verify.sh` | 验收脚本：检查udev规则是否已生效并执行真实挂起测试 |
| `99-gaokun3-usb-role.rules`、`gaokun3-usb-role*` | Linux环境规则与systemd挂起钩子：将未绑定gadget的`a600000.usb`角色设为`host`，避免半初始化`device`角色在挂起时触发整板复位 |

## 三、在救援Linux环境中部署挂起钩子

在救援Ubuntu/Debian环境中（未使用USB gadget），可通过`system-sleep`钩子在每次挂起前确保`a600000.usb`处于`host`角色（仅靠开机设置一次会被迟到的Type-C/UCSI初始化改回`device`）：

```sh
sudo install -m755 gaokun3-usb-role-host.sh   /usr/local/bin/
sudo install -m644 gaokun3-usb-role.service   /etc/systemd/system/
sudo install -m755 gaokun3-usb-role-sleephook /usr/lib/systemd/system-sleep/gaokun3-usb-role
sudo systemctl enable gaokun3-usb-role.service
```

验证时请使用`systemctl suspend`（直接写`/sys/power/state`不会触发`system-sleep`钩子）。Android环境下的动态USB角色切换逻辑见[`../../device/huawei/gaokun3/bin/gaokun3-usbrole.sh`](../../device/huawei/gaokun3/bin/gaokun3-usbrole.sh)。
