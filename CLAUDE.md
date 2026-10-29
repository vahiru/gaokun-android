# 项目：MateBook E Go (sc8280xp / gaokun3) 移植 Android

在华为 MateBook E Go 上跑原生 AOSP（现基于 crDroid 13 / Android 17；另有一条不公开的 HyperOS 移植线），目标是稳定运行 arm64 手游。本文件每个会话都整份读入 —— **只放"现在"和规矩**，历史进 `docs/project-log.md`。

## 现在（2026-10-30；每次开工更新，旧内容搬进 project-log）

* **阶段：1.0 发版前，1.0 改在 A17 上发**（crDroid 13 / `android-17.0.0_r1`，lunch `lineage_gaokun3-cp2a-userdebug`；用户 10-25 定：A16 停维护，rc.1 / rc.2 的 A16 验收不再作为 1.0 依据）。
  `dev` 10-26 已快进到 A17 线（`fcf069b`），`main` 仍停在 A16 时代。迁移 M0–M4、M6 已完成，M5 / M7 没做（`docs/a17-migration-plan.md` §7）。
  已发布的最新版仍是 A16 的 **`1.0.0-rc.2`**（rc2e，戳 `1792913333`，10-25 进 OTA；GitHub pre-release，Latest 仍是 v0.7.1-alpha）。⬜ A16 → A17 的 OTA 会被 Updater 的 `allow_major_upgrades` 拦下，rc.2 用户怎么升还要用户定。
  剩什么看 `docs/TODO.md` 顶部；发版标准 / 阻断项 / 待定决定看 `docs/v1.0-plan.md`。
* **HyperOS 移植**（分支 `hyperos-port`，**未推、不能公开分发**，`docs/hyperos-port-research.md`）：装在一套隔离分区（`super_hos` / `userdata_hos` / `metadata_hos` / `misc_hos`），从 ESP 上单独的 `gaokun3-hos-hyperos-r*` 条目启动。
  10-30 已部署到 **R18**（R17 内核，enforcing）；R19（光感 / 自动亮度 registry）只做了离线修复和只读诊断，没上机。用户 10-30 说以后**主力用 HyperOS**，但**默认启动项不改**（仍是 crDroid）。
  进度与未关闭项看 `docs/hyperos-adaptation-progress.md` 末尾几节；工具 `tools/hos/README.md`。
* 内核 v7.2.9 stable + `patches/0076`（撤回 stable 撤掉的双 DSI 绑定 PLL 修复，否则黑屏）；dev.9 起发布内核不带指纹的 0050（D21，只进实验内核）。
* 统一启动入口 `gk3boot.efi` + fastboot 执行端：E3–E8（含 OTA 自动回滚）、E6、E7 真机过；镜像默认动作模式 + BCB 分派开；E10 恢复出厂用户定不测（`docs/boot-entry-design.md`）。
* 发布构建（dev.1 起）= 关免授权 adb、`ro.debuggable=0`，变体仍是 userdebug。SELinux 默认 enforcing（用户 10-06 定 D5，dev.10 起）。
* 图形安装器（Flutter + Debian live，现行 `0.2.0-rc.x`，随 rc 发）真机装过、网络安装通；双系统 / Windows 伴随工具已写，多数没上过真 Windows（`docs/stage7-flutter-debian.md`）。
  ⚠️ 10-30 改过分区后 p2 / p12 不再按磁盘顺序排（见下），安装器 / 双系统脚本里按顺序假设布局的地方没复核过。
* 指纹：华为签名 TA 已在本机加载进 QSEE（app_id=5，#125）。10-29/30 离线逆向收口：FF_CMD 的 CREATE/INIT 不适用于 FTE7001，M2–M4 都走 wire 0x80xx 命令（`docs/fingerprint-driver-design.md`）。
  10-30 起 **M2 上机进行中**（先发只读探测 0x8031；需要带 0050 的实验内核）；剩 client driver + HAL（TODO T6）。

> ## ★★★ 开工前先读
>
> ### 设备与连线
> USB adb（**只走靠近电源键的口 port0**）与 TCP adb（`:5555`）。家里 SSID 下 crDroid 是静态 IP `192.168.10.239`（路由器 `192.168.10.1` 也做了保留），**HyperOS 下是 `192.168.10.182`**；
> 其他网络走 DHCP、会漂（本机网段也会漂）⇒ 跑 **`bash scripts/find-device.sh`**（绕沙箱，扫本机所在每个 /24、按协议认身份）。
> 手工找就扫**全网段**，别按"上几次落在哪"收窄（09-12 因此把好好的机器误判成挂死）；判据 `getprop ro.crdroid.device`（HyperOS 下没有这个属性，看 `getprop ro.boot.hos_build`），局域网里的小米手机 `pudding` 也开着 5555。
>
> ### 四条会让你损失一小时以上的运维坑
> 1. **`| tail` 之后的 `$?` 是 `tail` 的退出码**（`az` 停机、`make`、`gh release upload` 都栽过）⇒ 判据看**产物**。
>    大文件传完必核字节数 + sha256（`scp` 退出码 0、文件只有 77%）。续传：Linux 上 `rsync --partial --append-verify`；**本机 macOS 的 rsync 是 openrsync，没有 `--append-verify`**⇒
>    `ssh 构建机 "tail -c +$((已有字节+1)) 文件" >> 文件` 追加、比 sha256、循环设重试上限。
> 2. **沙箱代理掐断到构建机的 ssh、MITM 掉 `az` 的证书，两者要求相反**：ssh **绕沙箱**、用真实 IP（`cicd` 会被解析成 fake-IP `198.18.0.92`）；
>    `az` **留在沙箱内**并加 `AZURE_CLI_DISABLE_CONNECTION_VERIFICATION=1`（绕沙箱会证书失败，停机失败就一直计费）。停机后回查真实电源状态。
> 3. **一行命令里永远不要 `pkill -f` / `pgrep -f <名字>`** —— 会匹配到自己的 shell 并杀掉。用 `pkill -x`、`kill $(pidof …)` 或 pid 文件。
> 4. **内核树上的补丁只活在工作区（从未提交）** ⇒ 在那棵树上 `git checkout -- <路径>` / `git restore` / `git stash` 是**破坏性**的（2026-09-16 一次清空 0037–0045）。
>    撤补丁用 `git apply -R`；撤不掉就重放整条链 `scripts/kernel-apply-patches.sh <树>`（幂等）。
>
> ### 四条操作禁忌（每条都是一次事故换的）
> 1. ⚠️★★★ **camss 已 `runtime_error` 时不要 unbind** —— 拖死整机。**实验该在什么状态下做，是实验设计的一部分**（修 unbind 路径的补丁本该在健康状态下测）。
> 2. ⚠️★★★ **可能被门控的寄存器块，`/dev/mem` 读和写一样危险**（读 runtime-suspend 的 camcc → external abort、静默死机）。非读不可先钉住控制器、确认 `runtime_status=active`；**没人能按电源键时一概不做。**
> 3. ⚠️ **重启 / 装内核前征得用户同意**：失败要有人按电源键，oneshot 兜底≠不用动手。
> 4. ⚠️ **装机脚本的挂载点不要叫 `/mnt/esp`**（被另一个 shell 顺手 umount，安全网静默失败）。共享可变状态要么私有、要么加锁。
>
> ### 发布与构建
> * 构建机的树**不等于**本仓 checkout（咬过五次）：编内核前 `bash scripts/kernel-apply-patches.sh <树> --verify` 绿了再 make；同步设备树**只用 `bash scripts/sync-device-tree.sh <IP>`**
>   （手写 `rsync --delete` 删过四样不入库但必需的输入 adb_keys / firmware / hexagonrpcd-root / prebuilt-boot，md5 核对照样通过；脚本会断言它们在）。
> * 发版一律 **`release.sh --no-build`** —— 重跑构建发出去的不是硬件上验过的那一版。
> * ⚠️★★ **ROM 变体只用 `lineage_gaokun3-cp2a-userdebug`（A17 起；A16 时是 `-bp4a-`）**：`-user` 的 init 强制 enforcing（`selinux.cpp:112-116`），装上去起不来（#117 §15）。
> * **推仓库、发版要用户点头**；其余（本地提交、构建、staging、设备实验）直接做。
> * ★ **分支（用户 2026-10-18 定，有协作者了）：所有修改都在开发分支 `dev` 上改、提交、推 `dev`，不直接提交 / 推 `main`。**
>   `main` 只在发版时从 `dev` 合并（什么时候合由用户定）；协作者的 PR 也提到 `dev`。开工先 `git switch dev && git pull`。
>
> ### 现在设备上跑的是什么（2026-10-30，根据当天各会话的记录整理，没有重新实机读取；历次状态见 project-log）
> * ★ **分区 10-30 改过**（用户在 Ubuntu 救援里操作；脚本、日志与改前 GPT 备份在 `out/repart-20261030/`，复核记录 `docs/hyperos-repartition-20261030-review.md`）：
>   p12 `userdata_hos` = 扇区 616448–789178367（**376 GiB**），p2 `userdata`（crDroid）= 839845888–948561919（**51.8 GiB**）。分区号 / PARTUUID / PARTNAME / fs UUID 都没变，
>   但 **p2、p12 不再按磁盘顺序排，盘上也没有空闲空间了** ⇒ 任何按扇区或分区顺序假设布局的脚本（installer、hw-inventory）以这一条为准。
> * **crDroid**：active = `_a` = A17 戳 `1792983426`（enforcing，10-26 A 档过，A21 iris 15/15）；`_b` 被 update_engine 标成不可启动；**A16 已不在任何槽上**，
>   A16 的 `/data` 快照 10-30 已删 ⇒ 不能带着数据回 A16。ESP default 仍是 `*-android-a.conf`。crDroid 的 `/data` 10-30 从 HyperOS 里离线清到约 2.5 GiB
>   （游戏和日常应用删了，`packages.xml` 里还留着这些包的登记，下次进 crDroid 看会不会自动清掉）。**改完分区后 crDroid 还没起过。**
> * **HyperOS**：R18 的 system / HAL + R17 内核（ADC_TM5 内建、真实皮温 `skin-temp`）。常驻设置：USB 持久 **device-only**（`persist.vendor.gaokun3.usbrole.device_only=true`，
>   代价是这个口接不了 U 盘），无线 adb `persist.adb.tcp.port=5555`，家里 IP **`192.168.10.182`**（和 crDroid 的 `.239` 不是同一个）。
>   ⚠️ `scripts/find-device.sh` 靠 `ro.crdroid.device` 认机器，**在 HyperOS 下认不出来**。设置页按压色只是临时 overlay，没进镜像。
> * Ubuntu 救援（ESP 条目）在家网 `192.168.10.225`（`ssh user@…`），HyperOS 的 super 升级都在这里用 `tools/hos/update-isolated-super.py` 做；
>   救援里存在 `/var/lib/gaokun-hos-super-update.started.json` 时说明上次升级没收尾，**不要直接重试**，先核对任务和日志。
> * ESP 10-30 清理过（备份 `out/esp-backup-20261030-hos/`），只剩约 43 MiB。R15 条目还引用 `r6-baseline/gaokun3.dtb` 与 `r9-hyperos/ramdisk.img`，别删。
> * 内置盘 p3 上的 `gaokun3 installer`（live）是 0.2.0-rc.1（10-11 换的，旧版 `.bak-0927`）。
> * ⚠️ crDroid 里分派开着：**设置里的恢复出厂会真擦**（发布构建会丢 adb_keys ⇒ 连不上 adb）；`adb reboot bootloader|fastboot|recovery` 进 fastboot 执行端，Mac 上 `fastboot reboot` 回来。
> * 常驻（crDroid）：预置 `/data/misc/adb/adb_keys`（Mac + Windows）+ 持久化 `persist.sys.usb.config=adb`、`persist.adb.tcp.port=5555` ⇒ 发布构建下 USB / TCP adb 照常；`adb shell` 经 KSU 即 root（无 `adb root`）。
>   `persist.vendor.gaokun3.allow_suspend` 开发机持久为 0（不睡；镜像默认 1）；`persist.logd.audit.rate=1000`。⚠️ crDroid `/data` 被清过，这几项没复核。
> * A17 的 adbd 会把每条 shell 命令原样打进 logcat（`adbd service requested '…'`）⇒ 在设备上 `logcat | grep 关键字` 会先匹配到命令自己，判据前先 `grep -v "adbd service requested"`。

## 文档地图

| 想知道什么 | 去哪 |
|---|---|
| **全部文档的索引** | `docs/README.md` |
| 还剩什么、优先级 | `docs/TODO.md`（顶上总表） |
| 1.0 前修什么、什么顺序 | `docs/v1.0-plan.md`（已定的决定在第 6 节开头）；每版回归 `docs/release-checklist.md` |
| 统一启动入口 / fastboot | `docs/boot-entry-design.md`（实验 E0–E11、待定 U1–U11）、`tools/gk3boot/README.md`；旧稿 `docs/fastboot-design.md` 只剩执行端细节有效 |
| **结论怎么来的（最权威）** | `docs/stage4-findings.md` 的 `#NN` 案卷；另有各 Stage 专档 |
| 触摸调参 / 历史 / 安装 | 案卷 #114–#116 + `scripts/touch/README.md` / `docs/project-log.md` / `docs/INSTALL.md` |
| 硬件原始数据 | `docs/hw-inventory.md`、`docs/hw/` |
| 发版说明 / 投上游的补丁 | `docs/relnotes/` / `docs/upstream/`（未发） |
| 构建机 | `docs/build-machine.md` + `scripts/cicd.sh` |
| 图形安装器 | `docs/stage7-flutter-debian.md`；`scripts/live/installer-lib.sh`、`scripts/live/README.md` |
| 指纹 | `docs/fingerprint-driver-design.md`；案卷 #120/#123/#124/#125；`tools/fingerprint-bringup/` |
| A17 迁移 / HyperOS 移植 | `docs/a17-migration-plan.md`（§7 进度）/ `docs/hyperos-adaptation-progress.md`（末尾是现状）+ `docs/hyperos-r*-*.md`、`tools/hos/README.md` |
| 游戏特化（兼容 / 延迟 / 触摸 / 游戏模式 / 检测边界） | `docs/gaming.md`；过程 `docs/duck-detector-plan.md`；TODO「②′ 游戏环境」GAME-1–8 |

⚠️ **冲突时**：实机实测 > 案卷 > 本文件 > `project-log.md`。历史里大量结论已被实测推翻（推翻过程故意留着），别当现状。

## ⚠️ 给 AI 助手的强制规则

**这个平台没有任何 Android 移植的前人成果**，训练数据里没有这台机器上的 Android 知识。因此：

1. **任何具体的 kernel config 名、AOSP property 名、HAL 接口名、文件路径，必须从本地源码树 grep 出来并给出文件路径和行号**，不许凭记忆 —— 记忆里的名字在这里大概率是错的或过时的。
2. **不确定就明说"我不确定，需要验证"**。自信的错误答案比"我不知道"贵得多。
3. **实机行为以 dmesg / logcat / 用户的实际观察为准**，与你的判断冲突时以实机为准。
4. sc8280xp 硬件细节优先查 `refs/linux-gaokun`、`refs/jhovold-linux`；AOSP-on-mainline 的组织方式优先查 `refs/aospm-*`。

## 关键约束

- **没有高通 Android BSP / vendor blob** ⇒ 只能 **AOSP on mainline**，HAL 全部自建，参考 aospm。
- **原生不是 fastboot 设备**（UEFI）：`fastboot flash` / `by-name` 软链接 / A/B 的 AOSP 常规流程都要改写（1.0 自建了入口与执行端）。
  ⚠️ **fstab 用 PARTUUID，不用 PARTLABEL** —— Windows 建的分区 PARTLABEL 全是 `Basic data partition`（hw-inventory 第 8 节）。
- **没有暴露的串口**，早期启动失败 = 纯黑屏。崩溃日志走 **`efi_pstore`**；**ramoops 在这台机器上不可能工作**（固件每次复位都重新初始化 DRAM，高低两个地址都实测过）。见 hw-inventory 第 7bis 节、`scripts/pstore-ctl.sh`。
- **无 modem**（aospm 的 libqril/qrild/qrtr 全跳过）。**arm64 原生**，手游 arm64-v8a 直接跑。

## 硬件事实

SoC **SC8280XP**（8cx Gen 3）；代号 gaokun3（gaokun2 是另一套 EC 协议）；型号 **HUAWEI GK-W7X，SKU C233，2022 款，CSOT 面板，触摸固件 `41 07`**。
BIOS 本机 2.16，**不依赖 BIOS 版本**（2026-09-25 用户：已有人验证）。GPU **Adreno 690**（freedreno + turnip）。
屏幕 **Himax HX83121A / ppc357db11 WQXGA**，MIPI-DSI，与 Galaxy Tab S7 FE 同款。WiFi/BT WCN6855（ath11k + hci_qca）。
EC 华为自研（主线 6.15；UCSI 6.16；DSI 面板 7.1）。存储 **NVMe**（不是 UFS）。引导 UEFI（可关 Secure Boot，systemd-boot）。KVM/EL2 可用。
未完：指纹 FocalTech FTE7001（见上）；TPM 不支持；S4 未测。s2idle ✅（M16：真凶是我们自己加的 `dr_mode="otg"`，与内核、EC 无关）。

## 环境

- **编译机 Azure VM `CICD`**（资源组 `AIROUTER_GROUP`，`centralindia`，Dasv5，504 GB `StandardSSD_LRS` 盘 —— 比原 Premium 慢，别拿老记录估时间），**按分钟计费、用完停机**。
  crDroid 树 `~/crdroid`；现役内核树 `~/gk3-kernel-72y`（`~/gk3-kernel-iris`、`~/gk3-kernel` 是旧配方，build-machine §6）。
  **按负载选机型**（`cicd.sh` 在沙箱内跑）：`light`（D4，不跑 `m`）/ `kernel`、`module`（D16）/ `rom`（D32）/ `clean`（D64）；**任何 `m` 不低于 D16**。
  机器已在运行就不换机型；停机前 ssh 看有没有别的会话的构建（build-machine §3）。直连回家约 1.3 MB/s ⇒ 大文件走 **R2 中转**。⚠️ NSG 22 端口仍对 `*` 开放。
- **目标机只有一台**，Windows 已抹除，**没有对照机** —— "正常是什么样"只能靠救援 Linux、案卷和实测。本机 Mac **编不了 AOSP**。

## 本地参考树（`refs/`，不入库；新 checkout 先 `bash scripts/clone-refs.sh`，否则强制规则 1、4 无法执行）

```
linux-gaokun  right-0903/linux-gaokun  本机内核核心
matebook-e-go-linux  whitelewi1-ctrl/matebook-e-go-linux  GK-W7X patch
boot-works  matalama80td3l/matebook-e-go-boot-works  面板驱动
jhovold-linux  jhovold/linux (wip/sc8280xp-6.16)  已停更，仅历史对照
gaokun-buildbot  KawaiiHachimi/linux-gaokun-buildbot  ⭐ 内核基线补丁来源
egotouchrev-linux  chiyuki0325/EGoTouchRev-Linux  触摸 SPI 驱动
aospm-device-sdm845  aospm/android_device_generic_sdm845  ⭐ 设备树模板
aospm-manifests  aospm/android_local_manifests
aospm-system-core  aospm/platform_system_core  看 diff
aospm-tinyhal  aospm/tinyhal  音频 HAL
lineage-sepolicy  LineageOS/android_system_sepolicy (lineage-23.0)  ⭐ SELinux 唯一权威
```
（均在 github.com。）现役内核基线：**v7.2.9 stable + buildbot 19 个提交 + 本仓 `patches/`**（2026-10-05 起）。
固件：`matebook-e-go/uup-drivers-sc8280xp` + linux-firmware ≥ 20241210；`qcom/sc8280xp/HUAWEI/gaokun3/qc{adsp,cdsp,slpi}8280.mbn` 不在 linux-firmware 里，必须自己带。

## 阶段

0–5 ✅（主线 Linux → AOSP 启动 → 图形 → 输入音频 WiFi 电源 → freedreno/turnip；`docs/stage{2,4}-findings.md`、`stage5-freedreno.md`）· 6 crDroid + 产品化 · 7 图形安装器 · 现在 1.0。游戏适配 Stage 5 后已达成。验收原文见 project-log。

## 已知坑

- **触摸 IC 模式由 gpio174 在固件重载瞬间的电平决定**（低=SPI、高=I2C HID，I2C 间歇失灵），显示复位会静默触发重载 ⇒ 触摸随机死亡 / 幽灵触点 / 探测成功但全聋。修复 = pinctrl 恒拉低（`patches/0002-*`，#26）。空闲 IRQ 速率是状态指纹：≈显示扫描率（亮屏 ≈120/s）正常、0 停摆、乱 = 模式错乱。
- **`timeout N getevent > 文件` 会因块缓冲丢光输出** —— 用 `cat /dev/input/eventX` 录二进制离线解码（#26）。
- **UCSI 有缺陷**（`refs/linux-gaokun/README.MD:86-87`），常见 `PPM init failed`；不影响 adb（`dr_mode="otg"` 无 role 源时落 device），代价是只有 high-speed。
- **DT label 与物理地址不对应**：`usb_0` = `a6f8800`、`usb_1` = `a8f8800`；改 dwc3 前 `readlink -f /sys/block/sda` 确认。
- **`super.img` 是 sparse（`0xED26FF3A`），不能 `dd`**，先 `simg2img` —— 误 dd ⇒ init 主动复位、不留日志（stage2-findings §1）。
- **`CONFIG_SECURITY_SELINUX=y` 不等于启用**，还要在 `CONFIG_LSM` 里；查 `/sys/kernel/security/lsm`。
- **Android init 失败是主动 `reboot()` 不是 panic**，pstore 抓不到：改造 ramdisk 写 ESP（stage2-findings §5）；`androidboot.init_fatal_panic=true` 只覆盖 LOG(FATAL)，服务级 `reboot_on_failure` 仍是正常 shutdown，两条都要布。
- **cgroup v1 在 6.12+ 拆到 `*_V1` 后且默认关**：缺 `CPUSETS_V1` ⇒ `SetupCgroups` 失败 ⇒ `bootstrap-apexd-failed`；一并要 `MEMCG_V1`、`UCLAMP_TASK(_GROUP)`（同上 §8）。
- KMS 25 plane / 6 CRTC；modifier 只有 `LINEAR` 与 UBWC（hw-inventory §3）。
- 写进这里的预测要和实测一样接受复查（被推翻的旧条目见 project-log 2026-10-06 一节）。

## 捷径与协作

面板参考 Galaxy Tab S7 FE（SM-T733）设备树；止损可用 Cuttlefish。问 gaokun 社区（面板、EC、休眠、触摸）、aospm 社区（HAL、设备树）；平行项目见 `docs/parallel-mainline-generic.md`。
