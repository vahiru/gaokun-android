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

* ❌ **"醒不回来"没解决之前，一律不跑真实挂起**（`echo mem` 不带 `pm_test`）。
  真实挂起成功一次的后果是**机器睡进去醒不回来，只能长按电源键**。
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

## 目前的结论（截至 2026-08-21）

| 配置 | 有通过的开机 / 总开机 |
|---|---|
| 基线（`pcie_aspm.policy=powersupersave` + APST 开） | **0 / 约 16** |
| 只 `pcie_aspm=off` | 0 / 1 |
| 只 APST 关 | 0 / 1（另有一次挂死） |
| 去掉策略（ASPM 默认）+ APST 关 | 0 / 1 |
| **`pcie_aspm=off` + APST 关** | **3 / 5**（其中一轮连过 10/10） |

两个都必需，且**光去掉我们自己加的 `powersupersave` 策略不够**。
⚠️ 但**不是确定性修复** —— 同一条 cmdline 另有 2 次开机第 1 次就死，
而**同一次开机内行为高度一致**（连过 10 次 或 第 1 次就死）
⇒ 决定性因素是**开机时确定下来的状态**。`s2fp.sh` 就是为查这一层写的。
