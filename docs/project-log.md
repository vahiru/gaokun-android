# 工程日志（按时间倒序）

这里是 **CLAUDE.md 曾经的前言框与全部 Stage 里程碑，原样搬过来的**，一个字没删。

**为什么搬**：CLAUDE.md 是每次开工都会被整篇读一遍的文件。到 2026-09-14 它已经 1629 行，
其中约 1400 行是【历史叙事】—— 读它的代价每个会话都要付一次，而其中绝大多数内容
在当天是用不上的。把历史挪到这里之后，CLAUDE.md 只留【现在要知道的事】。

**怎么用**：
* 想知道"某个结论是怎么来的" → 先查 `docs/stage4-findings.md`（按编号 #NN 的案卷，最权威）
* 想知道"那一周发生了什么" → 查本文件
* 想知道"现在是什么状态、有什么禁忌" → 回 `CLAUDE.md`
* 想知道"还剩什么没做" → 查 `docs/TODO.md`

⚠️ 本文件里的内容**会过期**，而且有些条目当场就被后来的实测推翻了（原文保留了推翻过程，
那正是它的价值）。**与 `CLAUDE.md` 或 `docs/stage4-findings.md` 冲突时，以那两份为准。**

---

## 2026-10-30 从 CLAUDE.md 搬来：「现在」与「现在设备上跑的是什么」（截至 10-25 / 10-18 的版本）

**为什么搬**：10-25 之后有三件事改变了现状 —— 1.0 改在 A17 上发、`dev` 快进到 A17（10-26，开发机 `_a` 换成 A17 `1792983426`，A16 不在任何槽上）；
HyperOS 移植从调研推进到 R18 并成为用户的主力系统（10-26 → 10-30）；10-30 用户重排了 p2 / p12 两个数据分区。CLAUDE.md 的这两段已改写成 10-30 的状态，
下面是被替换掉的**原文**（`## ` 标题降了一级，引用框照旧，正文一字未改）。这期间的经过：A17 迁移看 `docs/a17-migration-plan.md` §7；
HyperOS 看 `docs/hyperos-adaptation-progress.md`（R3–R19）与 `docs/hyperos-r*-*.md`；分区重排看 `docs/hyperos-repartition-20261030-review.md`；
指纹离线逆向收口（10-29/30）看 `docs/fingerprint-driver-design.md`。

### 现在（2026-10-25；每次开工更新，旧内容搬进 project-log）

* **阶段：1.0 发版前。** **`1.0.0-rc.2` 2026-10-25 已发布并进 OTA 清单**（用户定：v0.7.1-alpha / rc.1 用户在「系统更新」里都会收到；**0.7.x → 1.0 升级（G6 / E11）没演练就发了**，发版说明里写了先备份）。发的是第五次构建 **rc2e，戳 `1792913333`**（dev `827ed7b` = rc2d + Parts 触摸手感 game / daily 开关；内核 `7636022f`、boot.img `f60c65d2` 与 rc2d 同），tag `v1.0.0-rc.2` = `6464492`（= origin/main = origin/dev）。R2：`builds/` + `install/crDroidAndroid-16.0-20261025-gaokun3-v12.11/` + `installer/0.2.0-rc.2/`（U 盘镜像带 rc2e 系统）；GitHub pre-release（不标 Latest，Latest 仍是 v0.7.1-alpha），13 个附件字节数核过。开发机 `_b` = rc2e、active = `_b`，A 档 47 / 0（`out/accept/1792913333-20261025-165335/`）。留档 `out/rc2-1792913333/`。⬜ 盯 issue / 群里 0.7.x → rc.2 的升级反馈。剩什么看 `docs/TODO.md` 顶部「▶ 1.0」；发版标准 / 阻断项 / 待定决定看 `docs/v1.0-plan.md`。
* 内核 v7.2.9 stable + `patches/0076`（撤回 stable 撤掉的双 DSI 绑定 PLL 修复，否则黑屏）；dev.9 起发布内核不带指纹的 0050（D21，只进实验内核）。
* 统一启动入口 `gk3boot.efi` + fastboot 执行端：E3–E8（含 OTA 自动回滚）、E6、E7 真机过；镜像默认动作模式 + BCB 分派开；E10 恢复出厂用户定不测（`docs/boot-entry-design.md`）。
* 发布构建（dev.1 起）= 关免授权 adb、`ro.debuggable=0`，变体仍是 userdebug。SELinux 默认 enforcing（用户 10-06 定 D5，dev.10 起）。
* 图形安装器（Flutter + Debian live，现行 `0.2.0-rc.x`，随 rc 发）真机装过、网络安装通；双系统 / Windows 伴随工具已写，多数没上过真 Windows（`docs/stage7-flutter-debian.md`）。
* 指纹：华为签名 TA 已在本机加载进 QSEE（app_id=5，#125）；剩 client driver + HAL（TODO T6）。

> ### 现在设备上跑的是什么（2026-10-18 22:3x 实机读到；历次状态见 project-log）
> * ★ **10-18 22:1x 起：active = `_a` = rc.2 候选 `1792312407`**（`install-ota-local.sh --go` 装、OneShot 直连条目起来、验收后 `set-active-boot-slot 0`，ESP default = `*-android-a.conf`），
>   内核 0085 版（`uname -r` = `6.6.30-android15-8-…`，真实 7.2.9）、enforcing；`_b` 仍是 `1791704075`。adb 便利持久化照旧（USB + TCP 5555 都通）。
>   ⚠️ 屏幕锁着（开机后没人解锁，焦点 `FallbackHome`），解锁前普通 App 的入口解析不到。
> * （以下是装 rc.2 之前的记录）**10-18 16:2x 只读复核**：跑的仍是 `_b` = `1791704075`、内核 `7.2.9-gaokun3+`、enforcing（USB adb 在，5555 局域网里没扫到）。
>   ⚠️ 但 **active 槽是 `_a`**（`bootctl get-active-boot-slot` = 0、`_a` 已不可启动），这次开机（10-18 14:28）是 gk3boot **回落**到 `_b`
>   （`androidboot.gk3boot.event=fallback`、`streak=1`）—— 之前有人把 active 设成了 `_a`、`_a` 没起来；谁设的、`_a` 为什么起不来没查（开机日志已滚掉）。
>   `vendor.gk3pend` 在 7.2.9 上每 10 秒崩一次重启（没有 `gk3_pen` 设备，PR #27 的内核侧不在这个内核里），无害但刷日志。
> * **active = `_b` = 新 ROM 戳 `1791704075`**（10-11 构建，带 tree-fix [22]、去掉 swangle sepolicy；**不带 [23]**——`/system/build.prop` 指纹仍是 `:userdebug/`，`getprop` 的 `:user/` 是运行时改的；内核槽里是 dev.11 的 `705b1387`；载荷不在本机 `out/`），enforcing。
>   18:2x 用户按电源键重启后走默认入口（`via=gk3boot`、`gk3boot-android-b.conf`）⇒ 内核是 `boot_b` 里的 **7.2.9**（`7.2.9-gaokun3+`）——
>   证实 `boot_b` 没换成 0085 内核（TODO GAME-4）。0085 内核 `out/dev12-k0085/vmlinuz-a8f7c90e.efi` 只能走直连槽条目 `8a29534f…-android-b.conf` + oneshot 起。
>   10-11 21:4x 重启过（B5 测试，oneshot 进内置安装器再回来）⇒ TrickyStore 已回来、走默认入口 gk3boot 起的 7.2.9。
  ⚠️ **内置盘的 `gaokun3 installer` 10-11 已换成 0.2.0-rc.1**（`nvme0n1p3:/gaokun3/live.squashfs` + ESP `…/live/initramfs.img`；旧 09-27 版是同目录 `.bak-0927`）。ESP `slot_b/Image` 是 0085 内核（live 里 `uname` 报 6.6.30）。
  ✅ enforcing 下 `boot-oneshot.sh` **不用** `setenforce 0` 就能写（10-11 实测，a1d303a 生效）。
>   设备上另有用户自装的 TrickyStore / PIF / LSPosed / HMA（`hma_oss_zygisk` disable 中）—— 测游戏流程见 `docs/gaming.md`。
> * 以下 `_a` 一条是 10-09 的记录，10-11 未复核（其中「active = `_a`」「`_b` = rc.1」已被上一条取代）：
> * 槽 **`_a` = `1.0.0-dev.11`**（戳 `1791554509`，内核 `705b1387…` = 7.2.9 + 0078），active = `_a`（ESP default 已同步），**SELinux enforcing**；`_b` = `1.0.0-rc.1`（戳 `1791285623`）。载荷 `out/dev11-1791554509/`（rc.1 的在 `out/rc1-1791285623/`，安装器 `out/rc1-wt/out/release-installer/0.2.0-rc.1/`）。`a1d303a`（efivarfs 标签）已在 dev.11 里；enforcing 下 `boot-oneshot.sh` 不 `setenforce 0` 也能写（10-11 在 `1791704075` 上实测）。
> * ESP 上的实验内核 `android/va39w/` 与条目 `gaokun3-va39w.conf` 10-19 已删（备份 `out/esp-backup-20261019-va39w/`）。
> * ⚠️ 分派开着：**设置里的恢复出厂会真擦**（发布构建会丢 adb_keys ⇒ 连不上 adb）；`adb reboot bootloader|fastboot|recovery` 进 fastboot 执行端，Mac 上 `fastboot reboot` 回来。
> * ESP 10-06 已清理（备份 `out/esp-backup-20261006/`）：菜单只剩 Android（入口 / 上一版入口 / fastboot / 两个直连槽）、`gaokun3 installer`（内置盘上的 live，能重装，安全网）、Ubuntu 救援。
> * 10-06 Mac 与设备都在热点「Xiaomi 17」（设备 `10.146.153.115`）。
> * 常驻：预置 `/data/misc/adb/adb_keys`（Mac + Windows）+ 持久化 `persist.sys.usb.config=adb`、`persist.adb.tcp.port=5555` ⇒ 发布构建下 USB / TCP adb 照常；`adb shell` 经 KSU 即 root（无 `adb root`）。
> * 常驻：`persist.vendor.gaokun3.allow_suspend` 开发机显式持久化为 0（不睡；镜像默认 1；旧名 `persist.gaokun3.allow_suspend` 的孤儿值无害）；`persist.logd.audit.rate=1000`。
>   2026-10-06 实机核过：`persist.vendor.gaokun3.allow_suspend` 仍为持久 0（开发机不睡）；`screen_off_timeout` 已是 600000（10 分钟）。

---

## 2026-10-06 从 CLAUDE.md 搬来：瘦身前的 CLAUDE.md（第 8 行起全文）

**为什么搬**：2026-10-06 CLAUDE.md 涨到 44 KB（每个会话都整份读入），其中大半是历史 —— 开头那一大段"当前阶段"
（2026-09-16 v0.6.2 → 2026-10-05 1.0 dev.8 的逐日进展）和"现在设备上跑的是什么"里一层层叠起来的旧状态（2026-09-29 → 2026-10-06）。
CLAUDE.md 瘦到约 15 KB，只留"现在"和规矩；下面是瘦身前第 8 行到末尾的**原文**（只把 `## ` 标题降了一级以免和本文件的结构混在一起，正文一字未改）。
时间跨度 2026-09-16 – 2026-10-06，比本文件下面最早那批（截至 2026-09-14）新，所以放在最前。

里面有什么、现在去哪看：
* **"当前阶段"大段**（下面第一段）：v0.6.2 → PR #6 相机 → 指纹 TA 加载（#125）→ SELinux 第五 / 六 / 七轮（#117 / #126 / #129）→ Stage 7 Flutter 安装器 M0–M4b →
  qcom-iris（#128）→ v0.7.0-alpha → issues #11–#16（#131）→ v0.7.1-alpha → 1.0 dev.1–dev.8。现状看 CLAUDE.md「现在」与 `docs/TODO.md`。
* **"开工前先读"框**：设备与连线、四条运维坑、四条操作禁忌、发布与构建 —— CLAUDE.md 里压缩了措辞、意思没变，事故的完整经过在这里；
  "现在设备上跑的是什么"的全部历次状态（2026-09-29 v0.7.0 → 09-30 SELinux 第七轮测试版 → 10-04 v0.7.1 → 10-05 dev.1–dev.7 → 10-06 dev.9），CLAUDE.md 只留了最新一条和仍成立的常驻事实。
* **已被推翻 / 过时、CLAUDE.md 已删的**：已知坑里两条划线（"EC 挂起 / 恢复会先炸" —— M4 / M16 推翻；"plane 少会 fallback 到 GPU 合成" —— 实测 25 plane）、
  协作里"两个圈子此前没有交集"（已有平行项目）、硬件表 BIOS 一格里"不要升级到 2.17"的旧说法、"待机挂得下去醒不回来"的旧说法、
  参考树里"现役基线 = v7.2-rc2 + 20 个补丁"（2026-10-05 起是 v7.2.9 stable + 19 个提交 + `patches/`）、环境里"内核树在 `~/gk3-kernel`"（现役是 `~/gk3-kernel-72y`）、
  阶段表 Stage 7 一格"下一步 M4 真装一台"（M4b 2026-09-26 已真装）、阶段表编号脱节的说明、Stage 0 采集清单、Stage 2 固件逐个文件名清单、
  "游戏多走 GLES，freedreno GL 与 zink-over-turnip 都试"、ramoops 实测的两个地址（`0xae900000` / `0x865d38000`）、pstore 落盘路径。

**当前阶段：Stage 6 收尾 —— 产品化。v0.6.2-alpha 已发布（2026-09-16，构建戳 `1789570683`）：触摸手感按实测定案（跳点判据、fuzz=0、按下 17 ms、触点面积轴）、驱动 6 个缺陷 + 可观测性、OTA postinstall 同步 cmdline。手上还剩画质（降噪/闪光过曝）、息屏 USB adb、Google 认证、手掌碎块 —— 都在 `docs/TODO.md` 的总表里。
2026-09-24 凌晨（用户睡觉、授权自主推进）：**PR #6 相机（AF / 朝向 / 崩溃）已接手修完并合并推送**（`9c79b78`）。
2026-09-24 早上：**后摄朝向 180 经用户目视确认**（0049 前身写的 270 是错的，已改写，#119 §7）⇒ 测试版 `1790184271` 作废；
重新构建的 **v0.6.3 候选版 `1790206017` 已装到 `_b` 并远程验收**（#122；待机默认改回 1＝S1，开发机持久 0 实测不睡）。
新查出：NTP 在国内从没成功过（B22，已写未编）、SLPI 自愈后传感器全丢（B21）、光感激活会让 SLPI 崩溃（#121）。
★★ **指纹翻案并上机成功**（#120→#123→#125）：用户提供 Windows 侧逆向报告 + 我实测本机 QSEECOM 通路已在跑 ⇒ 移植 QSEECOM LOAD（patch 0050），
2026-09-24 **在本机把华为签名的指纹 TA `fingerprint` 加载进 QSEE 成功（app_id=5，无挂机）**，整条路打通；剩 client driver + Android HAL（T6）。
⚠️ 设备当前跑的是 RAM 里的"fp 内核"（stock+0050 休眠，靠 oneshot 单独 Image.fp 启动、ESP 已复原）——**下次重启自动回 stock 内核 `1790206017`**。
路由器（OpenWrt，`192.168.10.1`）加了静态租约：`00:03:7f:12:*:*` → `.239`（本机 MAC 只有后两字节每次开机变），设备侧的静态 IP 仍保留。
新工具 `gaokun3-ncam-smoke`（应用视角的相机冒烟测试，设备上 `/data/local/tmp/`）。案卷 #119。
2026-09-18：SELinux 第五轮（案卷 #117）—— 补了 7 处规则（触摸服务的域 / `/dev/dri` 目录 / ESP 块设备类型 / OTA postinstall / 温控 HAL 的 sysfs_thermal / audioroute 的 tinymix / hwc 的 uevent socket），自研属性改名 `persist.vendor.gaokun3.*` 且 **allow_suspend 默认值改成 0**（⚠️ 不只新装机：v0.6.2 老用户多半没设过这个属性，OTA 后也会落到 0、失去待机 —— 发版前要用户定，见 `docs/TODO.md` S1）。构建机编译验证通过；`-userdebug` 重编后**装机成功**（戳 `1789737346`，槽 `_a`，60 秒起来）：温控 HAL 的 35 条 denial 归零且 `dumpsys thermalservice` 报真温度、audioroute 的 7 条归零、`/dev/dri` 与 ESP 的标签实机确认、功能零回归。⚠️ 中途误用 `-user` 变体导致一次装机失败（#117 §15）。⬜ 新查出的 4 处规则（hwc 的 create/bind、同进程 HAL 库、mediaswcodec、wakeup genfscon）**已写未验**，下次构建一起。
2026-09-24/25：**Stage 7 图形安装器重启，改 Flutter + Debian**（`docs/stage7-flutter-debian.md`）：M1 后端统一（loop 端到端 56/56，抓到双系统模式从来装不上）、M2 Flutter 13 屏（测试 42/42）、M3 Debian live 镜像（U 盘 317 MiB，开机冒烟干净）、M0.5 内核能跑 systemd 均已完成，全部在 Mac 上离线做的，均为本地提交未推送。2026-09-25/26 **M0 从内置盘跑了六轮，六项验收全过**（`scripts/live/m0-internal.sh` + oneshot，§5.7）：画面方向、触摸、键盘、tty2、两种渲染后端各浸泡 10 分钟（RSS 每分钟只长约 0.2 MiB，预想的逐帧泄漏没出现），默认保持 Impeller。同期按用户要求加了：**免 U 盘装双系统**（安装器侧 + Windows PowerShell 脚本，后者在 Parallels 克隆机里整条跑通，§5.8）、**去掉 BIOS 限制**、**界面改 Material Design 3**（字体打包进应用）、**重新安装 Android**（默认清除数据）、**手动调整磁盘页**、版本页退回 OTA 清单。2026-09-26：网快了（中科大镜像 6 MB/s），**`build-live.sh` 正式重建完成**（`out/live/`，包清单与 09-25 那次逐行相同）；补上 WiFi **隐藏网络**（roadmap 欠账的最后一项）；**C 版安装器已删**（`git show 445e978:live/installer/…`）。设备 p3 上已换成正式镜像。★★ **同晚 M4b：图形安装器第一次真的装了一台**（内置盘上的 live → 重新安装 + 保留数据，同版本 `1790206017`，约 3 分钟，数据逐项原样；设备因此从 `_b` 换到 `_a`，§5.9）；顺带补上"介质上没有 `gk3_apply` 日志"的缺口；★ 事后查出**安装把 ESP 写满了却报告成功**（目录名用了 live 每次开机现生成的 machine-id，另开目录又写一整套）—— 设备上已当场修好（不重启，下次启动与这次字节相同），安装器改成与 OTA postinstall 同一条选目录规则 + 按真实写入量核空间 + 每步查结果 + 写完 cmp（test-apply 114/114）。 2026-09-27：一轮独立审查修了 9 处同类问题（★ Windows 快速启动时缩 NTFS 会损坏数据 —— ntfsresize 看不出休眠、我们还加了 --force；救援分区写入不查），加了介质上的变体清单；**网络安装第一次在真机上跑通**（从开发机下载 1.25 GB 190 秒、apply 8.8 秒、数据原样），并抓到下载进度被管道攒块（已修，test-apply 120/120）。ssh 进 live 可以：连上同一个网即可（之前是 live 没连上热点）。设备 ESP 上常驻一个非默认的 `gaokun3-live.conf`（`gaokun3 installer`，装坏时的安全网；2026-09-27 起 M0 变体已撤），p3 上有 `gaokun3/live.squashfs`、`payload/`、`firmware/`，`m0-internal.sh remove` 全撤。✅ B23 用户 2026-09-27 定：GPU 固件随 live 镜像发，与 ROM 同待遇；安装器定为 **`0.1.0-preview`、随下一版发预览**（⏸ 2026-09-28 用户：v0.6.3 暂不发，等 iris 修复，可能直接 0.7.0；候选版 1790206017 不含 PR #10 麦克风修复，见 TODO 顶部）（`scripts/live/release-installer.sh`，§5.11）；Windows 脚本改成**只划安装器自己的空间**、Android 的空间在安装器里缩（D: 加密时脚本才问，§5.12）；Stage 7 的提交 2026-09-27 已推送（推之前清掉了历史里全部 Co-Authored-By 行，强推）。2026-09-27：**SELinux 第六轮**（案卷 #126，工具 `scripts/selinux/`）：完整开机普查 + 运行期 enforcing 试跑（ksu 域是 permissive，adb 锁不住），查出 enforcing 下会整机失效的 20 余处（**内核读不了 `/vendor/firmware`**、vendor build.prop 丢 `persist.adb.tcp.port` 等 6 个属性、vendor rc 属性触发器被丢、HWC / gatekeeper 注册失败、super 重标），规则写完、`m selinux_policy` 通过、**未上机**；bpfrelabel 退役。⚠️ 开发机设了 `persist.logd.audit.rate=1000`（持久，普查不再丢 denial）。⬜ 要用户定：新策略怎么出镜像（`out/` 被 v0.6.3 候选版占着）。2026-09-28：**视频编解码 qcom-venus → qcom-iris**（用户要求，为 v0.7.0；案卷 #128）：上游其实已支持本机（DT 回落串 `"qcom,sc8280xp-iris", "qcom,sm8250-venus"` → v7.2-rc2 自带的 `sm8250_data`），补丁 0053–0061（0058 已否）+ IRIS=y / VENUS=n 已入配方，venus 时代的 upstream-venus 退役；审查查出并修掉上游 b9c2215 在持锁断电时的死锁（本地 0056）。上机（k3–k5）查清收尾卡死的根因：上游在 CAPTURE streamoff 发 HFI_FLUSH_OUTPUT，华为固件读走就不再读命令（A/B 8/11），改成 venus 的 FLUSH_ALL 后 0/15 ⇒ **本地补丁 0065**（0062 已否）；k6–k12 上机：中途停止 / 播到结尾 / seek（0066，学 venus 不停会话）/ 播完后 seek（0067）/ 分辨率变化**全部通过**（k12 16 项回归，#128 §10–§16）。（"候选版一待机就复位"已查清是我插着 USB 直接 echo mem 的误报，#128 §17。）2026-09-28/29 **v0.7.0-alpha 出版**（用户定名；`docs/TODO.md` 的 v0.7.0 一节）：第一个候选版 `1790597477` 验收查出 Histen 效果库被 APEX 链接器命名空间拒载 ⇒ **补丁 0068**（音频 APEX 的 `linker.config.pb` 放行 `/vendor/${LIB}/soundfx`，tree-fixes [16]）；顺带按用户要求把 Parts 两页换成 SettingsLib 的 **M3E 样式**。**重编候选版 `1790605865`**（incremental `20260928143105`）装在 `_a`、验收除第 8 项的"待机后回插 USB-C 口坏"外全过（用户定：写成已知问题照发，A6 跟进），ESP default 已是 `_a`；ESP 上的 `slot_iris/` 与测试条目已删。⚠️ 构建机上的旧树 `~/gk3-kernel` 还打着 upstream-venus，新配方要在新 worktree（现成的是 `~/gk3-kernel-iris`）上打，脚本会拒绝旧树。2026-09-29/30：**SELinux 第七轮**（案卷 #129）：运行期 enforcing 试跑查出相机全挂（已在线验证修法）、审计查出块设备标签按分区号写死 ⇒ 安装器装的盘全错（改按 by-name），共 20 余处，提交 `572a060`/`d885275`/`4d93d4c`（本地）、编译过、**未上机**；⚠️ 一次 ksud 在线补丁后整机重启，原因未明，没人在场时别再做。2026-10-04：**处理 GitHub issues #11–#16**（回复已发）：hostapd 进镜像（#11）、声明 usb.host 让 UsbService 起来（#13，已上机）；#16 待机睡死查成三环 —— 发布内核 DPM 看门狗误留 10 秒、panic 不重启、ath11k 每次恢复赌高阶连续 DMA（**CMA 被 DT 放在 34 GiB 处，32 位设备用不上**）⇒ 配置断言 + `patches/0070`（MHI 保留固件表）+ `patches/0071`（CMA 限定 2–4 GiB），上机 12/12、ftrace 对照 GFP_DMA 高阶分配 69→0（案卷 #131）；#12 的 39 位地址空间测试内核**起不来**（死在 1 秒内，原因未明）。同日用户要求测完出完整镜像：**候选版 `1791053208`**（incremental `20261003184648`，含 0069 / 0070 / 0071 / hostapd / usb.host / SAE overlay，`release.sh --dry-run` 全过，载荷在本机 `out/issues-1791053208/`），⬜ 未装机。ESP 上的测试条目已删。★ **同日 15:35 作为 v0.7.1-alpha 发布**（用户定名；`release.sh --no-build`，GitHub release 5 个附件字节数核对、标 Latest，tag = `21e24fd`，说明 `docs/relnotes/v0.7.1-alpha.md`）。issues #12 / #16 已追加回复。2026-10-05：**1.0 发布构建 dev.1–dev.8**（发布构建 = 关免授权 adb / debuggable，dev.1–dev.7 上过机，dev.8 `1791199356` 编过未装机，记录见 TODO「▶ 1.0」各节）；内核升 7.2.9 后黑屏的根因是 stable 撤掉了双 DSI 绑定 PLL 修复，`patches/0076` 撤回之、dev.7 出图；统一启动入口 E3–E8（含 OTA 自动回滚）+ fastboot 执行端 E6 / BCB 分派 E7 真机过（E10 恢复出厂、双系统未验），dev.8 起镜像默认动作模式 + 分派开（`ca55b5c`）。（每次开工时更新这一行）**

> ## ★★★ 开工前先读（这一段是"现在"，历史在 `docs/project-log.md`）
>
> ### 设备与连线
> **两条连线路径**（2026-09-14 实测都通；现在在不在线见下面 ⚠️）：USB adb（`adb devices -l` 显示 `usb:...`）与
> TCP adb（`persist.adb.tcp.port=5555`，2026-09-14 实测 `192.168.10.239:5555`）。
> ⚠️ **IP 会漂**，本机自己的网段也会漂（一天内换过三次）。找设备前先 `ifconfig` 看自己在哪个网段，
> 再扫**全网段**的 5555 —— ★ 别按"上几次都落在哪"收窄，2026-09-12 就是这么把一台**好好跑着的**机器
> 误判成"内核挂死"并让用户去按电源键的。判据用 `getprop ro.crdroid.device`，
> 局域网里那台小米手机（`pudding`）也开着 5555。
> ★ 现在直接跑 **`bash scripts/find-device.sh`**（绕沙箱）：2026-09-23 起它在 macOS 上能用，扫本机所在的每个 /24 全段、按协议认身份。
> ★ **2026-09-23 起 IP 固定为 `192.168.10.239`**（家里那个 SSID 改成了静态 IP，#118 §1；
>   2026-09-24 路由器侧也做了保留，`00:03:7f:12:*:*` → `.239`，#119 §8 —— 池子 `.100–.249` 本来包含 `.239`）。
>   IP 以前会漂是因为 wlan0 的 MAC 每次开机都变。换了网络（别的 SSID）仍走 DHCP，那时再用 find-device。
>
> ### 四条会让你损失一小时以上的运维坑
> 1. **`| tail` 之后取 `$?` 拿到的是 `tail` 的退出码。** 在 `az`（打印"已下发停机"而机器没停）、
>    `make`（报 `KBUILD_RC=0` 而构建失败）、`gh release upload`（报"uploaded"而什么都没传）上
>    各栽过一次。判据要看**产物**（时间戳 / 大小 / 服务端列表），不看管道尾巴。
>    ★ 大文件传输同理：2026-09-16 `scp` 1.35 GB 的 payload **退出码 0、文件只有 77%**（连接中途断了）。
>    传完必看字节数 + sha256；续传用 `rsync --partial --append-verify`，别重来。
>    ⚠️ 2026-09-24：**本机（macOS）的 `/usr/bin/rsync` 是 openrsync（"2.6.9 compatible"），没有 `--append-verify`**，
>    只打印用法就退出 —— 包在"失败就重试"的循环里会变成死循环。本机上续传用
>    `ssh 构建机 "tail -c +$((已有字节+1)) 文件" >> 文件` 按偏移追加，最后比 sha256；循环要设重试上限。
> 2. **沙箱代理会掐断到构建机的 ssh，并 MITM 掉 `az` 的证书。**
>    长任务与大流量的 ssh 一律绕沙箱；`az` 加 `AZURE_CLI_DISABLE_CONNECTION_VERIFICATION=1`。
>    主机名 `cicd` 会被解析成 fake-IP `198.18.0.92`，**用真实 IP 直连**。
>    ⚠️★ 两者的要求是**相反**的（2026-09-14 实测）：**ssh 要绕沙箱，`az` 要留在沙箱内**。
>    绕沙箱跑 `az` 会直接 `Certificate verification failed` —— 而那次是 `vm deallocate`，
>    失败了机器就一直在计费。★ 停机之后**必须回头查一次真实电源状态**，别信命令输出。
> 3. **一行命令里永远不要 `pkill -f <名字>` / `pgrep -f <名字>`** —— 命令行自己就含那个名字，
>    于是把自己的 shell 杀了，后面什么都没跑。2026-09-14 一天栽了三次。
>    用 `pkill -x`、`kill $(pidof ...)`，或让长跑脚本自己写 pid 文件。
> 4. **内核树上的补丁只活在【工作区】里（从未提交）** —— 所以 `git checkout -- <路径>` /
>    `git restore` / `git stash` 在那棵树上是**破坏性**的，且毫无警告。
>    2026-09-16 我用它撤一个补丁，一次清空了 `0037`–`0045` 九个。
>    撤补丁用 `git apply -R`；撤不掉说明正文变了，那时重放整条链
>    （`scripts/kernel-apply-patches.sh <树>`，幂等）—— 那正是这个脚本存在的理由。
>
> ### 四条操作禁忌（每一条都是用一次事故换的）
> 1. ⚠️★★★ **camss 已经 `runtime_error` 时不要 unbind 它** —— 会拖死整机，只能长按电源键。
>    ★ 更值钱的半条：我当时是"顺便"测 `patches/0022` 才这么干的，而 0022 修的是 unbind 路径，
>    **本来就该在健康状态下测**。**一个实验该在什么状态下做，是实验设计的一部分。**
> 2. ⚠️★★★ **对可能被门控的寄存器块，`/dev/mem` 的【读】和写一样危险。**
>    2026-09-12 读 camcc（当时 runtime-suspend）触发总线 external abort、内核静默死亡，
>    机器停到用户醒来。非读不可时先钉住控制器并确认 `runtime_status=active`；
>    ★ **并且要看"此刻有没有人能按电源键"——没人能按时，这类探针一概不做。**
> 3. ⚠️ **重启 / 装内核前要征得用户同意**：失败要有人按电源键，oneshot 兜底≠不用动手。
> 4. ⚠️ **装机脚本用的挂载点不要叫 `/mnt/esp`** —— 2026-09-14 我在另一个 shell 里"顺手看一眼"
>    就把它 umount 了，于是安全网那一步静默失败。共享的可变状态要么私有、要么加锁。
>
> ### 发布与构建
> * 构建机**按负载选机型开**（`bash scripts/cicd.sh start <档位>`，2026-09-27 起），用完 **`bash scripts/cicd.sh stop`**（按分钟计费，见 `docs/build-machine.md`）。构建机的树**不等于**本仓 checkout ——
>   这个坑咬过五次，编内核前先 `bash scripts/kernel-apply-patches.sh <树> --verify`（绿了再 make），
>   同步设备树**只用 `bash scripts/sync-device-tree.sh <IP>`**，别手写 rsync：2026-09-16 一条
>   `rsync --delete` 把构建机上四样**不入库但构建必需**的输入（adb_keys / firmware / hexagonrpcd-root /
>   prebuilt-boot）全删了，而事后的 md5 核对还通过了 —— 两边一样地缺。脚本会断言它们在。
> * 发版一律 **`release.sh --no-build`** —— build stamp 每次构建都变，重跑构建发出去的
>   **不是**你在硬件上验过的那一版。
> * **不推仓库、不发版**是需要用户点头的两件事；其余（本地提交、构建、staging、设备实验）直接做。
>
> ### 现在设备上跑的是什么
> ★★★★★★ **2026-10-06 16:0x 起：槽 `_b` = `1.0.0-dev.9`**（戳 `1791215652`，内核 `912f22c3` = 7.2.9 + 0076、去掉 0050），**active = `_b`、经统一启动入口 `0.2.0-e5.g3d0cb032e81a` 开机**（动作模式 + 分派开，条目由 HAL 写、已祝福），VAB 已合并 ⇒ `_a`（dev.7）不可回落；载荷 `out/v1dev9-1791215652/`。回归见 TODO（显示 / iris 亮屏 15/15 / 相机 / s2idle 11/11 / A 档全过）。⚠️ 分派开着：设置里的恢复出厂会真擦（发布构建会丢 adb_keys）；`adb reboot bootloader` 进 fastboot 执行端。⚠️ 本机 Mac 现在也在热点「Xiaomi 17」上（`10.146.153.63`），设备 `10.146.153.115`。2026-10-06 已清理 ESP（测试条目与 Windows 残留，备份 `out/esp-backup-20261006/`），菜单只剩 Android（入口 / 上一版入口 / fastboot / 两个直连槽）、`gaokun3 installer`、Ubuntu 救援。用户定：充电上限保持现状不实测、恢复出厂 E10 不测。
> ★★★★★ **2026-10-05 17:2x：同一版 dev.7 在 `_a`，但 `persist.vendor.gaokun3.gk3boot=action`、ESP 默认走 `gk3boot-android-a.conf`（options 手加了 `gk3.dispatch=1` ⇒ BCB 分派开、GK3 已迁移）**，`adb reboot bootloader|fastboot|recovery` 会进 fastboot 执行端（Mac 上 `fastboot reboot` 回来）；⚠️ 分派开着时**设置里的恢复出厂会真擦**（发布构建擦掉 adb_keys 就连不上 adb）。退回：设 `off` 重启一次。
> ★★★★★ **2026-10-05 16:0x 起：槽 `_a` = `1.0.0-dev.7`**（戳 `1791185227`，7.2.9 + `patches/0076` + S7 执行端 + S9，载荷 `out/v1dev7-1791185227/`），ESP default = `*-android-a.conf`（直连条目）、`_a` 已 marked successful、`persist.vendor.gaokun3.gk3boot=off`。显示 / 相机 / s2idle（11/11，PACE=75）过；iris DRC drain 偶发固件错误待亮屏对照（TODO）。⚠️ ESP 上还留着非默认测试条目 `gaokun3-k15-test.conf` + `slot_k15/`（7.2.9 测试内核），可删。⚠️ 用户要求屏幕散热期间尽量不点亮，开发机 `screen_off_timeout` 设成了 15 秒。下面 13:2x 那条"手工拼的"状态已过时。
> ★★★★ **2026-10-05 13:2x 起（用户已关机散热）：开发机是"手工拼的"状态，别当成任何一个发布版。**`persist.vendor.gaokun3.gk3boot=off`（gk3boot 条目已撤）；默认走**直连条目 `_b`**，而 ESP 上 `slot_b/{Image,gaokun3.dtb}` 被手工换成了 **#16 内核**（`505c1be7…` / `877878de…`），boot_a / boot_b **分区里**仍是 dev.5 的 **7.2.9** 内核（7.2.9 下面板不出图）。非默认测试条目 `gaokun3-k15-test.conf`（目录 `slot_k15/`）当前装的是"7.2.9 + rc2 DSI 文件"测试内核。⇒ 下次开机先装 **`1.0.0-dev.6`**（`out/v1dev6-1791173750/`，#16 内核，戳 `1791173750`）回到正常状态，再按需删 `slot_k15/` 与测试条目。屏幕：#16 下有画面但有固定的细竖彩线、偶发 `vblank timeout`（早上同内核为 0）——疑排线受热或面板状态，用户在散热；新屏在路上。判据（不用人看屏）：亮屏时触摸中断 `himax-spi-ts` ≈120/s = 面板在扫描，0 = 停摆（脚本 scratchpad `kt-test.sh`）。
> ★★★ **2026-10-05 09:5x 起：槽 `_b` = `1.0.0-dev.5`（戳 `1791163499`，内核 7.2.9 + 统一启动入口 S9），`persist.vendor.gaokun3.gk3boot=action` ⇒ 每次开机经 gk3boot 动作模式**（退回：设 `off` 重启一次）；`_a` = dev.4（7.2.9）。装 OTA 用 `GK3_TRUST_GK3BOOT=1 install-ota-local.sh --go`。验收见 TODO。
> （06:32–09:5x）槽 `_b` = `1.0.0-dev.3`（戳 `1791151679`，批 2 新内核 #16 = 0072–0074 + 批 2 配置）**，验收见 TODO。`_a` = dev.2。
> （04:43–06:32）槽 `_a` = `1.0.0-dev.2`（戳 `1791144960`，oneshot 启动、验收见 TODO）**；`_b` = `1.0.0-dev.1`（`1791138567`）。两版都是**发布构建**。⬜ 开机成功后 default 应已同步成 `_a`，下次开工先 `boot-oneshot.sh --list` 核对。
> （03:10–04:43）槽 `_b` = 第一个 1.0 发布构建 `1.0.0-dev.1`（戳 `1791138567`），已 marked successful、ESP default = `_b`。**这是发布构建**：`ro.debuggable=0`、adb 要授权 —— 开发机靠预置的 `/data/misc/adb/adb_keys`（Mac + Windows）和持久化的 `persist.sys.usb.config=adb` / `persist.adb.tcp.port=5555` 照常 USB / TCP adb，`adb shell` 经 KSU 仍是 root（没有 `adb root` 也不需要）。验收记录见 TODO「1.0 批 0/1」一节。⚠️ USB adb 只走靠近电源键的口（port0）。
> ★★★ **2026-10-04 14:00 起：槽 `_a` = 候选版 `1791053208`**（incremental `20261003184648`，`install-ota-local.sh` 装、oneshot 开机成功后 boot_control 把 **ESP default 同步成了 `_a`**、`_a` 已 marked successful）。`_b` **已不能回落**：ESP 上 `_b` 的条目与内核还是 SELinux 第七轮测试版 `1790702971`，但 VAB 合并后 super 里只剩 `*_a`（2026-10-04 实机 `lpdump` 无任何 `*_b`、`bootctl is-slot-bootable 1` = 0，#118 §2 早有记录）—— 选它只会白白失败一次、init reboot 回 `_a`；回退只在新版首次开机、合并开始之前有效，之后只能用安装器重装（1.0 计划 OTA-3）；v0.7.0-alpha `1790605865` 已被覆盖，载荷仍在 `out/v070-1790605865/`。
> 装机验收：热点开得起来（#11）、`usb` 服务在（#13）、挂起/恢复 10/10 且 GFP_DMA 高阶分配 0（#16）、SAE 升级已关、avc 0；App 冒烟 8/8、出声音游（#130 §7）也过了；⬜ 只剩热点让真设备连上网。
> 以下是装机前的记录：
> ★★ **2026-10-04 issues 测试（#131）**：这次开机是 oneshot 到当时的 `gaokun3-issues-k3.conf`（k2 内核 #15 = 48 位 + 0070 + 看门狗 120 / panic 10，配 0071 的 dtb）——**下次重启回 `_b` 的原版 #13**，ESP default 没动。
> 测试条目与 `slot_test/`、`slot_k2/` 已按用户要求删掉（ESP 回到约 46 MB 空余）。`prebuilt-boot/` 已换成 k2 内核 + k3 dtb，旧版备份在 `out/prebuilt-boot-backup-v070/`。
> 测试内核在本机 `out/issues-k1/`、`out/issues-k2/`。重启后机器停在锁屏（有密码），App 类测试要用户先解锁。
> ★★ **2026-09-30 起：槽 `_b` = SELinux 第七轮测试版 `1790702971`**（#129；`install-ota-local.sh` 装、开机成功后 bootctl 把 **ESP default 同步成了 `_b`**，permissive）。
> 这次开机是 oneshot 到 `gaokun3-enforcing-test.conf`（同一版、只改 `selinux=enforcing`）—— **下次重启回到 `_b` 的 permissive**。
> `_a` = v0.7.0-alpha 正式版 `1790605865`（回落）。载荷在本机 `out/sel7-1790702971/`。⚠️ 在热点下 IP 这次是 `.127`（09-30 早上），会漂。
> 下面这段是 09-29 的状态，`_b` 那一行已过时。
> ★★ **2026-09-29 起跑在槽 `_a`：v0.7.0-alpha**（戳 `1790605865`，incremental `20260928143105`）——
> `install-ota-local.sh` 装进 `_a`、oneshot 验收（`out/v070b-accept/`，TODO 的 v0.7.0 一节）后 ESP default 改成 `*-android-a.conf`，`_a` 已 marked successful。
> ★ **现在有回落槽了**：`_b` = 第一个 v0.7.0 候选版 `1790597477`（2026-09-28 装、开过机、只差 Histen 与 M3E 开关）。
> 安装载荷另存在本机 `out/v070-1790605865/`（`_b` 那版在 `out/v070-1790597477/`）。
> ⚠️ **USB-C 口（port0）这次开机里坏着**：待机后回插 `-524` / `-110`（A6），USB adb 不通、TCP adb 正常 —— 重启即恢复。
> 另一张安全网：开机菜单里的 `gaokun3 installer`（`gaokun3-live.conf`，内置盘上的 live，能重新安装），另有救援 Ubuntu。
> 2026-09-27 按用户要求清理了启动项：菜单只剩 Android `_a` / `_b`、`gaokun3 installer`、Ubuntu 救援（M0 的 4 个变体与 Alpine 救援撤了，原件备份在本机 `out/esp-entries-backup-20260927/`）。
> `slot_a/`、`slot_b/` 下的 Image / dtb / ramdisk 都是安装器从 `boot.img` 拆出来的（`slot_a` 那份手放的 0048 dtb 已被覆盖；`.pre0048` 备份若还在只剩历史意义）。
> ⚠️ 在热点（`10.187.160.x`）下设备 IP 会变（09-26 一晚上 `.49` → `.34`），用 `find-device.sh`。
> 此前 bind-mount 的相机 HAL 随重启消失，现在跑的是镜像里的那份（同一份代码）。
> ✅ NTP：开发机上手动设的 `ntp_server` 已删（2026-09-28），现在走镜像里的服务器列表，实测 `ntp.aliyun.com` 对时成功。
> ⚠️ 本机待机仍然关着：属性已改名为 **`persist.vendor.gaokun3.allow_suspend`**（现值 0，
> 2026-09-24 起**显式持久化**；候选版起镜像默认 1 —— 开发机靠持久值保持 0，装上候选版后实测仍不睡，#122 §2）。旧名 `persist.gaokun3.allow_suspend` 在设备上还留着一个孤儿值，无害。
> ⚠️★★ **不要用 `-user` 变体构建本机的 ROM** —— user 构建的 init 强制 enforcing
> （忽略 `androidboot.selinux=permissive`，`selinux.cpp:112-116`），而我们的策略还不完整，
> 结果是**装上去起不来**。本机一直用 `lineage_gaokun3-bp4a-userdebug`。案卷 #117 §15。
> **v0.7.0-alpha 已全部发布**（2026-09-29 01:06，戳 `1790605865`）：`release.sh --no-build` 传 R2、清单最后传（设备侧抓取 200、timestamp = 戳）；
> GitHub release 9 个附件（ROM 5 个从构建机经 R2 取回后传、安装器 `0.1.0-preview` 4 个从本机传）服务端字节数逐一核对并标 Latest，tag `v0.7.0-alpha` = `a39dcdf`；
> 发版说明发布前经 workflow 逐条核对（改了 20 余处）。说明见 `docs/relnotes/v0.7.0-alpha.md`。上一版 v0.6.2-alpha（2026-09-16）。

---

### 文档地图（找东西从这里开始）

| 想知道什么 | 去哪 |
|---|---|
| **现在什么状态、有什么禁忌** | 本文件上面那个框 |
| **还剩什么没做、优先级** | `docs/TODO.md` —— 顶上有一张「现在在做 / 待办」总表 |
| **发 1.0.0 之前要修什么、按什么顺序** | `docs/v1.0-plan.md`（2026-10-04 全项目调研 + 逐条复核：发版标准、7 个阻断项、分批路线、23 项待用户拍板的决定；用户已定的写在第 6 节开头） |
| **统一启动入口 / fastboot（1.0 承接恢复出厂，设计稿）** | `docs/boot-entry-design.md`（方案 Y：`gk3boot.efi` 读 misc 选槽 / BCB 分派 / 从 boot_x 直接启动，fastboot 执行端 = 同内核 + initramfs 里的 `gk3-fastbootd`；实验 E0–E11、待定 U1–U11）；旧稿 `docs/fastboot-design.md` 只剩执行端细节有效 |
| **触摸调参的实机记录与工具** | `docs/stage4-findings.md` #114–#116 + `scripts/touch/README.md`（手册 `docs/archive/touch-morning-runbook.md` 已完成使命，留作 2026-09-16 那一晚的操作记录）|
| **某个结论是怎么来的**（最权威） | `docs/stage4-findings.md`，按 `#NN` 编号的案卷；Stage 5/6/7 另有专档 |
| 那一周发生了什么 | `docs/project-log.md`（本文件的历史，原样搬过去的） |
| 怎么装、用户会踩什么 | `docs/INSTALL.md` |
| 硬件原始数据 | `docs/hw-inventory.md`、`docs/hw/`（转储） |
| 发版说明 | `docs/relnotes/` |
| 要投上游的补丁 | `docs/upstream/`（未发，等用户点头） |
| **构建机怎么开、开多大**（按负载选机型、停机规矩、盘的代价） | `docs/build-machine.md` + `scripts/cicd.sh` |
| **图形安装器（Flutter + Debian，进行中）** | `docs/stage7-flutter-debian.md`（决定、里程碑、M1 实测）；后端 `scripts/live/installer-lib.sh` + `scripts/live/README.md` 的测试一节 |
| **指纹（TA 已在本机加载成功，进行中）** | `docs/fingerprint-driver-design.md`（架构+决策+复现+里程碑）；案卷 #120/#123/#124/#125；工具 `tools/fingerprint-bringup/` |

⚠️ **冲突时的优先级**：实机实测 > `stage4-findings.md` 的案卷 > 本文件 > `project-log.md`。
本仓的历史里有**大量被后来实测推翻的结论**，推翻过程是故意留着的 —— 但别把它们当现状。

### ⚠️ 给 AI 助手的强制规则

**这个平台没有任何 Android 移植的前人成果。** 训练数据里不存在这台机器上的
Android 相关知识。因此：

1. **任何具体的 kernel config 名、AOSP property 名、HAL 接口名、文件路径，
   必须从本地 checkout 的源码树里 grep 出来，并给出文件路径和行号。**
   不允许凭记忆给出。记忆里的名字在这个平台上大概率是错的或过时的。

2. **不确定就明说"我不确定，需要验证"**，不要给出听起来笃定的猜测。
   在这个项目里，一个自信的错误答案比"我不知道"贵得多——用户要花几小时
   才能发现你编的那个 config 项根本不存在。

3. **实机行为以 dmesg / logcat / 用户的实际观察为准，与你的判断冲突时以实机为准。**

4. 涉及 sc8280xp 硬件细节时，优先查 `refs/linux-gaokun` 和 `refs/jhovold-linux`；
   涉及 AOSP-on-mainline 的组织方式时，优先查 `refs/aospm-*`。

---

### 硬件事实

| 项目 | 值 |
|---|---|
| SoC | Qualcomm Snapdragon 8cx Gen 3 / **SC8280XP** |
| 设备代号 | gaokun3（8cx Gen 3 机型）；gaokun2 是另一套 EC 协议 |
| 型号 | **HUAWEI GK-W7X，SKU C233，2022 款，CSOT 面板，触摸固件 `41 07`** |
| BIOS | 本机 **2.16**（2023-01-31）。★ **2026-09-25 用户：已有人验证过，不依赖 BIOS 版本** ⇒ 安装器去掉了 BIOS 限制（预检只报版本号）。此前"不要升级到 2.17"的说法作废；它的老理由"两版触摸 SPI 总线和 GPIO 编号完全不同"本来就比错了表（那份 `DSDT_217` 是 8cx Gen 2 的，#120 §4） |
| GPU | **Adreno 690** —— mesa freedreno + turnip，主线支持成熟 |
| 屏幕 | **Himax HX83121A / ppc357db11 WQXGA**，MIPI-DSI。**与三星 Galaxy Tab S7 FE 同款面板** |
| WiFi/BT | WCN6855 —— ath11k + hci_qca，主线驱动 |
| EC | 华为自研，主线驱动 6.15 进；UCSI 6.16；**DSI 面板 7.1 进** |
| 存储 | NVMe（**不是 UFS**，不是手机那套分区布局） |
| 引导 | **UEFI，不是 fastboot**。可关 Secure Boot。GRUB/systemd-boot 加载 |
| 虚拟化 | KVM/EL2 可用 |
| 进行中 | 指纹（FocalTech FTE7001）：**2026-09-24 已在本机把签名 TA `fingerprint` 加载进 QSEE 成功（app_id=5，无挂机，#125）**，patch 0050 = QSEECOM LOAD/SHUTDOWN/listener + 32 位 TZ 内存约束。⬜ 剩 client driver（发 FF_CMD_TA_* 命令、enroll/auth 的安全存储 listener）+ Android HAL（见 TODO T6）。TPM 不支持；深度休眠 (S4) 未测 |
| 待机 (s2idle) | ✅ **已修复**（M16，v0.3.0-alpha）。⚠️ 本表此前写着"挂得下去、醒不回来、内核/EC 缺陷"，**两句都错**：M15 证明复位发生在**挂起进入**而非唤醒，M16 查出真凶是我们 Stage 2 自己加的 `dr_mode="otg"`，与内核和 EC 都无关 |

### 关键约束（每次都要记住）

- **没有高通 Android BSP。** 8cx 系列从来只发 Windows/Linux 驱动。
  不存在可扒的 vendor blob，所有 HAL 必须基于主线内核自建。
  → 路线只能是 **AOSP on mainline**，参考 aospm 项目。

- **不是 fastboot 设备。** 所有假设 `fastboot flash` / `by-name` 软链接 /
  A/B 槽位的 AOSP 常规流程都要改写。
  ⚠️ **fstab 用 PARTUUID，不能用 PARTLABEL** —— 实测内置盘上 Windows 建的分区
  PARTLABEL 全都是 `Basic data partition`，不唯一。见 `docs/hw-inventory.md` 第 8 节。

- **没有暴露的串口。** 早期启动失败 = 纯黑屏零信息，adb 要等 init 起来才有。
  → ✅ **已解决，走 `efi_pstore`（EFI 变量），不是 ramoops。**
  **ramoops 在这台机器上不可能工作** —— 固件每次复位都重新初始化 DRAM，
  低位 `0xae900000` 和高位 `0x865d38000` 都实测过，内容一律不存活。
  崩溃日志现在会自动落到 `/var/lib/systemd/pstore/`。
  详见 `docs/hw-inventory.md` 第 7bis 节，工具 `scripts/pstore-ctl.sh`。

- **无 modem。** aospm 的 libqril/qrild/qrtr 那一套全部跳过。

- **arm64 原生。** 手游 arm64-v8a 包直接跑，不需要任何转译层。

### 环境

- **编译机：Azure VM `CICD`**（资源组 `AIROUTER_GROUP`，`centralindia`，Dasv5 家族，
  504 GB 盘，静态公网 IP）。**按分钟计费，用完停机**（盘保留，开机约 1 分钟）。
  crDroid 源码树在 `~/crdroid`，内核树在 `~/gk3-kernel`。
  ★★ **2026-09-27 起（用户要求）：开机时先判断负载、按负载选机型** —— 一律用
  `bash scripts/cicd.sh start <light|kernel|module|rom|clean>`（换机型 + 开机 + 回读核对，**在沙箱内跑**），
  用完 `bash scripts/cicd.sh stop`。简表：不跑 `m` 的活 → `light`（D4）；内核 / 单编模块 → `kernel`/`module`（D16）；
  整包增量 → `rom`（D32）；冷构建 → `clean`（D64，配额上限）。**任何 `m` 都不低于 D16（64 GB）**。
  机器已在运行时不换机型（可能是别的会话在用）；停机前先 ssh 看有没有别人的构建。细则与依据：`docs/build-machine.md`。
  ⚠️ **系统盘 2026-09-27 起是 `StandardSSD_LRS`**（用户要求；原 Premium P20）：吞吐 150→100 MB/s、IOPS 低不少，
  冷启动的 Soong 分析受影响最大 ⇒ 构建时间别拿老记录估，新盘上的实测记到 `docs/build-machine.md` §4。
  ⚠️ **沙箱代理会掐断到它的 ssh** —— 长任务与大流量一律绕沙箱。
  ⚠️ 直连回家只有约 1.3 MB/s，大文件走 **R2 中转**（41 MB/s，出站免费）。
  ⚠️ NSG 的 22 端口目前对 `*` 开放 —— M6 说过要锁到自己出口 IP，**没落地**。
- **目标机：只有一台**（`gaokun3`）。⚠️ 本节此前写着"目标机 A 保留 Windows
  作参照 / 目标机 B 随便刷"，**那个设定 2026-08-20 就不成立了**：Windows
  已抹除、整机归 Android（M6）。**没有对照机** —— "正常应该是什么样"只能
  靠救援 Linux、案卷和实测，不能靠比对另一台。
- ⚠️ 本机自己**编不了 AOSP**（要 16 GB+ 内存、250–400 GB 盘）。

---

### 本地参考树（clone 到 `refs/` 下，供 AI 直接读源码）

> ⚠️★ **`refs/` 不在版本库里，新 checkout 上它并不存在** —— 而上面第 1、4 条
> 强制规则要求"从本地源码树 grep 出来、给出文件路径和行号"。
> 开工前先跑 **`bash scripts/clone-refs.sh`**，否则那两条规则无法执行，
> 只能退回"凭记忆给名字"，而那正是规则要禁的事。

```
refs/linux-gaokun/           github.com/right-0903/linux-gaokun          本机内核核心
refs/matebook-e-go-linux/    github.com/whitelewi1-ctrl/matebook-e-go-linux   GK-W7X patch + GRUB 配置
refs/boot-works/             github.com/matalama80td3l/matebook-e-go-boot-works  面板驱动
refs/jhovold-linux/          github.com/jhovold/linux (wip/sc8280xp-6.16)  ⚠️ 已停更，仅作历史对照
refs/gaokun-buildbot/        github.com/KawaiiHachimi/linux-gaokun-buildbot  ⭐ 现役内核基线
refs/egotouchrev-linux/      github.com/chiyuki0325/EGoTouchRev-Linux    触摸 SPI 驱动
refs/aospm-device-sdm845/    github.com/aospm/android_device_generic_sdm845    ⭐ 设备树模板
refs/aospm-manifests/        github.com/aospm/android_local_manifests
refs/aospm-system-core/      github.com/aospm/platform_system_core       看 diff 知道要改什么
refs/aospm-tinyhal/          github.com/aospm/tinyhal                    音频 HAL
refs/lineage-sepolicy/       LineageOS/android_system_sepolicy (lineage-23.0)  ⭐ SELinux 唯一权威
```

> ⚠️ **jhovold 树已不是基线。** 它停在 6.16（2025-09 最后推送），
> 缺 HX83121A 面板驱动（7.1 才进主线）。现役基线是
> **mainline v7.2-rc2 + `refs/gaokun-buildbot/patches/` 20 个补丁**。

固件来源：`matebook-e-go/uup-drivers-sc8280xp`（Windows 驱动扒 blob）+
linux-firmware ≥ **20241210**

**Stage 2 打 vendor 分区要抓的固件（实测 dmesg 加载路径）：**

```
qcom/a660_sqe.fw          qcom/a660_gmu.bin              GPU (Adreno 690)
qca/wcnhpbtfw21.tlv       qca/wcnhpnv21g.bin             蓝牙 WCN6855
qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn              ADSP
qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn              CDSP
qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn              SLPI
```

华为专有路径下那三个 `.mbn` 不在 linux-firmware 里，必须自己带。

---

### 阶段计划与验收

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| **0** ✅ | 主线 Linux 跑通 + 配好崩溃日志 + 采集素材 | 全部通过（pstore 走 efi_pstore） |
| **1** ✅ | 内核转 Android 配置 | 全部通过（UDC 出现，主机端 `configured` 枚举）|
| **2** ✅ | 引导链 + AOSP 启动 | **全部通过**（2026-08-17）：adb shell 通，keystore2/zygote/adbd 稳定运行。12 个问题的完整记录见 `docs/stage2-findings.md` |
| **3** ✅ | 图形栈（minigbm + drm_hwcomposer + swangle） | **全部通过**（2026-08-17）：桌面完整渲染。freedreno 留待 Phase B |
| **4** ✅ | 输入 / 音频 / WiFi / 电源 | **全部通过**：触摸（gpio174）、WiFi 免干预自动连、扬声器+耳机+双麦、蓝牙、待机。案卷 `docs/stage4-findings.md` |
| **5** ✅ | GPU：freedreno + turnip 硬件 Vulkan | **全部通过**（2026-08-19）：SMMU fault 0，22 分钟浸泡零错误。案卷 `docs/stage5-freedreno.md` |
| **6** | 转 crDroid 16.0 + 产品化 | 主体完成：OTA / root / SELinux 四步 / 传感器 / 硬解。案卷 `docs/stage6-crdroid.md` |
| **7** | LiveCD 图形安装器 + 轻量救援系统 | ▶ **2026-09-24 重启，改 Flutter + Debian**：M0–M3 完成（M0 六项真机验收 2026-09-26），C 版已删；下一步 M4 真装一台。`docs/stage7-flutter-debian.md`（前情 `docs/stage7-live-installer.md`） |

> ⚠️★ **本表编号一度与实际里程碑脱节**：原表写"5 = 游戏适配"，而实际
> Stage 5 做的是 GPU、Stage 6/7 表里压根没有。**游戏适配已经达成**
> （原神极高画质流畅，见 README 状态表），它不是一个独立阶段，
> 而是 Stage 5 GPU 打通后的结果。

**Stage 0 必须采集并记录在 `docs/hw-inventory.md` 的东西：**
- `.config` 中所有 QCOM / ath11k / hid 相关项
- `dmesg | grep -i firmware` 的完整固件加载路径
- **ALSA UCM2 配置文件**（Stage 4 要翻译成 mixer_paths.xml）
- `modetest` 完整输出：connector 名、plane 数量、**支持的 format 和 modifier**
  （Stage 3 配 minigbm 的关键依据）
- 触摸屏 / 键盘 / 触控板的 evdev 名和 evtest 输出
- 触摸屏走 SPI 还是 I2C

---

### 已知坑

- 触摸屏 I2C 模式有间歇性失灵，SPI 模式更稳
- **触摸 IC 的工作模式由 gpio174 在固件重载瞬间的电平决定**（低=SPI 高=I2C HID），
  上游两棵 DTS 都没配这个脚，全靠 UEFI 遗留电平碰运气；显示复位还会静默触发
  固件重载。症状是"触摸随机死亡/幽灵触点风暴/驱动探测成功但全聋"。
  修复=pinctrl 恒拉低，见 `patches/0002-*.patch` + `docs/stage4-findings.md` #26。
  空闲 IRQ 速率是状态指纹：≈显示扫描率=正常；0=IC 停摆；乱=模式错乱。
- **`timeout N getevent > 文件` 会因块缓冲丢光全部输出**——采集 evdev 要用
  `cat /dev/input/eventX` 录二进制再离线解码。见 `docs/stage4-findings.md` #26 方法论。
- ~~EC 挂起/恢复：Android 的 suspend 模型比 Linux 激进，预期这里会先炸~~
  ⚠️★ **这条预言错了，M4 实测推翻**：把 EC 驱动整个解绑，挂起照样失败 ⇒
  与 EC 无关；真凶是我们自己加的 `dr_mode="otg"`（M16）。留着它是因为
  它从 Stage 3 起误导了好几轮排查 —— **写进"已知坑"的预测，要和实测结论
  一样接受复查。**
- ~~DSI panel 的 KMS plane 数量少时 drm_hwcomposer 会 fallback 到 GPU 合成~~
  ✅ **担心不成立。** 实测 **25 个 plane / 6 个 CRTC**，硬件合成资源充裕。
  modifier 只有 `LINEAR` 和 `QCOM_COMPRESSED`(UBWC, `0x500000000000001`)，
  支持 UBWC 的 format 见 `docs/hw-inventory.md` 第 3 节 —— 那就是 minigbm 的配置依据。

- **UCSI 有缺陷**（`refs/linux-gaokun/README.MD:86-87`），常见
  `error -ETIMEDOUT: PPM init failed`，此时 `/sys/class/typec/` 为空。
  ✅ 但**不影响 adb**：`dr_mode = "otg"` 无 role 源时落到 device 侧，
  USB 数据通路不经 UCSI。代价是只有 high-speed，SuperSpeed 需要 UCSI 切 orientation。

- **DT label 编号与物理地址不对应**：`usb_0` 是 `a6f8800`，`usb_1` 才是 `a8f8800`。
  改 dwc3 前务必 `readlink -f /sys/block/sda` 确认启动介质在哪个控制器上。

- **`super.img` 是 Android sparse 格式，不能 `dd`** —— 必须 `simg2img` 展开。
  头部魔数 `0xED26FF3A` 是 sparse 标志，**不是** LP metadata。误 dd 会让 init
  读不到元数据、挂载失败后主动复位，且不留任何日志。见 `docs/stage2-findings.md` 第 1 节。

- **`CONFIG_SECURITY_SELINUX=y` 不等于 SELinux 已启用** —— 还必须出现在
  `CONFIG_LSM` 字符串里。buildbot 默认值只有 apparmor，导致 selinuxfs 从不注册、
  Android init 静默死亡。**只看 config 会误判，必须查 `/sys/kernel/security/lsm`。**

- **Android init 失败时是主动 `reboot()`，不是 panic** —— 所以 pstore 抓不到。
  抓日志要靠改造 ramdisk 写内置盘 ESP，方法见 `docs/stage2-findings.md` 第 5 节。
  `androidboot.init_fatal_panic=true` 可以把 LOG(FATAL) 类失败转成真 panic 走 pstore，
  但服务级失败（`reboot_on_failure`）仍是正常 shutdown，两条通路都要布。
- **cgroup v1 在 6.12+ 拆到 `*_V1` 选项后面且默认关** —— `CONFIG_CPUSETS=y` 只给 v2。
  Android 的 cgroups.json 要求 cpuset 走 v1，缺 `CONFIG_CPUSETS_V1` 时
  `SetupCgroups` 失败 → 所有服务起不来 → `bootstrap-apexd-failed` 复位。
  一并要 `MEMCG_V1` / `UCLAMP_TASK(_GROUP)`（task_profiles.json 引用）。
  见 `docs/stage2-findings.md` 第 8 节。
- 游戏多走 GLES，freedreno GL 路径和 zink-over-turnip 都试，先通再选

### 捷径备忘

- **面板与 Galaxy Tab S7 FE（gts7fe / SM-T733）同款** → DPI、时序、背光曲线
  可直接参考 Tab S7 FE 的 AOSP 设备树
- Stage 3 卡死时的止损方案：Cuttlefish（`cvd start --gpu_mode=gfxstream`），
  KVM 可用，是真 Android VM 不是容器

### 协作

- gaokun 社区（Linux 侧）：面板、EC、休眠、触摸的问题问他们
- aospm 社区（Android-on-mainline 侧）：HAL、设备树组织方式问他们
- ~~这两个圈子此前没有交集，本项目是第一个连接点~~
  **已有平行项目**：LineageOS 系 mainline-generic 正在做 gaokun3 live-ISO
  （同款 buildbot 内核），双方结论交叉验证一致。他们的 config fragment
  和模块清单是我们 Stage 3/4 的路线图，见 `docs/parallel-mainline-generic.md`。

---

> ## ★★★ 开工前先读：设备正常，相机可用（2026-09-13 晚）
>
> ✅ **设备在线**（2026-09-13 起是 **USB adb**，`adb devices -l` 显示 `usb:...`
> —— 所以动 WiFi 的实验不会切断自己的连接；⚠️ 走 TCP 时网段会漂，
> 用 `arp` 扫 5555 端口找，MAC 前缀 `00:03:7f` 是本机的 WCN6855）。
> ⚠️★ **运维两条新坑（2026-09-13）**：① 主机名 `cicd` 被本机代理解析成
> fake-IP `198.18.0.92`，**ssh 过去必被掐断**，用真实 IP 直连才稳；
> ② `az` 会被同一个代理 MITM 掉证书，**报错不是警告是真失败** ——
> 加 `AZURE_CLI_DISABLE_CONNECTION_VERIFICATION=1`。
> ⚠️★★ 我在这里第三次栽进同一个坑：`az ... | tail -3` 之后取 `$?` 拿到的是
> `tail` 的退出码，于是打印"已下发停机"而**机器根本没停**。
> ⚠️★★ **连本机的网段都会换** —— 2026-09-12 一天之内 `192.168.31.x` →
> `192.168.10.x` → `192.168.130.x`。找设备前先 `ifconfig` 看自己在哪个网段。
> ⚠️★ **扫全网段，别按"上几次 IP 都落在哪"收窄。** 2026-09-12 我把 ping 扫描
> 收窄到 `.150–.230`（前四次都在这里），结果设备拿到 `.124`，于是
> **在搜索范围外 10 分钟没找到，而那看起来和"内核挂死了"一模一样** ——
> 我据此写了"硬挂死、安全阀都没触发"，还让用户去按电源键。实际上它**一直好好地
> 跑着内核 `#5`**。★ 收窄搜索范围会把假阴性伪装成阳性结论。
>
> **⓪n ✅ v0.6.1-alpha 已发布（2026-09-14 17:5x，用户拍板）**：戳 `1789364282`，R2 `builds/` + `install/<ver>/` + 线上清单
> 已切（设备侧 curl 200、download 指 R2）；GitHub Release `v0.6.1-alpha` 打在构建源 `10acff3` 上，附件 boot.img /
> install-artifacts.sha256 / gaokun3.json / zip / super.img.zst **五个附件全部就位**（下载 URL 200、大小与 sha 文件一致）。仓库 `ota/gaokun3.json` 与线上逐字节同步。
> ⚠️ 下午的降噪 / 预闪 / usbrole v2 改动**不在**这一版里（只验过 bind mount，未进镜像），留给 v0.6.2。构建机已 deallocate。
>
> **⓪m 2026-09-14 下午两问（[#112](stage4-findings.md#112)）：噪点 & 息屏 USB adb。**
> ★ **UCSI 活了**（typec port0 有 partner），但数据角色**反的**（PC 插着报 `[host]`），靠 `init.gaokun3.usb.rc` 硬写 device 盖住。
> ★ dwc3 源码：device 模式挂起无条件 `dwc3_core_exit()`（PHY 下电，= #56 的复位点）且 gadget 总 soft disconnect
> ⇒ **上游没有"adb 穿越睡眠"**。折中已写：`gaokun3-usbrole.sh` v2 插着主机息屏不切 host、不睡；拔线再切。
> ⚠️ 本机 `persist.gaokun3.allow_suspend=0`（不知何时关的），现在根本不进 s2idle。
> ★ 相机：软件 ISP **其实每帧报 AnalogueGain/ExposureTime**（libipa `fillMetadata`），HAL 此前没读。已加增益自适应降噪
> （BLOB RGB ε 滤波 / 预览色度模糊）、预闪按亮度收敛。**实测同场景**：不开闪光高频残差 3.62 → 2.01（−44%，增益 15.5x 顶格）；
> 闪光片增益 15.5 → 4.95 说明预闪收敛生效，但白墙仍过曝/偏绿 —— 是动态范围 + AWB 统计不剔饱和像素，下一步 ExposureValue 负补偿
> + swstats 剔饱和（#112 §5）。**未进镜像**，平板上以 bind mount 跑着新 HAL（重启消失）。
> ⚠️ 测法坑：root 手动起 provider 会选 CMA 堆而分配失败；要用 `mount --bind` 盖到 vendor 路径再 `start` 服务。
>
> **⓪l ✅ 2026-09-14 13:26：v0.6.1（戳 `1789362233`）已装机验收通过（[#111](stage4-findings.md#111) §6），【未发布】。**
> `#19` / `_a` / IPA 目录对 / provider 在 `hal_camera_default` 域 0 denial / 后摄 flash TRUE / **快捷设置手电筒砖实测开关 LED**。
> ⬜ 应用内拍照闪光要解锁，等用户。⬜ 发布（R2 + GitHub）要用户点头。ESP 实验槽位全清（50 MB 可用）。
> ★ 第二版 `release-061d`（戳 **`1789364282`**，只多"相机 ID 按 Location 排、后摄=0"）已装到 `_b` 验收通过（14:07），
> **这一版是待发布候选**：staging 在 R2 `staging/crDroidAndroid-16.0-20260914-gaokun3-v12.11/`，发版说明 `docs/relnotes/v0.6.1-alpha.md`。
> 发布 = 构建机开机后 `release.sh --no-build`（清单）+ `gh release create v0.6.1-alpha`（附件）—— **等用户点头**。构建机已 deallocate。
> ⚠️ `sysui_qs_tiles` 运行时改不动（SystemUI 毫秒级覆盖回去），要在它重启的窗口里写。
>
> **⓪k 🔦 2026-09-14 下午：闪光灯接进相机 HAL（[#111](stage4-findings.md#111)），v0.6.1 带它重编中。**
> 没有同步 strobe ⇒ "闪光" = 预闪触发点灯、报几帧 PRECAPTURE 让 AGC 适应、到静态照片完成才灭（`firedPending_` 计数，
> 别在下一个请求入队时灭——框架把预览请求紧跟着拍照请求发来）。手电筒 = `setTorchMode` 写 brightness。
> ★★ 顺手抓到 HAL **从来没读 FMQ 里的请求设置**（框架优先走 FMQ，`AidlCamera3Device.cpp:1268`）—— 已修。
> ★ provider 此前跑在 `u:r:init:s0`（没 file_contexts 条目），现 `hal_camera_default`；`/dev/media*`/`v4l-subdev*`
> AOSP 没标，补成 `video_device`。★ 手电筒砖要 `FEATURE_CAMERA_FLASH`（features xml 里连后摄 `android.hardware.camera`
> 都没声明），要装镜像才能验。模组 EEPROM（0x50，16 KiB）读出来入库 `docs/hw/`。
> ⚠️ 本会话 `pkill -f` 自杀三次（`dmesg -w` / `camprov` / `com.android.systemui`）——一行命令里只用 `pkill -x` 或 `kill $(pidof)`。
>
> **⓪j ✅ 2026-09-14 中午：内核 `#19` 两路验收全过、闪光灯定案、v0.6.1 重新构建中（[#110](stage4-findings.md#110)）。**
> 正常 47 个 subdev / 前后摄各 12 帧；`ov13b10.fail_probe=1` 时 camss 20 s 后丢掉未绑端点、前摄照常（0035 生效）。
> **闪光灯 = PM8350C 闪光模块 1+4 路**（四路逐个 torch、用户看背面、成对对照定案），`patches/0036` 收成单节点，
> GPIO93 那个不亮的 gpio-led 删掉；dtb v2 上机只剩一个 `white:flash`。`prebuilt-boot/` = `#19` + dtb v2。
> ⚠️★ **早上那版 v0.6.1 候选（戳 `1789344148`）不能发**：构建机 `external/libcamera/Android.bp` 没有 #109 的修法
> （B0 第五次），会把"相机打不开"原样再发。已覆盖、逐文件 md5 对齐后重编（`~/release-061b.log`，`--stage-only`）。
> ⚠️ 本机 ESP 曾只剩 4.5 MB（实验槽位吃的），postinstall 要 56 MB ⇒ 装机会失败；`slot_cam`/`slot_cam4` 已删（B13）。
> ★ 用户 12:3x 指令：**不推仓库、不发版，其余直接做**（含 TODO 里的项）。
>
> **⓪i ⚠️★★★ v0.6.0 正式镜像上相机打不开（[#109](stage4-findings.md#109)）**：IPA 模块被装在 `/vendor/lib64/`，
> libcamera 却在 `/vendor/lib64/libcamera/ipa/` 找 ⇒ 软件 ISP 建不起来 ⇒ HAL 把裸拜耳当 RGB24 喂 libyuv ⇒ SIGSEGV 循环。
> 开发期那份是手动 push 到 overlay 的，`enable-verity` 一拆就露馅。用户机器已热修（remount + 符号链接，
> ⚠️ 下次 OTA 前 `enable-verity` 会抹掉）；正式修法：Android.bp `relative_install_path: "libcamera/ipa"` + HAL 两道保险，
> **必须进 v0.6.1**。★ 装机验收要包含一次经 HAL 的真实出流（枚举成功 ≠ 能出图）。
> ★★ 教训：overlay 里手工铺过的每个文件都是一条没进构建系统的依赖 —— #86 抓到 yaml，#109 漏了 IPA 目录。
> 另：用户反馈 #1 的 OTA 失败是手工分区无 PARTLABEL=esp（他自己解决了），postinstall/HAL 已改成按内容找 ESP。
>
> **⓪h ⚠️★★ 2026-09-14 中午：v0.6.0 一发就有用户反馈更新失败。** 原因几乎可以肯定是我昨夜把清单里的
> `download` 改成了 GitHub Release 附件 —— 它 302 到 `release-assets.githubusercontent.com`，**国内不可达/极慢**
> （Updater 的下载客户端确实跟随重定向，`HttpURLConnectionClient.java:275`，所以不是重定向的锅）。
> 已把线上清单换回 R2 `builds/`（zip 一直在），仓库 `ota/gaokun3.json` 同步。
> ★ 连带作废 v0.6.1 候选里的"Updater 清单切 raw.githubusercontent"—— raw 同样被墙，已改回；
> **释放 R2 桶之前必须先有国内可达的镜像**（同一 Cloudflare 域名下用 Worker 反代 GitHub 附件是最省事的路），
> 这是用户的决定点，不是我能单方面做的。★ 教训：托管地址的变更要按【用户所在网络】验证，本机能通不算。
>
> **⓪g 🚦 收尾计划上午段（2026-09-14）：两样东西编好等中午重启验收（[#108](stage4-findings.md#108)）。**
> 内核 `#19` 在 `slot_cam5`（`cam5` 正常 / `cam6` 带 `ov13b10.fail_probe=1`）：0035 camss 容忍未绑传感器、
> 0034 v3 get_selection、0036 PMIC 闪光四路试接线（⇒ ⓪j 定案 1+4 路）；验收脚本 `k19test.sh normal|fallback|flash`。
> ROM v0.6.1 候选（戳 `1789344148`）在 staging、payload 已预推：ov13b10 增益模型/属性/yaml、**Updater 清单切 GitHub**。
> ⚠️ v0.6.1 内核仍是 #18；#19 验过再定。A8（WAN 1/20）结案：平板与 PC 一样 8 MB/s。hangdump 改读 binderfs。
> 上游稿在 `docs/upstream/`（未发）。
>
> **⓪f ✅ v0.6.0-alpha 已发布（2026-09-14）**：装机验收全绿（`_b` 槽、戳 `1789318530`、内核 `#18`、47 subdev、
> 前后摄各 12 帧、PA=21、温控 HAL、0 崩溃 0 stuck），R2 清单已更新，GitHub Release 挂了全部产物
> （用户决定：以后滚动更新走 GitHub Release，R2 桶要释放；清单 `download` 已改指 GitHub 附件）。
> ⚠️ `install-ota-local.sh` 第三次被中间态骗：上一轮残留的 `finished last action ...` 让它在 30% 时报成功、
> 提前掰 default —— 已改成只认 "Update successfully applied"。下面是发布前的原文：
> **⓪f（原文）🚦 v0.6.0-alpha 已构建并传到 staging，【未发布】，等用户审核（2026-09-14 凌晨）。**
> 构建戳 **`1789318530`**，包 `crDroidAndroid-16.0-20260913-gaokun3-v12.11.zip`（sha `3e88b8ad…`，1.35 GB），
> `boot.img` sha `d9bc9203…`（内核 `#18` + 后摄 dtb），四条断言全过；位置 R2 `staging/<包名>/`，
> **`ota/gaokun3.json` 一字未动，没有用户会收到**。发版说明草稿 `docs/relnotes/v0.6.0-alpha.md`（待审）。
> ★ 镜像核对抓到一条：第一次构建**漏了 `hi846.yaml`**（平板上那份是 overlay 推的），补进 `device.mk` 后重编
> （戳 `1789316646` 那版作废，已被同名覆盖）。第二次核对：相机 HAL / libcamera ×3 / 两个 yaml / 温控 HAL /
> `wpa_cli` / `gaokun3-camera.rc` 无钉住 —— 全在。`tinymix` 在 `/system/bin`，不在 vendor，别再误报。
> ⬜ **醒来后的装机步骤**（要人在场，新 ROM 第一次开机）：payload 已预推到 `/data/local/tmp/`；
> ① `adb enable-verity && adb reboot`（overlayfs 开着 update_engine 拒装，**这一步会抹掉 overlay 里的相机 HAL**，
> 装完新 ROM 才回来）；② `bash scripts/install-ota-local.sh --check` → `--go`；③ 写 oneshot 到新槽、重启、验收
> （前后摄、扬声器 PA=21、温控、`stuck at` 0）；④ 审发版说明；⑤ 你点头后构建机上 `release.sh` 不带 `--stage-only`
> （用 `--no-build`！发验过的那一版）+ GitHub release。
> ⚠️ 用户拍板"后摄一起发"：S5K3L6 模组的机器会**前后摄一起消失**（v7.2 camss 等所有传感器），说明里已写明。
>
> **⓪e ★★★★ 后摄通了：它是 OV13B10（[#106](stage4-findings.md#106)，2026-09-14 凌晨）。**
> 板级证据来自华为 Windows 驱动包（`uup-drivers-sc8280xp` release 200.0.10.0 → `qccamrearsensor_extension8280.cab`
> → `CAMS_RES_QRD.bin`，简单 TLV，`bsdtar` 能解 .cab）：LDO2_B 2.8V / LDO2_C 1.8V / GPIO92 门控 / 复位 GPIO7 / MCLK4；
> 前摄同格式解出来与现役 DT **逐项一致**（格式由此校准）。DSDT 显示设备投 LDO2_B **1.8V**、后摄投 **2.8V**，
> RPMh 取最大 ⇒ 共用轨是板子的设计；驱动 `power_on()` 请求 2.8V、`power_off()` 放回。
> ★ 决定性一步：ID 读 NAK 时**在轨亮着的时候扫整条 CCI 总线** —— 0x36（OV13B10）与 0x50（EEPROM）应答、0x10 无人。
> ⚠️ 两个坑：`make dtbs` 失败而我没看 RC 就部署（旧 dtb 配新内核）；`VIDEO_DW9714=m`（第 15 个 =m 坑）让
> ov13b10 的 `lens-focus` 永远等不到 vcm ⇒ **前后摄一个 subdev 都不出**，`pending_async_subdevices` 一眼看穿。
> ✅ 内核 `#18`：47 个 subdev、`camtest --rear 2104 1560` 出帧、前→后→前交替出帧、HAL 枚举 2 个相机。
> ✅ **Aperture 直接打开后摄**（`Device 1 is open`，输入 4208x3120 GRBG），用户确认 L2B 抬到 2.8V 时屏幕正常。
> ⚠️ 第一眼是彩条 —— camtest 留下的 Test Pattern（已改成退出清零，未重编）；复位后预览近黑：夜里 + libcamera 没有 ov13b10 增益模型。
> ✅ 用户手电筒照镜头看到景物 ⇒ **`#18` + 后摄 dtb 已是默认槽**（`slot_a`），`#14` 留 `slot_cam4` 回落，prebuilt-boot 同步。
> ⬜ libcamera 侧 ov13b10 属性/增益模型/`ov13b10.yaml`（夜里预览近黑就是缺增益模型）；EEPROM@0x50。
> ⬜ 闪光灯走 PMIC 闪光模块（Windows FLSH 资源块为空；GPIO93 脉冲实测不亮）：要 `LEDS_QCOM_FLASH` + pmc8280c 节点 + 试 `led-sources`。
> ⚠️ 稳健性：v7.2 camss 要求端点上所有传感器都绑上，任一没绑前后摄一起消失（0018 当初的理由）。
>
> **⓪d ★★★★★ 电源域缺陷已根治（[#105](stage4-findings.md#105)，2026-09-14 凌晨）。**
> 根因 = `camcc-sc8280xp` 里 `camnoc_axi_clk_src`/`slow_ahb_clk_src`/`fast_ahb_clk_src` 用普通
> `clk_rcg2_ops`（关闭时不停靠 XO）。camss 用完相机后 CAMNOC AXI 的 RCG **指着一个已熄灭的 PLL**
> （clk debugfs：`parent=camcc_pll0_out_even`、`camcc_pll0 en=0`），GDSC 掉电/上电与 CAMNOC 握手
> 等不到 ⇒ 正是 #104 的 bit 19 签名与 `0xE` 冻结。修法 `patches/0031`（3 行）。
> ★ 路径：`#11` 否掉等待值假说（`0027`，硬件复位值确实是 2/2/0xf，保留）→ `#12` 用 `0029/0030`
> 旋钮二分：裸翻转 ife_0 干净、任何组合只要跑过 `vfe_get()` 就毒化 → 直接读 clk debugfs 看见死源。
> ★ 前面每条证据都对得上：BCR 复位让 bit 19 回来但上电仍要时钟；重试无用；0020/0021 都不改 RCG 源。
> ⚠️★★ **两个自己造的坑**：① `kernel-apply-patches.sh` 的指纹判据把 0031 的新增行（文件里别处已有 21 行
> 一样的）当成"已应用"静默跳过 —— **第一版 `#13` 根本没带修复**；现已改成正向 `--check` 优先。
> **指纹只回答"这些行在不在"，回答不了"是不是这个补丁放的"。** ② `dbg_skip=14` 同时跳 CSID+VFE
> ⇒ `vfe_get()` 没跑 ⇒ `vfe_flush_buffers` 空指针 panic（`panic=10` 自动回落）；我当场误读成
> "CSIPHY 写了没电的寄存器"，**读了 pstore 才纠正**。旋钮组合要保住硬件依赖顺序；
> 循环里"没拿到明确结果"必须是第三种状态，不能当阴性。
> ★ pstore 里还压着 `#6` 的记录：#83 那次"unbind 拖死整机"其实是 rebind 时 `gdsc_register()`
> kobject 重复初始化的 panic —— 正是 `patches/0022` 修的缺陷。
> ⬜ 发版形态 `#14`（0020/0022/0027/0031，无诊断）已编出并在测试槽验过 4/4，待进默认槽；
> 进了才可撤 `device/huawei/gaokun3/camera/gaokun3-camera.rc:13` 的开机钉住。
> 诊断补丁 0023/0028/0029/0030 已挪进 `DIAG_PATCHES`，发版路径发现树里有它们会大声退出。
>
> **⓪ ❌ 内核 `#6` 已上机测完 —— `patches/0021` 被否（第七条）。**
> 补丁确实生效（`ad00000.clock-controller` 作为 interconnect 消费者出现、
> 投票随 GDSC 起落 1/1↔0），故障仍一字不差复现；**失败之后那条投票还留在 1/1**
> ⇒ NoC 全程抬着也没用。产物仍在 `slot_cam3/` + `…-cam3.conf`，日常走默认的 `#5`。
>
> **⓪a ★★★★★ 但同一轮拿到了方向性发现：触发条件在【下电】那一侧。**
> 同一次开机、同一个域：**无流量塌缩 → 上电 ✅ 12 帧；跑过 camtest 再塌缩 → ❌ −110。**
> ⇒ "塌缩过就必败"这个模型也不对，**塌缩本身无害**。
> 而且 A/B 两格**在上电那一刻的状态完全相同**（同样 `PWR_ON=0 COLLAPSE=1`、
> 同样 clk_summary / MMCX / icc），差别只在历史里 ——
> **这解释了前面七条假说为什么全部落空：它们全在查上电缺什么。**
> ⚠️ A/B **分不开**两种读法：①真有 DMA/AXI 流量没静默就塌缩；
> ②那段 ON 期间被打开又关掉的是**一整套 camcc 时钟 + CSIPHY 稳压器**（走 STREAMON
> 的 pipeline PM），而 A 格只是朴素的 genpd resume。**下一步必须拆开这两件事**，
> 阶梯实验见 [#102](stage4-findings.md#102)（★ 关键性质：没毒化的那一级不消耗这次开机）。
> ⚠️ 阶梯的第 1–4 级需要给 `camtest` 加一个"停在第 N 步"的开关，它现在是固定流程。
>
> **⓪b2 ★★★★★ 触发点已钉到"一次 STREAMON/STREAMOFF 循环"（[#103](stage4-findings.md#103)）。**
> `camtest --stop N` 五级阶梯，探针用 `echo on > .../camss/power/control`
> （朴素 genpd resume，实测无害 ⇒ **没毒化的那一级不消耗这次开机**）：
> 1/2/3 级（接链、设格式、REQBUFS+QBUF）**都不毒化**——它们根本没让域上电；
> **第 4 级（STREAMON 后立刻 STREAMOFF、0 帧）毒化**。
> ❌ 并否掉"时钟泄漏"：`STREAMOFF` 后确实泄漏 8 个 camcc 时钟，
> 但钉着等 10 秒会自己释放，**等到 0 个再塌缩照样坏**。
> ⚠️ 诚实边界：第 4 级仍 `QBUF` 过，排除的是**持续数据流**，
> 没排除"DMA 引擎被启动过又没干净停下"。
> ⬜ 下一步两件：① 内核侧在 `gdsc_toggle_logic()` 超时时打 GDSCR/CFG_GDSCR
> 全字段（**走 regmap，不碰 `/dev/mem`**，这是目前唯一安全的"看硬件内部"办法）；
> ② `REQBUFS` 但不 `QBUF` 再 `STREAMON` = 真正的零 DMA。
>
> **⓪b3 ★★★★★ 硬件级证据链（[#104](stage4-findings.md#104)）—— 根因仍未破，但已知的东西完全不同了。**
> * **与 DMA 无关**：`camtest --noqbuf`（一个缓冲都不入队）照样毒化 ⇒ 是 `s_stream` 开关序列本身。
> * **签名**：干净掉电 `CFG_GDSCR=0x00088000`（6/6）vs 脏掉电 `0x00008000`（2/2），
>   **`GDSCR` 完全相同、只差 bit 19**，掉电那一刻就能看见。
>   `[19:16]` 像个状态机：`6`运行→`8`干净/`0`脏→`A`上电中→`7`成功/`E`卡死（**这是读法不是文档**）。
> * **不可逆**：重做"掉电→上电"3 次全败，`POWER_DOWN_COMPLETE` 再也没置起 ⇒ FSM 冻结在 `0xE`。
> * ❌ 否掉三条修法：上电后重试、塌缩前复位 CAMNOC+CPAS、塌缩前复位全部 21 个 BCR。
> * ⚠️★★★ 最后那条**让判据达成了而缺陷照旧** ⇒ **bit 19 是相关量，不是充分条件**。
>   ★ 判据的两个问题：「两种结果下会不会不同」**以及**「能不能在没修好时被满足」。
> * 取证工具：`patches/0023`（regmap 转储，**不碰 `/dev/mem`**）+ `camtest --stop N/--noqbuf`。
>   ⚠️ 抓日志必须用 **`dmesg -w` 跟随式**：audit 噪声 2 秒刷 126 行，事后 grep 一定抓空。
> * ⬜ 下一步最划算的是**带着完整寄存器轨迹问上游**：`CFG_GDSCR[19:16]` 是什么？
>
> **⓪c ⚠️★★★ 一条新的操作禁忌（用一次强制关机换来的）**：
> **camss 已经 `runtime_error` 时，不要 unbind 它 —— 会拖死整机**
> （USB 仍枚举、adbd 不应答、只能长按电源键）。
> unbind 要等设备的 runtime PM，而 runtime PM 正卡在一个永远上不了电的域上。
> ★ 更值钱的那半条教训：我是"顺便"测 `patches/0022` 才这么干的，
> 而 0022 修的是 unbind/rebind 的 genpd 注销，**本来就该在健康状态下测**。
> **一个实验该在什么状态下做，是实验设计的一部分，不是"反正机器现在闲着"。**
> ⬜ 于是 **`patches/0022` 至今未验证**，要在干净开机、camss 健康时补测。
>
> **⓪b ✅✅ WPA3 结案并已连上（[#107](stage4-findings.md#107)，2026-09-14）**：#100 判"AP 不认密码"被证实 ——
> 用户在手机上核对，密码是数字顺序记错了。改对之后 **SAE 一次连上**（group 19、H2E、PMF），
> 带流量浸泡 3 分钟 **0 次掉线** ⇒ 本机 SAE 栈端到端可用，issue #2 在这台华为 AP 上**不复现**。
> ★ 教训："用户给的密码"与"路由器上配的密码"是两个事实，前者要在另一台已连上的设备上核对过才算证据。
> 平板已回到局域网（TCP adb 可用）。下面这段是 #100 当时的原文：
> **⓪b（原文）WPA3 结案：不是 WPA3 的问题。** 用户家里那台 AP
> **不认用户给的那个密码**（⚠️ SSID/密码/BSSID 一律不写进本仓，这是公开仓库） —— SAE 与 WPA2-PSK 两条独立算法都在"校验"那一步失败，
> 且与故意写错的密码**失败签名逐行同形**。`WifiConfigStore.xml` 里存的与用户给的
> 逐字相同，且该网络**从没连上过**。见 [#100](stage4-findings.md#100)。
> ⬜ 因此 [issue #2](https://github.com/vahiru/gaokun-android/issues/2) **仍未复现**
> —— 我们连认证都没过，报告者是关联之后才被踢，**不是同一个故障**。
> 要推进它仍然需要**一个已知密码正确的 WPA3 AP**。
> ★ 可给出的正面结论：**本机 SAE 栈机制上是活的**（群协商/Commit/RSNXE/H2E 全通，
> AP 接受 Commit 并处理了我们的 Confirm）。
> ★ 工具已就位：`m wpa_cli` → 推到 `/data/local/tmp`，
> `-p /data/vendor/wifi/wpa/sockets -i wlan0` 挂上运行中的 supplicant，
> **这是绕开 `config_wifiSaeUpgradeEnabled` 做 WPA2 对照组的唯一办法。**
>
> **① ✅ ROM 已装机并验收通过（2026-09-12 中午）。**
> 设备现在跑 **slot `_a` · 构建戳 `1789140755` · 内核 `#3`**。
> 装机 93 秒（`update_engine` 的 `file://` 通路）。逐条验过：
> **`tinymix` 实测 `SpkrLeft PA Volume = 21`**（+6 dB 进镜像了）、
> 真温控 HAL 连上且 skin 44 °C 不关机（36 °C 那颗地雷已拆）、
> 触摸模式两文件都在、**导航栏出现了**（TODO A0 的修法生效）、HEVC 在表里、
> `logcat -b crash` 0 行、slot `_a` 已 `marked_successful`。
> ★ 事后才看清这次风险本来就低：postinstall 写进 `slot_a/` 的内核与当时在跑的
> `slot_b/` 内核 **sha 完全相同** ⇒ 只换了用户态。**上机前先量"改了几个变量"。**
> ⚠️ 装机脚本里揪出三个"长得像成功"的 bug（假阳性完成判据 ×2、
> 安全网 glob 写成下划线匹配不到条目），都已修并写进代码注释，见
> [#86](stage4-findings.md#86)。
> ⚠️★★★ **装完 OTA 后 `_b` 槽【不再存在】** —— `lpdump` 实测 super 里只剩 `_a`
> 一套逻辑分区。我在验收里写过"`_b` 是现成回落"，**错的**，而本文下面早就
> 写着"`bootctl is-slot-bootable` 不代表 super 里真有那套分区，先用 `lpdump` 查"。
> 真正的回落 = `default *-android-a.conf` + 两个救援条目。见 [#86](stage4-findings.md#86)。
>
> **② ❌ 相机内核 `#5` 已上机实测 —— 上游候选修复【被否】。**
> `patches/0020`（`unregister CAMCC_GDSC_CLK`）确实生效了
> （`clk_summary` 里 `camcc_gdsc_clk` 出现 **0** 次、34 个 `/dev/video*`），
> 但故障**一字不差复现**：第一次 camtest 12 帧 → 3 秒后 `titan_top_gdsc` 塌缩
> → 第二次 `-110`，同一条 `gdsc.c:185 titan_top_gdsc status stuck at 'off'`。
> ⇒ camss 电源域缺陷至此排除 **六条**。见 [#87](stage4-findings.md#87)。
> ★ 这个否定结果**是被预测到的**：#83 当时就记了反证（本机
> `camcc_gdsc_clk` 硬件使能一直是 `Y`）。教训是**当时低估了那条反证的分量**
> —— 上游告警文本与我们逐字相同，这种表面相似度压过了一条直接矛盾的实测。
> **文本相似度不是证据。**
> ⚠️ 另查出 **`camcc-sc8280xp` 的 unbind 路径是坏的**：probe 里 `gdsc_register()`
> 注册的 genpd 在 unbind 时没注销，rebind 撞名 `-22`，相机子系统会进入
> **"已解绑且绑不回去"**、只能重启恢复的状态 ⇒ "出错后重绑 camcc 恢复相机"
> 这条廉价规避不存在。
>
> ★★ 根因暂时搁置，**用户选择直接开工 libcamera HAL**。开发期用"开机即钉住
> camss"当桥（实测有效）；功耗的账留到真要发布时再算（那时再测 pin 的代价）。
>
> **③ 📷★★★★★ 相机日常可用了 —— 正常启动就能预览、拍照。**
> ![拍的照片](img/gaokun3-camera-shot.jpg)
> 走 `default` 普通启动（**不用 oneshot**）：内核 `#5`、`camss=active`、
> 相机数 1，开 Aperture 能预览，按快门存出正常 JPEG 到相册。
> 链路：`hi846 → CSIPHY3 → CSID0 → VFE0 RDI0 → libcamera simple 流水线
> → 软件 ISP（去拜耳+AWB+AGC）→ libyuv → 自研 AIDL HAL → 相机应用`。
> 案卷 [#88](stage4-findings.md#88)…[#99](stage4-findings.md#99)；
> 代码 `device/huawei/gaokun3/camera/`，libcamera 的 Soong 移植与四个补丁
> 在 `patches/libcamera/`。
>
> ⚠️ **仍欠的账（都记在 [#99](stage4-findings.md#99)）**：
> * **[#87](stage4-findings.md#87) 电源域缺陷没根治** —— 靠 `on boot` 钉住
>   camss 当桥，代价是相机电源域常开、**功耗未测**。
> * **画质只是修掉了偏色，不等于调好** —— 没有 CCM（要拍色卡标定），
>   AWB 是灰度世界法；`hi846.yaml` 给的是单位增益、不含标定信息。
> * HAL 与 libcamera 目前**只在 overlay 里**；`device.mk` 已放开
>   `PRODUCT_PACKAGES`，要下次构建 ROM 才进镜像。
> * ⚠️ libcamera 的 5 个生成 `.cpp` 与生成头仍靠**手动跑一次 meson 再拷贝**
>   （#82/#85 的形状）。正解是 Soong `genrule` 跑它自带的 Python 生成器。
>
> **③ ⬜ WPA3（[issue #2](https://github.com/vahiru/gaokun-android/issues/2)）本地测不了。**
> 判据很干净：同一台 ZTE 路由器、同一个 5 GHz 信道 36，只改安全模式就一正一反
> ⇒ 变量是 SAE；而且是**关联之后被踢**，不是认证失败。
> 已排除三层（supplicant 编译、Android 框架 feature set、ath11k 的 cipher 处理），
> 见 TODO A6b。⚠️ 周围零个 WPA3 网络（框架计数器证实它从没见过），
> **要么用户提供 WPA3 热点，要么请报告者跑 `scripts/wifi/wpa3-probe.sh`**。
> ⬜ 给报告者的回复**还没发** —— 发到 GitHub 是对外动作，等用户点头。
>
> ⚠️★★★ **一条用血换的规矩**：**对【时钟/电源可能被门控】的寄存器块，
> `/dev/mem` 的【读】和写一样危险。** 2026-09-12 我用 `devmem` 读 camcc 的
> 寄存器（当时 camcc runtime-suspend），触发总线 external abort、内核静默死亡，
> 机器停到用户醒来。Stage 5 的 `smmu-nostall.sh` 记过同形状，我没推广。
> 非读不可时先钉住控制器（`echo on > .../power/control`）并确认 `runtime_status=active`；
> ★ **并且要看"此刻有没有人能按电源键"—— 没人能按时，这类探针一概不做。**
>
> ★★ **相机已有可用的规避**（根因未破时的桥）：**在第一次塌缩之前**
> `echo on > /sys/devices/platform/soc@0/ac5a000.camss/power/control` ——
> GDSC 永不掉电，实测钉住时连跑 5 次 + 空闲 60 秒后再跑全部成功，解钉 8 秒后立刻复现失败。
> ⚠️ 必须在塌缩**之前**钉（已塌缩后再钉会当场触发失败并锁死 `runtime_error`）。
> ⚠️ 代价是相机电源域常开、功耗未测，**相机 HAL 落地前不要写进 ROM**。
>
> ⚠️★ **平时没有「可以随时启动的另一个槽」**：super 里只有 `_b` 一套逻辑分区，
> 这是 **Virtual A/B 的设计如此**。★ 真判据：**`bootctl is-slot-bootable` 读的是
> misc 里的标志位，不代表 super 里真有那套分区** —— 先用 `lpdump` 查。
>
> ⚠️★ **Alpine 救援的 squashfs 在 Ubuntu 救援分区上**（`p3:/gaokun3/rescue.squashfs`）
> —— Stage 7 提的「给 Ubuntu 瘦身/换掉」要先把它挪走，否则两个救援一起废。

> **★★★ Stage 7 M0（2026-08-23）：轻量救援系统上机完成。**
> Alpine 救援系统 **ssh 可达、WiFi 自动连上、分区工具齐全**
> （squashfs **55 MiB** + initramfs **2.7 MiB**，替掉 24.6 GiB 的 Ubuntu）。
> - ★ 真凶是**内建 ath11k 在 initramfs 阶段拿不到固件**
>   （probe 在 t=1.19s，**远早于 switch_root**）—— 所以 initramfs 从 648 KiB
>   涨到 2.7 MiB 是**故意的**：固件必须在 initramfs 里。
> - 安装器后端已在**真实磁盘**上验过，双系统方案算得出来
>   （63.9 GiB 空闲区 → /data 50.7 GiB）；图形安装器七屏已编译并离线渲染检查。
> - ⬜ 欠 `gk3_apply`（真写盘）与 DRM 后端。⏸ 用户 2026-08-23 决定暂缓（TODO B4）。
> - ⚠️ 详细设计见 `docs/stage7-live-installer.md`，构建脚本与六个坑见
>   `scripts/live/README.md` —— **那两份的"状态"段落比本条旧**，以本条为准。

> **★★★★ Stage 6 M20（2026-08-23 夜，用户睡觉期间）：root 进 ROM + SELinux 四步走完。**
> 完整案卷 [#76](stage4-findings.md#76) / [#77](stage4-findings.md#77)。
> - ★★ **第 3 步（sysfs 打标签）是本轮性价比最高的一击**：
>   `network_stack` 236→0、`hal_health_default` 235→0，**四条 `genfscon`、
>   零 allow 规则**。根因是 AOSP 的 genfs 只标了 `/class/...`，而那是符号链接
>   —— SELinux 标的是真实 inode，也就是 `/sys/devices/platform/...`。
>   手机 SoC 的那些路径 AOSP 顺手覆盖了，**sc8280xp 的没有**。
> - ★ **先定义域、再普查**这个顺序被再次证明是对的：大批
>   `binder → u:r:init:s0` 的 denial（system_server 打给我们的 HAL）
>   在打完标签后**自己消失**。反过来做会写出"允许 system_server 调用 init"
>   这种荒唐规则。
> - ⚠️★ **装机时拆掉一颗地雷**：`update_engine` 标记 slot_a 为 active 之后，
>   boot_control HAL **立刻把 `default` 改成了 `*-android-a.conf`** ——
>   新槽起不来就连回落都没有了。重启前必须把 `default` 掰回已知可用的那个槽，
>   只用 oneshot 过去。
> - ⚠️★★ **同一个陷阱一天咬了三次**：在挂载点/chroot **外面**用 `[ -e ]`
>   判**绝对**符号链接。`/sbin/init -> /bin/busybox` 从外面看解析到宿主的
>   `/bin/busybox`，于是好端端的东西被判成缺失。**判据要么在里面跑，
>   要么用不跟随链接的方式并单独确认目标。**
> - ⚠️★ **genfscon 是前缀匹配**：我给 UCSI 的 `power_supply` 打标签时
>   连带盖住了下面的 `wakeup23`（本该 `sysfs_wakeup`）。
>   症状是 **denial 的类型变了而不是消失** —— 极容易被误读成进展。
> - ★ **enforcing 的真正门槛不是规则数量**，是两个结构性的东西：
>   `hangdump` 读 debugfs 那条 neverallow **没有 userdebug 豁免**；
>   `smmustall` 要 `/dev/mem`（有豁免，但会让 enforcing 取决于构建变体）。
>   后者的正解是把 **B6** 做掉，脚本整个消失。
> - ⚠️ 另记：`screenrecord` 在**灭屏**时报 `UNASSIGNED_LAYER_STACK`，
>   那跟编解码器毫无关系。TODO A2 据此改写 —— 硬件编码不是"待验证"，
>   是 2026-08-22 查明后**故意关闭**的（rank 0x80 会压过软编，
>   导致应用失败而不是回退）。

> **★★★★ Stage 6 M19（2026-08-23）：root 通了；两条"以为是版本问题"的错判。**
> ★ **ReSukiSU 在主线 v7.2-rc2 上跑通**（据我们所知是 sc8280xp 上第一次在
> 非 GKI 主线内核上跑通 KernelSU 系）。实测证据是活的：
> `packages.list detected: 128`、`handle_setresuid from 0 to 10291`、
> `allow root for: 10299`、`/data/adb/ksud` 由管理器自己铺好。
> 完整案卷 #74，验收 `scripts/verify-root.sh`。
> - ★★ **两个配置换掉了一堆源码改动**：钩子选 tracepoint 而非手工钩子
>   —— ReSukiSU 的 `KERNEL_TYPE` **只按版本号判**，7.2 直接被算成 "GKI 2.0"，
>   于是**内核源码里一个钩子都不用插**（否则 6 个文件 8 处，每次 rebase 重对）；
>   `CONFIG_KALLSYMS_ALL=y` 又整块跳过 `static_export_check.mk`，
>   省掉 6 处 selinux 去 `static`。代价只是 `FTRACE_SYSCALLS=y`（本机默认关）。
> - ⚠️★ **我先选错了上游**（把 "resukisu" 读成 SukiSU-Ultra），但那一轮的失败
>   有价值：SukiSU-Ultra 的 4 个编译错误**一个都不是版本问题**，全是
>   "这棵树不是 Android common kernel"。**"新内核编不过"和"非 ACK 编不过"
>   是两回事**，默认按前者去查会一路查偏。
> - ⚠️★★ **`set -o pipefail` + `grep -q` = 稳定假阴性**：验收脚本第一版四个
>   配置项全报 FAIL 而值全对（`grep -q` 提前退出 → 写端 SIGPIPE 141 → pipefail
>   判整条管道失败）。它**长得和真失败一模一样**，而且打印出来的"实际值"还是对的
>   —— **判据自相矛盾时，先怀疑判据。**
> - ⚠️★ **`\(` 会破坏 make 的括号配平**：`$(shell grep -qE "…\(…")` 里那个转义
>   左括号照样被 make 计数，吞掉收尾的 `)`，报 `invalid syntax in conditional`。
> - ⚠️★ **别拿开机日志当判据**：本机 dmesg 环形缓冲**十几秒就绕回**，
>   `Initialized with driver version` 那行早没了。改成活体证据（数 hook 行数、
>   制造一次 execve 再数）。
> - ⚠️ 安全性已查：`init.c:268` 那个 `setenforce(true)` 只在 LKM 后加载分支，
>   内建构建 `ksu_late_loaded` 写死 false，**不会动 permissive**（已作为回归项）。
> - `su` 给 adb shell 用**还不通**（`allow_shell` 默认 false）。可用
>   `kernelsu.allow_shell=1` 打开，**但不建议默认开** —— 那等于任何能连 adb
>   的人无确认拿 root。
>
> **★★★ chainload 实测成立（#73），而且我第一轮用错了判据。**
> systemd-boot 的 `efi` 指令能在本机 LoadImage + StartImage 另一个 EFI 应用，
> 三轮实验全自动回到 Android，**没人碰过机器** ⇒ TODO B3 的安全阀是**验过的**。
> - ⚠️★ 我选的判据 `LoaderImageIdentifier` **只在未设置时才写**
>   （`export-vars.c:36`），嵌套实例永远不会改它 —— 这个判据在物理上就不可能
>   区分两种结果。★ **选判据先问"两种结果下它会不会不同"**，
>   一个恒定的观测量不是弱证据，是**零证据**，但读起来和阴性结果一模一样。
> - 真正的签名是 `boot.c:1381` 的自排除规则；靠它做了一次**预先声明的预测**
>   （oneshot 指向副本 → 该条目会从 `LoaderEntries` 里消失），实测逐字命中。
>
> **★ Stage 7 设计成型**（`docs/stage7-live-installer.md`，用户提的第 3、4 项）：
> 救援系统与 LiveCD 图形安装器是**同一件事**，一套镜像两个 profile；
> 救援不再是一个 24.6 GiB 的分区，而是内核 + initramfs + squashfs，
> 且**与 Android 共用同一个内核**（为此补了 `SQUASHFS=y`——原本是 `=m`，
> 第 14 个「=m 坑」——以及 `NTFS3_FS=y` / `NLS_UTF8=y`）。
> ⚠️★ `NTFS3_FS` 的 `depends on !NTFS_FS || m` 会把它**钉死在 =m**，
> 得先 `--disable NTFS_FS`，否则 `--enable` 写进去也会被 olddefconfig 改回来。
> ⚠️ ESP 只剩 26 MiB；`Persisted_Capsules.bin`（70 MiB）**决定不动**
> —— 拿固件更新通路去赌 70 MiB，赔率不对。

> **★★★ Stage 6 M18（2026-08-22）：光感 —— 先造工具，再做实验。**
> ★★ **纠正一条本仓记错的事实：SLPI 停得掉。** 此前写着"`echo stop` 返回 0
> 但状态还是 running（attached mode）"，实测直接变 `offline`
> （`remoteproc remoteproc0: stopped remote processor slpi`）。
> 判据一眼可见：sysfs 显示的是 **`running`** 而不是 **`attached`**，
> 且 `firmware` 属性有值 ⇒ 是 Linux 引导的。
> ⇒ "每个实验都得重启整机"不成立，循环降到 **60–90 秒**。
> - ★★★ **补上了 #70 缺的那一环：证明 DSP 真读了我的目录。**
>   阳性对照 = 把加速度计 JSON 从自定义根里删掉 → 该传感器消失而 SSC 照常上线。
>   同时再次确认 #71：只重启 hexagonrpcd 时，根目录换成**空目录**加速度计
>   照样满血 ⇒ SEE 初始化一次后不再依赖文件服务器。
> - ★★ **阴性对照定出失败形状**：把能用的加速度计地址改错（54→99），
>   失败签名与光感**一模一样** ⇒ 驱动在跑、在 probe，**芯片不应答**。
> - ★ **新结构：二供料。** IMU（sh3001 ✅／t1000）在 bus 1，
>   光感（tcs3701／sy3133cs）都在 bus 5。t1000 同参数却没注册 ⇒
>   SEE 的"探测择优"本身正常。
> - ❌ `is_dri = 0` ⇒ 光感**本来就轮询**，#43/#68 的"DRI 中断到不了"**不成立**；
>   TLMM 32 与 127 在 Linux 侧都是 `UNCLAIMED`。
> - ★ **`hw_platform = QRD` 是我们自己编出来喂 DSP 的**（主线不导出它）
>   ⇒ 这套是**高通参考设计**的配置，板级差异原在 Windows DriverData 注册表，
>   已随抹除 Windows 丢失（#37 当时只当是丢了校准精度）。
> - ⚠️ 做完实验**必须恢复**：SLPI 起回来、hexagonrpcd 用回 `/vendor` 根，
>   并且**验到框架层**（`dumpsys sensorservice`）—— SSC 通不代表 HAL 会话还在。
> - 完整案卷 `docs/stage4-findings.md` #72；工具与纪律 `scripts/ssc/README.md`。


> **★★★ Stage 6 M17（2026-08-22）：一个用户报告，一路查到上游活着的缺陷。**
> ★ **`ESR=0xf2000800` → EC=0x3C（BRK）⇒ 是 `BUG()` 断言，不是空指针。**
> 这一个数字就定了方向：去找那行 `BUG_ON`，别查内存越界。
> - ★ **救回证据的是 pstore**（45 条 efi_pstore 记录，两次事件）。
>   logcat 是内存的、`/data/anr/` 是空的、hangdump 看门狗也没命中
>   （它判 D 状态 ≥2 分钟，而机器压根没活到两分钟）。
>   Stage 0 布下的"没串口就走 EFI 变量"这条通路，**第一次在用户报告的问题上
>   付清成本**。⚠️ 读它要 root，而本机 `adb root` 前得先
>   `setprop service.adb.root 1`。
> - ★★ **与"设置"无关**：两次事件分属 `org.lineageos.updater` 与
>   `com.android.permissioncontroller`，两条不同的 ioctl 路径
>   （`dma_fence_driver_name` / `dma_fence_timeline_name`）。
>   任何 app 的 RenderThread 查 present fence 的名字都能触发
>   ⇒ **普通应用能把整台机器 panic 掉**。
> - ⚠️★ **我的第一个假说是"7.2-rc2 缺保护、后来修了"** —— 顺，但错。
>   救我的是没停在假说上：用 GitHub compare API 逐个查那四个提交在不在
>   `v7.2-rc2` 里，**全是 `behind`（早就在）**；再拉 master 对比，
>   那个 `BUG_ON` 一字未改。"应该已经修了"和"确认在不在"差着一次 panic。
> - 修法 `patches/0013`：**只删那一行 `BUG_ON`**。`fence_to_crtc()` 只有两个
>   调用者，而它们本身就是那张 ops 表的成员 —— 断言的是调用路径已经保证过的
>   事，事后再查只能引入竞态。
> - ★ 顺带堵住一个长期漂移源：`patches/*.patch` **此前没有任何消费者**
>   （全靠手动 `git apply`，本会话刚为此付过一次代价）。新增
>   `scripts/kernel-apply-patches.sh`——只打内核那几个、幂等、失败非零退出。
> - ⚠️ `Handover signaled` 噪声在崩溃日志里刷了 163 行，**把 pstore 的有效
>   内容挤掉了**（45 条里真正有用的不到 10 条）。它本身无害（#37 已证），
>   但它损害取证能力，值得单独治。

**上一阶段：Stage 6 M16 — ★★★★★ s2idle 两端都修好并已发版（v0.3.0-alpha，构建戳 `1787335922`）：Android 真实挂起/唤醒 ×4 零复位，救援 Ubuntu `systemctl suspend` 3/3。真凶是我们自己 Stage 2 加的 `dr_mode="otg"`——`a600000.usb` 的 role 停在 `device`（无 gadget、无 xhci），断电即整板复位；Android 的修法是息屏切 `host`、亮屏切回 `device`（代价：息屏时 USB adb 断，TCP adb 不受影响）。README/TODO/设备树注释已同步到这个事实**

> **★★ M17 补记（构建 + 上机）**：`kernel-apply-patches.sh` **第一次跑就拦下
> 两处真实回归** —— `patches/0009`（CPU cooling maps）从构建机内核树上掉了
> （没拦住的话新内核会**悄悄失去 CPU 温控降频**，无风扇平板会一路满频跑到
> 紧急关机，症状要等某次长时间游戏后突然关机才出现）；上游 Venus 补丁集
> **也只活在构建机工作区**，`sc8280xp.dtsi` 被回退过，`patches/0011` 依赖的
> `venus` label 没了 ⇒ **从干净的 v7.2-rc2 照本仓 `patches/` 根本重建不出
> 发版内核**。已入库 `patches/upstream-venus/`（8 个原样保存，0014 故意不用）。
> ⚠️ 其中 `0019` 需要 `patch --fuzz=3`，脚本会**明确打印用了 fuzz** ——
> 静默的模糊匹配是灾难的开始。
> - ⚠️★ **`patches/0014` 被自己的实测否掉**：`dev_err_ratelimited` 只抑制
>   **62%**（事件 5.05 Hz → 打印 1.95/s，正好是默认的 10 条/5 秒预算），
>   而它的立项理由是"别让噪声把 panic 栈挤出 pstore"，2/s 一小时仍七千行。
>   已改成 `DEFINE_RATELIMIT_STATE(60 * HZ, 1)`。
>   ★ **"用了标准做法"不等于"达成了目标"**，差一次测量就会把 62% 当成完成。
> - ⚠️★ 新坑：**`BOARD_PREBUILT_DTBIMAGE_DIR` 会把目录里所有 `*.dtb` 拼接**。
>   陈旧残留让 boot.img 带了两份 DTB（346052 = 恰好 2×173026），
>   **构建全程不报一声，唯一线索是大小是整数倍**。已在 `release.sh` 做成断言
>   （数 `d00dfeed`，≠1 就 die），并**拿真样本正反验过**（boot_a→1、boot_b→2）。
> - 运维：直连构建机只有 1.32 MB/s，**R2 中转 41 MB/s**，把 14 分钟计费空转
>   压成约 2 分钟。⚠️ 这个版本的 `update_engine_client` **没有 `--status`**，
>   判断装完看 `bootctl get-active-boot-slot` 是否切槽。

> **★ Stage 6 M16（2026-08-22）：发版 v0.3.0-alpha + 一次全仓文档对账。**
> 发布物就是**在硬件上验过的那一版**（`--no-build`，构建戳 `1787335922`）；
> R2 五条路径全 200，`release.sh` 三条断言全过。
> ★ 随后做了一次**文档与现实的对账**，结果值得记：**公开 README 仍在首屏写着
> "The machine cannot suspend"**，而那正是这一版的头号卖点；zh-CN 更落后一整版
> （耳机口还是 ❌ 且写着**已被 #40 推翻**的旧诊断、Venus 还是 ❌、
> 招募项还在说 `CONFIG_QCOM_FASTRPC` 是 `=m`——M13 早就实测 `=y`/0 模块）。
> 设备树注释同样：`init.gaokun3.rc` 整段还在讲"机器不真的睡"，
> `usbrole.rc` 与 `device.mk` 一个说"默认不启用"、一个默认设成 1，自相矛盾。
> ⚠️★ **教训：发版说明写了不等于文档更新了。** 首屏状态、状态表、招募项、
> 以及**代码里的成段注释**都是独立的副本，每一份都会各自变质。
> ★ 下次发版的收尾清单里必须有一条"grep 一遍旧结论的关键词"
> （`cannot suspend` / `不能待机` / `=m` / `❌`）。
> - 顺带修掉两处会误导人的产物/文档不一致：`ota/gaokun3.json` 的仓库副本
>   还是 `createjson.sh` 生成的 **sourceforge** 下载地址（线上那份是对的，
>   `release.sh` 在上传时重写；但仓库里留着假地址会让人以为发错了），
>   已改成与线上**逐字节相同**；`INSTALL.md` 说"v0.3 起带 `recovery-ramdisk.img`"
>   —— **没有任何一版发过它**（recovery 还在复位循环），已改成实话。

> **★★ Stage 6 M15（2026-08-21）：s2idle 从"内核缺陷、无从下手"变成两个可定位的问题。**
>
> ★★★**最重要的一条：复位发生在【挂起进入】，不是唤醒。**
> 把 RTC 闹钟从 40 秒改成 **180 秒**，机器仍在几秒内复位（adb 27 秒就回来）。
> ⇒ **M4/#47 那句"挂起成功、死在任何唤醒"是错的**，也正因为这个错误方向，
> 之前十几轮"解绑某某再试"必然全部无效。
> - ★ **`pm_test` 平反了**：#49 判它"本平台无效判据"，理由是"它自己就会复位"
>   —— **那个理由本身就是结论**。同一次开机跑：`pm_test=freezer` **rc=0 活着**、
>   `pm_test=devices` **复位** ⇒ 故障夹在 `dpm_suspend_start()` +
>   `dpm_suspend_noirq()` 之内。而且它 5 秒自动返回、**不需要唤醒源**，
>   是目前最安全最快的复现器。
> - ★★ **唯一出现过成功的配置是 `pcie_aspm=off` + `nvme_core.default_ps_max_latency_us=0`**
>   （两个都必需；**光去掉我们自己加的 `pcie_aspm.policy=powersupersave` 不够**）。
>   正式测量：**基线约 16 次开机 0 次通过；该组合 5 次开机有 3 次通过，
>   其中一轮 `pm_test=devices` 连过 10/10。** 差异是真的（p≈0.002）。
>   ⚠️ **但不是确定性修复** —— 同一条 cmdline 另有 2 次开机第 1 次就死。
>   ★ 而**同一次开机内行为高度一致**（要么连过 10 次、要么第 1 次就死）
>   ⇒ 决定性因素是**开机时确定下来的状态**，不是每次挂起的随机性。**这一层未查。**
>   ⚠️ 因此**暂不把它写进发版 cmdline**：那会让待机变成"有时能睡、有时炸"，
>   比确定不能睡更糟。
> - ★ 该组合下唯一那次真实挂起**睡进去了但没醒** ⇒ **"醒不回来"是另一个
>   独立问题**，被"挂起时复位"盖了整整两轮。
> - ⚠️★★ **本轮最贵的教训是关于判据的**：这个故障的**单次失败率约 93%**，
>   所以"改一次、试一次、炸了"对任何配置都是大概率事件，**几乎没有证据力**。
>   我因此被 #39/#43（同一 cmdline 一正一反）带偏两轮。
>   正确做法 = **每次开机把 `pm_test=devices` 跑到失败或跑满 10 次，只数 `rc=0`**。
>   ⚠️ 工具本身也有坑：`pm_test` 周期后会留 pending 唤醒事件，紧接着再挂起
>   会**立刻 `-EBUSY` 返回**；第一版脚本把那个非零返回当成"通过"，
>   于是报出假的 10/10。判据是**耗时**（真实周期 5–8 秒，`-EBUSY` 是 0–1 秒）。
> - ★**一整批假说被实测否掉**（都不是推理）：EC 的 PM 回调 / EC 的中断 /
>   **EC 本身（解绑）** / Venus / geni I²C 的 noirq 重构 / PDC 唤醒映射 /
>   **我们全部 20 个 buildbot 补丁**（零补丁 v7.2-rc2 同样复位）/
>   cpuidle 深空闲态 / 硬件看门狗 / 音频+USB+三个 remoteproc 全解绑。
> - ⚠️★ **#49 的两行数据不可信**：那张表里"纯 7.2"的行与本轮受控重做
>   （`git checkout v7.2-rc2` + 我们自己的 .config）**结果不符**。
>   ★ 教训：对照实验的两格若**失败模式不同**（一个干净 abort、一个硬复位），
>   它们比较的就不是同一件事。
> - ⚠️★ **本轮让用户按了三次电源键**，是我的纪律问题。已定规矩：
>   **"醒不回来"没解决之前不跑真实挂起**（`pm_test=devices` 够用且安全）；
>   实验条目加 **`panic=10`** 配合已开启的 `CONFIG_DPM_WATCHDOG`；
>   改 DTB/引导链前先明说可能要按电源键。
> - ★ 运维：让脚本**只准备不挂起**（"stay 模式"），机器停在救援 Ubuntu 且
>   **ssh 可达**，一次开机能连做多个实验。⚠️ 救援机 IP 会漂
>   （本轮 `.230`，Android 在 `.46`），用"ping 全网段填 ARP → 逐个取 hostname"找。
> - 完整案卷：`docs/stage4-findings.md` #50 / #51。
>
> **★★ Stage 6 M14（2026-08-21）：耳机口 + Venus 内核侧 + 一条运维能力翻案。**
> 新 ROM 经**真实 OTA 通路**（update_engine + postinstall + boot_control）装机验收。
> **设备现在跑的是 slot_a、构建戳 `1787247612`、内核 `#19`，
> 与 R2 上待发版的那一组产物完全同源。**
> 四条音频通路**开机即用、零手工步骤**实测：耳机 PCM0 `RUNNING`、
> 扬声器 PCM1 `RUNNING`、内置麦 PCM3 与耳机麦 PCM2 各录到 384000 帧。
>
> ★**耳机口不出声，真凶是 RX 插值器链从来没接上** —— 不是"后端打不开"。
> 我原先只设了 `RX_MACRO RXn MUX` 就以为通路成立，实际 rx-macro 内部还有一级
> 插值器停在默认值（`RX INTn_1 MIX1 INP0 = ZERO`、`RX INTn DEM MUX =
> NORMAL_DSM_OUT`、`CLSH/LO Switch = Off`）。DAPM 路径不完整 → 后端 DAI 拿不到
> 有效通路 → PCM open 失败，而**内核不为此打任何日志**，所以看起来像后端坏了。
> ⚠️ 我在 #40 里给的三个候选（拓扑缺 APM 图 / soundwire 没上电 / q6apm 静默失败）
> **一个都不是**，案卷保留了它是怎么错的。
> 补齐 9 个控件后实测：`tinyplay -D 0 -d 0` rc=0、`pcm0p` `state: RUNNING`、
> **hw_ptr 2 秒前进 97920 帧 = 48960 帧/秒**（正好实时 48 kHz，证明 DMA 在真实
> 消耗数据而不是空转）。**新镜像开机自动应用，无需手工**。
> - 另外两处阻塞：策略里压根没声明耳机设备（已加 `WIRED_HEADPHONE` /
>   `WIRED_HEADSET` / `IN_WIRED_HEADSET`，地址 `CARD_0_DEV_0` / `_2`）；
>   框架 `WiredAccessoryManager` 找的是 `/sys/class/switch/h2w`，**本机 ENOENT**
>   （主线没有 h2w 驱动），开关是框架资源 `config_useDevInputEventForAudioJack`。
>   那个 evdev 插孔设备**本来就在、而且已经在被读**（`Classes: KEYBOARD | SWITCH`）。
> - ★**配方来源：救援 Ubuntu 上的上游 ALSA UCM2** ——
>   `sc8280xp.conf` 用 `Regex "HUAWEI.*MateBook E.*"` 把本机直接 include 成
>   `LENOVO-X13s.conf`。**上游本来就把华为 MateBook E 与 ThinkPad X13s 视为同一套。**
>   ★ 方法论：本机凡是 LPASS 音频的事，先去抄那个目录，别自己推 DAPM 图。
> - ★ HPH 音量的方向**从内核算，不猜**：`wcd938x.c:2620`
>   `SOC_SINGLE_TLV(..., 0x18, 1, line_gain)` + `:192 DB_SCALE(-3000, 150, 0)`
>   → 控件值 v = −30+1.5v dB。**默认 24 = +6 dB**（满增益直接进耳朵），
>   上游的 2 = −27 dB。交叉验证同配方里 `ADC2 Volume 10` = +15 dB 麦增益，合理。
> - **仍需用户做一件事**：插一次耳机听。框架资源只能构建期 overlay，
>   `WiredAccessoryManager` 在 SystemServer 启动时只读一次，框架层也没有
>   插拔模拟命令（`cmd audio help` 里没有 device/connect/jack）。
>
> ★**Venus 硬件视频编解码：内核这一半打通了**（据我们所知 sc8280xp 首次）。
> `/dev/video0` = `qcom-venus-decoder`、`/dev/video1` = `qcom-venus-encoder`，
> `aa00000.video-codec` 绑 `qcom-venus`、`abf0000.clock-controller` 绑
> `sm8350-videocc`，供应方（videocc / IOMMU / rpmhpd / 4 条 interconnect）
> 全部解析，延迟 probe 队列空，**固件加载失败 0 行**。
> - ★**固件我们一直在装，只是名字骗了我**：`firmware-name` 指向
>   `qcvss8280.mbn`，而 README 里那一行当初被我标成"语音服务（未用到）"
>   —— **VSS = Video SubSystem，不是 Voice**。
> - ★**时钟控制器主线已有**：`videocc-sm8350.c` 自己就认 `qcom,sc8280xp-videocc`。
> - 8 个上游补丁打 7 个（0014 纯格式清理、主线已分叉，跳过）。
>   compatible 选 `sc8280xp` 而非 `sm8350`：两个资源结构只差 `freq_tbl`，
>   `sm8350_res` 借用 sm8250 的表，`sc8280xp_res` 才是本 SoC 自己的。
> - ⚠️★ **必须关 `CONFIG_VIDEO_QCOM_IRIS`**，否则 Venus 编不过而**报错完全
>   看不出跟它有关**（`sm8350_reg_preset` / `VPU_VERSION_IRIS2` undeclared，
>   报在 core.c，像补丁打错）。主线 v7.2 用 `#if !IS_ENABLED(IRIS)` 把本机需要
>   的资源全编掉了，而 iris 的 of_match 里**没有 sc8280xp**，永远服务不了本机。
> - ⚠️★ 又是"=m 坑"，这次整条链上**五个**（`MEDIA_SUPPORT` / `VIDEO_DEV` /
>   `VIDEOBUF2_DMA_CONTIG` / `V4L2_MEM2MEM_DEV` / `SM_VIDEOCC_8350`）。
>   MUST_Y 断言 44 → 52 条。
> - **还没做的另一半**：Android 需要一个跟 V4L2 说话的 Codec2 组件，
>   66 个解码器仍全是软解。★ `external/v4l2_codec2` **本来就在 manifest 里**。
>   ⚠️ poolmask 不能照抄它 README 的 `0xf50000`（那是 ION，本机**没有 ION**），
>   要用 BLOB 的 `0xfc0000`。
>
> ★★**推翻 M4/M6：Android 上【能】写 EFI 变量。** cmdline 里确实有
> `efi=noruntime`，但 `mount -t efivarfs` 挂得上、78 个变量读得出、
> `LoaderEntryOneShot` **写得进且回读正确**（属性 `0x07` + UTF-16LE + 双 NUL；
> 覆盖已存在的变量前要 `chattr -i`）。
> ★ 机制**我们 Stage 0 就记下来了**（`hw-inventory.md` 第 8 节）：本机 EFI 变量走
> 高通 TrustZone 的 `uefisecapp` 后端、不依赖 EFI 运行时服务，所以 `efi=noruntime`
> 本来就不影响读写。M4/M6 那个判断与本仓自己的记录是矛盾的 ——
> **跨阶段的结论要回头对一遍旧案卷**，否则会重新发明一个错误。
> **意义：Android 自己就能安排"下次进救援系统"，而且是 oneshot，失败自动回落。**
> 本轮就靠它安全地试了 Venus 内核和新槽位 —— `default` 全程保持已知可用的那个。
> - ⚠️ 顺带查明：**boot_control HAL 每次启动都把 `default` 改成当前槽位**，
>   于是 `INSTALL.md` 承诺的"挂死→拍电源键→自动回落到可远程接入的系统"
>   **在首次进 Android 之后就静默失效了**。症状只是"`adb reboot` 本想去 Ubuntu
>   却又回到 Android"。已记 TODO D4，正解是让 HAL 只管槽位、`default` 永远留救援。
>
> ★**死锁取证看门狗**（#38 用户报过但我们从未复现）。现实是死锁时用户只会重启、
> 证据就没了，所以证据必须自动留下。探针刻意做得很便宜：**只读 `/proc` 线程状态、
> 不跑 dumpsys**，60 秒一次；判据是**同一个 tid 连续三次采样都在 D**（≥2 分钟），
> 不是"出现过 D"。命中后写 stack/wchan/QRTR 服务表/PCM 状态/binder 日志/logcat
> 到 `/data/vendor/gaokun3/hangdump-<uptime>/`，每次启动只采一份。
>
> ★**光感收敛到一处嫌疑**（#43）。逐字段对照能用的加速度计：
> ❌ 排除"DSP 够不到 PMIC 轨"（两者用**同一条** `/pmic/client/sensor_vddio`）；
> ❌ "SLPI 用不了主 SoC TLMM 脚"也站不住（加速度计同样 `irq_is_chip_pin=1`）；
> ★ 收敛到 **SLPI 侧 I2C 实例号 1（能用）vs 5（不通）**，
> 这也正好解释"启用光感会污染整个 SSC 会话"。
>
> ⚠️**三个自己造成的坑，都值得记**：
> 1. `boot_control/main.cpp` 删函数时留下多余花括号 → 构建 3 分钟就失败。
>    本地编不了 Android 目标，至少要过一遍**花括号平衡体检**。
> 2. ★**`make ... | tail -30` 之后取 `$?` 拿到的是 tail 的退出码** ——
>    第一次内核构建报 `KBUILD_RC=0` 而 `drivers/media` 其实失败了。
>    判据要看**产物时间戳**（`Image` 还停在旧的）。本仓在 az CLI 上记过同一个坑。
> 3. ★**"toybox mount 要求 /etc/fstab"是错的** —— 干净 A/B（root 身份、把 fstab
>    移走）证明 efivarfs 与 vfat 都照样 rc=0。真因是**非 root 挂载**时 toybox 会去
>    查 fstab。我当时同时改了两个变量（造 fstab + 重新拿 root），所以归错了因。
>    ⚠️ `adb root` 每次重启都要重做。
>
> ⚠️**`update_engine` 拒绝在 overlayfs（`adb remount`）生效时应用 OTA**
> —— `ErrorCode::kOverlayfsenabledError (64)`。要先 `adb enable-verity`
> （会报 `boot_b does not look like a vbmeta footer`，无害）**并重启**才生效。
>
> ★**内置麦克风此前【完全是断的】，而且谁都没发现**（#40 末尾）。
> 策略里 `Built-In Mic` 指的是 `CARD_0_DEV_3`，而那个 PCM 压根打不开
> —— 和耳机同一类：前端混音器没接（`MultiMedia4 Mixer VA_CODEC_DMA_TX_0` = Off）
> 且 va-macro 的 DMIC 序列一条没设。补齐后安静房间 **RMS −30.5 dBFS**（健康电平）。
> ⚠️ **两个采集 PCM 都只支持双声道**，传 `-c 1` 会得到 `cannot set hw params`
> —— 我因此一度以为耳机麦也坏了，其实它一直是好的。
>
> ⚠️★**#44 是一次我自己的误判，值得记**：装 OTA 时看到 10 KB/s + 45% 丢包 +
> dmesg 刷 `ath11k_pci: msdu_done bit in attention is not set`，我就断定是
> ath11k 的负载触发缺陷 —— 时间相关性极诱人（25 条全落在下载那几分钟，
> 开机到下载前一条都没有，且是唯一一种 ath11k 报错）。**三个实验推翻了它**：
> 空闲 ping 仍 40% 丢包但 msdu_done 新增 **0**；27.5 MB 小下载 881 KB/s、
> msdu_done **0**；★**ping 网关 0% 丢包**（1400 字节大包也是）；
> ★★**从本机拉 200 MB 跑到 61.7 MB/s、msdu_done 仍是 0**。
> → **WiFi 与 ath11k 都没问题**；丢包只在到 1.1.1.1 的 WAN 路径上（多半 ICMP 限速）。
> 真正剩下的问题是**设备的 WAN 吞吐只有 PC 的 1/20**（1–2 MB/s vs 36.9 MB/s，
> 同一网络同一 URL），**原因未定**，别再往 ath11k 上记。
> ★ 方法论：**"同时出现"不等于"因果"** —— 只要多做一步"到网关 ping"
> 和"LAN 大流量"，结论就整个反过来。
>
> ★ 顺带得到一条很实用的运维手段：**`update_engine` 支持 `file://`**。
> 在这台没有 recovery、没有 sideload 的机器上，
> **R2 → 本机 → USB → 设备**再 `--payload=file:///data/local/tmp/payload.bin`
> 是最快也最可控的装机路径 —— 1.1 GB 的包 **76 秒**装完
> （让设备自己走 WiFi 那次要二十多分钟）。五条链路的实测速度见 #44。
>
> **发版已备好但【未发布】**：产物在 R2 的
> `staging/crDroidAndroid-16.0-20260820-gaokun3-v12.11/`
> （`super.img.zst` / `boot.img` / OTA zip / sha256 / gaokun3.json），
> **`ota/gaokun3.json` 清单一字未动**，所以没有任何用户会收到。
> 发布是对外动作，等用户定；发版说明草稿在 `scratchpad/relnotes-030.md`。
> ★ 发布用 `scripts/release.sh`（TODO B5 已做，本轮端到端跑通）。
> ⚠️ 发那一版要带 **`--no-build`** —— build stamp 每次构建都变，重跑构建
> 发出去的就不是验过的那一版。我今晚正是这么踩的：设备上验了 `1787246871`，
> `release.sh` 又构建一次，staging 里成了 `1787247612`；
> 收尾是把 `1787247612` 也装到机器上重验了一遍。

> **★★ Stage 6 M13（2026-08-20）：按 Android 分区规范把内核纳入 OTA。**
>
> 用户明确要求"遵守 Android 分区规范"，所以不再让内核留在 OTA 之外。
> **终局是自研 EFI 加载器**（读 misc 选槽 + 解析 boot 镜像），本轮完成前两段。
> - ★**内核 `CONFIG_QCOM_FASTRPC=y` 实机验证**：`#18` 内核起来后
>   `/dev/fastrpc-*` 四个节点都在而 `/proc/modules` 是 **0 行** ——
>   不再需要 insmod，第 13 个「=m 坑」关闭。
> - ★**`CONFIG_EFI_ZBOOT`**：`Image` 39,422,464 → `vmlinuz.efi` **14,021,120**
>   字节（37→13 MB，2.8 分之一），`PE32+ EFI application`，
>   **经 systemd-boot 实机启动成功**。这是能在 300 MiB 的 ESP 上放两份内核的前提。
> - ★**真 `boot_a`/`boot_b`**（各 64 MiB，nvme0n1p5/p6），`boot` 进
>   `AB_OTA_PARTITIONS`，`TARGET_NO_KERNEL := false` + header **v2**
>   （一个分区装齐 kernel+ramdisk+dtb；v3+ 会强制引入 vendor_boot）。
>   构建日志实证：`--partition_names boot:product:system:system_ext:vendor`、
>   `Using generator FullUpdateGenerator() for partition boot`。
>   空间来自**删除 userdata-old**（用户授权），余 63.9 GiB 未分配；GPT 已备份。
> - ★**ESP 改成每槽独立目录** `slot_a/` `slot_b/`，两个 BLS 条目各指自己的目录
>   —— 于是 postinstall 只写"刚刷好的那个槽"，**绝不碰正在运行的内核**，
>   回滚天然安全。实机启动验证通过。
> - ★**过渡期的那一步**：`gaokun3-bootimg-extract`（include 树内 `<bootimg.h>`，
>   不手抄偏移）+ `gaokun3-ota-postinstall.sh`。argv[1] 是目标槽位整数 ——
>   出处 `postinstall_runner_action.cc:355-357`，不是猜的。
>   ⚠️ `POSTINSTALL_OPTIONAL=false` 是故意的：宁可 OTA 失败，也不能出现
>   "新 system + 旧内核"。
> - ★**两个解包实现交叉校验**（设备上的 C++ 与安装器里的 python）：
>   同一个 boot.img，三个文件 sha256 逐个一致。偏移抄错的典型后果是
>   "magic 通过但内容错位"，只有对校验和才看得出来。
>
> **本轮拆掉/发现的坑（都值得记）**：
> - ⚠️★ `boot_control` HAL 里的 `EnsureSlotProbeHints()` **已删除**。它把
>   `by-name/boot_a|b` symlink 到 `/dev/null` 好让 libboot_control 数出 2 槽；
>   现在那两条路径是真实分区，一旦它抢在 ueventd 之前跑成功，
>   update_engine 就会把 boot 镜像写进 `/dev/null`，**静默毁掉整个更新**。
> - ⚠️★ `deploy-from-ubuntu.sh` 的 `.bak-prev` 备份**已去掉** —— 那正是把
>   296 MiB 的 ESP 吃到 95%（63 MB 全是备份）的元凶，再部署一次就会写出被
>   截断的内核。**现在"另一个槽就是备份"**，这才是 A/B 的意义。
> - ⚠️★ 变量名是 **`BOARD_INCLUDE_DTB_IN_BOOTIMG`**（BOOTIMG 连写）。写错不会被
>   忽略，而是在 **lunch 阶段**炸成 `Don't have a product spec for:
>   'lineage_gaokun3'` —— 极易误判成 manifest 没拉全设备树。
>   ★**判据**：grep 一个变量名**返回空**本身就是信号。
> - ⚠️★ 必须用 `TARGET_PREBUILT_KERNEL`。Lineage 的 `kernel.mk:171-176` 另有一条
>   "扫 PRODUCT_COPY_FILES 里 dest=kernel"的分支，但那段写的是
>   `$(ifeq kernel,$(_dest), ...)` —— **`ifeq` 不是 make 函数**，整个展开成空，
>   那条路根本不生效（Lineage 自己的 bug）。设了它之后再用
>   `PRODUCT_COPY_FILES` 拷一份会撞 `overriding commands for target .../kernel`。
> - ⚠️★ `PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false`。有了真 boot.img
>   这道检查才生效，而它不认识主线 7.2：`No kernel entry found for kernel
>   version 7.2 at kernel FCM version 202504`（Minimum LTS 6.12.0），连
>   `7.2.0-rc2-gaokun3+` 都解析不了。
> - ⚠️★ `BOARD_KERNEL_CMDLINE` **早已与实机 BLS 条目漂移**（此前它没有消费者，
>   漂了没人发现）：原写 `deferred_probe_timeout=30` 而实际是 10，且缺
>   `veritymode`/`flash.locked`/`verifiedbootstate`/`iommu.*` 四项。已对齐。
>   **教训：没有消费者的配置一定会漂。**
> - ⚠️ 编内核**两处**都要 `CROSS_COMPILE=aarch64-linux-gnu-`：编译时不带会在
>   `prepare0` 报 `gcc: unrecognized command-line option '-mlittle-endian'`；
>   `olddefconfig` 不带会用**宿主 gcc** 评估 `CC_HAS_*`，静默改掉一批符号。
> - ⚠️ `bacon` **不重建 super.img** —— 发布安装包要单独 `m superimage`。
> - ⚠️★ `scripts/kernel-config-android.sh` 里有 5 项（`PM_WAKELOCKS`/`CPUSETS_V1`/
>   `MEMCG_V1`/`UCLAMP_TASK(_GROUP)`）**一直没被应用**：它们写在续行里，而前面
>   是**普通 `#` 注释** —— shell 里注释会终止续行，命令提前结束，只留一句
>   `--enable: command not found`。现有机器没出问题纯属侥幸（.config 里早就有）；
>   新树上会重现 cgroup v1 开机失败。已拆成独立命令并补进断言（37→44 项）。
>
> ★★**新 ROM 已部署并验收**（`ro.build.date = Thu Aug 20 11:06:26 UTC 2026`）：
> `bash scripts/verify-kernel-ota.sh` **13 项全过 / 0 失败** —— 内核、
> FASTRPC=y（4 个节点 / 0 模块）、boot_a 的 Android 镜像、bootctl 2 槽、
> 解包器与 postinstall 钩子、ESP 两槽文件齐、以及★**传感器开机自启**
> （`hexagonrpcd` 由镜像拉起，框架里 Z≈9.88，`gaokun3-ssc-test` 3 秒 151 条）。
> **`scripts/sensors-up-android.sh` 那套手动步骤从此不需要了。**
>
> ⚠️★**运维：不要让构建机干等大文件传输 —— 用 R2 中转。** 本轮直连
> 构建机→救援机只有 1 MB/s（2.7 GB 要 45 分钟，而机器按分钟计费）；
> 改成 zstd 压到 1.2 GB → 上传 R2 **27 秒（43 MB/s）** → 停机 → 本机拉 →
> 局域网推到救援机 **51 秒（23 MB/s）**。R2 出站免费。
> - ⚠️★ **别在上传前去探对象**：我先 curl 了一次那个还不存在的路径，
>   Cloudflare 把 **404 缓存住了**，上传成功后照样 404，
>   加个 `?cb=…` 才拿得到。差一点误判成"上传失败"。
> - ⚠️★ **沙箱代理会掐断到构建机的 ssh**：连续几次 `Connection closed by
>   ...port 22`，一度以为机器挂了（Azure 查询显示 running）。
>   同一条命令绕开沙箱就通 —— 长任务与大流量的 ssh 一律绕沙箱。
> - ⚠️ 家里到 Cloudflare 只有 **71 KB/s**（救援机 WiFi 直连），
>   所以"让设备自己从 R2 拉"这条路走不通，得经本机中转。
>
> ⬜ **还欠**：端到端 OTA 实测；
> 第 3 段的 **EFI 加载器**（读 misc + 解析 boot 镜像 + 装 initrd/DTB 协议，
> 用 systemd-boot 的 `efi` 指令 chainload，起不来就选别的条目 —— 安全阀）。
> ⬜ 另记用户提议：**把救援 Ubuntu 换成/补上 Android recovery**。我的判断是
> 补充而非替代 —— recovery 也是 boot 镜像格式（systemd-boot 同样读不了），
> 而它的主要能力 `adb sideload` 已被系统内 OTA 覆盖；而 `sgdisk`/`resize2fs`/
> sshd 这些**本轮重新分区真正用到的东西** recovery 给不了。
> 合理的优化是给 Ubuntu 瘦身（最小 rootfs 1–2 GiB，现在 24.6 GiB）。

> **★★ Stage 6 M12（2026-08-20）：sensors HAL 落地，自动旋转接上。**
>
> ★**实机验证**（`dumpsys sensorservice`）：
> ```
> Active sensors: SH3001 Accelerometer (handle=0x00000001, connections=2)
> SH3001 Accelerometer: last 50 events
>      1 (ts=612.371743828, wall=17:42:46.858) -0.04, 0.05, 9.88,
> ```
> 两个消费者正是自动旋转的 `WindowOrientationListener$AccelSensorJudge`
> 与 `FaceDownDetector`。★**框架还自动融合出 Game Rotation Vector /
> Gravity / Linear Acceleration** —— 有 accel+gyro 就有，游戏体感由此可用。
> - **做法是改造 AOSP 默认实现**（`hardware/interfaces/sensors/aidl/default`），
>   不是从零写。⚠️ **必须复制而不能继承**：那个 `Sensors` 类的传感器列表在
>   构造函数体里硬加而 `mSensors` 是 private，且 `libsensorsexampleimpl` 的
>   `visibility` 只放行 `hardware/interfaces` 子包，设备树链不了。
>   改动只三处：列表收敛为 accel+gyro（★那 7 个假传感器**必须删**，留着框架会
>   以为本机有气压计/湿度计）、两个 `readEventPayload` 换成真数据（没数据时报
>   UNRELIABLE，**不编一个 9.8 出来**）、`SensorInfo` 写真名字。
> - ⚠️★**最贵的一个坑：HAL 自己的重试把传感器枚举弄坏了。** 初版在查不到
>   传感器时每 10 秒**重建整个 SSC 会话**，结果之后连独立命令行客户端都报
>   「没有传感器提供 data_type=accel」，必须重启 hexagonrpcd 才恢复。
>   干净 A/B：停掉 HAL、重建会话，独立客户端立刻又读到 Z≈9.88。
>   根因是每次重建都在 SSC 上留下一个**被丢弃的客户端** —— 与「使能光感会
>   污染会话」是同一类现象。改成**在同一个 client 上重试 FindSensor**。
> - ★**必知时序**：`registry` 服务比**物理传感器先注册**，所以 WaitForService
>   成功后立刻查 accel 会得到「没有传感器提供」，要再等约 20 秒。
> - ⚠️ **VINTF 清单是 servicemanager 开机时读的**：运行时把 fragment 丢进
>   `/vendor/etc/vintf/manifest/` 它看不见，`addService` 会返回 -3 并让
>   `CHECK_EQ` abort（症状是 HAL 静默退出、日志全空，只有 tombstone 能看出来）。
> - ⚠️ **`adb shell stop; start` 会连带断 WiFi**（网络由框架管），
>   adb over TCP 当场掉线，而且回来时 IP 会变。别当成机器挂了。
> - ⬜ **欠三样**：sepolicy（现 permissive，logcat 一串 avc denied）、
>   ~~轴向标定~~ ★**已由用户实机确认方向正确，不需要纠正**（安装矩阵全零
>   → 单位矩阵，而传感器坐标系本来就与面板一致，运气好）、
>   `CONFIG_QCOM_FASTRPC=y`（现仍 `=m`，重启后要跑
>   `scripts/sensors-up-android.sh` 手动补）。
> - ⚠️ 另记：**boot_control HAL 会把默认启动项改成当前槽位的 Android 条目**
>   （M6 的设计），所以「默认永远留救援系统」这条纪律**装了 A/B 之后已不成立**
>   —— 本轮重启就直接回到了 Android。远程救援仍可用（Android 里 adb 可达），
>   但要恢复原纪律得动那个 HAL。

> **★★ Stage 6 M11（2026-08-20）：Android 侧传感器读数打通。**
>
> ★**自研客户端一次跑通**（`device/huawei/gaokun3/ssc/`，实机 Android）：
> ```
> 传感器 accel 的 UID = 61ab5376b4a5c9aa58442ede47acd316
>   X=-0.086191 Y= 0.052672 Z= 9.883265  accuracy=3
> ```
> Z≈9.88 = 重力，accuracy **3**（最高）。这条链路 = QRTR lookup → QMI 组包
> → protobuf → SUID 查找 → 使能 → 解读数，**就是 sensors HAL 逻辑的 90%**。
> - ★**陀螺仪也通了**（静止各轴≈0 rad/s，accuracy=3）。**Linux 侧从未验证过**
>   —— 那边装的 `ssccli` 压根不支持 gyro。于是不只自动旋转，**游戏体感/陀螺仪
>   瞄准**也有了基础。
> - ★**本机传感器清单（SSC 亲口回答，不是推测）**：`accel` ✅、`gyro` ✅、
>   **`mag` ❌ 没有磁力计**（所以没有指南针）、`rotv` ❌ 未注册（多半要磁力计）、
>   `ambient_light` ❌ 污染会话。⚠️ `mag`/`rotv` 查询失败**不会**污染会话。
> - ★**协议规格 `docs/sensors-ssc-protocol.md` 已被实现验证** —— 里面的线格式
>   （7 字节 service_header + TLV 的 u16 长度前缀）、消息 ID、哨兵 UID 全对，
>   后人可以照抄不必再逆向。libssc **不能移植**（glib/gobject/libqmi），但
>   `.proto` 只有几百行、AOSP 自带 protoc → 重写便宜得多。
> - ⚠️ 两个构建坑：①`proto: { canonical_path_from_root: false }` 必须加，否则
>   生成的头带全仓库前缀、`.proto` 之间的 import 也解析不了；
>   ②**protobuf 要静态链接** —— `libprotobuf-cpp-lite.so` 只在 `/system/lib64`
>   且文件名带版本号，vendor 命名空间看不见（`tinymix`/`libtinyalsa` 同一个坑）；
>   静态版又引用 `__android_log_write`，所以还得补 `shared_libs: ["liblog"]`。
> - ⬜ **只剩 AIDL `android.hardware.sensors` HAL**。要额外处理：安装矩阵全零
>   （轴向得实机对着屏幕标定一次）、采样率/batching/flush 语义、sepolicy。

> **★★ Stage 6 M10（2026-08-20）：传感器管道在 Android 上打通。**
>
> ★**判据达成**：Android 里 `gaokun3-qrtr-lookup` 列出 **`400 1 9 13`**
> —— Snapdragon Sensor Core 服务上线，与 Linux 侧观测一致。逐段确认：
> `/dev/fastrpc-sdsp` 出现 → AOSP 编出的 `hexagonrpcd` 运行（进程停在
> `fastrpc_internal_invoke`，20 秒内 DSP 发来 **2880 行**文件请求，序列与
> Linux 上完全一致）→ SSC 在 DSP 上起来并注册服务 400。
> - ★**hexagonrpcd 上游自带完整 Android 构建支持**（Android.bp ×5，
>   `hexagonrpcd-sdsp.rc` 里 service 跑 system:system、`-R /vendor/etc/hexagonrpcd-root`）
>   —— 不用我们写 bp，工作量远小于预估。它**只依赖 libc**（3443 行 21 个文件）。
> - ★**bionic 的 `uapi/misc/fastrpc.h` 逐符号比对通过**：需要的 6 个 ioctl
>   （含关键的 `FASTRPC_IOCTL_INIT_ATTACH_SNS`）与 5 个 uapi struct 全都有。
> - ★**协议规格已整理成 `docs/sensors-ssc-protocol.md`** —— 把"逆向 libqmi +
>   libssc"变成"照规格实现"，每条都注了来源行号。最容易记错的一条：
>   **QRTR 上的 QMI 没有 QMUX 头也没有 marker 字节**，上线就是 7 字节
>   `service_header` + TLV（`qmi-endpoint-qrtr.c:547` 注释原文）。
>   **libssc 不能移植**（拖着 glib/gobject/libqmi），但 `.proto` 只有 389 行，
>   而 AOSP 自带 protoc + libprotobuf-cpp-lite → 重写比移植依赖便宜。
> - ⬜ **还缺**：QMI/protobuf 客户端（下一个可验证增量 = 独立命令行客户端，
>   对标 `ssccli`，跑通 SUID 查找→使能→打印 XYZ，那就是 HAL 逻辑的 90%）、
>   AIDL `android.hardware.sensors` HAL、sepolicy。
>   另外 `CONFIG_QCOM_FASTRPC` 仍是 `=m`（本轮靠 insmod 验证），
>   已进 `kernel-config-android.sh` 断言，**待重编内核**（第 13 次 =m 坑）。
> - ⚠️★**拆掉一颗地雷：`int-crdroid.conf` 已不可启动。** A/B 化后 fstab 四条
>   logical 行都带 `slotselect`，first-stage mount 必须靠 cmdline 的
>   `androidboot.slot_suffix` 才能把 `system` 解析成 `system_a`；两个条目的
>   kernel/dtb/initrd **完全相同**，唯一差别就是 `android-a.conf` 带
>   `slot_suffix=_a`。少了它就是 M6 那个"进 Android 就重启、pstore 全空"的坑。
>   `deploy-from-ubuntu.sh` 的默认值和本文说明都已改指 `android-a.conf`。
> - ⚠️ 更正一条自己的判断：`device_kernel_headers` **不用我们提供**，
>   AOSP 在 `build/soong/Android.bp:56` 就用 `kernel_headers` 类型定义了它。
>   我建了空壳导致构建报 `module already defined` —— 因为当时**只 grep 了
>   `device/` 目录**。判断"树里有没有这个模块"要 grep 全树。
> - ⚠️ 又被同一个转义坑咬了两次（`
` / `
` 被中间层 collapse 掉，一次把真 CR
>   写进文档、一次让生成的 python 字符串断行）。**结论固化进脚本注释：
>   生成代码时一律用 `chr()`，不写反斜杠转义。**


> **★★ Stage 6 M9（2026-08-20）：传感器翻案 —— 加速度计真的通了。**
>
> ★**推翻 M5 的"主线此路不通"**（下面那段 M5 原文保留，但结论已作废）。
> 一位贡献者给了 Linux 侧部署指南（`docs/contrib/slpi-sensors-deploy.md`），
> 在救援 Ubuntu 上逐条复现：**静止时 Z≈9.87 m/s²，15 秒 131 行读数**。
> 这一个数字验证了整条通路
> `fastrpc → hexagonrpcd 的 VFS → SLPI 上的 DSP 注册表 → SSC → QMI → libssc`。
> 关键在于**思路要换**：不是 AP 去驱动芯片（AP 确实无总线可达，M5 这半截是对的），
> 而是**AP 给 DSP 当只读文件服务器**。
> 一键复现 `scripts/slpi-sensors-setup.sh`，案卷 `docs/stage4-findings.md` #37。
> - ★**于是「自动旋转」对本机是可达的**，缺的只是 **Android 侧 sensors HAL**
>   （把 hexagonrpcd 移过去 + 拿 libssc 包一个 AIDL HAL）。README 招募项已重写
>   —— 难的那一半做完了，据我们所知其他 sc8280xp 设备都没跑通过。
> - ❌ **光感（tcs3701）不通**：使能后从不返回读数，而且**会污染整个 SSC 会话**
>   —— 之后连加速度计也读不到，必须重启 hexagonrpcd（★还要等约 20 秒沉降，
>   6 秒就读实测是 0 行；**"读不到"不等于"坏了"**）。
> - ❌ **负面结果：别用 `sscregistrygen` 预生成注册表。** 生成 142 个文件后
>   加速度计一起坏掉，挪走 141 个只留**空 registry** 当场恢复（干净 A/B）。
> - ★**"给 hexagonfs 加写入"不是小补丁**：`hexagonfs.h:34-45` 的 ops 表只有
>   close/openat/readdir/read/stat/seek，**没有 write，只读是设计使然**；
>   且 DSP 想写的那个目录是 `hfs_mkdir` 出来的虚拟目录，没有后端可写。属上游活。
> - ❌ **出厂校准永久丢失**：它存在本机 Windows 的 DriverData 里、不在任何驱动包中，
>   而本机 Windows 已抹除 → **安装矩阵全零**（libssc 退回单位矩阵）。
>   ★**但实测无害**：M12 用户确认自动旋转方向正确 —— 单位矩阵恰好与面板方向
>   一致，不需要在上层纠正。丢的只是 bias 补偿那点精度。
>   ⚠️ **给还留着 Windows 的人：先把那个 registry 目录拷出来再装系统。**
> - ⚠️ 两条我自己下错又更正的判断：①"registry 服务起不来所以光感失败"——错，
>   那份 debug 日志是在**我自己装出来的坏状态**下抓的（**诊断日志必须在已知
>   good 状态下重抓**）；②"`Handover signaled` 是 SLPI 崩溃循环"——错，良性噪声，
>   对照实验里**工作正常的加速度计同样每 12 秒 13 条**。
> - ⚠️ 环境坑：`droid-juicer` 0.4.2 会无限 `openat` 死循环把 apt 卡死 43 分钟
>   （`systemctl mask` 掉）；`libssc` 上游已删 `-Dmocking` 选项，照指南抄会报错。

> **★★ Stage 6 M5（2026-08-20）：M4 的成果全部固化进镜像 + 传感器判死
> （⚠️「判死」这半截已被上面的 M9 推翻）。**
>
> ★**用户实测：原神画质开到极高流畅可玩。** GPU 91% 时间待在最低档 270 MHz、
> 峰值 690 MHz、最高温 50 °C、零降频、`GMU 错误 0 / a6xx_recover 0 / SMMU fault 0`。
> 余量非常大。**唯一遗憾是清晰度**：渲染缓冲被钉在 1080×1728，这是原神按
> **设备白名单**给的档位，不是我们的技术限制（实测把逻辑分辨率改小，缓冲不变
> → 绝对上限而非比例）。要突破只能伪装机型，**有账号风险，留给用户决定，未做**。
>
> ★**新 super 已构建、刷入、验收通过**
> （sha256 `131c7f67…`，`ro.build.date = Aug 19 20:26:05 UTC`）。
> M4 那 4 个"只活在 overlay 里"的修复现在真在镜像里了：
> `display_settings.xml`（横屏）、`thermal-guard.sh` + `thermalguard.rc`（CPU 温控）、
> 删掉 MMAP 的音频策略。**验收判据是「overlay 里 0 个文件」** ——
> 刷 super 会连带抹掉 scratch，所以 overlay 空了还一切正常，就证明东西在镜像里。
> 只刷了 super：产物 ramdisk 与 ESP 上那份 sha256 完全相同，内核也没重编。
> - 流程：**先比两棵设备树的 md5 清单再传**。这次差异正好是预测的 3 缺 2 改，
>   且**"VM 上没有本地缺失的文件"**——确认了这条才敢整树覆盖，
>   否则不入库的华为固件会被 tar 抹掉，而且要到下次刷机声卡不注册才发现。
> - ⚠️ 两个会误报的核对坑：`grep -c mmap_no_irq_out` 得 1（那是**注释**，
>   判据要写 `grep -c 'name="mmap_no_irq_out"'`）；**`debugfs` 读得了
>   `vendor.img` 却读不了 `system.img`**（shared-blocks 去重），
>   `tinymix` 就是这样被我误报成缺失的 —— 它本来就该在 `/system/bin`。
> - **拆掉两颗地雷**：`deploy-from-ubuntu.sh` 的 `flash_boot` 一直往
>   `$ESP/Image` / `ramdisk.img` 写（**旧 AOSP 条目**的文件名），而
>   `crdroid.conf` 读的是 `Image-kb23` / `ramdisk-crdroid.img`
>   —— 跑 `all` 会"更新成功"却毫无作用，是 M3 那个坑的翻版；已改成从 BLS
>   条目里解析实际路径。另一颗是 `bootctl set-oneshot` 失败时静默，已改成炸出来。
>   > 附带查明：**`efi=noruntime` 不妨碍 `bootctl`**，oneshot 实测可写可回读，
>   > 远程救援闭环是可靠的。
>
> ★**传感器：不是缺 DTS 节点，是整套跑在 SLPI DSP 上。**
> ⚠️**"主线此路不通"这个结论已被 M9 推翻**（加速度计实测已通）——
> 下面的"AP 无总线可达"是对的，错的是由此推出"不可达"：
> 正解是 AP 给 DSP 当文件服务器，不是 AP 直接驱动芯片。
> 从本机 Windows 分区读出驱动库：没有任何 AP 侧传感器芯片驱动，只有
> `qcsensors.inf` + `qcsensorsconfigqrd8280`（里面是 `sns_*` 的 SEE 模块配置
> 和 **`libsdsprpc.dll` = Sensor DSP RPC**）。器件是 `sh3001`(IMU)、
> `tcs3701`(ams 光感+接近, I2C 0x39)、`sy3133cs`、`t1000`、`stm_lid_angle`(铰链角)，
> **全挂在 SLPI 自己的总线上，AP 够不着**。
> → ~~自动旋转、自动亮度在主线上做不了~~ **已作废，见 M9**：加速度计通了，
> 自动旋转可达（缺 Android HAL）；光感仍不通，自动亮度确实做不了。
> ★**游戏不受影响。**
> 工具 `scripts/probe-windows-sensors.sh`，完整证据 `docs/stage4-findings.md` #37。
>
> **★★ Stage 6 M7/M8（2026-08-20）：温控根治 + 120 Hz + 取消刷机后脚本。**
>
> ★**CPU 温控从 DTS 层根治**（`patches/0009`）。主线 `sc8280xp.dtsi` 全文只有
> 一个 `cooling-maps`（在 `gpu-thermal` 下），8 个 `cpuN-thermal` 只有一条
> 110 °C critical、没有任何 cooling device —— CPU 一路满频跑到内核紧急关机。
> 补丁给 8 个温区各加 85 °C passive trip 并绑本簇 cpufreq cooling device。
> 同机只换 DTB 实测：**cdev 0→1、trip 1→2**，`cpu0→cpufreq-cpu0`、
> `cpu4→cpufreq-cpu4`；反解 DTB 也核过（cooling-maps 1→10 处）。
> ★因此 `thermal-guard.sh` **从镜像撤掉** —— 不是"留着当双保险"：温区绑定后
> `cur_state` 归内核 thermal core（step_wise 持续写），用户态再写就是打架。
>
> ★**渲染解锁 120 Hz**。面板硬件模式本来就是 120（`activeMode vsyncRate=120`），
> 钉住渲染的是 `config_defaultRefreshRate = 60`
> （`frameworks/base/core/res/res/values/config.xml:5870`）。
> ⚠️**`Settings.System.peak_refresh_rate` 改不动它** —— 实测设 120 后调度器
> duration 仍是 16.67 ms；管事的是 base 不是 peak。用框架资源 overlay 设成 120，
> 实机验证 `defaultRefreshRate: 120`、**SF duration 15.67→7.33 ms**。
> 选"钉住"而非 peak=120+default=0：60/120 两个模式在**不同 mode group**，
> 之间切换是非无缝 modeset。
>
> ★**刷机后脚本 `android-post-flash.sh` 已删除**，七件事逐条有下落：
> 两条本来多余（`stay_on_while_plugged_in` 的 AOSP 默认就是 false；
> 横屏已由 `/vendor/etc/display_settings.xml` 负责）；
> 两条 adb 属性**早就在 device.mk:36**（adbd 先读 `service.` 回落 `persist.`，
> 全新刷机时 persist 那份已生效 —— 本轮实测：`service.adb.tcp.port` 为空而
> TCP adb 正常连着）；
> 三条新编进去（`def_screen_off_timeout` 120000、captive portal 两条 URL）。
> 唯一仍需人工的是"首次带密码手连一次 WiFi"（#29 的解禁条件，无法预置）。
>
> ⚠️**captive portal 的正确改法**（源码明写）：必须覆盖
> `config_captive_portal_*_url` 而**不是** `default_*` ——
> `packages/modules/NetworkStack/res/values/config.xml:10-15` 说 `default_*`
> 不可 overlay、改它 "will break if the enforcement of overlayable starts"。
> `config_*` 在该模块 `<overlayable name="NetworkStackConfig">` 里（policy
> product|system|vendor），且它在 `res/values/config.xml:41` **定义为空串**，
> `NetworkMonitor.java:2671-2674` 非空则用、否则回落 `default_*`。
> ★**不新建 RRO**：树里已有 `vendor/lineage/overlay/rro_packages/
> NetworkStackOverlay`，打包/manifest/targetName 都对，只需往它 res 里加资源；
> 但**必须配 `<add-resource>`** —— 构建期 overlay 只能覆盖目标资源表里已存在
> 的条目，那个 RRO 自己只定义了 `config_dhcp_client_hostname`。
> 实机验证：探测日志里实际用的是 `connect.rom.miui.com/generate_204`，
> `isValidated=true`。（`config_captive_portal_fallback_urls` 未覆盖，
> 日志里还能看到一次 `play.googleapis.com`，待办。）
>
> ⚠️**两个自己造成的构建失败，都值得记**：
> 1. `<add-resource>` 缺失 → `error: resource ... does not override an
>    existing resource`。构建期 overlay ≠ 能新增资源。
> 2. ★**XML 注释里不能有 `--`**。我在注释里原样引用 aapt2 的提示
>    `--auto-add-overlay`，于是 `xml parser error: not well-formed (invalid
>    token)` 且报在 **line 0**，极具迷惑性。现在所有 overlay XML 都过一遍
>    解析体检。
>
> ⚠️**GPU SMMU 中断：查清了但没动**。实际 DT 是全局 672/673、context bank
> 从 **678** 起（678=CB0 … 689=CB11）。对照 D6 观测（硬件拉 675/680）：
> **680 声明了但被分给 CB2，675 整张表里根本没有**（674–677 是空的），
> 很像 CB 起始偏移就错了。但只凭"675/680 挂起"推不出正确映射
> （若 CB0=675 则 680 会是 CB5，而实现的 CB 只有 2 个，自相矛盾）。
> **改错了没有任何征兆，只是继续收不到 fault**，故未改。
>
> ⚠️**运维两条**：
> * **`gaokun3-rescue.local` 在 git-bash 的 ssh 里解析不了**（MSYS 走 Cygwin
>   解析器、不做 mDNS；Windows 自己的 `ping` 能解）。可靠做法是**按 hostname
>   扫网段**：ping 全段填 ARP，再逐个 ssh 取 `hostname` 比对。
> * **USB adb 会掉**（#27），而且 IP 会漂。本轮就靠**扫 5555 端口 + 比对
>   `ro.crdroid.device`** 找回设备。这正是把 `persist.adb.tcp.port=5555`
>   编进镜像的价值 —— 实测它救了场。
>
> **★★ Stage 6 M6（2026-08-20）：公开发版 + A/B OTA 打通。**
>
> ★**系统内 OTA 已接通并实机验证**（截图为证：「系统更新」页显示
> 「由 vahiru 维护」、「上次检查 11:02」、「暂无可用更新」）。
> 托管在 **Cloudflare R2 + 自定义域 `ota.072172.xyz`**（桶 `gaokun-android`）。
> 布局：`ota/gaokun3.json`（清单）、`builds/<zip>`、`install/<ver>/…`。
> - crDroid 的 `org.lineageos.updater` 本来就装着，**只差把它指向我们的清单**。
>   接线只有一条路：`R.string.updater_server_url`，**没有属性可覆盖**
>   （`UpdatesNetworkDataSource.kt:20`）。而且必须是【构建期】overlay ——
>   Updater 没为这些字符串声明 `<overlayable>`，运行时 RRO 会被拒。
>   overlay 路径要镜像模块的 `resource_dirs`：
>   `overlay/packages/apps/Updater/app/src/main/res/`。
> - ⚠️★**抓清单的客户端设了 `.followRedirects(false)`**，任何 3xx 直接失败。
>   实测（从**设备**测，沙箱自己挡出站 HTTP，那边的结论不可信）：
>   `github.com/…/raw/…` = **302 会挂**；`raw.githubusercontent.com` 与
>   R2 自定义域 = 200。它还 `require()` 强制 https。
>   **下载链接反而可以重定向**（走 `HttpURLConnectionClient`，默认跟随）。
>   ⚠️ 别在这个主机名上加 Redirect/Page Rule —— 报错只有
>   `Unexpected HTTP status: 301`，绝对想不到是那条规则。
> - ⚠️ 不用桶自带的 `pub-*.r2.dev`：Cloudflare 明确说它是开发用途、限速、
>   不走完整缓存。**但自定义域也没缓存住 1 GB 的 zip**
>   （实测 `cf-cache-status: BYPASS`，免费版单文件缓存上限 512 MB）——
>   好在 **R2 出站免费**，成本没问题，只是没有边缘加速。
> - ★**清单的 `timestamp` 必须等于该构建的 `ro.build.date.utc`**。
>   `createjson.sh` 有时填的是【打包时刻】（`bacon` 之后又跑 `m` 就会错开），
>   那会让装上此版本后 Updater **永远显示"有更新"**，而那就是同一个版本。
>   判据在 `UpdatesRepository.kt:113-115`（`==` 视为当前版本、`<` 视为更旧）。
> - 上传用 `scripts/r2-upload.py`（纯标准库、凭据只从环境变量读）。
>   **云到云传**：构建机 → R2，2.4 GiB 用 66 秒；从本机传要 8 分钟。
>   只把 S3 密钥送上构建机，**绝不送账户级 API 令牌**（后者能改 DNS、删桶）。
>   **顺序：产物先传，清单最后传**，否则中间清单会指向不存在的文件。
> - ⚠️★**救援系统的 IP 会漂**（DHCP，实测 .230 → .54，害我死等了 7 分钟）。
>   **avahi 在跑，用 `gaokun3-rescue.local`**，别再写死 IP。
>
> **仓库已公开**：https://github.com/vahiru/gaokun-android
> （★2026-08-20 起改为 **GPL-3.0-or-later**；发版当时是 Apache-2.0）。
> Release **v0.1.0-alpha** 含全新安装产物（`super.img.zst` / `Image` / dtb /
> `ramdisk.img` + sha256 清单）与 A/B 更新包
> （`crDroidAndroid-16.0-20260820-gaokun3-v12.11.zip`）+ `gaokun3.json`。
> - 公开前**改写了历史**（用户选的）：`git filter-branch` 抹掉构建机静态公网 IP
>   与家里 WiFi 的 SSID，77 个提交全改。**验证要点：`--all` 会把
>   `refs/original/*` 和 `refs/remotes/origin/*` 算进去，那会让你以为没抹干净**
>   —— 只验要推的 `main`（`git log main -S` + 遍历 `rev-list --objects main`
>   的全 blob 扫描，两法都是 0 才算过）。
>   ⚠️ 强推后 GitHub 仍保留旧对象（按 SHA 可取，但 SHA 从未公开过，风险≈0）；
>   要硬保证就删库重建（我们有 verified bundle）。**真正的补救是把 NSG 的 22
>   端口锁到自己的出口 IP** —— 那个 IP 是静态的，deallocate 也不释放。
> - 历史审计结论：**固件 blob 与密钥从未进过历史** ✅。
>
> ★**A/B 走的是 Virtual A/B，分区布局一个字节没改** —— 每槽镜像合计 2.53 GiB，
> 12 GiB 的 super 连传统 A/B 都放得下，而 Virtual A/B 连扩都不用扩。
> 只新增了 4 MiB 的 `misc`（本机用的是盘头 GPT 之后闲置的 1007 KiB，
> 扇区 34–2047，没动那个 64 GiB 备份）。
> 内核前提：`CONFIG_DM_SNAPSHOT=y` ✅，`CONFIG_DM_USER` 无 → 只能非压缩快照，
> 故只 inherit `virtual_ab_ota/launch.mk`。
>
> ★**自研 boot_control HAL**（`device/huawei/gaokun3/boot_control/`）。
> 标准 `boot-service.default` 把槽位写进 misc 就指望 bootloader 去读，
> 而 systemd-boot 不认识那个结构。我们这个原样复用 `libboot_control`
> （misc 仍是唯一真相源，`bootctl`/update_engine 行为不变），额外把槽位
> 镜像进 ESP 的 `loader.conf`（写 glob `default *-android-a.conf`，
> 于是不必知道 machine-id）。
> **实机验证**：`bootctl get-number-slots`=2、当前槽 `_a`、
> slot0 bootable+successful、HAL 日志 `systemd-boot default now: ...`、
> `vold`/`update_engine` 都报 `Using AIDL version of IBootControl`。
>
> ⚠️**M6 踩的两个坑，都是"看着完全无关"的那种**：
> 1. ★**fstab 的 logical 行必须带 `slotselect`**。少了它 fs_mgr 去找字面叫
>    `system` 的逻辑分区，而 A/B 的 super 里只有 `system_a`/`system_b`
>    → first-stage mount 失败 → **init 主动 `reboot()` 而非 panic**
>    → pstore 全空、屏幕一闪而过，**唯一症状是"进 A 槽就重启"**。
>    我第一反应是"ramdisk 没跟着换"，换了照样重启（猜错一次）。
>    判据：`device/linaro/dragonboard/fstab.common` 是树内现成的 A/B 设备，
>    每条 logical 行都带 `slotselect`，逐字对照即得。
> 2. ★**`nb_slot` 会是 4 而不是 2**。`InitDefaultBootloaderControl()` 靠
>    `stat()` 探测 `<miscdir>/boot_a..boot_d` 数槽位，本机**没有 boot 分区**
>    （内核/ramdisk 是 ESP 上的文件），探不到就按设计回退成 `kMaxNumSlots=4`。
>    后果很阴：`(cur+1)%4` 让**第二次** OTA 从 `_b` 切到不存在的 `_c`，
>    故障晚一个版本才爆。修法：HAL 的 `main()` 里把两个探测路径 symlink 到
>    `/dev/null`（`stat` 跟随符号链接会成功，正好数出 2；boot 不在
>    `AB_OTA_PARTITIONS` 里，没人读写它们）。
>
> ⚠️**`otapackage` target "不存在"是假象**：`build/make/core/Makefile:5793-5806`
> 在 `TARGET_NO_KERNEL=true` 且无 boot 镜像、以及无 recovery.fstab 时把
> `build_ota_package` 关掉。加 `PRODUCT_BUILD_GENERIC_OTA_PACKAGE := true`
> 一次跳过全部三条（AOSP 注释明说就是给树外内核用的）。
> crDroid 的打包 target 叫 **`bacon`**，它顺带跑 `createjson.sh` 生成
> Updater 要的 `gaokun3.json`。
>
> **安装器**：`scripts/install-gaokun3.sh` —— 从任意 arm64 Linux live 环境
> 一条命令装好（分区 + super + systemd-boot + 救援系统 + 两个槽位条目）。
> 默认启动项给**救援系统**而不是 Android：默认落点必须是能远程接入的系统。
> LiveCD 尚未打包，但这个脚本就是它的内核。
>
> ★★**Windows 已抹除，整机归 Android；U 盘不再是必需品**（2026-08-20，用户授权
> "数据都备份了，直接不要"）。
> - **引导链已搬进内置 ESP**（`scripts/esp-migrate-to-internal.sh`，纯增量）。
>   **拔盘实测通过**：`/dev/block/sd*` 不存在，Android 照常启动。
> - **删除 Windows 的 p2/p3/p4/p5/p6/p7**。现在盘上只有：
>   p1 ESP 300M / **p2 userdata 376G** / p8 super 12G /
>   p9 `userdata-old` 64G（迁移前备份，暂留）/ p10 metadata 32M /
>   **p3 Ubuntu 救援 24.6G**。未分配 1007 KiB。
> - ★**`/data` 62 GiB → 370 GiB（可用 294 GiB）**。此前它**已经 100% 满**
>   （原神 34G + 明日方舟 19G，非 root 只剩 235 MiB，装不下任何东西）。
>   做法是**新建 + `dd` 整盘克隆 + `resize2fs` + 换 PARTLABEL**，不是原地扩容：
>   `dd` 逐字节复制，SELinux 扩展属性/capabilities/硬链接零解释带过去；
>   `fstab.gaokun3` 用的是 `by-name/userdata`（**PARTLABEL**）所以改标签即可，
>   fstab 一字未动；**旧 p9 全程只读并保留**，出问题重启就是原来那台机器。
>   校验：文件数 49999=49999、字节数 63 237 600 073 相同、大文件 md5 抽查 3/3。
> - **Ubuntu 救援系统已搬进内置盘**（`rsync -aHAXx` 克隆活动根，134403 个文件），
>   hostname `gaokun3-rescue`、root=`nvme0n1p3`、ssh 仍是 192.168.31.230。
> - ⚠️★**固件的启动优先级会变，别当成一次测定的事实**：装引导链时
>   `LoaderDevicePartUUID` = `d5cb76b5…`（U 盘）；**删掉 Windows 分区之后变成
>   `825eaf3a…`（内置盘）**。于是"内置 ESP 只在 U 盘不在时才用到"当场失效，
>   而它的默认项当时是 Android → **自动回落安全网悄悄断了**，
>   症状只是"莫名其妙进了 Android"。动过分区表就要重读这个变量确认。
> - **现役闭环（两个方向都实测通过）**：默认 → 内置救援 Ubuntu；
>   `sudo bootctl set-oneshot <mid>-android-a.conf` → Android；
>   Android 里 `adb reboot` → 自动回落救援系统。
>   ⚠️ 条目名带 **`int-`** 前缀，U 盘那套旧名（`<mid>-crdroid.conf`）已非现役。
>
> **★★ Stage 6 M4（2026-08-19 夜）：s2idle 定性 + 音频解锁。**
>
> ★**"不能待机"的判决：挂起成功、resume 失败、然后整机被复位 —— 而且在
> Ubuntu 里用同一棵内核复现得一模一样。** 所以与 Android、与 SystemSuspend、
> 与我们的设备树**全都无关**，是内核/EC 层面的缺陷，根治属上游活。
> 证据：Ubuntu 侧 `echo mem > /sys/power/state` 后日志停在那一行，
> 紧随的 "resume 成功" 标记从未写出，机器回来后 `uptime` 是全新启动。
> RTC 闹钟**确实按时触发**，约 13 秒后才重启 → **坏在 resume，不在 suspend**。
> - **三个元凶已排除**（各自卸掉再挂起，照样醒不来）：himax 触摸驱动、
>   三个 remoteproc（ADSP/CDSP/SLPI）、★**EC 驱动本身**
>   —— 最后这条**推翻了 CLAUDE.md 从 Stage 3 起的预言**（"EC 挂起/恢复会先炸"）。
> - 那两个"本该修好它"的补丁**其实一直都在**（buildbot 无条件 `git am`
>   `patches/upstream/*` 与 `patches/others/*`）：`upstream/0012`
>   （EC 的 PM 回调 NOIRQ→SYSTEM_SLEEP，自述就是修 "resume fail silently"）、
>   `others/0017`（EC 加 `device_init_wakeup`）。都在，照样挂 → **别再指望它们**。
> - **没法继续二分**：内核没开 `CONFIG_PM_DEBUG`，`/sys/power/pm_test` 不存在
>   （两侧都没有）；挂起瞬间的日志也拿不到（userspace 已冻结，journald 来不及落盘；
>   clean hang 不产生 panic，efi_pstore 抓不到）。→ 真要修，第一步是编个带
>   `CONFIG_PM_DEBUG` 的内核。
> - **落地取舍**：wakelock **保留**（这是正确的工程决定，不是偷懒），
>   加一条逃生口 `persist.gaokun3.allow_suspend=1` 供将来复测；
>   但把 `svc power stayon true` 与 `screen_off_timeout=INT_MAX` **删掉** ——
>   持 wakelock 时息屏是安全的。**于是本机的"待机" = 息屏但机器不真睡。**
> - ⚠️**会浪费两小时的陷阱**：持有 wakelock 时读 `/sys/power/wakeup_count`
>   会**永久阻塞**（实测 cat 挂死 120s）。Android 的 SystemSuspend 就卡在这一读上，
>   这也解释了 `suspend_stats/success` 恒为 0。别在探测脚本里 cat 它。
>
> ★**远程救援闭环本轮实战验证成功**：Android 挂死 → 自动复位 → 默认启动项
> Ubuntu → ssh 进去 → `bootctl set-oneshot ...crdroid.conf` → 回 Android。
> **全程不需要有人在机器边。**"默认启动项永远留 Ubuntu"这条纪律兑现了价值。
>
> **音频**：`tinymix` 首次真的编进来了（M3 只是排了队）。硬件路径**实测通**：
> 291 个混音器控件、路由回读正确（`>AIF1_PB`/`>RX0`/DAC on/BOOST off/PA=12
> —— ⚠️ `PA=12` 已于 2026-09-08 改为 17/试 21，见 #78）、
> `tinyplay` 让 `/proc/asound/card0/pcm1p/sub0/status` 变成 **`state: RUNNING`**
> 且 DMA 实时消耗。★**2026-08-20 用户实机确认：音频可用（听到声音）**。
> 框架路径于此确证 —— 此前 `Total writes: 0` 只是因为这个 ROM 里没有任何应用
> 能处理音频、我试过的每种无头触发都没能让 AudioTrack 起来，属"未测"而非"坏"。
> ⚠️ `tinymix` 动态链接 `libtinyalsa.so`，放 `/vendor/bin/` 时那个 .so
> 必须一起进 `/vendor/lib64/`（vendor 命名空间搜不到 system 的那份）。
>
> **蓝牙：撤销 #30**。实测 `state: ON`、地址读出、`crashed 0 times`。
> `android-post-flash.sh` 里的禁用两行已删（留着会在每次重刷 userdata 后
> 把好的蓝牙重新关掉）。
>
> **搁置并说明理由**：传感器（⚠️**此处的"此路不通"已被 M9 推翻** ——
> 整套确实跑在 SLPI DSP 上、AP 无总线可达，但经 hexagonrpcd 给 DSP 当文件
> 服务器后**加速度计已实测通**，见 #37；光感仍不通，故自动亮度做不了，
> 自动旋转则只差 Android HAL；**游戏不受影响**）、
> Venus 硬解、UCSI、SELinux 转 enforcing。
> ⚠️**记一条将来的地雷**：热管理 HAL 是 AOSP mock，它报的 skin/battery
> **SHUTDOWN 阈值只有 36.0 °C**，而 `ThermalManagerService.shutdownIfNeeded()`
> 到 SHUTDOWN 会直接 `powerManager.shutdown()`。现在因 mock 值恒定打不到，
> **将来换成读 `/sys/class/thermal` 的真 HAL 时必须同时改阈值**，
> 否则开机几分钟就自动关机。
> 详见 `docs/stage6-crdroid.md` 的 M4 段。

> **★★ Stage 6 M3 已于 2026-08-19 完成：Android 跑在 Adreno 690 硬件 Vulkan 上。**
> `Turnip Adreno (TM) 690`、`boot_completed` t+48s、锁屏/桌面渲染正常
> （3.83 MB screencap 逐像素对）；22 分钟带负载浸泡：**GMU 错误 0 /
> a6xx_recover 0 / SMMU fault 0**，桌面四进程 PID 全程不变。
> 一键复验 `bash scripts/verify-turnip.sh`，案卷 `docs/stage6-crdroid.md`。
>
> 做法是**把 Stage 5 的补丁树整棵铺回来**，不是重跑生成管线；
> 另外首次把 `smmu-nostall.sh`（GPU SMMU stall 解锁器，常驻安全网）
> 写进了构建配置。
>
> **M3 顺带推翻了三条旧结论（都写进了案卷）**：
> 1. ★"crDroid 与我们的 mesa 是同一个 commit" —— **假阳性**。
>    `git log -1` 比的是 `.git` 的 HEAD，而 mesa 26 当年是**铺在工作树上
>    从未提交**的，所以两棵树必然显示同一个 commit。实际是
>    25.3.0-devel vs **26.0.3**，`git diff` 3791 个文件。
>    判"两棵源码树是否相同"，`git status --porcelain | wc -l` 才是那一句。
> 2. ★"s2idle 的 **wakelock 挡不住**" —— 挡是一直挡住了（⚠ 别把这条读成 "s2idle 是好的"；M4 已证明 **resume 确实坏**）。`/sys/power/wake_lock` 是
>    `radio:wakelock` 0660，shell 连读都读不了，我把 `cat` 的
>    "Permission denied" 当成了"内容为空"。实测 26 分钟 `suspend entry` = 0。
> 3. ★"`adb root` 不生效" —— **两步可解**：
>    `adb shell setprop service.adb.root 1` 然后 `adb root`
>    （permissive 下 shell 能写这个属性，而 adbd 自己那一步没生效）。
>    顺带查明 `ro.build.type` 是 **user** 而非 userdebug。
>    ★ 这把钥匙的真正价值是**远程救砖**：拿到 root 就能挂 U 盘 ESP
>    （`/dev/block/sda1`，**不是**内置盘 `nvme0n1p1`）直接改 `loader.conf`。
>
> **M3 最费时间的坑**：刷完 super 后 oneshot 到了 `android.conf`，
> 那是**旧 AOSP 的 Image(kb18)+ramdisk**，crDroid 要用 `crdroid.conf`
> （`Image-kb23`）。用错内核 → 没有 `patches/0007` → bpffs 标签崩溃循环，
> 症状一路指向刚换的 turnip 而其实毫无关系。
> **判据：`adb shell uname -a` 的编译时间必须与本次内核一致。**
> `scripts/deploy-from-ubuntu.sh` 已改成设 oneshot 到 `crdroid.conf`，
> 且不再篡改默认启动项（默认必须留 Ubuntu，那是唯一的自动回落安全网）。
>
> **M4 起点（已实测）**：触摸设备在（`Himax Capacitive TouchScreen`）、
> 声卡注册、解码器 66、蓝牙 HAL 装着但被 `android-post-flash.sh` 禁着；
> ⚠️ **音频当前是断的** —— `tinymix` 从没进过 `PRODUCT_PACKAGES`
> （`audio-route.sh` 找不到它就直接放弃），已补进 device.mk 待下次构建；
> ⚠️ **WiFi 硬件全通但网络被框架永久禁用**
> （`NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT`，stage4 #29 复发），
> 解禁需要一次带密码的用户发起连接。

> **★★ Stage 6 M2 已于 2026-08-19 完成：crDroid 16.0 完整启动进桌面。**
> `sys.boot_completed=1`（t+40s），surfaceflinger/system_server/systemui/launcher3
> 全部在跑；**解码器 66 个**（含 mp3/aac/flac/amrnb/amrwb/g711）；
> 铃声 130 + 通知音 92 + 闹铃 45 + UI 音效 25。执行案卷 `docs/stage6-crdroid.md`。
>
> **四个真凶（都不是配置写错，是真实的不兼容）**：
> 1. ★**`media.c2.hal.selection` 默认是 `hidl`** —— 这就是追了两个阶段的
>    "解码器一个都没有"（#36）。HIDL Codec2 在 Android 15+ 已不可用
>    （hwservicemanager 被移除）。**与 crDroid 无关，AOSP 16 上同样如此**，
>    只是真机设备树都会设它。必须走 `PRODUCT_SYSTEM_EXT_PROPERTIES`
>    （该属性上下文 `codec2_config_prop`，vendor 无权设）。
> 2. ★**bpffs 的 SELinux 标签**（`patches/0007` 内核补丁）——
>    主线 bpffs 在 inode 创建时急切赋标签，Android 依赖的 genfscon
>    惰性路径匹配失效 → ClatCoordinator 逐字比对标签失败 → system_server
>    崩溃循环。用户态无法修（`chcon` 报 ENOTSUP，因为策略对 bpf 用 genfscon
>    而非 `fs_use_xattr`）。**这个坑对任何"Android on 新主线内核"都成立。**
> 3. ★**crDroid 的 `SetSafetyNetProps()`**（`property_service.cpp:1168`）
>    在解析 cmdline 之前硬写一整张表，把 `ro.debuggable`/`ro.adb.secure`/
>    `verifiedbootstate`/`flash.locked` 全部盖成"已锁定已验证 user 版"。
>    这让 `WITH_ADB_INSECURE`、`PRODUCT_SYSTEM_EXT_PROPERTIES`、cmdline
>    三条路改了都没用，极具迷惑性。开关 `SPOOF_SAFETYNET` 只在 eng 变体关，
>    故用 `scripts/crdroid-tree-fixes.py` 改默认值（eng 会关 dexpreopt，不可取）。
> 4. **AOSP 基座必须由设备树自己 inherit**（`full_base.mk`）——
>    `vendor/lineage/` 下全是补充配置。少了它构建"成功"但产出空壳
>    （`system.img` 29.8 MB、无 apex/app/services.jar）——**比构建失败更危险**。
>
> **已补上（M3 复核）**：s2idle 休眠其实一直是好的 —— init 的
> `write /sys/power/wake_lock` 从第一次就成功了，我把 `cat` 的
> "Permission denied"（节点是 `radio:wakelock` 0660，shell 读不了）
> 当成了"内容为空"。实测 uptime 21 分钟时 `suspend entry` 计数 **0**
> （修之前 45–60 秒必挂且醒不来）。
>
> **还欠的**：见首屏 M3 段的「M4 起点」——音频（tinymix 漏装，已补待构建）、
> WiFi（网络被框架永久禁用，解禁要密码）、蓝牙（装着但禁用中）。

> **★决策（2026-08-19）：硬件使能告一段落，下一步转 crDroid 移植。**
> 理由：剩下卡住的东西（App/媒体没声音）**不是硬件或内核问题**，
> 而是这棵手搓最小 AOSP 缺产品级配置 —— `MediaCodecList` 是空的
> （`/vendor/etc/media_codecs.xml` 原本不存在，拷进去仍空，
> `ro.media.xml_variant.*` 全未设）、`/system/media/audio/` 整个缺失
> （铃声/UI 音效一个没有）。这些是 Lineage/crDroid 设备树的标准组成部分，
> 换轨后大概率自动消失。详见 `docs/stage4-findings.md` #36。
> - **已定：直接上 crDroid**（不先过 Lineage）。执行案卷 `docs/stage6-crdroid.md`。
>   - crDroid `16.0` = LineageOS 23.2 布局，AOSP 基线 tag **`android-16.0.0_r4`**；
>     `lunch lineage_gaokun3-bp4a-userdebug`（release config `bp4a`，实名核实）。
>   - manifest 1180 个项目，本机只缺 `device/linaro/dragonboard`
>     → `manifests/local_manifest_gaokun3.xml`。
>   - ★**铃声缺口 crDroid 自带解决**（`vendor/lineage/audio/audio.mk` 装 44 个音频到
>     `product/media/audio/`）。
>   - ★**解码器缺口的最终结论（已解决）**：不是 `media_codecs.xml` 缺失，
>     也不是"servicemanager 的 declared 集"问题（这两个归因都被推翻了）。
>     真凶是 **`media.c2.hal.selection` 默认为 `hidl`**
>     （`frameworks/av/media/codec2/hal/common/HalSelection.cpp:57`），
>     而 HIDL Codec2 在 Android 15+ 已随 hwservicemanager 一起消失。
>     设成 `aidl` 后解码器从 0 → 66。详见首屏的 M2 段与 `docs/stage6-crdroid.md`。
>   - ★**mesa 管线一个都不能丢**：lineage-23.2 的 `external/mesa3d` 就是 AOSP 那份，
>     `BOARD_MESA3D_*` 是平行项目自带 mesa 仓库的机制，不是 Lineage 的
>     （作废 `docs/stage5-freedreno.md:212` 的猜测）。
>     ⚠️⚠️ **"两边是同一个 commit"这条结论是错的，2026-08-19 M3 当场推翻**：
>     `git log -1` 在两棵树上都给 `d4b6f1eba289…`，但那是 **`.git` 的 HEAD**，
>     而 Stage 5 的 mesa 是**直接铺在工作树上、从未提交**的上游 mesa
>     —— 于是这个对比必然给出假阳性。逐字节比才看得见真相：
>     crDroid 树内 = **mesa 25.3.0-devel**，Stage 5 补丁树 = **mesa 26.0.3**，
>     `git diff` 3791 个文件 / 36.5 万行，是真实的上游版本差
>     （`gl_shader_stage`→`mesa_shader_stage` 这类重命名、新增文件都在）。
>     ⚠️ 连带作废：早先那句"'mesa 26' 是被本地改过的 VERSION 文件误导"也是错的。
>     **正解 = 铺回归档树** `~/keep/mesa3d-patched.tar.zst`（M3 已这么做）：
>     它就是实测 SMMU fault=0 的那棵，patches/0004 v3 全在里面。
>     退路：`git checkout . && git clean -fd` 一句话回到 crDroid 原版 25.3。
>   - 构建机磁盘：**整棵删了 `~/aosp`**（157G → 451G 可用）。
>     "只删 out 腾到 264G"的算术不够 —— crDroid 源码+.repo≈190G + out 100–130G。
>     删之前已归档 `~/keep/`（super/ramdisk/turnip.so/mesa 全树，`sha256sum -c` 通过）。
> - **可平移的成果（换 ROM 不用重做）**：kb21 内核配置 +
>   `scripts/kernel-config-android.sh` 的断言、DTB（含 gpio174 触摸补丁）、
>   固件集与 audioreach 拓扑固件的**正确路径名**、mesa turnip
>   `apply-0004v3.py`（ANB 延迟绑定，纯 mesa 修复）、
>   `smmu-nostall.sh`（SMMU stall workaround）、
>   `audio-route.sh`（混音器路由）、蓝牙用 AOSP 原装 HAL 即可、
>   以及 docs/ 里全部踩坑记录。
> - **要重做的**：Lineage 布局的设备树、UEFI 引导集成（无 fastboot）、
>   整棵 ROM 的构建。参考 `docs/parallel-mainline-generic.md`（同款内核的
>   Lineage 系平行项目，可借它的 gaokun3 配置）。

> **Stage 4 音频/蓝牙已于 2026-08-19 完成（硬件层）**：
> 声卡 `SC8280XP-HUAWEI-GAOKUN3` 注册、内置扬声器实机出声（用户确认，
> 整曲播放通过）；蓝牙 adapter `state: ON`、地址从芯片读出、零崩溃。
> 三个 `=m` 断点（LPASS pinctrl ×2 + LPASS 时钟）+ 未编的 `SND_SOC_WSA883X`
> + 拓扑固件路径名 + `RT_GROUP_SCHED=y`（挡住蓝牙的 SCHED_FIFO）——
> 全部记在 `docs/stage4-findings.md` #33–#36。
> ⚠️ 起停爆音在 `BOOST Switch` 打开时出现（A/B 实听定案），默认已关。
> ⚠️★ 更正（#78）：它**不是功放升压器**，是 SoundWire 端口使能；
> 关掉它**不损失任何响度**（实测 ±0.3 dB 以内）。

> **Stage 5 GPU 战况（2026-08-19 凌晨）：★GPU 攻坚完成 —— Android 用硬件
> turnip 启动进桌面，SMMU fault 归零，帧读回正常。** 案卷
> `docs/stage5-freedreno.md` D4–D10，工具集 `scripts/gmu-forensics/`
> （**11 条会反复中招的坑，动手前先读 README**）。
> - **"GMU 必死"从头到尾不是电源管理问题**，是一条空指针放大链：
>   turnip 没实现 ANB 延迟绑定 → image 没内存、`iova` 恒 0 →
>   **GPU 往地址 0 写** → GPU SMMU translation fault →
>   本平台 fault 中断打不到 CPU → `SCTLR.CFCFG=1` 永久 stall →
>   AHB 总线 stall → CP 断粮 → `GX_BW_PERF_VOTE` 超时 → 看门狗 →
>   stall 拖住掉电（`cx gdsc didn't collapse`）→ 死循环。
>   `GX_BW_PERF_VOTE 超时`是**果**，不是因。
> - ★**真凶（D10 三探针实测定性）**：Android 的 libvulkan 走**延迟绑定**——
>   `vkCreateImage` 时不带 ANB，gralloc buffer 是在 `vkBindImageMemory2`
>   那一刻才用 `pNext` 的 `VkNativeBufferANDROID` 递进来的
>   （实测 pNext=`{NATIVE_BUFFER_ANDROID, BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR}`，
>   调用链 ANGLE → libvulkan → turnip）。mesa 那句
>   `/* TODO handle VkNativeBufferANDROID */` 说的就是这条路，**从没人实现**。
>   **权威修复 = `scripts/gmu-forensics/apply-0004v3.py`**
>   （patches/0004 的 v1/v2 都是错的，v2 的"安全跳过"正是残余 fault 的来源）。
>   编译只需 `m vulkan.freedreno`（约 1.5 分钟），部署
>   `scripts/gmu-forensics/deploy-turnip.sh`（overlayfs，**不刷 super**）。
> - **实测对比**：SMMU fault **66 → 0**；未绑定 image 建 view **96 → 0**；
>   `screencap` 从**永久卡死 → rc=0 出 3.27MB 正常图**（截图逐像素正确，
>   证明按 gralloc 真实 modifier 重算布局那步是对的）；
>   t=36s 进桌面，GMU 错误 0，`a6xx_recover` 0，桌面四进程 PID 稳定不变。
> - ★**D6 悬案的物理机制找到了**：fault 当场读 `GICD_ISPENDR22` 发现
>   SMMU 拉的是 **SPI 675 / 680**，而 DT 声明、内核注册并使能的是
>   **SPI 678/679**（`/proc/interrupts` 计数恒 0）。**内核在听 678，
>   硬件在喊 675** —— 不是"SMMU 不拉中断"也不是"内核没使能"。
>   ⚠️ 顺带作废我自己一度下的"DT 跳号正常"判断。
>   下一步可根治：改 DTB 的 gpu_smmu context interrupts（不用重编内核），
>   成功就能彻底丢掉轮询脚本。
> - **常驻 workaround**：`scripts/gmu-forensics/smmu-nostall.sh`（轮询清
>   `SCTLR.CFCFG` 让 fault 走 terminate + 抓 FSR/FAR/GICPEND）。
>   ⚠️⚠️ **只能扫 CB0/CB1（`NCB=2`）**：实现了几个 CB 看
>   `/proc/interrupts` 的 `arm-smmu-context-fault` 条数；扫到未实现的 CB
>   → external abort → **内核静默死亡**（Android 连续三次启动到
>   post-fs-data 后消失、无 tombstone 无 pstore 无 adb）。
> - **运维**：cmdline 已带 `androidboot.flash.locked=0
>   androidboot.verifiedbootstate=orange` → `adb remount` 走 overlayfs，
>   改 vendor 不用开构建机（每次重启挂回 ro，写前重跑 remount）。
>   **默认启动项已改成 Ubuntu**（Android 挂死拍电源键自动回落）→ 要进
>   Android 得在 Ubuntu 里 `bootctl set-oneshot …android.conf`
>   （deploy 脚本已内置中转）。adb 彻底不通时走
>   `scripts/gmu-forensics/overlay-rescue.sh` 离线读写 overlay
>   （scratch 是 super 里 4 段 extent 拼的 f2fs，要 dm-linear 拼回去，
>   且得用 `ubuntu-kb19` 启动项才有 f2fs）。
> - ⚠️ **旧结论作废**：属性名是 `debug.mesa.tu.debug`（不是 `debug.tu.debug`），
>   故 2026-08-18 前所有 tu_debug 实验旗标从未生效；
>   `msm.enable_preemption=0` 是有害参数（内核判断语义反转）已从 cmdline 删。
> - mesa 26 的 AOSP 构建管线已全套入库：`scripts/mesa-tool-fixes.py`
>   + `scripts/mesa-bp-merge.py` + `scripts/join_meson_continuations.py`
>   + `patches/0003..0006` + `device/huawei/gaokun3/mesa/`。
>   软渲染兜底仍在（`ro.hardware.vulkan=pastel` 一行可切回）。
> - **下一场**：音频 —— 内核链全 =y、ADSP 三兄弟 running、固件（含
>   audioreach-tplg.bin）进 ramdisk+vendor，但声卡未注册（macro/soundwire
>   的 deferred probe 不收敛，`suppress_bind_attrs` 封死手动补绑定）。
>   另有蓝牙（#30）、s2idle、以及可选的 DTB 中断根治。

> **Stage 4 触摸已于 2026-08-17 完成：触摸丝滑可用。**
> 根因是 gpio174（模式选择脚）无人驱动，见 `docs/stage4-findings.md` #26
> 和 `patches/0002-*.patch`。✅ 补丁已应用进 VM 内核树（kb18 起自带）。
>
> **Stage 4 WiFi 已于 2026-08-17 完成：冷启动免干预自动连网。**
> 内核 kb18（ath11k 全家 =y + PWRSEQ）+ 晚绑定 + goldfish wifi HAL +
> supplicant 配置 + 国内验证端点，全程见 `docs/stage4-findings.md`
> #28/#29。adb over TCP 已开（5555 端口，缓解 #27）。
> ⚠️ 重刷 userdata 后必须跑 `scripts/android-post-flash.sh`。
> 剩余：音频、蓝牙（#30，无 HCI HAL 暂禁用）、挂起/恢复（s2idle）、
> UCSI 拔插（#27）、Ubuntu 侧 DTB 触摸补丁（等 USB_STORAGE=y 或进 Ubuntu 手做）。

> **Stage 3 已于 2026-08-17 完成验收：Android 桌面完整渲染**
> （Launcher3 + SystemUI 稳定，1600×2560，截图为证）。
> 图形 = swangle 软渲染（Phase A）；Phase B 换 freedreno 见
> `docs/parallel-mainline-generic.md` 路线图。
> ⚠️ 已确认坑：闲置 52 秒自动 s2idle 休眠后醒不来（CLAUDE.md 预言的
> EC 挂起坑），临时用 `svc power stayon true` 顶着，Stage 4 正修。

> Stage 0 / Stage 1 已于 2026-08-13 完成验收。
> **Stage 2 已于 2026-08-17 完成验收：`adb shell` 通，Android 16 稳定运行**
> （`gaokun3 device product:aosp_gaokun3`，内核 7.2.0-rc2-gaokun3+）。
> 全部 12 个实测问题及修复见 `docs/stage2-findings.md`。
> Stage 3 起点：surfaceflinger 崩于 "couldn't find an OpenGL ES
> implementation"（mesa/gralloc/hwc 未装，abort message 见
> `docs/stage2-acceptance-live.txt`）。
