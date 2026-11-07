# 深度待机

息屏待机走s2idle，CPU停下来不等于SoC下电。真正省电要DDR进自刷新、CX下电、AOSS睡下去。看`/sys/kernel/debug/qcom_stats/`下的计数，时间单位是19.2 MHz的tick：

- `ddr`：DDR自刷新。
- `cxsd`：CX下电。
- `aosd`：AOSS睡眠，最深的状态。

三个都涨才算进了深度待机。

## 之前卡在哪

APPS留在RPMh sleep集合里的投票不清空，AOSS就不会关DDR和CX。有三处：

- **PCIe链路不断开（`patches/kernel/pcie-pm/0003`）。**根端口的SLTCAP里`HPC=1`，`pci_bridge_d3_possible()`返回`false`，根端口留在D0。`pci_host_common_d3cold_possible()`要求总线上的设备都在D3hot，所以`dw_pcie_suspend_noirq()`直接返回，链路不走L2。链路在线时`pcie-qcom`留一票1 kBps的PCIe-MEM投票，这票带`ALWAYS`标签，一直在sleep集合里。该补丁是上游的`d4c79b63d82d`，放开根端口的D3hot。
- **链路断开后整机复位（`patches/kernel/pcie-pm/0001`）。**原来的L2路径用`icc_disable()`撤掉PCIe-MEM投票。两条链路都断开后，PCI0 BCM在AMC和WAKE集合里归零，下次唤醒前后SoC复位，Linux没有日志。该补丁改成`ACTIVE_ONLY`标签的1 kBps投票：PCI0留在AMC和WAKE集合，sleep集合是空的。`patches/kernel/pcie-pm/0003`不能脱离`0001`单独用，否则第一次待机就走到无票路径。
- **蓝牙串口（`patches/kernel/pcie-pm/0002`）。**系统休眠时GENI串口的`pm_runtime_put_sync()`不起作用，时钟和ICC投票不放，CX下不去。

`xo.lvl`留在sleep集合里、内部USB保持绑定和唤醒、端口1唤醒，这些都不挡深度待机。

## 链路断开时的供电

| | NVMe（`pcie2a`） | Wi-Fi（`pcie4`） |
|---|---|---|
| `PERST#` | 拉低 | 拉低 |
| `vddpe-3v3` | `VCC3_SSD`断电 | `VCC_WLAN_3R9`保持（`patches/kernel/dts/0020`） |
| `WLAN_EN` | 没有 | 设备开了唤醒时保持 |

Windows的PEP表也是这样：D3时拉低SSD的供电，Wi-Fi的3.9 V和`WLAN_EN`不动。`qcom_pcie_deinit_2_7_0()`无条件关`vddpe-3v3`，`skip_pwrctrl_off`只保`WLAN_EN`，所以3.9 V靠`regulator-always-on`。

NVMe每次待机都做一次shutdown，醒来重新建队列。两条链路在noirq阶段重新训练，SSD那条约0.35秒，Wi-Fi那条约0.25秒，是醒来时最慢的一段；两个控制器并行恢复（`patches/kernel/pcie-pm/0008`），不再一条等一条。

## EC唤醒

插充电器、合盖这类事件由EC报给主机，中断走PDC（`pdc 215`，GPIO 103）。SoC进了AOSD之后，电平触发的中断常常接不住插充电器：PDC一直把SoC叫醒，CPU每次去看时线又是高的，GIC上没有挂着的中断，系统要等下一个闹钟或电源键。看起来EC这时只发很短的低电平脉冲。所以EC驱动在`suspend_noirq`里把中断改成下降沿，PDC把它当作边沿交给GIC；恢复时改回DT里的电平，再读一遍EC排着的事件（`patches/kernel/ec-usb/0015`）。醒着时一直是电平触发：打开中断时线如果已经是低的，边沿触发就收不到这个事件。睡眠中插线不亮屏、而`qcom_stats/aosd`的Count在一次睡眠里涨了几千，就是这个问题。

## Wi-Fi

开着WoW时，固件在`PERST#`拉低后继续运行，关联不断。碰上唤醒条件时，固件拉低`WAKE#`（TLMM 139，经PDC，AOSD里也能叫醒）。PCI核心把它登记成Wi-Fi的专用唤醒中断，低电平触发（`patches/kernel/pcie-pm/0011`）；它只在设备自己的DT节点上找`wake-gpios`，所以这根线写在`wifi@0`上，不在`pcie4`上（`patches/kernel/dts/0021`）。从固件字符串和测试推断，固件要在链路走L2、`PERST#`拉低之后才拉这根线，所以`WAKE#`离不开上面断链待机的那几个补丁。Wi-Fi关着时固件不运行，`pcie4`挂起时会打一条`Timeout waiting for L2 entry`，这是预期的。

默认的唤醒条件只有断线和GTK更新失败（`device/huawei/gaokun3/wifi/wpa_supplicant.conf`的`wowlan_triggers`）。收包唤醒的规则由vendor服务`gaokun3_wifiwake`（`bin/gaokun3-wifi-wake.sh`）设置，做法照ChromeOS：只让推送连接的服务器发来的TCP包叫醒系统。它从`/proc/net/tcp`、`/proc/net/tcp6`找远端端口是5228–5230的已建立连接（Google Play服务的FCM长连接用这几个端口；分不出连接属于哪个应用，别的应用连到这几个端口也会算进去），每个服务器地址一条规则，连同上面两个条件用`iw phy <phy> wowlan enable ... patterns ...`设下去；醒着时每10秒核对一次，地址变了、或者`wpa_supplicant`重设触发条件把规则冲掉了，就重新设。一分钟内被叫醒超过3次、或十分钟内超过10次（`ath11k_wow`的`event_count`），就撤掉规则10分钟，防止唤醒风暴。不走Google推送的应用（比如国内版微信、QQ）匹配不上，还是要等下一次唤醒（见[known-limitations.zh-CN.md](../known-limitations.zh-CN.md)）。Play服务换了服务器之后，要等系统醒着满10秒规则才跟上；走VPN，或者IPv4连接经464XLAT转成IPv6时，`/proc`里的地址和Wi-Fi上的包对不上，规则不会匹配。

Wi-Fi叫醒系统之后，会拉着`WAKE#`一会儿。恢复时内核先撤掉唤醒中断的唤醒设置，再重新打开设备中断（`patches/kernel/pcie-pm/0012`），否则低电平触发的`WAKE#`中断会在这段空档里反复触发，一次恢复多出几千到两万多个中断。固件因为收到的东西叫醒系统时（唤醒原因不是`UNSPECIFIED`），`ath11k_wow`唤醒源保持`ath11k.wow_wakeup_ms`毫秒（默认1000，`patches/kernel/wifi/0027`），让收包的进程来得及运行，不然Android约0.1秒后就又挂起了。它的`event_count`就是固件叫醒系统的次数，用户态可以拿来给收包规则限流。

排查：被`WAKE#`叫醒时`/sys/power/pm_wakeup_irq`是`/proc/interrupts`里`0006:01:00.0:wakeup`那一行的中断号。内核开了`CONFIG_ATH11K_DEBUG`时，`echo 2 > /sys/module/ath11k/parameters/debug_mask`之后日志里有`wow wakeup host reason`，`UNSPECIFIED`一般是主机自己醒来时固件的回答，`PATTERN_MATCH_FOUND`是收包规则匹配上了。现在设着的规则看`iw phy phy0 wowlan show`，服务的记录看`logcat -s gaokun3-wifiwake`。

Wi-Fi进了D3hot之后、主机发`PME_Turn_Off`之前，固件如果碰上触发唤醒的事件，会通过链路给主机发PME。根端口的PME中断走GIC ITS的MSI，不能唤醒系统，挂起时已经被`pcie_pme_suspend()`关掉，这条PME没人处理。固件这时不回应`PME_Turn_Off`，照样断电的话几次之后链路回不来，所以`dw_pcie_suspend_noirq()`发`PME_Turn_Off`之前先看根端口的Root Status，有PME就给发PME的设备记一次唤醒、让这次挂起失败（`patches/kernel/pcie-pm/0009`）。日志里是`PME from 01:00.0, aborting suspend`加一行`failed to suspend noirq: error -16`，Android随后重试挂起，这是预期的。

`mac80211`休眠时拆掉RX BA会话并发`DELBA`，但路由器不一定跟着拆：它可能保留原来的会话，也可能在主机睡着时由固件代答新的`ADDBA`。这时路由器继续发A-MPDU，REO的窗口却是1，TCP看到大量乱序，而且下一次休眠没有会话可拆，状态一直留到重新关联。`patches/kernel/wifi/0013`记下这些TID，每次醒来发一次`DELBA`，直到新会话建立。路由器不一定马上响应，所以偶尔要到下一次唤醒才恢复。`/sys/kernel/debug/ieee80211/phy0/netdev:wlan0/stations/*/agg_status`里TID 0的RX列是当前有没有会话，这个文件要`CONFIG_MAC80211_DEBUGFS`，发版内核没开。

平板睡着时持续收到下行，路由器可能把平板从关联表里删掉，下一次发帧时收到`Reason: 7`，醒来几秒内自动重连。这和L2无关，链路不断开时也一样。

## Wi-Fi固件崩溃

- **radio关着（`patches/kernel/wifi/0011`）。**reset在`ath11k_mac_op_reconfig_complete()`结束，`mac80211`只为重启的radio走到那里。radio关着时`is_reset`不清，之后每次加载固件都持60秒`ath11k_recovery`。现在没有radio要重启时直接结束reset。
- **崩溃循环（`patches/kernel/wifi/0016`、`0017`）。**重启途中再崩也会reset。距离上一次reset不到600秒（boottime，睡眠也算）的崩溃算同一个循环：前两次立刻reset，之后从30秒起每次翻倍，等待时放掉唤醒源，6次reset之后再崩就放弃。放弃后下一次挂起给设备下电，醒来重新加载固件。
- **重启途中崩溃（`patches/kernel/wifi/0023–0025`）。**`op_start`、`add_interface`不再因此失败。否则`mac80211`关掉接口，Android的`WifiSelfRecovery`跟着关Wi-Fi，要用户手动打开。日志里的`restarted without firmware`表示radio回来了、固件还没有，下一次reset或挂起下电会接手。退避等待时挂起不动固件，醒来由reset接手；放弃之后WoW被拒绝，挂起走下电路径。
- **链路丢了（`patches/kernel/wifi/0026`）。**端点从总线上消失后，reset里MHI准备失败。之后的挂起下电或关机不再重复释放MHI。Wi-Fi在重启前无法使用，之后的挂起能否让它恢复尚未经验证。
- 其余：0175重启失败时留着已注册的hw，0176不重复关PCI中断，0179不让删除station失败，0180–0183、0185是上游和邮件列表的修复。

## 恢复失败

- **链路重试（`patches/kernel/pcie-pm/0005`）。**链路在恢复时没起来，就按一次挂起和恢复的流程再来一次：`PERST#`、PHY、控制器都下去，NVMe的`VCC3_SSD`断电3秒。只重试一次，日志是`Link didn't come back`。
- **系统盘没了就重启（`patches/kernel/pcie-pm/0006`、`0007`）。**盘没了ext4会关掉`/data`和`/metadata`，内核不会自己重启，也挂起不了。重试后NVMe链路还是没有，或者走到`nvme_mark_namespaces_dead()`，就panic，靠`CONFIG_PANIC_TIMEOUT`重启。成功重置之后才移除namespace的路径没有覆盖。Wi-Fi的链路丢了不panic，交给`ath11k`，见上一节。
- **内部USB控制器死掉（`patches/kernel/ec-usb/0014`）。**日志里有`HC died; cleaning up`，`/sys/bus/usb/devices/3-3`不存在。键盘、触摸板和笔到重启前都没有反应，挂起不受影响。深度待机的测试内核上出现过，原因还没有查清。
- **panic不留记录。**发版cmdline带`efi_pstore.pstore_disable=1`，DT也没有ramoops。待机时自己重启的机器，临时`echo N > /sys/module/efi_pstore/parameters/pstore_disable`，复现一次，读`/sys/fs/pstore`，再跑`scripts/pstore-ctl.sh clear`。不要默认打开，NVRAM写满的问题还在。

## 排查

- 开机后约150秒内、SLPI还没睡过（`qcom_stats/slpi`的Count是0）时，计数不动不代表有问题。
- 亮屏时传感器客户端让SLPI一直忙，`cxsd`、`ddr`按约50 Hz跳动而`aosd`不动，这不是进了深度待机。
- 看根端口有没有进D3：

  ```
  echo 'file pci-driver.c +p' > /sys/kernel/debug/dynamic_debug/control
  ```

  挂起时应该看到`pcieport 0002:00:00.0: PCI PM: Suspend power state: D3hot`。
- 根端口D3有问题时，cmdline加`pcie_port_pm=off`，根端口留在D0，回到不断链的待机，不用改内核。cmdline来自`boot.img`：整批回退就在`BOARD_KERNEL_CMDLINE`加这一项再发OTA。单台机器可以用root改ESP的直连条目（`sed -i '/^options /s/$/ pcie_port_pm=off/' <条目>`），从菜单选它启动，下次OTA会覆盖。
- 接着电源时单次睡眠最长约55秒，那是health HAL的60秒闹钟，拔线后是600秒。
- 恢复时链路训练失败，host会留着（`patches/kernel/pcie-pm/0004`）。日志是`Device not found`、`Device found, but not active`或`Link failed to come up`之一，接着是`Link didn't come back`，然后按上一节重试。
- `aosd`的Count比`/sys/power/suspend_stats/success`少，有的睡眠整段不涨：s2idle里几个CPU轮流申请集群下电，固件拒绝后，被拒的CPU几微秒内又申请，跟踪显示这时别的CPU还在固件里、genpd也还算它们下电，它又成了最后一个，就形成循环，这段时间SoC一直醒着，环里几乎没有中断。`patches/kernel/pcie-pm/0010`让被固件拒绝（`-EPERM`）的CPU在s2idle里先忙等3 ms再申请，之后没有再见到环；固件拒绝的原因还不知道。别的失败（`-EOPNOTSUPP`，没下电就返回）不在环里，不等。判断：`/sys/devices/system/cpu/cpu*/cpuidle/state1/s2idle/usage`八个CPU加起来，一次睡眠涨8到十来次，一觉涨几千以上就是环。偶尔被拒还会有（一夜几次到二十次左右），每次多等3 ms，这是预期的。
- `vendor.hexagonrpcd-sdsp`退出码4、`Could not attach to FastRPC node: No space left on device`：`drivers/misc/fastrpc.c`每冻结一次泄漏一个ctx编号，用完255个之后就起不来，不是SLPI崩溃。`patches/kernel/core/0009`修了这个问题，还看到的话先确认内核带着它。
