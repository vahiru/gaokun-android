# 深度待机

息屏待机走 s2idle，CPU 停下来不等于 SoC 下电。真正省电要 DDR 进自刷新、CX 下电、AOSS 睡下去。
看 `/sys/kernel/debug/qcom_stats/` 下的计数，时间单位是 19.2 MHz 的 tick：

- `ddr`：DDR 自刷新。
- `cxsd`：CX 下电。
- `aosd`：AOSS 睡眠，最深的状态。

三个都涨才算进了深度待机。

## 之前卡在哪

APPS 留在 RPMh sleep 集合里的投票不清空，AOSS 就不会关 DDR 和 CX。有三处：

- **PCIe 链路不断开（patches/0191）。** 根端口的 SLTCAP 里 HPC=1，`pci_bridge_d3_possible()` 返回 false，根端口留在 D0。
  `pci_host_common_d3cold_possible()` 要求总线上的设备都在 D3hot，所以 `dw_pcie_suspend_noirq()` 直接返回，链路不走 L2。
  链路在线时 pcie-qcom 留一票 1 kBps 的 PCIe-MEM 投票，这票带 ALWAYS 标签，一直在 sleep 集合里。
  0191 是上游的 d4c79b63d82d，放开根端口的 D3hot。
- **链路断开后整机复位（patches/0188）。** 原来的 L2 路径用 `icc_disable()` 撤掉 PCIe-MEM 投票。两条链路都断开后，
  PCI0 BCM 在 AMC 和 WAKE 集合里归零，下次唤醒前后 SoC 复位，Linux 没有日志。
  0188 改成 ACTIVE_ONLY 标签的 1 kBps 投票：PCI0 留在 AMC 和 WAKE 集合，sleep 集合是空的。
  0191 不能脱离 0188 单独用，否则第一次待机就走到无票路径。
- **蓝牙串口（patches/0190）。** 系统休眠时 GENI 串口的 `pm_runtime_put_sync()` 不起作用，时钟和 ICC 投票不放，CX 下不去。

`xo.lvl` 留在 sleep 集合里、内部 USB 保持绑定和唤醒、端口 1 唤醒，这些都不挡深度待机。

## 链路断开时的供电

| | NVMe（pcie2a） | Wi-Fi（pcie4） |
|---|---|---|
| PERST# | 拉低 | 拉低 |
| `vddpe-3v3` | `VCC3_SSD` 断电 | `VCC_WLAN_3R9` 保持（patches/0189） |
| WLAN_EN | 没有 | 设备开了唤醒时保持 |

Windows 的 PEP 表也是这样：D3 时拉低 SSD 的供电，Wi-Fi 的 3.9 V 和 WLAN_EN 不动。
`qcom_pcie_deinit_2_7_0()` 无条件关 `vddpe-3v3`，`skip_pwrctrl_off` 只保 WLAN_EN，所以 3.9 V 靠 `regulator-always-on`。

NVMe 每次待机都做一次 shutdown，醒来重新建队列。两条链路在 noirq 阶段重新训练，SSD 那条约 0.35 秒，Wi-Fi 那条约 0.25 秒，是醒来时最慢的一段；两个控制器并行恢复（patches/0196），不再一条等一条。

## Wi-Fi

开着 WoW 时，固件在 PERST# 拉低后继续运行，关联不断。Wi-Fi 现在叫不醒系统（见 [known-limitations.zh-CN.md](known-limitations.zh-CN.md)），
要等下一个闹钟：内核还没有处理 PCIe 的 WAKE#。Wi-Fi 关着时固件不运行，pcie4 挂起时会打一条 `Timeout waiting for L2 entry`，这是预期的。

mac80211 休眠时拆掉 RX BA 会话并发 DELBA，但路由器不一定跟着拆：它可能保留原来的会话，也可能在主机睡着时由固件代答新的 ADDBA。
这时路由器继续发 A-MPDU，REO 的窗口却是 1，TCP 看到大量乱序，而且下一次休眠没有会话可拆，状态一直留到重新关联。
patches/0174 记下这些 TID，每次醒来发一次 DELBA，直到新会话建立。路由器不一定马上响应，所以偶尔要到下一次唤醒才恢复。
`/sys/kernel/debug/ieee80211/phy0/netdev:wlan0/stations/*/agg_status` 里 TID 0 的 RX 列是当前有没有会话，这个文件要 `CONFIG_MAC80211_DEBUGFS`，发版内核没开。

平板睡着时持续收到下行，路由器可能把平板从关联表里删掉，下一次发帧时收到 `Reason: 7`，醒来几秒内自动重连。
这和 L2 无关，链路不断开时也一样。

## Wi-Fi 固件崩溃

- **radio 关着（patches/0170）。** reset 在 `ath11k_mac_op_reconfig_complete()` 结束，mac80211 只为重启的 radio 走到那里。
  radio 关着时 `is_reset` 不清，之后每次加载固件都持 60 秒 `ath11k_recovery`。现在没有 radio 要重启时直接结束 reset。
- **崩溃循环（patches/0177、0178）。** 重启途中再崩也会 reset。距离上一次 reset 不到 600 秒（boottime，睡眠也算）的崩溃算同一个循环：
  前两次立刻 reset，之后从 30 秒起每次翻倍，等待时放掉唤醒源，6 次 reset 之后再崩就放弃。放弃后下一次挂起给设备下电，醒来重新加载固件。
- **重启途中崩溃（patches/0184–0186）。** `op_start`、`add_interface` 不再因此失败。否则 mac80211 关掉接口，
  Android 的 WifiSelfRecovery 跟着关 Wi-Fi，要用户手动打开。日志里的 `restarted without firmware` 表示 radio 回来了、固件还没有，
  下一次 reset 或挂起下电会接手。退避等待时挂起不动固件，醒来由 reset 接手；放弃之后 WoW 被拒绝，挂起走下电路径。
- **链路丢了（patches/0187）。** 端点从总线上消失后，reset 里 MHI 准备失败。之后的挂起下电或关机不再重复释放 MHI。
  Wi-Fi 到重启前用不了，之后的挂起能不能把它带回来没有验证过。
- 其余：0175 重启失败时留着已注册的 hw，0176 不重复关 PCI 中断，0179 不让删除 station 失败，
  0180–0183、0185 是上游和邮件列表的修复。

## 恢复失败

- **链路重试（patches/0193）。** 链路在恢复时没起来，就按一次挂起和恢复的流程再来一次：PERST#、PHY、控制器都下去，
  NVMe 的 `VCC3_SSD` 断电 3 秒。只重试一次，日志是 `Link didn't come back`。
- **系统盘没了就重启（patches/0194、0195）。** 盘没了 ext4 会关掉 /data 和 /metadata，内核不会自己重启，也挂起不了。
  重试后 NVMe 链路还是没有，或者走到 `nvme_mark_namespaces_dead()`，就 panic，靠 `CONFIG_PANIC_TIMEOUT` 重启。
  成功重置之后才移除 namespace 的路径没有覆盖。Wi-Fi 的链路丢了不 panic，交给 ath11k，见上一节。
- **内部 USB 控制器死掉（patches/0172）。** 日志里有 `HC died; cleaning up`，`/sys/bus/usb/devices/3-3` 不存在。
  键盘、触摸板和笔到重启前都没有反应，挂起不受影响。深度待机的测试内核上出现过，原因还没有查清。
- **panic 不留记录。** 发版 cmdline 带 `efi_pstore.pstore_disable=1`，DT 也没有 ramoops。待机时自己重启的机器，
  临时 `echo N > /sys/module/efi_pstore/parameters/pstore_disable`，复现一次，读 `/sys/fs/pstore`，再跑 `scripts/pstore-ctl.sh clear`。
  不要默认打开，NVRAM 写满的问题还在。

## 排查

- 开机后约 150 秒内、SLPI 还没睡过（`qcom_stats/slpi` 的 Count 是 0）时，计数不动不代表有问题。
- 亮屏时传感器客户端让 SLPI 一直忙，`cxsd`、`ddr` 按约 50 Hz 跳动而 `aosd` 不动，这不是进了深度待机。
- 看根端口有没有进 D3：

  ```
  echo 'file pci-driver.c +p' > /sys/kernel/debug/dynamic_debug/control
  ```

  挂起时应该看到 `pcieport 0002:00:00.0: PCI PM: Suspend power state: D3hot`。
- 根端口 D3 有问题时，cmdline 加 `pcie_port_pm=off`，根端口留在 D0，回到不断链的待机，不用改内核。
  cmdline 来自 boot.img：整批回退就在 `BOARD_KERNEL_CMDLINE` 加这一项再发 OTA。单台机器可以用 root 改 ESP 的直连条目
  （`sed -i '/^options /s/$/ pcie_port_pm=off/' <条目>`），从菜单选它启动，下次 OTA 会覆盖。
- 接着电源时单次睡眠最长约 55 秒，那是 health HAL 的 60 秒闹钟，拔线后是 600 秒。
- 恢复时链路训练失败，host 会留着（patches/0192）。日志是 `Device not found`、`Device found, but not active` 或 `Link failed to come up` 之一，接着是 `Link didn't come back`，然后按上一节重试。
- `aosd` 的 Count 比 `/sys/power/suspend_stats/success` 少，有的睡眠整段不涨：s2idle 里几个 CPU 轮流申请集群下电，
  固件拒绝后内核立刻重试，形成循环，这段时间 SoC 一直醒着。环内没有设备中断，固件拒绝的原因还不知道。
  `/sys/devices/system/cpu/cpu*/cpuidle/state1/s2idle/usage` 一觉涨几千以上就是它。
- `vendor.hexagonrpcd-sdsp` 退出码 4、`Could not attach to FastRPC node: No space left on device`：
  `drivers/misc/fastrpc.c` 每冻结一次泄漏一个 ctx 编号，用完 255 个之后就起不来，不是 SLPI 崩溃。
  patches/0171 修了这个问题，还看到的话先确认内核带着它。
