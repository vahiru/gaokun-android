# s2idle 取证工具集（gaokun3 / sc8280xp / 主线 7.2）

2026-08-21 产物。案卷 `docs/stage4-findings.md` #50 / #51，待办 `docs/TODO.md` A9。
**这套东西与 Android 无关，全部在救援 Ubuntu 里跑。**

## ⚠️ 动手前必读的三条

### 1. 单次试验没有证据力

这个故障的**单次失败率约 93%**。所以"改了 X，试一次，炸了"对**任何**配置都是
大概率事件 —— 它几乎不构成证据。我在 2026-08-21 就因为这个被同一条 cmdline
的一正一反（#39 通过 / #43 复位）带偏了整整两轮。

★ **任何结论都必须来自"跑到失败或跑满 10 次"的循环**（`s2loop.sh` / `s2fp.sh`）。
在 7% 的单次存活率下，连过 10 次的概率是 10⁻¹¹，那才叫证据。

### 2. `pm_test=devices` 是本平台最好的复现器，**不是**"无效判据"

我一度把它判成"本平台无效"，理由是"它自己就会复位" ——
**那个理由本身就是结论**：它会复位，正因为 bug 就在它覆盖的窗口里。

夹逼结果（多轮稳定）：`pm_test=freezer` 通过 → `pm_test=devices` 复位
⇒ 故障在 `dpm_suspend_start()` + `dpm_suspend_noirq()` 之内。

它的**两个好处**：5 秒自动返回、**不需要唤醒源** ⇒ 不会把机器睡死，
所以不需要任何人在机器边。

### 3. ⚠️ 计数陷阱：`-EBUSY` 不是"通过"

`pm_test` 周期结束后会留下 pending 唤醒事件，**紧接着再 `echo mem` 会立刻
`-EBUSY` 返回**。第一版脚本把那个非零返回也当成"通过"，于是报出**假的 10/10**。

★ **判据是耗时**：真实周期 **5–8 秒**，`-EBUSY` 是 **0–1 秒**。
本目录的脚本只把 `rc=0` 计数，非零就等 10 秒重试。

## 靶场纪律

* ★ **这条纪律已经完成它的使命，但留着**：当年的规矩是"'醒不回来'没解决之前
  一律不跑真实挂起"。后来证明**"醒不回来"根本不存在** —— 复位发生在挂起
  【进入】时，被误读成了唤醒失败。⚠️ 一般化的那条仍然成立：
  **先分清失败是"可恢复"（复位 → 自动回落救援系统）还是"不可恢复"
  （睡死 → 只能长按电源键）**，不可恢复的那种在有 RTC 兜底之前不要碰。
* ✅ 实验用的 BLS 条目**加 `panic=10`**：内核已开 `CONFIG_DPM_WATCHDOG`，
  设备回调卡住会 panic，配 `panic=10` 就能自动重启回默认项。
* ⚠️ 改 **DTB / 引导链**这类可能连 userspace 都到不了的实验，
  动手前先跟用户说明"可能要按一次电源键"——脚本的自动回落救不了这种。
* ⚠️ 批量解绑时**必须保住 RTC**（`pm8xxx_rtc`）：解掉它
  `/sys/class/rtc/rtc0/wakealarm` 就没了，真实挂起会变成"睡死"。
  `sx.sh` 里已有这条硬检查。

## 文件

| 文件 | 用途 |
|---|---|
| `s2fp.sh` | ★主力。每次开机先拍一份完整**开机指纹**（绑定设备清单 / `/sys/class/wakeup/` / `/proc/interrupts` / 所有 `runtime_status` / PCI 链路与电源状态 / ASPM+APST 实际值 / 网络关联 / genpd / dmesg），再跑 `pm_test=devices` 循环，最后重启。**自带自动串链**：每轮开头就把下一轮的 oneshot 和武装设好，所以中途整板复位也不会断链 |
| `s2cmp.sh` | 把"连过 10/10 的开机"与"第 1 次就死的开机"的指纹逐项 diff（dmesg 会先剥时间戳） |
| `s2loop.sh` | 轻量版：只跑 `pm_test=devices` 循环，不采指纹。用来快速比较 cmdline 变体 |
| `sx.sh` | 单次挂起助手。**没有 rtc0 或闹钟没设上就拒绝挂起** |

## 用法

装到救援 Ubuntu 的 `/usr/local/bin/`，靠 `s71test.service`
（`ExecStart=/usr/local/bin/s71test.sh`，`multi-user.target.wants` 里建 symlink 即武装）
在开机后自动跑。脚本第一件事就是**自己解除武装**，所以不会意外重复触发。

从 Android 侧部署（挂 ext4 分区改文件 + 写 `LoaderEntryOneShot` EFI 变量）
的流程见 `docs/stage4-findings.md` #51；EFI 变量的写法（属性 `0x07` +
UTF-16LE + 双 NUL，覆盖前 `chattr -i`）见 M14 段。

## ★★ 结论：s2idle 通了（2026-08-21 夜）

**两道坎叠在一起**，所以整晚每个单变量实验都失败、每个假说都看起来被"否定"：

| 坎 | 症状 | 修法 |
|---|---|---|
| `a600000.usb` 的 role 停在 `device`（无 gadget、无 xhci、半初始化） | 设备挂起阶段**整板复位**，零日志 | **`role=host`**（子 xhci 0→1）|
| EC（`15-0038`）`suspend_noirq` 超时 −110 | 干净失败 | **发版内核本来就修好了**（buildbot 的 EC ordering 补丁）|

**实测**：我们带补丁的内核 + `role=host`，救援 Ubuntu 上真实 s2idle 挂起
**5/5**（`success=5 fail=0`）。
★ 判据不只看计数：每次**墙钟走约 43 秒而内核 printk 时间只走约 2.7 秒**
—— 本地时钟停了、墙钟靠 RTC 补回来，这才是"真睡"的签名。

★★★ **方法论（本轮最贵的一条）**：**当排除法把所有候选都排干净时，
先怀疑"是不是有两个原因"，而不是继续找第三个候选。**

## 落地

* `99-gaokun3-usb-role.rules` —— 装到救援 Ubuntu 的 `/etc/udev/rules.d/`，
  开机自动把 role 置成 host。**救援系统不用 USB gadget，零代价。**
* ⚠️ **Android 侧不能照搬**：USB adb 的 UDC 就在这个控制器上
  （`sys.usb.controller=a600000.usb`），置成 host 就没有 USB device-mode adb。
  而 Android 的 `device` 角色**有真实 gadget**，未必受影响 —— 需要一次实测
  （用带 `CONFIG_PM_DEBUG` 的内核 + `pm_test=devices`）。
  ⚠️ 那个内核**不要带调试插桩**：我给 `device_prepare()` 加的 DPM 看门狗在
  Ubuntu 上无害，但 Android 的 SystemSuspend 会不停发起挂起尝试，
  每次给约 700 个设备各建一个定时器 —— 疑似把系统拖死过一次。

| 文件 | 用途 |
|---|---|
| `s2real.sh` | 真实挂起测试（role=host + RTC 闹钟），⚠️ 有睡死风险，需人在旁边 |
| `s2verify.sh` | 验收：**不自己写 role**，检查 udev 规则是否开机生效，不是 host 就拒绝挂起 |
| `99-gaokun3-usb-role.rules` | 救援 Ubuntu 的永久修复 |
