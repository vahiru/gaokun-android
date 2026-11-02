# 硬件清单 —— MateBook E Go (sc8280xp / gaokun3)

采集日期：**2026-08-13**
采集方式：Ego 从 U 盘启动 Ubuntu 26.04，SSH 接入后运行 `scripts/collect-hw-inventory.sh`
内核：**`7.1.0-rc3-gaokun3+`**（buildbot 2026-05-14 镜像）
原始 dump：`hw-inventory-20260812-231530/`（已 gitignore，184 KB，22 个文件）
缺失工具：**无**

> 本文所有"实测"值均来自上述 dump。参考树给出的预期值单独标注。
> 二者冲突时以实测为准（CLAUDE.md 规则 #3）。

---

## Stage 0 验收结果（CLAUDE.md 第 96 行）

| 验收项 | 结果 | 依据 |
|---|---|---|
| vulkaninfo 认出 a690 | ✅ **通过** | `deviceName = Turnip Adreno (TM) 690` |
| modetest 点屏 | ✅ **通过** | `DSI-1 connected`，1600x2560@120/60 |
| pstore 能读到 panic | ✅ **通过** | **走 efi_pstore，不是 ramoops**。见 7bis |

## ✅ Stage 0 完成（2026-08-13）

自编内核 **`7.2.0-rc2-gaokun3+`**（mainline v7.2-rc2 + buildbot 20 个补丁 +
`CONFIG_PSTORE*` + `CONFIG_MAGIC_SYSRQ`）。

崩溃日志实测可恢复 —— `echo c > /proc/sysrq-trigger` 后重启读回：

```
Panic#1 Part1 .. Part11          11 个分片，约 10 KB
Kernel panic - not syncing: sysrq triggered crash
CPU: 7 UID: 0 PID: 2806 Comm: bash  7.2.0-rc2-gaokun3+ #2
Hardware name: HUAWEI GK-W7X/GK-W7X-PCB, BIOS 2.16 01/31/2023
Call trace: ... __handle_sysrq ... write_sysrq_trigger ...
```

日志归档在 `panic-log.tar.gz`。日常操作用 `scripts/pstore-ctl.sh`。

---

## 0. 机器身份 ✅

来源：`scripts/` 下 Windows 预检报告 + `00-identity.txt`

| 项 | 实测值 |
|---|---|
| 厂商 / 型号 | **HUAWEI GK-W7X**，SKU **C233** |
| 序列号 | NCSBB23131801419 |
| **生产年份** | **2022 款**（触摸固件 cfg = `0x41`） |
| **面板厂** | **CSOT**（THP 日志 `Found CSOT panel!`，display version `0x07`） |
| **触摸固件** | **`41 07`** — 上游已测配置 |
| **BIOS** | **2.16**（2023-01-31）⚠️ 见 0.1，**不要升级** |
| 内存 | 15,741,152 kB（16 GB 机型） |
| LTE 版 | **否** |
| 指纹 | FocalTech（= FTE7001，不支持） |
| Secure Boot | 已关闭 |
| BitLocker | 未启用 |

`refs/linux-gaokun/README.MD:258-260`：`43/41 07` 与 `3e 00` 是已烧录并测试过的。
其中提到的「suspend 后滑动异常」只针对 `43 07`（2023/CSOT），本机 `41 07` 不在此列。

**与镜像的机型差异：** buildbot 和触摸驱动都标注目标为 **2023 款**
（`refs/gaokun-buildbot/README.md:5`），本机是 2022 款。DTS / EC / 面板 / 触摸 GPIO
两代共用，但**摄像头 DTSI 按 2023 的 s5k4h7 写，本机是 hi846**，相机开箱不工作。

### 0.1 ⚠️ BIOS 2.16：不要升到 2.17

`refs/matebook-e-go-linux/docs/ACPI_TOUCH_DIFF_216_vs_217.md` 比对两版 DSDT，
**触摸的总线与 GPIO 编号是完全不同的两套**：

| THPA 主触摸 | **2.16（本机）** | 2.17 |
|---|---|---|
| SPI 控制器 | `\_SB.SPI7` | `\_SB.SPI1` |
| SPI 速率 | `0x00B71B00` = **12 MHz** | `0x00BEBC20` = 12.5 MHz |
| reset GPIO | `0x63` = **99** | `0x36` = 54 |
| IRQ GPIO | `0xAF` = **175** | `0x71` = 113 |
| I2C HID | `I2C5` @0x4F | `I2C2` @0x4F |

上游 Linux 驱动的假设是 reset **99** / IRQ **175** / **12 MHz**
（`refs/matebook-e-go-linux/README.md:14,263`；`refs/linux-gaokun/README.MD:200-203`）
—— 与 **2.16 一致**。该文档结论：跨版本套用会让触摸指向错误的总线。

---

## 1. 内核 config

来源：`01-kconfig-relevant.txt`（完整版 `01-kconfig-full.txt`，209 KB）

### Android 相关已经开了一半

buildbot 的 `gaokun3_defconfig` 本来就带（大概率为 Waydroid）：

```
CONFIG_ANDROID_BINDER_IPC=y
CONFIG_ANDROID_BINDERFS=y
CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder"
```

**实测验证**（SSH 手动挂载）：

```
# mkdir -p /dev/binderfs && mount -t binder binder /dev/binderfs
# ls /dev/binderfs/
binder  binder-control  features  hwbinder  vndbinder
```

→ **CLAUDE.md 第 97 行 Stage 1 的第一条验收「`/dev/binderfs` 存在」内核层面已满足。**
Ubuntu 不自动挂它，Android 的 init 会自己挂。

### 缺的两项

- `# CONFIG_PSTORE is not set` —— defconfig 里连 `PSTORE`/`RAMOOPS` 字样都没有
- dwc3 peripheral —— 见第 12 节

---

## 2. 固件加载路径 ✅

来源：`02-firmware-paths.txt` / `02-dmesg-firmware.txt`
**这就是 Stage 2 打包 vendor 分区要抓的文件清单。**

| 固件路径 | 属于 |
|---|---|
| `qcom/a660_sqe.fw` / `a660_sqe.fw` | Adreno 690 (SQE 微码，复用 a660) |
| `qcom/a660_gmu.bin` | Adreno GMU |
| `qca/wcnhpbtfw21.tlv` | WCN6855 蓝牙固件 |
| `qca/wcnhpnv21g.bin` | WCN6855 蓝牙 NVM |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn` | ADSP |
| `qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn` | CDSP |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn` | SLPI（传感器） |

⚠️ **CLAUDE.md 第 88 行的蓝牙固件名不对。** 文档写 `hpbtfw21.tlv` / `hpnv21.b8c`，
实际加载的是 **`wcnhpbtfw21.tlv`** / **`wcnhpnv21g.bin`**。

注意华为专有路径 `qcom/sc8280xp/HUAWEI/gaokun3/` —— 这三个 `.mbn` 是从 Windows
驱动里扒出来的，不在 linux-firmware 里，Stage 2 必须自己带。

---

## 3. 显示 / KMS ✅ —— Stage 3 的关键依据

来源：`03-modetest.txt`（59 KB）/ `03-drm-sysfs.txt` / `03-panel.txt`

- DRM 驱动：**msm**
- Connector：**`DSI-1`**，状态 **connected**，物理尺寸 266x166 mm
- 模式：
  - `1600x2560 @ 120.00 Hz`（**preferred**）htot 1880，vtot **2736**
  - `1600x2560 @ 60.00 Hz` htot 1880，vtot **5472**（倍增）

  与 `refs/matebook-e-go-linux/README.md:329-330` 完全吻合：
  「120 Hz: native vtotal (2736)，60 Hz: doubled vtotal (5472)，same link speed」

- **CRTC 数量：6**
- **Plane 数量：25**

> ✅ **CLAUDE.md 第 118 行的担心不成立。** 原文假设「DSI panel 的 KMS plane
> 数量少时 drm_hwcomposer 会 fallback 到 GPU 合成」。实测 25 个 plane、6 个 CRTC，
> 硬件合成资源充裕，不需要预先接受性能损失。

### Format 与 Modifier（minigbm 配置依据）

每个 plane 支持 41 种 format。Modifier 只有两种：
**`LINEAR(0x0)`** 和 **`QCOM_COMPRESSED(0x500000000000001)`**（即 UBWC）。

**支持 UBWC 压缩的 format**（minigbm 应优先用这些）：

```
AR24  AB24  AR30  XR30  XR24  XB24  BG16
P010  NV12  NV21  NV16  YUYV  YVYU  YU12  YV12
```

**仅 LINEAR 的 format**：

```
RA24  BX24  BA24  RX24  RG24  BG24  RG16
AR15  AB15  RA15  BA15  XR15  XB15  RX15  BX15
AR12  AB12  RA12  BA12  XR12  XB12  RX12  BX12
NV61  VYUY  UYVY
```

Plane 属性含 `zpos`(0-255)、`alpha`(0-65535)、`pixel blend mode`、`IN_FORMATS` blob。

### 面板

- 背光走 **DCS**（`refs/linux-gaokun/README.MD:123-125`）
- 详见 `03-panel.txt`
- **工厂色彩校准在 ACPI 的 `DLUT` 表里**，内核据此校色并切换 sRGB、Display P3，见 [display-color.md](display-color.md)
- **BOE / CSOT 两种屏靠主板的两根 ID 引脚区分，不靠面板寄存器**：UEFI `DisplayDxe` 的 `GetLcdId`（BIOS 2.16，VA `0x31da8`）
  读 GPIO 102（`GPIO_LCD_ID0`）和 GPIO 91（`GPIO_LCD_ID1`）：

  | ID0 | ID1 | 面板 ID | 面板 |
  |---|---|---|---|
  | 0 | 0 | `0x0D` | CSOT |
  | 0 | 1 | `0x0F` | BOE |
  | 1 | 任意 | `0x0B` | 都不是（通用 eDP 配置） |

  ACPI 的 `\_SB.GPU0._ROM` 用同样的 ID 选面板配置。两根脚都是无上下拉的输入，设备树里没有别的用途。
  BIOS 2.18、C233 上实测：BOE 一台 ID0 低、ID1 高，CSOT 一台两根都低。
  查法（要 root）：`grep -E 'gpio(91|102) ' /sys/kernel/debug/gpio`。
  面板驱动在 probe 时读这两根脚（`panel-id-gpios`，patches/0123、0124），只有读到 BOE 的值才换成 BOE 的初始化序列并更新
  设备树里的 compatible，其余情况保持设备树写的 CSOT。
- BIOS 2.17 的 `GetLcdId` 读的是 GPIO 178/179。那一版触摸的总线和 GPIO 也整套不同（见 0.1），是另一种硬件；
  本机的 178/179 是两个功放的 `powerdown-gpios`。

---

## 4. GPU ✅

来源：`04-gpu-vulkan.txt`（132 KB）/ `04-gpu-gl.txt`（128 KB）

| 项 | 实测 |
|---|---|
| Vulkan 设备 | **`Turnip Adreno (TM) 690`** |
| Vulkan API | 1.3.335 / 1.4.335 |
| Mesa | **26.0.3-1ubuntu1** |
| 软件回退 | `llvmpipe (LLVM 21.1.8, 128 bits)` |

Stage 0 验收通过。Stage 5 的 GLES 路径（freedreno GL vs zink-over-turnip）
可在此基础上实测对比。

---

## 5. 输入设备 ✅ —— 触摸确认走 SPI

来源：`05-input-devices.txt` / `05-input-bus.txt`

| 设备 | evdev 名 | 节点 | 总线 |
|---|---|---|---|
| 触摸屏 | `Himax Capacitive TouchScreen` | event11 | **`Bus=001c` = BUS_SPI** |
| 触控板 | `HID 12d1:10b8 Touchpad` | event12 | `Bus=0003` = USB |

**触摸屏 sysfs 路径：**

```
/devices/platform/soc@0/9c0000.geniqup/998000.spi/spi_master/spi0/spi0.0/input/input15
```

驱动绑定：`/sys/bus/spi/devices/spi0.0/driver -> /sys/bus/spi/drivers/himax-spi`

dmesg：
```
himax-spi spi0.0: pressure/touch_major axes disabled by module parameter
input: Himax Capacitive TouchScreen as .../998000.spi/.../input/input15
```

→ 加载的正是 `refs/egotouchrev-linux` 那套算法驱动（`disable_pressure` 默认 true）。

**[已解决] 之前存疑的 ACPI `SPI7` vs Linux `SPI6`：** Linux 侧实际是
`998000.spi` 下的 `spi0.0`。ACPI 的 `\_SB.SPIn` 命名与 Linux QUP 实例编号不对应，
不构成冲突。

触控板走 USB（`12d1:10b8`），`usbhid.quirks` 已在 cmdline 生效。

---

## 6. 音频 ✅

来源：`06-audio.txt`（108 KB）/ `06-ucm2/`

- ALSA card 0：**`SC8280XP-HUAWEI-GAOKUN3`**（短名 `SC8280XPHUAWEIG`），驱动 `sc8280xp`
- 设备：MultiMedia1/2 Playback，MultiMedia3/4 Capture
- UCM：`/usr/share/alsa/ucm2/conf.d/sc8280xp/sc8280xp.conf`

⚠️ **与 `stage0-findings.md` 第 5 条的预期不同。** 那里根据
`refs/linux-gaokun/README.MD:121` 预期 card 名为
`HUAWEI-GK_W7X-M1010-GK_W7X_PCB`（指向 `LENOVO-X13s.conf` 的符号链接）。
实际镜像用的是 `refs/matebook-e-go-linux/README.md:302-309` 的做法：
在 `sc8280xp.conf` 里加 HUAWEI 匹配块。**Stage 4 按实测的 `sc8280xp.conf` 走。**

`06-ucm2/` 已完整抓取 Qualcomm 全平台 UCM 树（含 sc8280xp、x1e80100 等）供对照。

---

## 7bis. 🔴 ramoops 在这台机器上不可用（2026-08-13 实测结论）

> 这是本项目独有的发现，上游文档和任何参考树都没有记录。

### 实测过程

用自编的 `7.2.0-rc2-gaokun3+`（`CONFIG_PSTORE/PSTORE_RAM/PSTORE_CONSOLE/PSTORE_PMSG=y`）
测了两个物理地址，**ramoops 每次都成功注册，但重启后 `/sys/fs/pstore` 始终为空**：

| 地址 | 来源 | 注册 | 跨热重启存活 |
|---|---|---|---|
| `0xae900000` | DTS `reserved-memory` 节点，低位 bank 尾部 | ✅ | ❌ |
| `0x865d38000` | `reserve_mem=2M:4096:oops` 内核自选，高位 bank | ✅ | ❌ |

注册成功的证据（两次都有）：

```
OF: reserved mem: 0xae900000..0xaeafffff (2048 KiB) map non-reusable ramoops@ae900000
printk: legacy console [ramoops-1] enabled
pstore: Registered ramoops as persistent store backend
ramoops: using 0x200000@0xae900000, ecc: 0
```

测试方法：`echo <标记> > /dev/pmsg0` → `sync` → `systemctl reboot` → 查 `/sys/fs/pstore/`。
`PSTORE_CONSOLE=y` 意味着 console 日志也在持续写入该区域，因此**即使不 panic，
正常重启后也应能读到 `console-ramoops-0`**。实测两者皆无。

### 结论

**固件在每次复位时重新初始化 DRAM，与物理地址无关。** 高位、低位都试过了。

根本原因推测：DT 的 `reserved-memory` 只告诉**内核**别用这段内存，
但**固件完全不知道**——DTB 是 systemd-boot 加载后传给内核的，UEFI 自己不读。
所以下次启动时 UEFI 的 DDR 初始化会照常覆盖它。

**[待确认]** 未验证冷启动（断电）与热重启是否有差异，但既然热重启都不存活，
冷启动只会更糟。

### 副产品：两条可复用的事实

1. **`reserve_mem=` 可用且地址稳定。** 两次启动内核都选中 `0x865d38000`。
   语法：`reserve_mem=2M:4096:oops ramoops.mem_name=oops`
2. **ramoops 可纯靠 cmdline 配置，不需要 DTS 节点。**
   `fs/pstore/ram.c` 的 `ramoops_register_dummy()` 只要 `mem_size` 非零就会建
   dummy platform device。改地址只需改 cmdline，省掉重建 DTB 的整个循环。

### ✅ 解决方案：efi_pstore（已实测通过）

**结论：本机的持久化崩溃日志走 EFI 变量，不走 ramoops。**

实测流程与结果：

1. 用 plain DTB + 不配任何 ramoops 参数启动 → ramoops 不注册
2. 切换 `pstore_disable` 参数触发重新初始化 → `pstore: Registered efi_pstore as persistent store backend`
3. `echo c > /proc/sysrq-trigger` 触发 panic
4. 重启后 NVRAM 变量 78 → 89，出现 11 个 `dump-type0-*`
5. 再次触发注册后，`/sys/fs/pstore/` 出现 11 个 `dmesg-efi_pstore-*`，
   拼起来是完整的 panic 调用栈 + 崩溃前的启动日志

**两个反直觉的操作陷阱**（都已封装进 `scripts/pstore-ctl.sh`）：

**陷阱一：开机不会自动注册，每次都要手动触发。**
efi-pstore 编进内核后在 `device_initcall` 阶段跑 `efivars_pstore_init()`，
但那时 `efivar_supports_writes()` 还是 false —— 因为提供 EFI 变量读写的
高通 uefisecapp 驱动要到 **0.763 秒**才注册 efivars：

```
[0.762616] qcom_scm firmware:scm: qseecom: found qseecom with version 0x1402000
[0.762902] qcom_qseecom qcom_qseecom: setting up client for qcom.tz.uefisecapp
[0.763310] efivars: Registered efivars operations
```

`efi-pstore.c` 只有 `module_init`，**没有挂 `efivar_ops_nh` 通知链**，
所以扑空之后永不重试（静默 `return 0`）。

绕法：切换 `/sys/module/efi_pstore/parameters/pstore_disable`（1 再 0），
其 setter `efi_pstore_disable_set()` 会重新调用 `efivars_pstore_init()`。

> **✅ 已解决 —— 见 `patches/0001-efi-pstore-register-backend-when-efivars-ops-arrive-.patch`**
>
> 让 efi-pstore 挂上 `efivar_ops_nh` 通知链，efivars 一就绪就自动注册。实测：
>
> ```
> [0.790296] efivars: Registered efivars operations
> [0.790311] pstore: Registered efi_pstore as persistent store backend
> ```
>
> **晚 15 微秒**，基本是能做到的最早时机。不再需要手动切 `pstore_disable`。
>
> 写这个补丁时踩到一个必须绕开的死锁：`efivars_register()`
> 从 `vars.c:68` 到 `vars.c:91` 全程持有 `efivars_lock` 信号量，
> 而通知链在第 86 行调用；若在回调里直接跑 `pstore_register()`，
> 它会经 `pstore_get_records()` → `efi_pstore_open()` → `efivar_lock()`
> 去抢同一个信号量。所以注册必须推到 workqueue 的进程上下文里做。
> （efivarfs 的同类回调只改 `SB_RDONLY` 标志、不碰 efivar，所以天然没这问题，
> 不能照抄它的写法。）
>
> 另外把 `CONFIG_EFI_VARS_PSTORE` 改成 `=m` 也能绕过顺序问题，但那要等
> systemd 起来才加载，对「早期启动崩溃」没用 —— 而那恰恰是 Stage 2 最需要的。

**陷阱二：pstore 的 erase 不会真的删掉 EFI 变量，NVRAM 会泄漏。**

两条路径都验证过，NVRAM 计数纹丝不动（89 → 89）：

- 手动 `rm /sys/fs/pstore/*`
- `systemd-pstore.service` 自动归档（它把文件 "moved to" 磁盘后从 pstorefs 移除）

必须直接删 `/sys/firmware/efi/efivars/dump-type*`（先 `chattr -i`）。清干净后回到 78。
**每次 panic 泄漏 11 个变量约 10 KB，不清理迟早撑爆 NVRAM。**

→ 用 `pstore-ctl clear` 做这件事。已装到 Ego 的 `/usr/local/bin/pstore-ctl`。

**好消息：崩溃日志会自动落盘。** `systemd-pstore.service` 在启动约 5 秒时
把记录归档到 `/var/lib/systemd/pstore/<时间戳>/001/`，其中 `dmesg.txt`
是拼好的完整版。所以就算忘了看 `/sys/fs/pstore`（它会被清空），日志也不会丢：

```
[5.010671] systemd-pstore.service - Platform Persistent Storage Archival...
[5.038786] systemd-pstore[453]: PStore dmesg-efi_pstore-... moved to /var/lib/systemd/pstore/...
```

**排查崩溃时看 `/var/lib/systemd/pstore/`，不是 `/sys/fs/pstore/`。**

**局限**：efi_pstore 只记录 oops/panic 的 dmesg，**没有** console 持续捕获，
**没有** pmsg。ramoops 那套 `PSTORE_CONSOLE` / `PSTORE_PMSG` 在本机全部失效。

---

### 附：原始的替代方案分析

已确认的相关事实：

- **EFI 变量能跨重启存活。** 实测写入自定义变量 `ClaudePersist`（属性 `0x07`
  = NV|BS|RT），重启后仍在。
- 本机的 EFI 变量走**高通 TrustZone 的 `uefisecapp` 后端**，不依赖 EFI 运行时服务：
  dmesg 同时有 `EFI runtime services will be disabled.` 和
  `efivars: Registered efivars operations`。所以 cmdline 里的 `efi=noruntime`
  **不影响**变量读写。
- 内核已有 `CONFIG_EFI_VARS_PSTORE=y`，`/sys/module/efi_pstore` 存在。

**为什么它没生效：pstore 同时只接受一个后端**，ramoops 抢先注册了。
去掉 ramoops 配置后 efi_pstore 应能接管。

⚠️ 代价：efi_pstore 把日志写进 NVRAM，有写入寿命和容量问题，且**只在 oops/panic
时记录**，没有 console 持续捕获和 pmsg。

---

## 7. pstore / ramoops ❌ —— Stage 0 唯一阻塞项

来源：`07-pstore.txt`

实测状态：

```
/sys/fs/pstore/            不存在
mount | grep pstore        无输出
/sys/module/ramoops/*      不存在
dmesg | grep pstore        无输出
DT reserved-memory         无 ramoops 节点
```

根因：

```
# CONFIG_PSTORE is not set
```

**内核根本没编进 pstore**，不是配置或挂载问题。`gaokun3_defconfig` 里
`PSTORE` / `RAMOOPS` 字样一个都没有。

### 现有 DT reserved-memory 节点

```
adsp-region@86c00000      adsp-rpc-remote-heap@84a00000
cdsp0-region@8a100000     cdsp1-region@8c600000
cmd-db-region@80860000    gpu-mem@8bf00000
pil_video_region@86700000 slpi-region@88c00000
smem-region@80900000      linux,cma
reserved-region@{80000000, 80880000, 80b00000, 83b00000, 85b00000, aeb00000}
```

### 实测物理内存布局（`/proc/iomem`，root）

System RAM：
```
80c00000-826fffff    8e400000-9efbffff    9f000000-9f5cffff
9f600000-aeafffff    c8600000-ffa75fff    fff22000-3ffffffff
800000000-87fffffff
```

顶层 reserved：
```
80600000-806fffff  80880000-808affff  808c0000-808effff
9efc0000-9effffff  9f5f7000-9f5fffff  aeb00000-bfffffff
c6200000-c85fffff  ffa76000-fff21fff
```

### 需要做的

1. 内核加：`CONFIG_PSTORE=y`、`CONFIG_PSTORE_RAM=y`、
   `CONFIG_PSTORE_CONSOLE=y`、`CONFIG_PSTORE_PMSG=y`（Android logcat 持久化用）
2. DTS 加 `reserved-memory` 下的 ramoops 节点，**固定物理地址**（ramoops 靠固定
   地址跨重启保活，不能用 `alloc-ranges` 让内核动态分配）

**[待确认] 地址选择没有先例可抄。** 已 grep 全部参考树：`ramoops` 节点只出现在
老的 Qualcomm 手机平台（msm8953 / msm8996 / msm8998 / sdm630 等）和非高通板子上，
**没有任何 sc8280xp / x1e 笔记本平台定义过 ramoops**。所以地址要自己定并实测验证。

候选：`9f600000-aeafffff` 这段 System RAM 的尾部，紧邻已 reserved 的 `aeb00000`，
取 2 MB → `0xae900000` 长度 `0x200000`。**必须实测：写入 → 热重启 → 读回。**

---

## 8. 存储 / 分区 ✅

来源：`08-storage.txt`

**两个 PCIe 根端口的 SLTCAP 都有 HPC=1**，见 [deep-sleep.md](deep-sleep.md)。

**内置 NVMe `nvme0n1`（476.9 GB，GPT）：**

| 分区 | 大小 | 文件系统 | PARTLABEL | LABEL |
|---|---|---|---|---|
| p1 | 300M | vfat | EFI system partition | SYSTEM |
| p2 | 16M | — | Microsoft reserved partition | — |
| p3 | **120G** | ntfs | Basic data partition | **Windows** |
| p4 | **336.6G** | ntfs | Basic data partition | **Data** |
| p5 | 1G | vfat | Basic data partition | WINPE |
| p6 | 18G | ntfs | Basic data partition | Onekey |
| p7 | 1G | ntfs | Basic data partition | WinRE |

**U 盘 `sda`（114.6 GB）：** sda1 1G vfat PARTLABEL=`EFI` → `/boot/efi`；
sda2 11G ext4 PARTLABEL=`rootfs` → `/`

> ⚠️ **CLAUDE.md 第 55 行「fstab 用 PARTLABEL」在内置盘上行不通。**
> Windows 建的分区 PARTLABEL 全是 `Basic data partition`，**不唯一**。
> Stage 2 要么用 **PARTUUID**（已记录在 dump 里，唯一），要么自己新建分区时
> 写入有意义的 PARTLABEL。

Stage 2 可用空间：p4 的 336.6 GB「Data」是最有余量的目标。

---

### 8bis. ⚠️ 上表已过期 —— 2026-08-20 实测布局（Android 已就位）

上面那张表是 Stage 0 的快照。Stage 2 从 p4「Data」里切了 76 GB 出来给 Android，
**分区号从 7 个变成 10 个**。以下是从运行中的 Android 直接读
`/sys/block/nvme0n1/nvme0n1p*/size` 得到的实测值（总容量 488 386 MiB）：

| 分区 | 大小 | 用途 |
|---|---|---|
| p1 | 300 MiB | **内置 ESP** —— ⚠️ 里面**只有 Windows 的引导器**，见下 |
| p2 | 16 MiB | Microsoft reserved |
| p3 | 120 GiB | Windows |
| p4 | **256 GiB** | Data（NTFS，从 336.6 GB 缩过） |
| p5 | 1 GiB | WINPE |
| p6 | 18 GiB | Onekey |
| p7 | 1 GiB | WinRE |
| **p8** | **12 GiB** | **Android `super`**（system/system_ext/product/vendor + scratch） |
| **p9** | **64 GiB** | **Android `userdata`** |
| **p10** | **32 MiB** | **Android `metadata`** |

**U 盘 `sda`（114.6 GiB）**：sda1 1 GiB vfat = **实际在用的 ESP**；
sda2 11 GiB ext4 = Ubuntu 根文件系统（`root=UUID=a2447957-fc4d-40d4-ab64-faf74f697471`）。

### 8ter. 🔴 引导链全在 U 盘上 —— 拔掉 U 盘就只能进 Windows

实测两块 ESP 的内容：

| | 内置 `nvme0n1p1`（296 M，剩 188 M） | U 盘 `sda1`（0.9 G，剩 523 M） |
|---|---|---|
| 内容 | `EFI/Microsoft/`（28 M）、`EFI/Boot/bootaa64.efi`（2.9 M）、`Persisted_Capsules.bin`（70 M） | `EFI/systemd/`、`loader/entries/`（含 **`crdroid.conf`**）、`<machine-id>/`（476 M 的内核与 ramdisk） |
| 能引导 Android 吗 | ❌ 完全不能 | ✅ 唯一的通路 |

**后果：这台机器目前离不开那支 U 盘。** 想「抹掉 Windows 只留 Android」，
**第一步必须是把引导链搬进内置 ESP**，否则删完 Windows 机器就彻底不能开机。

**可行性已算过（搬得动）**：最小引导集

| 项 | 大小 |
|---|---|
| Android：`Image-kb23` 37 M + `ramdisk-crdroid.img` 11 M + dtb 0.16 M | ~48 MiB |
| Ubuntu 救援：`Image` 37 M + `initrd` 34 M（共用同一个 dtb） | ~71 MiB |
| `systemd-bootaa64.efi` ×2 | 0.25 MiB |
| **合计** | **~120 MiB** |

内置 ESP 有 **188 MiB 空闲**，**不删任何 Windows 文件就装得下**。
（U 盘那 476 M 里大半是历史内核：kb17/18/20/21/22/23 + 各种 `.bak`。）

⚠️ 没有 `efibootmgr` 可用的余地 —— 所有 BLS 条目都带 `efi=noruntime`。
唯一的杠杆是**可移动介质回落路径** `EFI/Boot/bootaa64.efi`，
所以搬迁时必须覆盖内置 ESP 上那一份（**先备份**）。
> 有趣的是 `efi=noruntime` **并不妨碍 `bootctl set-oneshot`**：
> 实测能写能回读 `OneShot Entry`。远程救援闭环靠的就是它。

### 8quater. ✅ 引导链已搬进内置 ESP（2026-08-20 完成，待拔盘实测）

`scripts/esp-migrate-to-internal.sh` 已执行，**纯增量、未删改任何 Windows 文件**。
内置 ESP 现在的内容（占用 108 MiB，**剩余 80 MiB**）：

```
EFI/Boot/bootaa64.efi              124416 B  ← systemd-boot（回落路径）
EFI/Boot/bootaa64.efi.bak-windows 3120168 B  ← 原文件备份
EFI/systemd/systemd-bootaa64.efi   124416 B
EFI/Microsoft/…                              ← 原封未动（bootmgfw.efi 3120168 B 仍在）
loader/loader.conf                           ← default = int-crdroid
loader/entries/<mid>-int-crdroid.conf        ← Android
loader/entries/<mid>-int-ubuntu.conf         ← Ubuntu（rootfs 仍在 U 盘，拔盘则起不来）
<mid>/android/{Image-kb23, ramdisk-crdroid.img, sc8280xp-huawei-gaokun3.dtb}
<mid>/7.2.0-rc2-gaokun3+/{linux, initrd.img-…, dtb-otg.dtb}
```

★**备份文件 3120168 B 与 `EFI/Microsoft/Boot/bootmgfw.efi` 字节数完全相同**
—— 证实被覆盖的那份就是 Windows Boot Manager 的副本，而本体没被动过。
Windows Boot Manager 会被 systemd-boot **自动发现并列进菜单**，无需写条目。

#### ★ 关键实测：固件仍然优先 U 盘 ESP

装完后重启一次（U 盘不拔），读 EFI 变量：

```
/sys/firmware/efi/efivars/LoaderDevicePartUUID-4a67b082-0a4c-41cf-b6c7-440b29bb8c4f
  → d5cb76b5-d8be-4505-ab84-c492bd3bae6e
/dev/sda1         PARTUUID=d5cb76b5-d8be-4505-ab84-c492bd3bae6e   ← 命中
/dev/nvme0n1p1    PARTUUID=825eaf3a-52b8-4a93-a747-921ecefd2ded
```

**结论：U 盘在的时候，走的仍是 U 盘那份，默认 Ubuntu，自动回落安全网完好。**
内置这份只在 U 盘不在时才会被用到 —— 而那时 Ubuntu 的 rootfs（U 盘 sda2）
也不在了。所以**内置 ESP 的默认项设成 Android 才是对的，且不损失任何安全网**。

#### 拔盘实测：✅ 通过（2026-08-20，用户实机）

拔掉 U 盘开机，Android 自动起来；`/dev/block/sd*` 不存在，只有 `nvme0n1`
—— 证明整条引导链已脱离 U 盘。

---

### 8quinquies. ✅ Windows 已抹除，全盘归 Android（2026-08-20）

用户确认「数据都备份了，直接不要」。执行结果：

| 盘上顺序 | 分区 | 大小 | 用途 |
|---|---|---|---|
| 1 | p1 | 300 MiB | ESP（引导链） |
| 2 | **p2** | **376 GiB** | **Android `userdata`**（原 64 GiB） |
| 3 | p8 | 12 GiB | Android `super` |
| 4 | p9 | 64 GiB | `userdata-old` —— 迁移前的完整备份，暂留 |
| 5 | p10 | 32 MiB | Android `metadata` |
| 6 | **p3** | **24.6 GiB** | **Ubuntu 救援系统**（`gaokun3-rescue`） |

未分配空间 1007 KiB（全盘用尽）。**Windows 的 p2/p3/p4/p5/p6/p7 全部删除。**

#### `/data` 扩容用的是「新建 + 克隆 + 换标签」，不是原地 resize

`/data` 当时已经 **100% 满**（62 GiB 用掉 59 GiB，非 root 只剩 235 MiB，
原神 34 GiB + 明日方舟 19 GiB）。做法：

1. 删 p2/p3/p4 → 前面腾出 376 GiB 连续空间，建 `userdata-new`
2. **`dd` 整盘克隆** 旧 p9 → 新分区（不是 rsync）。逐字节复制，
   SELinux 扩展属性 / capabilities / 硬链接全部原样带过去，零解释零遗漏。
3. `e2fsck -f` → `resize2fs` 扩到 376 GiB → 再 `e2fsck -f`，三步全 rc=0
4. 校验：文件数 49999=49999、字节数 63 237 600 073 相同、大文件 md5 抽查 3/3
5. **先把旧的改名 `userdata-old`，再把新的改名 `userdata`**
   —— `fstab.gaokun3` 用的是 `/dev/block/by-name/userdata`（**PARTLABEL**，
   不是 PARTUUID），所以换个标签就完事，fstab 一个字都不用改。
6. 旧 p9 **全程只读、至今保留**，任何一步出问题重启就是原来那台机器。

结果：`/data` 62 GiB → **370 GiB（可用 294 GiB）**，游戏数据不用重下。

#### ★ 固件的启动优先级会变 —— 删分区之后翻转了

这是本轮最容易踩空的一条：

| 时间点 | `LoaderDevicePartUUID` | 实际引导的 ESP |
|---|---|---|
| 刚把引导链装进内置盘时 | `d5cb76b5-…` | **U 盘 sda1** |
| **删掉 Windows 分区之后** | `825eaf3a-…` | **内置盘 nvme0n1p1** |

于是"内置 ESP 只在 U 盘不在时才用到"这个前提**当场失效**，
而内置那份的默认项当时被设成了 Android ——
**「Android 挂死 → 拍电源键 → 自动回落到可远程接入的系统」这条安全网断了**，
而且断得毫无征兆（表现只是"莫名其妙进了 Android"）。

修法：内置 ESP 的默认项改成 `-int-ubuntu.conf`（内置救援系统）。
> 教训：**引导优先级不是一次测定就永久成立的事实**，
> 动过分区表就要重新读 `LoaderDevicePartUUID` 确认一遍。

#### 远程救砖闭环（全内置，实测两个方向都通）

```
默认      → 内置救援 Ubuntu (root=/dev/nvme0n1p3, hostname gaokun3-rescue, 192.168.31.230)
oneshot   → sudo bootctl set-oneshot <mid>-int-crdroid.conf && reboot → Android
Android 里 adb reboot → 自动回落到救援 Ubuntu
```

⚠️ 条目名带 **`int-`** 前缀。U 盘上那套旧条目（`<mid>-crdroid.conf`）仍在，
但已不是现役 —— 用错名字就是 M3 那个"用错条目跑旧内核"的坑。



---

## 9. EC / 电源 / 热 ✅

来源：`09-ec-power.txt`（30 KB）—— 详细数值见 dump

预期对照（`refs/linux-gaokun/README.MD`）：EC 在 i2c-15 @0x38；
电池 sysfs `/sys/class/power_supply/gaokun-ec-battery/`；20 路热传感器。

⚠️ UCSI 已知问题：`README.MD:86-87` 说开机前插 Type-C 可能触发自动重启。
**本次 USB 启动全程未触发**，该风险在简单存储设备上似乎不成立。

---

## 10. WiFi / 蓝牙 ✅

来源：`10-wifi-bt.txt`

- 网卡 `wlP6p1s0` 正常工作（本次采集全程走 SSH over WiFi，IPv4 + IPv6 均通）
- `ath11k_pci 0006:01:00.0: chip_id 0x12 chip_family 0xb board_id 0xff soc_id 0x400c1211`
  - **`chip_id 0x12` = 十进制 18**，与 `refs/matebook-e-go-linux/README.md:209`
    说的「MateBook E Go requires `qmi-chip-id=18`」吻合
- **未出现** `DISASSOC_LOW_ACK` / 40 秒重协商症状

> 之前担心的 board-2.bin 校准缺陷（2026-03-20 ~ 2026-07-11 构建的镜像）
> **本机未表现出症状**。WiFi 稳定可用，暂不需要修。若后续出现周期性断连再处理。

---

## 11. Device tree ✅

来源：`11-devicetree.dts`（356 KB，`dtc -I fs` 从 `/proc/device-tree` 反编译）

remoteproc / interconnect 状态见 dump 末尾。可与
`refs/linux-gaokun/dts/sc8280xp-huawei-gaokun3.dts` 做差异比对。

---

## 12. dwc3 / USB —— Stage 1 第二条验收，尚未满足

来源：实测 SSH

三个 dwc3 控制器全部绑定为 **host 模式**：

```
/sys/bus/platform/drivers/dwc3/a400000.usb -> .../soc@0/a4f8800.usb/a400000.usb
/sys/bus/platform/drivers/dwc3/a600000.usb -> .../soc@0/a6f8800.usb/a600000.usb
/sys/bus/platform/drivers/dwc3/a800000.usb -> .../soc@0/a8f8800.usb/a800000.usb
```

- `/sys/class/udc/` **为空** → 没有任何 USB gadget 控制器
- `lsusb -t` 三条总线全是 `xhci-hcd`（host）
- `/proc/device-tree` 下**没有找到任何 `dr_mode` 属性**

> **CLAUDE.md 第 97 行 Stage 1 的第二条验收「dwc3 进 peripheral 模式」未满足。**
> 这直接决定 Stage 2 的验收「adb shell 通了」—— 没有 gadget 控制器，
> adb 永远枚举不出来，而这台机器没有串口，等于零调试通道。
>
> 需要在 DTS 里把某个控制器设成 `dr_mode = "peripheral"`（或配 `usb-role-switch`
> 走 Type-C DRP）。这与 pstore 一样需要重编内核 + DTB，可以合并为同一次构建。

---

## 结论：下一步

Stage 0 的采集部分全部完成，只剩 pstore。而 pstore 和 dwc3 都需要重编内核，
且 binderfs 已经就绪 —— **把 Stage 0 收尾和 Stage 1 合并成一次内核构建最省事**：

1. 以 `refs/gaokun-buildbot/defconfig/gaokun3_defconfig` 为基线
2. 加 `CONFIG_PSTORE*`
3. DTS 加 ramoops 节点（地址待定并实测验证）
4. DTS 设一个 dwc3 为 peripheral
5. 用 `refs/gaokun-buildbot/scripts/ci/` 的流程构建

构建机：用户的 15 核 / 32 GB 机器（内核构建约 30–60 分钟，远轻于 AOSP）。

## USB-C 物理口与控制器对应（2026-10-05 实机 + 用户目视确认）

| 物理位置 | typec 端口 | dwc3 控制器 | 能力 |
|---|---|---|---|
| **靠近电源键** | `port0` | `a600000.usb`（父 `a6f8800.usb`，DT 的 `usb_0`） | host / device 都能当；USB adb 只走这个口（usb_role `a600000.usb-role-switch`） |
| 另一个口 | `port1` | `a800000.usb`（父 `a8f8800.usb`，DT 的 `usb_1`） | 只能当 host（`/sys/class/typec/port1` 的 supplier 是 `platform:a800000.usb`） |

依据：线插在另一个口时 `port1-partner` 出现、`a600000` 的 UDC 为 `not attached`；换到靠近电源键的口后 `port0-partner` 出现、UDC `configured`、USB adb 回来。
键盘盖走的是第三个控制器 `a400000.usb`（`usb3` 下 `3-3` = `12d1:10b8`），与两个 USB-C 口无关。

