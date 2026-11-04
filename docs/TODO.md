# 待办清单

最后更新：**2026-11-05**（状态回填到1.0.0-rc.3发布和dev合入PR #91为止；下面各段原有的日期照旧）。**2026-10-06**重整（本文件只回答"还剩什么、下一步是什么"。已完成项的过程记录、1.0 dev.1–dev.9 的逐版装机记录、已结案的调查、
v0.6.x / v0.7.x 时期已收口的条目和旧的分梯队总表，原样搬到 [`archive/TODO-history-2026-10.md`](archive/TODO-history-2026-10.md)（按原顺序、标题层级不变）。
被搬走的节在原位置留了**同名标题 + 结论 + 指引**，所以别的文档里"TODO 的 v0.7.0 一节""TODO B1""TODO「▶ 1.0 批 0/1」dev.5 段"这类引用照旧能找到入口；
旧文件的 `TODO.md:NNN` 行号引用已对不上，按节名去 archive 里找。原来的文件头（历次对账说明）与 09-23 那行没重算过的统计已删，原文见 archive 的 [`#file-header`](archive/TODO-history-2026-10.md#file-header) / [`#summary-stats`](archive/TODO-history-2026-10.md#summary-stats)；条目数以下面总表为准。）

排序按 **1.0 发版路径**：发版阻断 → 1.0 应做 → 等用户 / 等硬件 → 1.0 之后。每条都写出下一步 —— 没有下一步的只是愿望。
1.0 各条目的定义、证据与批次以 [`v1.0-plan.md`](v1.0-plan.md) 为准；每版都要回归的项在 [`release-checklist.md`](release-checklist.md)；
结论的出处在案卷（[`stage4-findings.md`](stage4-findings.md) 等）；历史在 [`project-log.md`](project-log.md)。

状态：⬜ 没做 · ◐ 做了一部分（多半是"代码已进镜像、没上机验"）· ✅ 完成。编号沿用原来的：V0–V18 是「▶ 1.0 批 0/1」一节的验收号，
D1–D23 是 v1.0-plan §6 的决定，其余是 v1.0-plan 与本文件的条目号（旧编号去向见总表末尾的索引）。

---

## ⚠️★★★ SEC-1 efi_pstore 撑满 NVRAM（2026-10-30 用户报告，安全修复）

所有已发布版本（`patches/0001` 自 08-15 首个提交起）每次内核 oops 往 UEFI 变量写约 10 个 `dump-type0-*`（record_size 1024、kmsg_bytes 10240、
PSTORE_COMPRESS 关；10-30 实机只读核对），Android 不清 ⇒ 报告者两天 103 个 / 约 98 KB，固件建不了 Boot####、U 盘 / 官方恢复都不出现，售后判"换主板"。
✅ 止血（随1.0.0-rc.3发布，10-30）：`BoardConfig.mk` cmdline 加 `efi_pstore.pstore_disable=1`（现有内核即生效，经 OTA 同步进 ESP 条目 / gk3boot / 救援条目）；
内核 config 加 `EFI_VARS_PSTORE_DEFAULT_DISABLE`；live USB cmdline 同步（rc.3没有重做安装器，要等下一次重做才进发布物）；FAQ 中英加检查与清理步骤。
✅ rc.3上机核过`pstore_disable=Y`、backend 为 `(null)`（[rc.3 发版说明](relnotes/v1.0.0-rc.3.zh-CN.md)）· ⬜ 已写入变量的自动清理放哪（要用户定：
gk3boot 开机删 / Android 开机一次性服务删 / 只靠 FAQ；rc.3发版说明目前请用户按#56手动清）· ⬜ 报告里的 oops `sync_file_ioctl → drm_crtc_fence_get_timeline_name` 另立条目查 ·
⬜ HyperOS 分支同样带 0001、cmdline 未改 · ✅ 随rc.3发布。

## 总表：现在在做 / 待办（2026-10-18 更新；下面「设备现状」一段是 10-06 的，现状以 CLAUDE.md「现在」为准）

**2026-10-18 用户定：rc.2 先不发，能做完的都做完再发。** 当天无人值守能做的已做完（V0 余项、V2、V5、GAME-1 源码排查、GAME-6、HAL 误报 Windows、
WoWLAN 说法订正、live 控制台刷屏、#46 合入并修 0105、rc.2 第三次构建 `1792312407` + 安装器 `0.2.0-rc.2`）。**剩下的全要用户在场或用户拍板**：
~~上机装 `1792312407` 并验收~~（✅ 10-18 已装、A 档零 FAIL）、rc.2 发版说明定稿与发布、② 表「要用户在场的」各项、批 4 测量、B4 新装机、B2 R2 密钥轮换、~~推送本地提交~~（✅ 10-19 推到 `origin/dev`；`main` 等发版时从 dev 合并）、③ 表的决定。
**10-19 起分支规矩**：所有修改在 `dev` 上改、推 `dev`；`main` 只在发版时从 `dev` 合并；PR 提到 `dev`（CLAUDE.md「发布与构建」、CONTRIBUTING.md）。⬜ 仓库默认分支设成 `dev`。
**★ 2026-10-19 剩下 4 个 PR 合入 `dev`（用户定"都合进去，不对的直接改"）**：#46 早已在 dev（改目标分支后自动标成已合并）；**#49** `ro.hardware.egl=angle`（关开发者选项后 `persist.graphics.egl` 被设置写空、下次开机 zygote 找不到 GLES；Loader.cpp android-16.0.0_r4 :72-76 / :299-311 / :505 / :523 已核，device.mk 里"ro.hardware.egl 对 ANGLE 不生效"的旧注释是错的）；**#48** cmdline 加 `fw_devlink.sync_state=timeout`（PWR-10）；**#41** USB 传文件（内核 0096、0112，端口 HAL 写连接状态，MTP / PTP / RNDIS / MIDI，游戏模式只改 `adb_enabled`）。合并时改的：#41 与 dev 的两处冲突（usbrole 注释取 v7.2.9 行号、init 的旧内核兜底规则补回会话内有效 / 同值 setprop 的说明）；0112 定号（0111 留给 NVMe 实验）、挪到 0110 之后；release-checklist B14 改成"重启后保持原状"、新增 B23（USB 用途）；v1.0-plan PWR-6 / PWR-10 回填。0096 / 0112 在 v7.2.9 的 configfs.c / Kconfig / Makefile 上 `git apply --check` 干净。⬜ **要重编内核和 ROM 才生效，都没以 ROM 形式上过机**：上机后跑 B14、B23；#49 关开发者选项→重启能起来；#48 游戏负载、s2idle、外接 DP。rc.2 候选 `1792312407` 不含这些。

**设备现状**：槽 `_b` = **`1.0.0-rc.1`**（戳 `1791285623`，内核 `912f22c3`），**SELinux enforcing**，active = `_b`。**rc.1 验收（10-06）**：在 dev.10（enforcing）上经 OTA 装（第二次 enforcing 下 OTA ✅），A 档 **PASS 47 · FAIL 0**（发布闸门要的零 FAIL：验收期间临时关了开发机的 5555，跑完恢复）、iris 15/15、前后摄、s2idle 11/11、稳定 MAC、fcitx5 是系统应用（`UPDATED_SYSTEM_APP`，签名吻合）。载荷 `out/rc1-1791285623/`；安装器 `0.2.0-rc.1`（U 盘镜像带 rc.1 系统，Windows 包不带）在 `out/rc1-wt/out/release-installer/0.2.0-rc.1/`。踩到：enforcing 下 Android 侧写 EFI 变量被拒（efivarfs 没标签，`a1d303a` 修，进下一版；只影响开发脚本）。✅ **2026-10-06 21:xx 发布为候选版**：ROM `release.sh --no-build --stage-only` → R2 `staging/crDroidAndroid-16.0-20261006-gaokun3-v12.11/`（8 个文件 HTTP 200，校验清单与本机逐字节同；**不动 OTA 清单**）；安装器 `0.2.0-rc.1` 6 个文件 → R2 `installer/0.2.0-rc.1/`（公开域名逐个核字节数）；GitHub **pre-release** `v1.0.0-rc.1`（tag = `8b7c5ab`，不标 Latest），12 个附件服务端字节数逐一核对一致（OTA 包只在 R2）。安装器产物另存 `out/rc1-installer-0.2.0-rc.1/`。⚠️ 用户这次在聊天里贴了 R2 S3 密钥与 Cloudflare 账户令牌 ⇒ 要轮换（B2）。⚠️ 分派开着：设置里的恢复出厂会真擦。

**用户已定**：D1 发布版关 debuggable · D2 留 test-key 并披露 · D3 `/data` 不加密并披露 · D4 恢复出厂交给统一启动入口 · D6 保留 root ·
D7 只发 GApps 版 · D10 fcitx5-android 从 GitHub 取（2026-10-06，已入构建）· D13 备份默认 Seedvault · D15 默认时区上海 ·
D17 充电上限保持现状、**不做写 EC 的上机实测**（2026-10-06）· **D5 SELinux 切 enforcing**（2026-10-06，dev.10 起默认）· **E10 真机恢复出厂不测**（2026-10-06）· D21 0050 移出发布内核（dev.9 起）·
其余按 v1.0-plan §6 的"建议"列执行（2026-10-04）。仓库已推送到 origin。

**★ 2026-10-09 `1.0.0-dev.11` 上机（发布构建，戳 `1791554509`，槽 `_a`，载荷 `out/dev11-1791554509/`）**：构建机 D32 —— 内核 `~/gk3-kernel-72y` 打 0078（apply 时 0018 报打不上 = #110 的已知误报）、`--verify` 打不上 0 / fuzz 0 / 47 文件一致、配置断言 164/2、`.config` 与 va39w 实验内核一致、dtb 与 rc.1 逐字节同（`c84f26f7…`），vmlinuz `705b1387…`（`~/v1-work/k78/`，本机 `prebuilt-boot/` 已换，旧的备份在 `out/prebuilt-boot-backup-k77/`）；整包 `release.sh --dry-run` 约 20 分钟，第 2 步断言全过。payload 经 R2 `staging/dev11-1791554509/` 中转（⚠️ 单连接只有约 230 KB/s，8 段并发约 14 MB/s）。`install-ota-local.sh --go`（先 `setenforce 0`）→ oneshot **40 秒起来**。实测：enforcing；`mmap_rnd_bits=24`、482 个进程 0 个映射 ≥ 2^39；`ro.egl.blobcache.multifile=true`；镜像里有相机 `fd use` 与 crash_dump tmpfs 规则；avc 只有已知两类（HID `country`、`com.android.se`）。**A 档 `out/accept/1791554509-20261009-225132/`：PASS 45 · FAIL 2（A3 = 开发机持久化的 5555，预期）· WARN 1（上面两类 avc）· SKIP 1（A11 没给 --video）**。enforcing 下 `bootctl set-active-boot-slot 0` 成功、ESP default 自动同步成 `_a`、零 avc ⇒ **`309f735` 过**。⬜ 要解锁的：App `kill -SEGV` 出 tombstone（`a3c354c`）、Aperture 预览 5 分钟零 `tclass=fd`（#23）；⬜ 英雄联盟进对局、抖音复测；⬜ `a1d303a`（enforcing 下 oneshot 不 setenforce）。

**dev.11 带上的（上面已上机）**：`5c48d08` fcitx5 原样安装、`309f735` HAL 的 ESP 认法（EBUSY / enforcing）、`ec5f2a5` 相机 HAL 用合成器栅栏（issue #23）、`a3c354c` crash_dump 的 memfd（enforcing 下无 tombstone）、**内核：`5b5534c` 0078 进链（要重编内核）**，`ro.egl.blobcache.multifile=true`（issue #34），安装器 S1 / S2 修复与风险确认页（安装器侧，重建 live 镜像时带上）。

**★★★★★ 2026-10-30 `1.0.0-rc.3`发布（Android 17 / crDroid 13，戳`1793322048`）**：首要是SEC-1的安全修复，另带A17、扬声器（#50、#52）、自动亮度（#54）、触摸（#51）、手写笔平滑（#55）。
经OTA装进开发机备用槽、OneShot起、enforcing：A档PASS 46 · FAIL 0 · WARN 2 · SKIP 0，硬解15/15，前后摄冒烟过；实际走的是A17 → A17，**A16 → A17的OTA没有重新演练**。
没有重做安装器。验收与下载见[`relnotes/v1.0.0-rc.3.zh-CN.md`](relnotes/v1.0.0-rc.3.zh-CN.md)。rc.3之后合入dev的#57–#91还没进任何ROM构建。

**★★★★ 2026-10-25 `1.0.0-rc.2` 第五次构建（rc2e，戳 `1792913333`）✅ 10-25 上机验收、✅ 10-25 发布并进 OTA 清单**（用户：做触摸 game / daily 开关，发一版 rc.2 进 OTA、把 v0.7.1-alpha 用户带上来；E11 用户定不演练）：仓库 dev `827ed7b`（rc2d + Parts 触摸手感开关）`git archive` 到构建机 `~/v1-work/repo-rc2e/`（956 个文件）；内核沿用 rc2d `7636022f`（boot.img 内核逐字节断言过）；设备树 `sync-device-tree.sh` 237 个文件一致。`release.sh --dry-run`（D32，约 23 分钟）第 2 步断言全过；APK 里有 `TouchModeSettingsActivity` / `TouchModeTileService`。`--no-build --stage-only` → `staging/crDroidAndroid-16.0-20261025-gaokun3-v12.11/`；开发机 payload `staging/rc2-1792913333/`（`payload.bin` 1396335676 字节、sha256 `7a7b8ae3…`，已下回本机 `out/rc2-1792913333/payload/` 核过）。r2-prune 删了 rc2c 两组。构建机已停。⚠️ **进 OTA 清单之前**：v1.0-plan G6 / E11（0.7.x → 1.0 原地升级、保留数据）从没在真机上演练过，而清单一改，所有 v0.7.1-alpha 用户都会走这条路。
**上机（10-25，用户在场）**：payload push 核 sha → `install-ota-local.sh --go` 装进 `_b` → oneshot **50 秒起来**（戳 `1792913333`、enforcing、`6.6.30-android15-8-…`）→ 解锁后 uiautomator 点「设置 → 系统 → 触摸手感」两次：`touch_mode` game→daily→game，`algo/track_smoothing` 0→1→0、`hand_enabled` 0→1→0，无相关 avc；#48 / #49 只核了属性（cmdline 有 `fw_devlink.sync_state=timeout`、`ro.hardware.egl=angle`），功能没测。
A 档（临时关 5555）：第一次 **FAIL 1 = A15 找不到 smmustall 心跳** —— 假阴性：logcat 主缓冲 256 KiB 被后台第三方应用（QQ 等）刷到只装得下约 1 分钟，心跳本身在（`round=1663 抓 fault=0`）；临时 `logcat -G 2M` 重跑 **PASS 47 · FAIL 0 · WARN 1 · SKIP 1**（`out/accept/1792913333-20261025-165335/`），跑完改回 256K、没留持久属性。⬜ accept.sh / verify-turnip 的心跳判据别依赖 256 KiB 缓冲（自己 `logcat -G` 或改读服务状态文件）。avc 27 类里 25 类是 untrusted_app（QQ / Bugly / Turing SDK 探测系统，被拒是对的），系统侧仍只有已知两类。之后 `bootctl set-active-boot-slot 1`、5555 恢复。
**发布（10-25）**：安装器 `0.2.0-rc.2` 用 rc2e 的 boot / super 重新打（`--payload out/rc2-1792913333/install-payload`，usb.img.xz `4a344301…`），留档 `out/rc2-1792913333/installer-0.2.0-rc.2-payload/`；发版说明改成进 OTA 的版本（`6464492`）；dev 推送、`dev:main` 快进、tag `v1.0.0-rc.2`；构建机 light 档 `release.sh --no-build`（正式）：验收报告过闸、REL-13 过、清单最后传，设备上 `curl` 清单 200、指向 `builds/…20261025….zip`（sha `ab710211…`）；r2-prune 删了 `…20260916…` 正式组与 `staging/rc2-1792421010/`。安装器 6 个文件经 `r2-upload.py` 传 `installer/0.2.0-rc.2/`（没用 `release-installer.sh --r2`：它会按新 HEAD 重造、sha 对不上）。GitHub pre-release 13 个附件字节数核过、不标 Latest。构建机已停（沙箱内回读 deallocated）。

**★★★ 2026-10-19 `1.0.0-rc.2` 第四次构建（rc2d，戳 `1792421010`）已传 staging，⬜ 未上机（用户说"重编 rc2"）**：仓库 dev `61c2c7c`（rc2c + #41 / #48 / #49 + 收尾）；`git archive` 到构建机 `~/v1-work/repo-rc2d/`（952 个非链接文件 sha 一致）。内核树应用模式打 0096、0112（0018 / 0089 照旧是 #110 误报），`--verify` 打不上 0 / fuzz 0 / 73 文件一致；配置相对 rc2c 只多 `USB_ANDROID_USB_STATE` / `USB_CONFIGFS_F_MIDI` / `USB_F_MIDI`；vmlinuz **`7636022f…`**、dtb `a1f7abfc…`（同 rc2c）；本机 `prebuilt-boot/` 已换（上一版备份 `out/prebuilt-boot-backup-rc2-1792312407/`）。设备树从 dev 工作树同步（工作树先补上不入库的 Histen / fcitx5 / gk3boot 输入），233 个文件一致。`release.sh --dry-run`（D32）第 2 步断言全过；镜像核对：cmdline 带 `fw_devlink.sync_state=timeout`、vendor `ro.hardware.egl=angle`、init.gaokun3.usb.rc 新规则、ueventd 的 android_usb 行、`android.software.midi.xml`、vendor sepolicy 6 处 `sysfs_android_usb`。`--no-build --stage-only` → `staging/crDroidAndroid-16.0-20261019-gaokun3-v12.11/`（9 个文件 HTTP 200；r2-prune 删了 rc2c 的 `…20261018…` 组和 `rc2-1791727610`）；开发机 payload `staging/rc2-1792421010/`（`payload.bin` 1396333699 字节、sha256 `641d4c1e…`）。boot.img `f60c65d2…`、super.img.zst `26f5c479…`。构建机已停（回读 deallocated）。留档 `out/rc2-1792421010/`。带系统的 U 盘安装器 `0.2.0-rc.2`（`release-installer.sh --payload out/rc2d-install-payload`，boot / super 从 R2 下回核过 sha；内核 = 发布清单 boot.img）：usb.img.xz 1590 MB `d9681c8b…`，产物 `out/rc2-1792421010/installer-0.2.0-rc.2-payload/`（SHA256SUMS 复核过）。发版说明文件表、戳已换成 rc2d；正文实测仍是 rc2c 的。**⬜ 下一步**（上机要用户同意、在场）：payload 下回 → `install-ota-local.sh --go`（装进 `_b`）→ `accept.sh --stamp 1792421010`（发版闸门那份临时关 5555）→ checklist B14、B23；#49 关开发者选项后重启；#48 游戏负载、s2idle、外接 DP；复测 rc2c 那批（WoWLAN、ADSP 睡眠、LPASS 宏）→ 改发版说明正文。

**（被 rc2d 取代）★★ 2026-10-18 `1.0.0-rc.2` 第三次构建（rc2c，戳 `1792312407`）已传 staging，✅ 10-18 已装上开发机 `_a` 并验收（A 档零 FAIL）；用户定：rc.2 先不发，能做完的做完再发；长跑测试不做**：本地 main `4bf5e42`
（= origin `8552266`〔#44 / #45 已合〕+ 本地合入 #46〔`c14ec82`〕+ 0105 修复 `c7e7c0c` + live loglevel=3 `2567f1d` + 文档；**均未推送**）；`git archive` 到构建机
`~/v1-work/repo-rc2c/`（947 个文件 sha 一致）。内核树应用模式打 0097–0110（0018 / 0089 是 #110 的已知误报），`--verify` 打不上 0 / fuzz 0 / 69 文件一致；
配置与 rc2b 无差别；vmlinuz **`2d6de830…`**、dtb `a1f7abfc…`（与 rc2b 同，没改 dts）；本机 `prebuilt-boot/` 已换（上一版备份 `out/prebuilt-boot-backup-rc2-1791727610/`）。
`release.sh --dry-run`（D32）第 2 步断言全过（kernel 逐字节、发布构建、版本 `1.0.0-rc.2`）；`--no-build --stage-only` → `staging/crDroidAndroid-16.0-20261018-gaokun3-v12.11/`
（9 个文件 HTTP 200，**这次带 `crdroid-manifest.xml`**：`release.sh` 找不到 `repo` 的问题已修 `a996be0`；r2-prune 删了 10-11 那组和 `rc2-1791720612`）；
开发机 payload `staging/rc2-1792312407/`（sha256 `6dbf37fe…`）。boot.img `c4bc8695…`。小文件与脚本留档 `out/rc2-1792312407/`。构建机已停（回读 deallocated）。
安装器 `0.2.0-rc.2`（`7274398`，GUI-15：用的就是这一版的 boot.img，`--artifacts` 核过）：✅ 本机造好、未上传（`release-installer.sh` 核对：版本 / 提交 / 内核 = 发布清单里的 boot.img `c4bc8695`、带 gk3-misc、无 M0 变体）；6 个产物留档 `out/rc2-1792312407/installer-0.2.0-rc.2/`，SHA256SUMS 全部复核（含 xz 解压后的 usb.img `a3234d3c…`）。上传 GitHub / R2 `installer/0.2.0-rc.2/` 要用户点头。
**✅ 10-18 22:1x–22:3x 上机**（用户说"继续"，在场）：payload 从 R2 下回本机核 sha → push → `install-ota-local.sh --go`（装进 `_a`、BCAB 撤回 `_b`、OneShot 走直连条目）→
`adb reboot` **46 秒起来**（`_a` = `1792312407`，enforcing，`uname -r` = `6.6.30-android15-8-…`）→ 验收过后 `bootctl set-active-boot-slot 0`（ESP default 跟成 `*-android-a.conf`）。
**A 档**：照常 PASS 45 · FAIL 2（A3 = 开发机持久化的 5555）· WARN 1（已知两类 avc：HID `country`、`com.android.se`）· SKIP 1（没给 --video）；
**发版闸门用的那份**（同 rc.1：临时关 5555、跑完恢复）`out/accept/1792312407-20261018-222504/report.txt` = **PASS 47 · FAIL 0 · WARN 1 · SKIP 1**。
追加实测：WoWLAN enabled（`disconnect`、`gtk_rekey_failure`）；`ath11k_scan` wakeup source 在工作（开机扫描 19 次）；四个 LPASS 宏 runtime suspended、autosuspend 100 ms；
**ADSP 睡眠计数 6**（rc.1 时代一直 0）；s2loop `WOW=1`（`f2c3979`）real 3/3 PASS、Wi-Fi 没断、declined 0（用户定长跑不做，停在第 3 轮）；
GAME-1 上机 5 项全符合源码排查（只有 `reports kernel level` 一条 WARNING、无 LTS 告警、`ksud` KMI = android15-6.6、`ro.kernel.version` = 6.6、`CONFIG_USERFAULTFD` 没开）；
GAME-3 全机 0 条 `execmem`；GAME-4：直连条目起的是 0085 内核（默认入口用同一个 boot.img，⬜ 下次自然重启核）；gk3pend 常驻、0 次崩溃；NET-7 国家码 CN；
NET-10 开机探测打到 hicloud 与 miui 两个端点都 204；fcitx5 在输入法列表里；root 进程 `kill -SEGV` 出 tombstone（a3c354c）。
⬜ 解锁后才能测的：应用进程崩溃出 tombstone、相机预览 5 分钟（#23）、fcitx5 实际打字。⬜ 要人与外设的：笔 / 手掌、USB 系列、关开路由器、音游延迟。
⬜ 长跑（用户定不做）：s2loop 100 轮、息屏过夜。发版说明草稿 `docs/relnotes/v1.0.0-rc.2*.md` 已写（`d4397d2`），等用户定稿。
**带系统的 U 盘安装器**（同 rc.1：`release-installer.sh --payload out/rc2-install-payload`，boot.img / super.img.zst 从 R2 staging 下回核过 sha）：提交 `d4397d2`、usb.img.xz **1590 MB** `2aaca2f3…`，产物 `out/rc2-1792312407/installer-0.2.0-rc.2-payload/`（SHA256SUMS 全部复核）；早先 `7274398` 那版不带系统（231 MB），不发。

**（被 rc2c 取代）****★ 2026-10-11 23:3x `1.0.0-rc.2` 重新构建完、已传 staging，⬜ 未上机（发布构建，戳 `1791727610`）**：仓库 `ce06b1b`（本地 main = origin `90f4185`〔PR #36–#40〕+ 另一会话 5 个本地提交 + 文档；`git archive` 到构建机 `~/v1-work/repo-rc2b/`，931 个文件 sha 一致）。构建机树先 `git apply -R` 旧 0048、旧版 0012（备份 `~/v1-work/rc2b/gaokun3.dts.before-migrate`），再打新 0012 + 0086–0095（fuzz 0）。⚠️ 打满的树上再跑应用模式会把 0018、**0089** 误报"打不上"（#110 盲区，0089 被 0090–0092 叠改），判据只看 `--verify`：打不上 0 / fuzz 0 / 55 文件一致。配置与上一版只差 `TYPEC_MUX_GPIO_SBU=y`。vmlinuz **`d5023d96…`**、dtb **`a1f7abfc…`**（核过：l9d `regulator-always-on`、usb_0 otg + role switch、无 `select-utmi-as-pipe-clk` / high-speed、EC `wakeup-source`）；本机 `prebuilt-boot/` 已换（上一版备份 `out/prebuilt-boot-backup-rc2-1791720612/`）。`release.sh --dry-run`（D16）第 2 步断言全过；镜像核对：`wowlan_triggers=disconnect gtk_rekey_failure`、`android.hardware.usb-service.gaokun3` + rc + VINTF、`hal_usb_default` 有 `sysfs_udc` 的 `lnk_file` 读、ueventd 两行、指纹 `…:user/release-keys`。`--no-build --stage-only` → `staging/crDroidAndroid-16.0-20261011-gaokun3-v12.11/`（**同名，覆盖了上一版**；8 个文件 HTTP 200，不动 OTA 清单，r2-prune 删 0 组）；开发机 payload `staging/rc2-1791727610/`（`payload.bin` 1396319495 字节、sha256 `56bbb86f…`）。boot.img `5ea003e6…`。小文件与脚本留档 `out/rc2-1791727610/`。构建机已停（回读 deallocated，机型 D16）。**⬜ 下一步**同下面那版的"下一步"（`--stamp 1791727610`），再加本段下一段列的 USB 系列与 #40 验收项。

**★ 2026-10-11 rc.2 推迟（用户定）**：下面这版 `1791720612` 构建时还没有 PR #40（`47eac92`：`wpa_supplicant.conf` 的 `wowlan_triggers` 由 ath11k 不支持、整行作废的 `any` 改成 `disconnect gtk_rekey_failure` ⇒ 休眠时不再断开 Wi-Fi、不给芯片断电，不再每 21 秒被 `WriteBufferAlarm` 叫醒；挂起加恢复 2.3 → 0.9 s）⇒ **不作为 rc.2 发布**，rc.2 从含 #40 的仓库重新构建。USB 系列 #36–#39（l9d 常开 / UCSI / EC 唤醒 / USB 端口 HAL）卡在补丁编号与 main 的 0083–0085 冲突、#39 的 sepolicy（`sysfs_udc` 链接）上 ⇒ **用户 10-11 定：rc.2 也等它们**，作者 rebase 到最新 main、从 0086 起顺延编号、修掉 review 意见后合入（review 意见 10-11 已发到各 PR：#37 的 0085 端口数上限、0089 迟到完成；#39 的 `r_dir_file(hal_usb_default, sysfs_udc)`、UDC 重建后重新 `WatchAttr`）。⚠️ 合 #37 时构建机树要先 `git apply -R` 旧 0048 和旧版 0012 再重放（脚本不查"已撤出列表却还打着"，CLAUDE.md 运维坑 4）。重新构建后照下面的"下一步"验收，另加 USB 系列的项（enforcing 下插电脑 USB adb、U 盘 / 显示器 / 充电器、device 角色带充电器 / 电脑多次真挂起、插充电器过夜的 EC 唤醒次数）和 #40 的项：`iw phy phy0 wowlan show` 已开；`scripts/s2idle/android-ath11k-s2loop.sh` 100 次以上 + 息屏一夜；dmesg 无 `failed to enable wow` / `failed to wakeup wow` / restart；休眠中关开路由器后能重连；关 Wi-Fi 时仍走断电路径。

**（以下是被推迟的那一版）2026-10-11 `1.0.0-rc.2` 已构建、已传 staging，⬜ 未上机（发布构建，戳 `1791720612`）**：仓库 `7f85271`（`git archive` 到构建机 `~/v1-work/repo-rc2/`，913 个文件 sha 一致）。内核 `~/gk3-kernel-72y`（D16）：`--verify` 打不上 0 / fuzz 0 / 53 文件一致；ReSukiSU 0001–0003 已在树；配置与 dev.11 只差 `TOUCHSCREEN_HIMAX_HX83121A_PEN=y`；vmlinuz **`a0ea6ea4…`**（含 0078–0085，uname 报 6.6.30）、dtb `d3448553…`（0080 改了 dts，与 dev.11 不同）；本机 `prebuilt-boot/` 已换（旧的备份 `out/prebuilt-boot-backup-k78/`）。crDroid 树：tree-fix [23] **这次才第一次打上**（`1791704075` 没有，见 gaming.md §1）。整包 `release.sh --dry-run` 增量 8 分 34 秒、第 2 步断言全过；镜像核对：指纹 `…:user/release-keys`、`out_latency_ms=10`、`blobcache.multifile=true`、vulkan=freedreno、vendor sepolicy 0 条 execmem。`release.sh --no-build --stage-only` → R2 `staging/crDroidAndroid-16.0-20261011-gaokun3-v12.11/`（8 个文件 HTTP 200，**不动 OTA 清单**）；开发机用的 payload 另传 `staging/rc2-1791720612/`（`payload.bin` 1396242813 字节、sha256 `ed848592…`）。小文件留档 `out/rc2-1791720612/`。**⬜ 下一步**：先把 adb 便利持久化（同 dev.11），下 payload → `install-ota-local.sh --go`（用户在场）→ `accept.sh --stamp 1791720612` → 手写笔 / 手掌（PR #27 / #30）、Arcaea 延迟、Duck 复测 → 过了再写 `docs/relnotes/v1.0.0-rc.2*.md` 并用 `--no-build` 发 GitHub pre-release。

### ① 发版阻断（v1.0-plan §2 的 B1–B7 与发版标准 G1–G11）

| 编号 | 一句话 | 状态 | 下一步 | 证据 |
|---|---|---|---|---|
| B1 · G1 | 发布构建关掉免授权 adb、5555、debuggable、维护者公钥 | ◐ | dev.1 起每版 `release.sh` 断言过、只读判据过（`ro.adb.secure=1`、`ro.debuggable=0`）。**V10 交互项 2026-10-11 在 `1791704075` 上做了能做的**（用户在场）：✅ 新电脑（临时生成的 adb 密钥）连上弹「允许 USB 调试吗？」、点允许后连上，勾「一律允许」才写进 `adb_keys`（测完已删回 2 行）；✅ 授权后 `adb shell` 直接 uid 0（`u:r:ksu:s0`，经 KSU）；✅「USB 调试」关了再开：断开约 1 秒后自己连回、不用重新授权；✅「无线调试」配对码 `adb pair` 成功、mDNS 找到连接端口、连上即 root；✅ `adb bugreport` 21 秒 5.9 MB；`logcat` 与 `logcat -b kernel`（`logd.kernel=true`）可用；`dmesg` 在真正的 `shell` 域被拒（`syslog_read`），但发布版 adb shell 经 KSU 是 root ⇒ 照样能用。⬜ 只能在全新装机上验：没有 5555 监听、adbd 不在跑、重启后 USB 调试复位成关（开发机持久化了 `persist.sys.usb.config=adb` / 5555，测不出） | 下文 V1 / V10；archive [`#v1-batch01`](archive/TODO-history-2026-10.md#v1-batch01) dev.1 段 |
| B2 · G2 | 系统与 OTA 用 AOSP test-key 签名 | ◐ | 按 D2 不换 key，披露已进发版说明草稿。**⬜ R2 写密钥轮换 + 域名自动续费**（9-29 发版时密钥在会话输出里显示过一次；Cloudflare 后台做，不用构建） | v1.0-plan B2；archive [`#v070`](archive/TODO-history-2026-10.md#v070) 末行 |
| B3 · G3 | `/data` 不加密 | ✅ 决定 | D3：不加密、披露（known-limitations 已写；roadmap 的"FBE 在管 /data"已改正，`7a10ae2`）。发版说明定稿时再核一遍措辞 | v1.0-plan B3 |
| B4 · G4 | 新用户的安装路径都没在真机上跑过 | ⬜ | **M4a**：外接盘写出厂布局 → U 盘启动（两个口各一次）→ 整盘清空 → 开机并 OTA 一次 → 双系统 → 默认的"清除数据重装" → 首次开机向导走一遍；Windows 脚本找一台真 Windows（否则按 D18 标预览）。安装器代码侧（批 3 前后端、S10 / S15、Windows 伴随工具）都已写、本机测过；live 镜像 10-06 本机重建、rootfs 体检过 | 下文「等安装器」；[`stage7-flutter-debian.md`](stage7-flutter-debian.md) §5.13–§5.15 |
| B5 · G5 | live 写盘期间电源键 / 合盖会关机或挂起 | ✅ 10-11 真机 | ✅ **2026-10-11 真机过**（用户在场）：开发机内置盘的 `gaokun3 installer` 换成 0.2.0-rc.1 的 live（`nvme0n1p3:/gaokun3/live.squashfs` + ESP `live/initramfs.img`，sha 与发布的 `rescue.squashfs` / `initramfs.img` 同；旧的 09-27 版改名 `.bak-0927` 留着）、oneshot 进去：5 个 sleep target masked、logind 6 个键 ignore、`systemctl suspend` → `Access denied`；短按电源键与合盖 logind 都记到（`Power key pressed short.` / `Lid closed.`）且被忽略，90 秒内 uptime 连续、`suspend_stats/success` 0 → 0。⚠️ 顺带发现：live 文本控制台被 `ucsi connector is not initialized yet` 刷屏（约 2 条/秒，开机起不停）⇒ 终端没法用，要先盲打 `dmesg -n 1`（见下文「安装器控制台刷屏」） | 下文「等安装器」 |
| B6 · G3 | 设置里"清除所有数据"静默失效 | ◐ | 统一启动入口接管：1.0 镜像默认动作模式 + 分派开（`ca55b5c`），QEMU exec-wipe 过；**用户定 E10 不测** ⇒ 已知限制里"别指望设置里的清除数据"那段保持原样，推荐"图形安装器 → 重新安装 + 清除数据" | [`boot-entry-design.md`](boot-entry-design.md) E10；下文 A5 |
| B7 · G7 / G8 | 用户默认配置下的待机与长稳没测过 | ◐ | 做深度待机（PR #80，下一版起）时在开发内核上拔线过夜实测约0.51 W（README待机一行）；发布构建默认配置下的下面几项仍没做。批 4（用户在场、拔线）：`allow_suspend=1` 放 8 小时（前后读 `qcom_stats`、`charge_now`、`suspend_stats`）、亮屏硬解 1 小时、72 小时狗粮；先用 V14 验 `scripts/perf/standby.sh` 的输出格式（✅ 插线部分 10-09）。**10-09 线索**：rc.1 息屏 25.7 小时 ADSP 累计只睡 5 秒，此时无 PCM、macro 全 runtime suspended，`LPASS_HW_MACRO` / `DCODEC` 投票仍是 4/4（怀疑相关，未验证）；用户反馈待机功率仍高（版本未确认，PWR-3 只在 rc.1） | v1.0-plan B7 |
| G6 · OTA-2 | OTA 新槽起不来时自动回落 | ◐ | 入口的启动计数与自动回滚已真机过（E5 / E6 / E8）。⬜ **E11：0.7.x → 1.0 那一跳的迁移演练**（那次 OTA 由旧 HAL 执行） | archive [`#v1-batch01`](archive/TODO-history-2026-10.md#v1-batch01) dev.5 段 |
| G9 · SEC-4 | SELinux：1.0 切 enforcing | ◐ **用户 10-06 定：切** | ✅ dev.9 经 OneShot 真 enforcing 开机：普查只剩两类已知上游缺口；显示 / 触摸 / Wi-Fi / 传感器 / 音频 / iris 15/15 / 前后摄 / A 档 44 / s2idle 11/11（含 USB 角色切换与回插）全过（`out/sel-d5-*`）。BoardConfig 默认改 enforcing，dev.10 起。✅ enforcing → enforcing OTA：rc.1 在 dev.10（enforcing）上经 OTA 装过（10-06，第二次 enforcing 下 OTA），dev.11 再过一次；OTA-5 手工分区 ESP 名字不标准的机器 enforcing 下 OTA 失败（INSTALL 已写改名办法） | BoardConfig.mk 注释 |
| G10 | 用户文档 | ◐ | README 中英、INSTALL 中英（`33faa9b`）、FAQ、known-limitations、1.0 发版说明草稿（`8ccdd35`）都有了。⬜ 定稿（V17）：草稿里十余处"⬜ 还没在真机上测过"逐条回填或删掉 | `docs/relnotes/v1.0.0-draft*.md` |
| G11 | 回归清单与安装器一致性 | ◐ | `release-checklist.md` + `scripts/accept.sh` 已用于 dev.1–dev.9。⬜ GUI-15：RC 时用 1.0 的 boot.img 重建安装器、`release.sh` 断言两边 sha 一致 | release-checklist |

**安装器触摸左右镜像（2026-10-11 用户报）** ⬜：开发机内置盘的 0.2.0-rc.1 live（cage `transform 270`，内核 = ESP `slot_b/Image` 的 0085 内核 `a8f7c90e`）里触摸**左右反**，画面方向是对的。
已查：Android 里触摸正常且没有任何坐标补偿（无 IDC、DT 无 `touchscreen-inverted-*` / `swapped-x-y`），内核报竖屏原生 X 0–1599 / Y 0–2559 ⇒ 内核坐标与面板原生方向一致；cage 只做旋转，旋转本身产生不了镜像 ⇒ 某一层多了一次翻转。
09-25 M0（旧内核 + 当时的 live）用户目视"方向对、触摸准"。⬜ 下一步：进 live 取 `wlr-randr`、`libinput list-devices`（校准矩阵）、`libinput debug-events` 点四角的原始坐标；对照换回旧内核 / 09-27 的 live（`.bak-0927`）各一次，判是内核还是 live 用户态引入的。

**安装器控制台刷屏（2026-10-11 B5 真机时发现）** ⬜：live 的文本控制台被 `ucsi_huawei_gaokun.ucsi …: ucsi connector is not initialized yet` 刷屏（约 2 条/秒、开机起不停，13 分钟 1686 条；UCSI 老缺陷），
终端输入照收、但看不见 ⇒ 用户要先盲打 `dmesg -n 1`。原因是 live 的 cmdline 用 `loglevel=4`（`dev_err` 是 3 级 ⇒ 上控制台）。改法：`loglevel=3`，三处一起改 —— `scripts/live/build-usb.sh:156`（U 盘）、
`scripts/live/installer-lib.sh:1556`（安装器写救援条目）、`device/huawei/gaokun3/bin/gaokun3-ota-postinstall.sh:252`（OTA 重写救援条目），外加 `tools/gk3boot/test/postinstall/run.sh` 的期望值；要随下一次 ROM / 安装器构建走。

### ② 1.0 应做（v1.0-plan §3 / §4）

**已进 dev.9 镜像、等上机验**（判据写在下面各节）：

| 编号 | 一句话 | 状态 | 下一步 | 证据 |
|---|---|---|---|---|
| NET-4 · B20 | 稳定的 Wi-Fi MAC | ✅ rc.1 | dev.9 上查出原修法没生效（随机化总开关关着时框架不碰 MAC）⇒ `b2c8c44`：开机在 Wi-Fi 打开前把 `wlan0` 写成序列号派生值；手动验证过、挂起 11 轮固件重载后地址不变。✅ rc.1 验收里过了"稳定MAC"（上面总表rc.1 段），rc.1 发版说明已写 | 下文「批 2 · 网络」 |
| NET-2 | 热点与 Wi-Fi 同时开 | ◐ | dev.9 `wlan1` 预建就位。⬜ 用户在场：连着 Wi-Fi 开热点，Wi-Fi 不断、另一台手机连热点能上网（顺带收掉 #11 的尾巴） | 同上 |
| NET-7 / NET-10 | 热点 5 GHz / WPA3 / 国家码；联网探测备用地址 | ◐ | ✅ 10-18：国家码 CN；NET-10 开机探测打到 hicloud 与 miui 两个端点都 204（overlay `com.android.networkstack.overlay` 生效；settings 里的两项是 null，正常）。⬜ 手机连 5 GHz / WPA3 热点 | 同上 |
| NET-1 | 息屏时 AP 断开后自动重连（软件 PNO） | ◐ | DeviceConfig 那道门已去（`e3c5ba3`，tree-fix [18]，dev.2 起）。⬜ V13 实测 | 下文 V3 / V13 |
| AV-4 / AV-10 / AV-16 | USB 声卡、有线耳机麦、裁掉示例设备 | ◐ | dev.9 静态项在（`IModule/usb`、cmdline 的 `snd_usb_audio.index`）。⬜ 插 USB 声卡（port1）+ 开机前插着冷启动一次；带麦耳机录 5 秒；`dumpsys media.audio_policy` 静默核对 —— 全部不放音 | 下文「批 2 · 音频」 |
| PWR-16 | Parts 待机开关 | ◐ | ⬜ 开关在「电池」页、拨关拨开属性随动、重启保留、开发机以前 setprop 的 0 显示成"关"；enforcing 下 avc 为 0 | archive `#v1-batch01`「批 2 ROM 侧」 |
| LIVE-5 · PERF-6 | power_profile；后台 cpuset 限到 0-3 | ◐ | dev.9 两个后台 cpuset = 0-3 ✅。⬜ `dumpsys batterystats` 看到 2 个 policy、21 / 18 步、容量 4483，「电池用量」页有排行；`game-perf.sh` 前后帧率不降；timeInState BPF 没加载那半条（下次自然重启抓 bpfloader 日志） | 同上 |
| PWR-14 · D17 | EC 充电上限 | ✅（按决定） | 代码随版本走、默认关；dev.9 上 IChargingControl 与四个 EC 节点属主就位。不做写 EC 的实测；两点没证据（停充时是否旁路供电、EC 对 start=0 / end=100 的行为）发版说明草稿写的是"EC 到达上限时具体怎么表现还没核对" | 同上 |
| B21 · DISP-14 | SLPI 崩溃自愈后传感器全丢 | ◐ | 恢复服务 `gaokun3_sscrecover` 已进 dev.9。⬜ V18：征得同意后让 SLPI 崩一次（唯一允许碰光感的地方），看恢复日志、约 60 秒内 accel 回来、自动旋转跟手 | 下文 V18 |
| DISP-15 · HW-6 | 键盘关着时拔插仍关；去掉 vibrator example | ◐ | dev.9 已无 vibrator HAL ✅。⬜ 用户拔插键盘盖；设置里"振动和触感"一项消失 | 下文 V18 |
| BKUP-5 · D15 | 备份默认 Seedvault；默认时区上海 | ◐ | 只在全新 `/data` 上看得到 ⇒ 并进 B4 的新装机验收；"从没设过时区的老机器 OTA 后变上海时间"写进发版说明 | 下文 V18 |
| DISP-3 · D10 | 中文输入法 fcitx5-android | ◐ | APK 已入构建（`f2fd400`），随rc.1发布（rc.1验收：是系统应用、签名吻合）。⬜ 上机：设置里能启用、拼音出字、实体键盘中英切换（与 Android 自己的 Ctrl+Space 冲不冲突）、候选窗 | 下文 B12a |
| PWR-4 · USB-1 · USB-2 | USB-C 口异常 / 插电脑落成我方供电时弹通知；host 时不再每秒重绑 UDC | ◐ | 代码 dev.2 起在（`5dfe7dd` / `ccd54f9` / `d511d18`）。⬜ V11 里复现时看通知（插 Mac 落成我方供电、待机后回插坏口）；~~小改进：接收器改 directBootAware~~（10-19 核：10-05 起 `UsbPortNotifier` 与 `BootReceiver` 都已 `directBootAware`、收 `LOCKED_BOOT_COMPLETED`，`parts/AndroidManifest.xml:149-178`） | 下文 A6 |
| PWR-5 · STOR-1 · STOR-2 · STOR-5 | port0 有下游时保持 host；U 盘挂载；`/data` 保留块 | ◐ | STOR-5 dev.1 实测 ✅（保留块 32768 / gid 1065）。⬜ V11：U 盘 FAT32 / exFAT 插 port1 出现在「文件」；port0 插外设息屏亮屏 5 次仍是 host；开机插着 U 盘再换插 PC 时 adb 能否回来 | 下文 V11 |
| BATT-1/2/3 · DISP-1 | 电池按位解码（0072）；EC 初始 LID（0073） | ◐ | dev.3 起在。⬜ 用户在场：插电脑 USB 口带负载放电采 `ec_raw` / `current_now` 正负 / `capacity_level`。✅ 合盖开机（`initial lid state: closed`，开机结束后熄屏，开盖唤醒）、三种合盖姿态（合盖 SW_LID 1、开盖 0、翻到背面没有事件）⇒ 默认合盖熄屏：`config_lidControlsSleep`，已有的安装由 Parts 的 `LidBehaviorDefault` 改一次；恢复时的状态见 0168 | archive `#v1-batch01` dev.3 段 |
| PWR-13 · LIVE-13 · APP-17 | 内核配置项 | ◐ | dev.9 看到 THERMAL_STATISTICS、ARMV8_DEPRECATED ✅。⬜ PWR-13 另一半：SKIN 温度其实是芯片温度，要标定 EC 温度通道 | 同上「批 2 ROM 侧」 |

**还没做、要用户在场的**（v1.0-plan 批 1 / 批 2 的验收项）：

| 编号 | 一句话 | 状态 | 下一步 |
|---|---|---|---|
| DISP-2 · D9 | 接键盘横放时竖屏 App 把整屏转竖 | ⬜ | `wm set-ignore-orientation-request` 运行时 A/B，倾向回 Android 默认 |
| AV-1 | 录屏 / 录像从没产出过文件 | ◐ | Aperture前后摄都录出过（后摄1080p，30 fps）；Recorder录屏还没测 |
| AV-12 | 前摄方向没目视（DT 是 0） | ✅ dev | 设备树的0 / 180是对的（按横屏取景），HAL少算了转到竖屏自然方向的90°，所以前后摄画面都转了90°；PR #83在 HAL 里补上，实机前后摄预览和照片都正，下一版起（见下文 A7） |
| APP-4 | 银行 / 支付 App 与 Play Integrity 从没测 | ⬜ | 支付宝、微信支付、招行、工行、云闪付、12306 + 一个 Integrity 检测 App |
| INST-18 · D19 | 开机菜单不接键盘能否操作 | ⬜ | V15 实测音量键 / 电源键；再定 timeout |
| V12 | 亮屏后方向、陀螺仪、亮度 | ⬜ | 见下文 V12 |

**还没做、可以无人值守的**：

| 编号 | 一句话 | 状态 | 下一步 |
|---|---|---|---|
| V0 余项 | 批 0 只读核查的子项 | ✅ 10-18 | 开发机 `1791704075` 只读：**lpdumpd** tombstone_10（10-04 22:26）是 `SlotSuffixForSlotNumber` 的 `slot_number == 0 \|\| 1` 检查失败 —— 有人传了非 0 / 1 的槽号（元数据有 3 个槽）；现在 `lpdump`、`lpdump -s 1` 都正常、无新 tombstone。**loopback** 的 SIGSEGV（tombstone_49）是 14:12 重编之前那版写 `corr[24096]` 越界一个（x20 + 0x2F100 = 分配长度），入库版有 `z - a > RATE/2` 跳过、走不到（`4bf5e42` 顺手改了过时的头注释；没放音重跑）。**Histen** 关着时音效 HAL（pid 742）只映射我们的 `libgaokunhisteneffect.so`、没有 `libhw_histen_processing.so` ⇒ effects/README「关着不加载任何库」成立。`mEnablePalmRejection: 0`、`getLidBehavior=LID_BEHAVIOR_NONE` 复核如旧 |
| V2 · REL-7 | v0.7.1 的 GPL 源码补录 | ◐ 10-18 | ✅ `v0.7.1-alpha-sources.md` 文末改成 "Recorded afterwards"：buildbot 层 20 个提交（iris 树 reflog 09-28 后没动）、`--verify` 44 / 1（camera.dtsi 后来被动过）、KernelSU `b2ac2fc8`、ROM 清单 `v0.7.1-alpha-crdroid-manifest.xml`。"没跑成"= 非交互 ssh 的 PATH 没有 `~/bin/repo`（`release.sh` 已改，rc2c 起清单随 staging）。⬜ 补传 release 附件 `out/v071src/kernel-base-patches.tar.gz`（要用户点头，同 V7c） |
| V5 | "待构建机核实"的源码出处 | ✅ 10-18 | 六条都按构建机源码核完、引用处已改（[`source-audit-20261018.md`](source-audit-20261018.md) 任务 A）。要点：configfs 写 UDC 失败**不留 pending**，adb 回来靠 adbd 重试 ⇒ V11 盯 `sys.usb.adb.disabled` 回 0；Settings 文案 7 处对不上（「关于本机」「Build 号」「提示音和振动」「Bug 报告」…）已改；SF `--latency` 在 crDroid 16 只给 **64 帧** ⇒ `game-perf.sh` 改 `sleep 0.2` |
| V14 | 采样脚本 | ◐ | ✅ 10-09 rc.1 插线验 `standby.sh` once / start / status / stop / pull 与摘要格式；`adb reconnect device`（adbd pid 不变）后采样器照跑；新增 `wl` / `ws_act` / `ws_top` / `lpass` 字段与 qcom_stats 睡眠时长占比。⬜ 真拔 USB 后存活 / `ev=wake` 行（随 B7 一起）；`game-perf.sh` 图层名与 64 帧窗口（源码已核，上机数一次行数应 ≤ 64） |
| HAL 误报 Windows | 手工删过 Windows 的机器 ESP 上留着 `bootmgfw.efi` ⇒ Parts 显示"重启到 Windows" | ✅ 10-18 | 开发机 `vendor.gaokun3.bootentry.windows=0`（10-06 删残留之后）；选"文档写清"：INSTALL 中英双系统一节加了原因与办法（删 ESP 上的 `EFI/Microsoft`），英文页名顺手改成 Parts 实际的 *Startup options* |
| 执行端余项 | fastboot 执行端 | ⬜ | 主菜单的电源键确认路径（Reboot to Android）、`flash` 都没真机验过 |
| 批 4 测量 | 只在 1.0 前做一次（v1.0-plan 批 4） | ⬜ | PERF-2 游戏基线 + UBWC A/B（B5b）、APP-5 熄屏推送延迟、AV-3 蓝牙耳机、AV-8/15 播放回归与 Histen 插耳机、AV-13/14、LIVE-7 60/120 Hz 电流、PERF-5、DISP-4 DP 输出、DISP-5/11/12、HW-10 人脸解锁、BATT-5/6/10、OTA-15、APP-19 QQ 平板模式、BKUP-10 —— 多数要用户在场或要外设 |

### ②′ 游戏环境（2026-10-10 起；总览 [`gaming.md`](gaming.md)，过程 [`duck-detector-plan.md`](duck-detector-plan.md)）

| 编号 | 一句话 | 状态 | 下一步 | 证据 |
|---|---|---|---|---|
| GAME-1 | 向应用报告的内核版本 `patches/0085`（系统组件看到真实版本，设置 → 系统可改；rc.2、rc.3 是全局覆写） | ◐ | 用户 10-11 定进发版，默认值不变。✅ 11-02 rc.3 ROM + 测试内核：A17 全系统跑真实 `uname`，开机、netd、Wi-Fi 正常，崩溃和 SELinux 拒绝与旧内核相同；⬜ ROM 构建后：`vendor_init` 能读 `gaokun3_app_kernel_prop` 并写节点，Parts 能写属性，普通应用读不到这两个属性，应用里 `uname()` 和 `os.version` 一致（logcat 没有 `Could not refresh os.version`），`ro.kernel.version` 为空；⬜ 带 ACE 的游戏和英雄联盟手游进对局；⬜ 构建机 grep `ro.kernel.version` 的其他读者（tree-fix [37]）；⬜ 发版说明定稿 | gaming.md §4.1 |
| GAME-2 | netlink 隐私边界 `patches/0084` | ◐ | 开发机 10-11 两条泄露都 `EACCES` ✅。⬜ 随下一个发版内核走 `--verify` + A 档；⬜ Wi-Fi / 热点 / `ip neigh`（root）照常 | gaming.md §1 |
| GAME-3 | `system_server execmem` 去掉（swangle sepolicy 移出、`…EXECMEM_FOR_SWIFTSHADER` 注释掉） | ◐ 已提交 `53b34ff` | `1791704075` 实测 Denied ✅。✅ 10-18 `1792312407` enforcing 下全机 avc 0 条 `execmem` | BoardConfig.mk 注释 |
| GAME-4 | 0085 内核固化进 `boot_b`（现在只能走直连槽条目） | ◐ 10-18（`1792312407` 装进 `_a`，直连条目起来 `uname -r` = 6.6.30；`boot_a` 与 ESP `slot_a/Image` 出自同一个 boot.img ⇒ 默认入口应同样是 6.6.30 —— 推断，⬜ 下次自然重启走默认入口时核一次） | **10-11 证实**：走默认入口起的是 7.2.9（`boot_b` 里没换）。✅ 10-11 rc.2：`prebuilt-boot/` 换成 `a0ea6ea4`（含 0079–0085）、`release.sh` 断言 boot.img 的 kernel 与它逐字节同。⬜ 装上 rc.2 后默认入口起来 `uname -r` = `6.6.30-android15-8-…` | gaming.md §4.3 |
| GAME-5 | 原 `patches/0086`（KernelSU 子目录）进正规入口 | ✅ 10-11 | 已改名 `patches/resukisu/0003-…`（`kernel-setup-resukisu.sh` 打、`release.sh` 核）；本机对 pin `--check` 过。✅ 10-11 构建机：0001–0003 都已在树里、脚本幂等跳过、rc.2 内核编过；树上另一处手改（`adb_root.c` 的 include）撤掉后照样编过 ⇒ 不需要（`patches/resukisu/README.md`） | gaming.md §4.2 |
| GAME-6 | `game-mode.sh` 10-11 审查修复 + 上机 | ✅ 10-11 上机 | ✅ hide / unhide（数据保留）、`on 1` 走 TCP 与 USB 都自动恢复、开机兜底、noforge / forge。上机挖出的真凶：定时器在 adbd 的 cgroup 里被 init 一起杀（gaming.md §2 第 2 条）。✅ 10-18 查明 `unhide` 的 `install-existing` 必然失败：单用户第三方应用 `uninstall --user 0` 是整包删除、`-k` 只留数据（`DeletePackageHelper.java:494-504`），备份那条才是主路径 | gaming.md §2 |
| GAME-7 | 验收：英雄联盟手游 / Wild Rift 进对局 | ⬜ | `on` + `hide` 后测（先 forge，不行再 noforge 对比）；记卡在哪一层 | #17、gaming.md §3 |
| GAME-8 | 低延迟音频 0083 发版构建复测 | ◐ | `1791704075` 上 FastMixer `mixPeriod=10 ms` ✅；⬜ Arcaea 端到端延迟、满载欠载复测 | gaming.md §1 |

### ③ 等用户 / 等硬件

| 编号 | 事情 | 状态 | 要什么 |
|---|---|---|---|
| D5 · SEC-4 | 1.0 切不切 enforcing | ✅ 用户定切（10-06） | 见 ① 表 G9 |
| V7b | 1.0 预不预装 ReSukiSU 管理器 APK | ⬜ | 用户定；预装就改 known-limitations / FAQ / INSTALL 里"先装管理器"那句与 `gsf-android-id.sh` 的提示 |
| V7c | issue 表单渲染核对；v0.7.1 GPL 附件补传 | ⬜ | 仓库已推送 ⇒ 去 `issues/new/choose` 把 7 个表单打开看一遍；补传 release 附件要用户点头 |
| B2 R2 轮换 | 见阻断表 B2 | ⬜ | 用户在 Cloudflare 后台做 |
| B12 · D8 | OTA 与下载的长期基础设施（R2 / Worker 反代） | ⬜ | 用户定方案，改完在国内网络实测下载 |
| 原 D7 | v0.2.0-alpha 的 R2 产物删不删 | ✅ 10-11 删 | 并入 R2 滚动机制（下文 D7） |
| 原 D5 | PR #3 的回复 | ⬜ | 用户定措辞 |
| 原 D6 · 上游 | `drm_crtc` BUG_ON 报 dri-devel；`docs/upstream/` 5 份内核补丁稿；libcamera `0002`（hi846）/ `0004`（ov13b10）；camcc RCG shared | ⬜ | 对外发言，用户点头 |
| T2 | Google 未认证 | ⬜ | 用户自己拿 Android ID 去登记（工具与文档就位） |
| A6b · NET-11 | issue #2 WPA3 连上即断 | ⬜ | 等报告者在当前版本上跑 `scripts/wifi/wpa3-probe.sh` |
| #12 / #15 | 抖音（已不复现）/ 英雄联盟秒退 | ⬜ | 要 tombstone / logcat |
| A6 硬件 | U 盘、纯充电器、扩展坞各跑一次 `scripts/usb/ucsi-snapshot.sh` | ⬜ | 用户手边的外设 |
| 外设清单 | USB 声卡、带麦耳机、DP 显示器、U 盘、蓝牙耳机、外接 NVMe、带 Windows 的 MateBook E Go | — | 分别对应 AV-4 / AV-10 / DISP-4 / STOR-1 / AV-3 / B4 / GUI-7 |
| 开发机清理 | `/data/local/tmp` 约 2.8 GB 历次测试内核（现状未核）；Ubuntu 救援分区（24.6 GiB）回收 | ◐ 10-19 | ✅ ESP 上的 `va39w` 实验内核与 `gaokun3-va39w.conf` 已删（用户"能做就做"；备份 `out/esp-backup-20261019-va39w/`，sha 核过；ESP 可用 53 → 69 MB）。`/data/local/tmp` 现 2.2 GB，混着还在用的（测试视频、`esp-bak`、`boot.img`、`playprobe/`），`/data` 只用 23% ⇒ 不动；Ubuntu 救援分区回收等用户 |

### ④ 1.0 之后（v1.0-plan §7 + 本文件的长期项）

指纹 T6（③ client driver、④ Android HAL；APP-18 支付指纹）· 硬件视频编码 A2 · BT SCO 通话 ·
A6待机后回插复测（0086之后）与 B14 息屏 USB adb 原生化 · WoW 收包唤醒（#88、#89）在ROM构建上、enforcing下上机核对 · GPU SMMU 中断根治 B6 · 相机变焦 / EV / 画质（T3、A7 余项）·
游戏音频基础延迟（B25 尾巴 / AV-7）· NET-4 按网络的随机 MAC（连同一次性迁移）· Power HAL 真实现与 RT uclamp · 120 Hz 无缝降频 ·
quota / projid / casefold · FUSE passthrough · zram · Wi-Fi Direct · 海外 regdomain · B0 构建机的树换成本仓 checkout ·
B3 余项（退役 ESP 派生文件、AVB）· tinymix 的 vendor 变体 · #12 的 39 位地址空间测试内核起不来（搁置）· iris 零碎（`venus_compat_gfmt=N` 复测、HAL 的 POLLPRI 空转、SYS_ERROR 后 `invalid uc_region`；seek 到更低分辨率后裁剪不更新：flush 之后固件不发序列事件；字节缓冲 + 遗留数组模式下分辨率变大时报 `tried 5 realloc and failed`：DecodeComponent 不上报尺寸变化）·
#16 尾巴（`ath11k_mhi_start` 失败不 unprepare，每次漏约 4 MB DMA，`mhi.c:446-450`）· recovery、原神渲染上限（见 E 节）。

**ART 的 GC（10-19 发现，下一版再定）**：发版内核 `# CONFIG_USERFAULTFD is not set`（`1792312407` 上 `zcat /proc/config.gz` 实测；`scripts/kernel-config-android.sh` 里没提过它，
不是有意关的），而镜像里 `ro.dalvik.vm.enable_uffd_gc=true` ⇒ ART 用不上 uffd 的 CMC 收集器，实际是 *Background concurrent copying GC*（logcat 实测）。
功能没问题，只是少一项内存 / 停顿上的优化。要开的话：`kernel-config-android.sh` 加 `USERFAULTFD=y` 并断言、重编内核、跑 A 档 + 一段游戏对比；
⚠️ 配上 0085 后 ART 的 UFFDIO_MOVE 探测会失败、退回 COPY 模式（`docs/source-audit-20261018.md` 任务 B #9b），仍是 CMC，只是少了 MOVE。

### 旧编号索引（原第一 / 第二梯队表与对账表里的号，原表见 archive [`#tier1`](archive/TODO-history-2026-10.md#tier1) / [`#tier2`](archive/TODO-history-2026-10.md#tier2) / [`#user-actions`](archive/TODO-history-2026-10.md#user-actions) / [`#paused`](archive/TODO-history-2026-10.md#paused) / [`#reconcile`](archive/TODO-history-2026-10.md#reconcile)）

| 号 | 现状 | 去处 |
|---|---|---|
| S1 待机默认值 | ✅ 镜像默认 1（`release.sh` 断言），开发机持久 0；1.0 起 Parts 有开关（PWR-16） | — |
| B15 全新安装丢触摸参数 · B17 find-device | ✅ 2026-09-23 | — |
| B16 TCP 缓冲 · B18 remoteproc · B22 NTP · B23 zap shader 随 live 发 | ✅（v0.6.3 / v0.7.0 / v0.7.1 验收） | — |
| B19 没有回落槽 | VAB 的设计；回落由统一启动入口的自动回滚承担（E8 过） | G6 |
| B20 MAC 每次开机都变 | ✅ rc.1 | NET-4 |
| B21 SLPI 自愈后传感器全丢 | ◐ 已进 dev.9 | ② 表 B21 |
| B24 内置麦克风 App 录音 | ✅ v0.7.0 验收 A1–A5（#127 §7） | — |
| B25 音游播放丢块 | ✅ 0069 随 v0.7.1 发、出声音游过（#130 §7）；播放回归 = AV-8（批 4），基础延迟 = 1.0 之后 | — |
| T3 相机画质 · T4 息屏 USB adb | ⬜ | ④（T3）；B14 |
| T5 平板声明 | ✅ `tablet`；QQ 平板模式 = APP-19 | 批 4 |
| T6 指纹 | ◐ TA 已加载（#125）；0050 已移出发布内核 | ④ |
| T7 扬声器增强 Histen | ✅ 随 v0.7.0 发（0068） | — |
| R 下一版发不发 | 1.0 发版要用户点头 | — |

---

### ▶ 1.0 批 0/1 已合并、待构建与验证（2026-10-05）

> 过程记录见 [archive/TODO-history-2026-10.md#v1-batch01](archive/TODO-history-2026-10.md#v1-batch01)（本节原文，含 dev.1–dev.9 逐版装机记录、「批 2 ROM 侧」五项、已合并改动表、V0–V18 全文、「等安装器」全文）。

**结论**：v1.0-plan 批 0 / 批 1 的 12 组改动 + 安装器组 10-05 合进 main；批 1 剩余 14 项（`27b50f8`…`bb8abf2`）、批 2 内核 / ROM / 网络 / 音频、统一启动入口 S7 / S9 / S11 / S15 随后合入。
九个 1.0 发布构建，每版一行：

* **dev.1** `1791138567`（10-05 03:10，`_b`）：第一个发布构建，V1 / V7a / V9 过；KSU 的 adb root 在 `ro.debuggable=0` 下照样生效（已写进 known-limitations 的 root 一条）；`release.sh` 的 pipefail 静默退出已修（`bed602c`）；`repo manifest -r` 没跑成（V2）。
* **dev.2** `1791144960`（04:43，`_a`）：批 1 剩余 14 项；postinstall 删无引用的 recovery ramdisk；smmustall 心跳缺失 → `c1d11dc`（dev.3 验过）。
* **dev.3** `1791151679`（06:32，`_b`）：批 2 内核 0072 / 0073 / 0074 实测；查出 AOSP `init.rc` 把 hung_task 写 0 → `0030c79`（dev.4 起 120）。
* **统一启动入口两道门槛**（06:14–08:3x）：E3 / E4 / E5 / E6 / E7（观察与动作模式，BCB 不消费）真机过，日志 `docs/hw/gk3probe-e3-…`、`gk3boot-e4-…`、`gk3boot-e5-e6-…`、`gk3boot-e7-…`。
* **dev.4** `1791156793`（08:1x，`_a`）：SEC-9 内核 v7.2-rc2 → v7.2.9；iris 15/15、相机、10 次真挂起过。
* **dev.5** `1791163499`（09:5x，`_b`）：S9 Android 侧入口三档闭环；**E8 真 OTA 回滚演练通过**（`docs/hw/gk3boot-e8-20261005.txt`）。
* **❌→✅ 更正 / 7.2.9 黑屏根因（结案）**：不是屏幕硬件故障，是 v7.2.y stable 的 `5de981b7db`（Revert "drm/msm: dsi: fix PLL init in bonded mode"，v7.2.6 进）撤掉了双 DSI 绑定 PLL 的修复 ⇒ `patches/0076` 撤回它（`ee81f67`），SEC-9 重新应用（`2333e2d`）。判据：亮屏时 `himax-spi-ts` 中断 ≈120/s = 面板在扫描。"判为面板 / 排线故障"作废。
* dev.6（`1791173750`，#16 内核）在本文件里没有单独的记录段（见 CLAUDE.md 的历史状态）。
* **dev.7** `1791185227`（16:0x，`_a`）：7.2.9 + 0076 + S7 执行端，显示恢复、相机过。**Wi-Fi 节拍（结案）**：s2idle 第 7 轮起 Wi-Fi 不重连 = 测试每 12 秒一轮撞上框架自动重连限速（约 50 秒内 5 次后停发约 6 分钟），s2loop 加 PACE=75（`5c8f6f8`）后 11/11。**iris 13/15（结案）**：测试中途关了屏，dev.9 亮屏 15/15、错误行 0。
* **fastboot 执行端与 BCB 分派真机通过**（17:0x–17:2x）：E6 执行端 + E7 分派六步，`docs/hw/gk3boot-e6exec-e7dispatch-20261005.txt`；执行端版本串的 `-dirty` 已修（`29f4614`）。
* **批 2 ROM 侧**五项：PWR-16 `4568f99`、PWR-14 `f242063`、LIVE-5 `ceb1167`、PERF-6 `2f12e43`、PWR-13 / LIVE-13 / APP-17 `f6d1001`（上机判据见 archive 本节开头）。
* **dev.8** `1791199356`（19:5x）：编过、`release.sh --dry-run` 过，未装机（用户：别测了、继续推进）；1.0 起默认动作模式 + 分派开（`ca55b5c`）。
* **dev.9** `1791215652`（10-06 00:0x 编，15:0x–16:0x 装 `_b`）：dev.8 + 批 2 音频 / 传感器与默认值 + 内核去掉 0050（D21，`912f22c3`）；**`install-ota-local.sh` 的 S11 新第 4 步首跑**；回归全过（见总表）；NET-4 更正（`b2c8c44`）；HAL 误报 Windows（见 ② 表）。

**V0–V18 现状**（全文与判据见 archive 本节）：

| V | 内容 | 状态 | 剩什么 |
|---|---|---|---|
| V0 | 批 0 只读验收 | ◐ | `accept.sh` 已在 dev.1–dev.9 跑；子项见 ② 表"V0 余项" |
| V1 | B1 关键源码 grep（不过就不编） | ✅ | dev.1：`ProductNotDebuggableInUserdebug` 只影响属性，init 仍允许 permissive；`GAOKUN3_DEV_BUILD` / `GK3_VERSION` 能进 Kati |
| V2 | GPL 补录与构建脚本 | ⬜ | 见 ② 表 |
| V3 | NET-1 第二道门 | ✅ 代码 | `e3c5ba3`；实测归 V13 |
| V4 | PWR-4 止损通知 | ◐ | `5dfe7dd`；坏口复现时看弹窗（V11） |
| V5 | 其余"待构建机核实"的出处 | ✅ 10-18 | 见 ② 表 V5 行 |
| V6 | 编译 + 发布产物断言 | ✅ | dev.1 起每版；REL-5 版本属性 `ro.vendor.gaokun3.version`、`ssc_test` 的 toggle 模式都已写 |
| V7a | 开发机迁移（adb_keys、持久化 adb 属性） | ✅ | dev.1 之前 |
| V7b | 预装管理器 APK | ⬜ | 等用户（③） |
| V7c | 对外动作：表单、附件 | ⬜ | 等用户（③） |
| V8 | 新 HAL 前的同会话开关实验 | ✅（被 V9 覆盖） | toggle 模式已写；PWR-3 新 HAL 已随 dev.1 上机，亮灭屏 20 次"使能 / 停用"各 21 次、读数正常、无看门狗误报 |
| V9 | 新构建装后检查 | ◐ | dev.1 过（传感器功耗、STOR-5、NTP、usbrole、avc）。剩：亮灭屏 ≥50 次 + lights 的 errno 记录、二次开机不再 `Setting reserved block count`、B21 被动检查、"连上网到第一次对时"的耗时 |
| V10 | B1 上机验收（交互） | ◐ | 见阻断表 B1；另：开发构建 `adb root` 时桥接只报 EEXIST / EBUSY；发布构建上 `dmesg` / `logcat -b kernel` / `bugreport` 能不能用 |
| V11 | USB / 存储 / usbrole（用户在场，全程 TCP adb） | ⬜ | STOR-1 两种格式插 port1；PWR-5 port0 插外设息屏亮屏 5 次；开机插 U 盘再换插 PC 看 UDC（判据：切回 device 后 `getprop sys.usb.adb.disabled` 为 0，2 秒内 `cat /config/usb_gadget/g1/UDC` 非空 —— 内核不留 pending，见 V5）；PWR-4 坏口时 `vendor.gaokun3.usbrole.broken=1`。STOR-3（port0 = 靠近电源键的口）✅ |
| V12 | 亮屏后方向、陀螺仪、亮度 | ⬜ | 旋转及时、三种陀螺仪情形、融合传感器；亮度只有出现可见后果才做内核侧根治（`himax_bl_update_status`） |
| V13 | NET 实测 | ⬜ | 息屏醒着时 AP 断开再恢复 2 分钟内重连；`allow_suspend=1` 定时唤醒后重连；软件 PNO 的耗电；`ntp.aliyun.com` 海外可达（要海外用户） |
| V14 | 采样脚本与 A22 | ⬜ | 见 ② 表；A22 = 在 USB adb 下真跑一次 `cmd wifi start-softap` |
| V15 | 文档里"待补 / 未验证"的事实 | ⬜ | 进 UEFI / 启动菜单的按键与无键盘操作；前摄方向；WPA2/WPA3 混合模式；QQ / 微信平板登录；R2 国内直连速度；BATT-1 低功率电源显示"充电" |
| V16 | 救援系统里 FAQ 写到的行为（要重启进救援） | ⬜ | systemd-pstore 会不会把 EFI 里的记录挪进内存并删变量；`ssh root@gaokun3-live.local`（avahi）；`gk3-boot-android` 写 EFI 变量 |
| V17 | 1.0 发版说明与"从 1.0 起"的说法 | ◐ | 草稿 `8ccdd35`（中英）；定稿前在已验收的发布构建上确认 `ro.adb.secure=1` / `ro.debuggable=0` / 没有 5555 |
| V18 | 传感器恢复、键盘拔插、振动器、备份、时区 | ◐ | HW-6 dev.9 静态过；B21、DISP-15、BKUP-5、D15、D10 见 ② 表 |

**V5 要核的出处**（✅ 2026-10-18 全部核完、已回填，结论见 [`source-audit-20261018.md`](source-audit-20261018.md)；下面是原清单）：`system/libbase/file.cpp` 写失败后 errno 是否保留；`system/vold` 的 `DiskSource::matches` fnmatch flags 与 `vold.has_reserved` 出处；crDroid `build/make/core/sysprop.mk` / `config.mk` 与 `refs/aosp-build` 是否一致；
内核 `drivers/usb/gadget/configfs.c` 写 UDC 失败后 gadget 是否留在 pending（决定 V11 里 adb 能否自己回来）；crDroid 16 Settings 的实际菜单文案；`dumpsys SurfaceFlinger --latency` 的列格式与 128 帧窗口。

**等安装器**（`scripts/live/`、`live/installer-flutter/`、`scripts/windows/`；代码都已写、本机测过，下面全是真机项，并进 B4 一次做）：

* B5：见阻断表；另外停滞判死阈值（10 KiB/s 持续 60 秒）在国内走 R2 慢速时不误判；systemd 257 认不认 `--what` 的 handle-suspend-key；侧栏关机点一次。
* 批 3 前端：live 内核里 `gaokun-ec-battery` / `gaokun-ec-adapter` 在不在（电量预检）；纯 SAE 的 AP 真连一次；取消下载时 curl 跟着死；真 systemd 下界面被杀后重新起来能接上 job（GUI-11）；"保存日志"存到另插的 U 盘、Windows 上能打开（GUI-12；U 盘分区改 0700 前先实测固件回落启动）；英文界面（`gk3.lang=en`）走一遍。
* STOR-5：真盘装一次后 `tune2fs -l` 确认保留块。INST-6：从 Windows 安装包开始装一次、救援条目能起。INST-17 / OTA-10：OTA 后两条救援条目都能起。INST-12：救援里写 EFI 变量（V16）。
* GUI-7 / INST-11：`Suspend-BitLocker -RebootCount 2` 够不够、恢复保护时按哪条启动路径重新封存 —— 要一台带 Windows 的 MateBook E Go。S10 / S15 双系统安装侧与 Windows 伴随工具（预览）：`tools/gk3boot/README.md` §17、stage7 §5.15、`scripts/windows/README.md`。
* `release-installer.sh --r2` 没跑过（发布要用户点头）；"指向 latest"要等用户定是否每版都附安装器。
* `m0-internal.sh` 的发布构建提示：授权后 `adb shell` 是否直接 uid 0（V10）。

### ▶ 1.0 批 2 · 网络（NET-2 / NET-4 / NET-7 / NET-10）已写、未编译、未上机（2026-10-05）

> 过程记录见 [archive/TODO-history-2026-10.md#v1-batch2-net](archive/TODO-history-2026-10.md#v1-batch2-net)（提交表、源码出处、NET-4 两次更正的原文）。

**现状**（2026-10-06）：四项已随 dev.8 编过、dev.9 装机；tree-fix [19][20] 在真树上跑过（dev.8 起编译通过）。上机判据（用户在场的并进"热点加 Wi-Fi 同时开"那次）：

* **NET-2**：`ls /sys/class/net` 有 wlan1（dev.9 ✅）；连着 Wi-Fi 开热点，Wi-Fi **不断**、另一台手机连热点能上网；`dumpsys wifi` 里 HalDeviceManager 的 chip mode 有 STA+AP 组合。⚠️ 双信道组合（STA 5 GHz + AP 2.4 GHz）驱动报了支持，固件实际表现未验证。
* **NET-4**：1.0 **不开**随机化总开关（老网络存的 ALWAYS 会突然生效、无法安全迁移）；稳定设备 MAC 靠 `gaokun3-wlan-ap.sh` 开机写 `wlan0`（`b2c8c44`，dev.10 进镜像）。判据：两次重启 `wlan0` 地址相同且首字节含 0x02；`cat /sys/devices/soc0/serial_number` 非空。以后要开随机化：连同一次性迁移（WifiConfigManager 加标记）一起做，[20] 已备好。
* **NET-7**：`cmd wifi get-country-code` 非 null；热点设置里能选 5 GHz、WPA3；手机连上能上网。
* **NET-10**：`dumpsys network_stack` 里 https / http URL 是两项；临时用 iptables 拒掉 `connect.rom.miui.com` 后网络仍判"已连接"。
* ⓘ 热点 MAC 随机化、ACS、11ac/ax 热点这一轮都没动。

### ▶ 1.0 批 2 · 音频（AV-4 / AV-10 / AV-16，外加 AV-5 的记录）已写、未编译、未上机（2026-10-05）

> 过程记录见 [archive/TODO-history-2026-10.md#v1-batch2-audio](archive/TODO-history-2026-10.md#v1-batch2-audio)（提交表与关键核实的原文）。

**现状**（2026-10-06）：三项已随 dev.9 编过并装机（dev.9 = dev.8 + 批 2 音频）；dev.9 上 `IModule/usb` 与 cmdline 的 `snd_usb_audio.index=-2,-2,-2,-2` 在。
提交：AV-16 `2ab0372`、AV-4 `7240cd1`、AV-10 `3218dde`（`patches/0077`）。

**AV-10 条目本身**（策略 XML 里原来写着"见 docs/TODO.md"、而 TODO 里一直没有这一条 —— 现在就是这里）：有线耳机的麦克风不能用。硬件通路在：hw:0,2（`TX_CODEC_DMA_TX_3 → MultiMedia3`），混音器 TX 通路 `bin/audio-route.sh` 开机就摆好了（"耳机麦路由已应用（PCM2 / TX）"那段；控件名全部出自那里，这一轮没加新控件）。缺的只是"HAL 打开哪个 PCM"，上面那一行已补。⚠️ hw:0,2 以前只在**没插耳机**时录过（RMS −62 dBFS、51% 精确零，开路的样子，`docs/stage4-findings.md` #40），真信号与麦偏压（MIC BIAS 是否由 DAPM 自动打开）**待上机核实**；上游 UCM 原文本仓没有存档（`docs/hw-inventory.md` 提到的 `06-ucm2/` 不在仓库里），对不上就要回 Linux 救援里 `alsaucm` 对照。

**AV-5（ADSP SSR 后混音路由丢失，推测、从没观测到过）**：批 1 那一半（hangdump 记 remoteproc 状态变化）AV-6 已经做了（`bin/gaokun3-hangdump.sh` 的 `rproc_sig()` / `rproc_event()`，ADSP 的 q6v5 ready 计数一涨就记一条到 `/data/vendor/gaokun3/rproc-events.log`），不用再补。⬜ 剩下的：真碰到一次 ADSP 重启后，**静默**核对路由还在不在 —— `tinymix -D 0 'WSA_CODEC_DMA_RX_0 Audio Mixer MultiMedia2'`、`'MultiMedia4 Mixer VA_CODEC_DMA_TX_0'`、`'MultiMedia3 Mixer TX_CODEC_DMA_TX_3'` 都应是 On（`audio-route.sh` 设的值）；是 Off 就坐实 AV-5，修法是让 audioroute 在声卡重新注册后重跑（现在只在 `sys.boot_completed=1` 跑一次，`etc/audioroute.rc`）。hangdump 自己不跑 tinymix：它的域没有 `audio_device` 权限（`sepolicy/gaokun3_scripts.te` 只给了 audioroute），为一个推测去开权限不值。**不要为了验它去手动触发 SSR。**

**上机判据（全部静默，不放音）**：
* AV-16：`dumpsys media.audio_policy` 里没有 Telephony / FM Tuner 设备、Outputs 里没有 `compressed_offload`；有 App 放音（音量 0）时 `pcm1p` 仍 RUNNING。
* AV-4：`dumpsys media.audio_policy` 的 HW Modules 有 `usb`。**插 USB 声卡（先用 port1 —— port0 会触发 A6）**：内置声卡仍是 card0、USB 卡 ≥1，出现 `USB Device Out` / `USB Headset Out`，App 放音时 `card1/pcm0p` RUNNING。**开机前就插着 USB 声卡冷启动一次**：内置声卡仍是 card0、`logcat -s audioroute` 正常。
* AV-10：`getprop ro.vendor.audio.primary.alsa.in_headset` = `CARD_0_DEV_2`；插**带麦**耳机后 `Wired Headset Mic` 连上；录 5 秒（`gaokun3-mic-smoke`），logcat 有 `… gives card id 0, device id 2`、`card0/pcm2c` RUNNING、对着麦说话那段 RMS 明显高于 −62 dBFS；拔掉回到 `pcm3c`；三段耳机（无麦）不应连上 `Wired Headset Mic`。
* 回归：`scripts/audio/mic-verify.sh` 四个用例 + `dumpsys media.audio_flinger` 的 latency 与上一版相同。

### ▶ v0.7.0-alpha（已发布 2026-09-29，戳 `1790605865`）

> 过程记录见 [archive/TODO-history-2026-10.md#v070](archive/TODO-history-2026-10.md#v070)（两个候选版的验收全文、装机清单、Histen 0068 的诊断）。

结论：第一个候选版 `1790597477` 因 Histen 被 APEX 链接器命名空间拒载而不发 → `patches/0068` → 重编 `1790605865` 验收除第 8 项"待机后回插 USB-C 口坏"外全过（用户定写成已知问题照发，跟进见 A6）。
遗留：⬜ R2 密钥轮换（见阻断表 B2）；验收里新见的两条 SELinux denial 都已在第七轮处理（`hal_audio_default` 自调用 → `572a060`；`vendor_init` 写 printk → 09-29 删掉那行写入）。

### ⏸ 发版暂停（用户 2026-09-28）—— 已结束

> 过程记录见 [archive/TODO-history-2026-10.md#release-pause-0928](archive/TODO-history-2026-10.md#release-pause-0928)。

结论：v0.6.3 候选版 `1790206017` 不发，直接出 v0.7.0（带 B24 麦克风、iris、安装器预览、Histen 引擎 —— 用户定"带着发"）。"候选版一待机就复位"是测试方法错（插着 USB 直接 `echo mem`，#128 §17）。
遗留的 iris 零碎（`venus_compat_gfmt=N`、POLLPRI 空转、`invalid uc_region`）列在总表 ④。

### （暂停前）v0.6.3 候选版 `1790206017` —— 未发布，已被 v0.7.0 取代

> 过程记录见 [archive/TODO-history-2026-10.md#v063-candidate](archive/TODO-history-2026-10.md#v063-candidate)。

### 🐞 GitHub issues #11–#16（2026-10-04 逐条核查；处理随 v0.7.1-alpha 发布）

> 过程记录见 [archive/TODO-history-2026-10.md#issues-11-16](archive/TODO-history-2026-10.md#issues-11-16)（逐条根因、出处与 v0.7.1 装机验收原文）。

| issue | 结论 | 还剩 |
|---|---|---|
| #11 热点开不了 | ✅ hostapd 进镜像，v0.7.1 上 `start-softap` 成功 | 真设备连上并上网 → 并入 NET-2 |
| #13 天翼云电脑必崩 | ✅ 声明 `usb.host`，UsbService 起来 | — |
| #16 待机睡死 | ✅ 看门狗配置 + `patches/0070`（MHI 保留固件表）+ `0071`（CMA 限 2–4 GiB），GFP_DMA 高阶分配 69 → 0、挂起 12/12（#131） | ⬜ `ath11k_mhi_start` 失败不 unprepare 漏约 4 MB DMA（1.0 之后）；⬜ 请报告者给 `dmesg \| grep cma` 与 `alloc_pages_fail` |
| #12 抖音播放必崩 | 报告者：抖音 40.6.0 在同一 48 位内核上已不崩；39 位测试内核起不来、搁置 | 要 tombstone 才能继续 |
| #14 实测报告 | 与本表一致；MAC 部分并入 NET-4 | — |
| #15 实测报告 | 无代码改动 | 英雄联盟秒退要 logcat |

### 🐞 GitHub issues #17–#24（2026-10-08 逐条核查）

| issue | 结论 | 还剩 |
|---|---|---|
| #17 英雄联盟手游秒退（`il2cpp_init+28` 写 `0x10`） | ★★ **2026-10-08 A/B 定案：是 48 位用户地址空间**。官方 7.3.0.2813（`libil2cpp.so` BuildId `c171e91e…` 与 tombstone 同）装在开发机：原内核（48 位，`mmap_rnd_bits` 33）启动 2 秒 `UnityMain` SIGSEGV `fault addr 0x10`，与报告一致；实验内核（`patches/0078`，`--with-va39w`：页表仍 48 位、64 位进程默认地址窗口压到 2^39、`ARCH_MMAP_RND_BITS_MAX` 24）跑过 `il2cpp_init`、Unity 脚本层起来、停在用户协议弹窗 2 分钟无崩溃。该内核 `2b93abcb…`（构建机 `~/gk3-kernel-va39w`，配置相对发版只差这两项，dtb 与 k77 逐字节同）在 ESP `android/va39w/Image` + 条目 `gaokun3-va39w.conf`（options 与 `_b` 同）；oneshot 上机：34 秒开机、enforcing、577 个进程 0 个映射 ≥ 2^39、三角洲行动 / Phigros 无崩溃。#20 是同一崩溃，#19 抖音嫌疑同类 | ✅ **用户 2026-10-08 定进发版**（`5b5534c`：并入 KPATCHES、`kernel-config-android.sh` 打开并断言 `ARM64_USER_MMAP_WINDOW_39` 与 `ARCH_MMAP_RND_BITS_MAX=24`；代价：mmap 随机化 33→24 位，与手机一致）；⬜ dev.11 的内核要在 `~/gk3-kernel-72y` 上重打链（`--verify` 绿）重编；⬜ 进游戏对局（要登录）；⬜ 抖音同法复测 |
| #20 英雄联盟手游（国际版同包名） | 与 #17 同一崩溃（tombstone_09/10：同 BuildId、同 pc `0x3c4a880`）⇒ 按重复关闭；附带的 tombstone_36 是 system_server 的 HWUI 走 Vulkan 时 `queueCount < kRequestedQueueCount`（turnip 只报 1 个队列，`stage5-freedreno.md` 已记） | — |
| #18 Wi-Fi + 热点同开 | rc.1 预建 `wlan1`，等报告者验 | 并入 NET-2 |
| #19 抖音闪退 | x18 / SCS 说法已核否（10-06 回复）。**2026-10-08 本机 A/B**：抖音官方 64 位包（`v5072_400601`，versionName 40.6.0，带 `libbyteVC2dec.so`），原内核（rc.1，7.2.9，48 位）与 0078 内核各刷 12 个视频，BVC2 解码线程在跑、**两边都不崩** ⇒ 本机复现不了（报告时是 7.2.0-rc2 #15）；0078 下无回归 | 等报告者在 rc.1 上复测与 `/proc/<pid>/maps` |
| #21 手写笔 | PR #22（mashen11）10-08撤回，issue同日关闭；面板从片`spi20`路线由PR #27另行实现，随rc.2发布 | ✅ DISP-13完成 |
| #23 相机会话报错 + 画面转 90° | ★ 会话报错的根因：enforcing 下 `hal_camera_default` 用不了 drm_hwcomposer 发的 sync_file 栅栏（`tclass=fd`）⇒ binder `BR_FAILED_REPLY` ⇒ CAMERA_ERROR。rc.1 验收没抓到（预览没跑到缓冲带着栅栏回来的那一轮）。规则已补（`ec5f2a5`），**进 dev.11** | ⬜ dev.11 上 Aperture 预览跑满 5 分钟零 `tclass=fd`；✅ 方向：不是机型差异，HAL对每台机器都少算了转到竖屏自然方向的 90°，PR #83 修了（下一版起，见 AV-12）；⬜ 浏览器里后摄 R/B 对调、部分网站开不了后摄（要网站名与复现时的 logcat） |
| #24 待机时 `System (AUDIO)` 耗电高 | 开发机静置时 audioserver / 音频 HAL 30 秒 CPU 为 0，未复现。那一项是估算值（"音频输出开着的时长" × power_profile 的 `audio` 30 mA），要先分清是真掉电还是只是记账 | 等报告者的 `dumpsys batterystats` / `media.audio_flinger` |
| （顺带）enforcing 下 App 崩溃不出 tombstone | crDroid bionic 把 `/proc/*/maps` 先拷进 `memfd:axion:*`，本机 memfd 落在通用 `tmpfs`，`crash_dump` 写被拒 ⇒ `Failed to parse maps` ⇒ 无 tombstone（英雄联盟崩溃、`kill -SEGV` Twelve 都复现）。规则 `a3c354c`，**未编译** | ⬜ dev.11 前在 crDroid 树 grep axion 看上游策略；⬜ dev.11 上 `kill -SEGV` 普通 App 出 tombstone、零 crash_dump avc；进 `release-checklist` |

### 🐞 GitHub issues #26–#34（2026-10-09 逐条核查）

| issue | 结论 | 还剩 |
|---|---|---|
| #26 M-Pencil / PR #27 | 审查：方向对、与 main 无冲突；要改两处才能合 —— 笔电量 `power_supply_register` 没关唤醒源 ⇒ `m-pencil/wakeup/` 串成笔电量标签、system_suspend 在 enforcing 下被拒；`solver.h` 缺 EGoTouchRev 的 MIT 版权行与全文。建议：`suppress_bind_attrs`、`-EPROBE_DEFER` 别吞、`mcu_find` 改按 sysfs VID/PID 找。⚠️ `usbcore.quirks=12d1:10b8:b` 作用的就是键盘盖（`hw-inventory.md:741`），每次休眠恢复都 USB 复位键盘 | 10-09 已在 PR 留言；作者 10-09 改完（另加引用计数 / 启动加锁 / `DRM || !DRM`），10-10 复审无新问题、**已合入 main（`f85c36b`，KPATCHES 按 0079→0082 排）**，随下一个 RC 发。我们的音频补丁让号 0079→0083，#27 保留 0079/0080。剩下的我们自己做：构建机 `--verify` + `m gk3pend` + sepolicy 测试；enforcing 实机零 avc、按键唤醒正常、「关键盘」恢复后仍 inhibited |
| #29 手掌识别 / PR #30 | 审查：补丁能干净打上（`hx-algo.c` 来自 buildbot `patches/others/0003`），0081 的排序修正对，协议 B 撤回顺序对；要改：`algo/hand_*` 无范围校验（`hand_hold_frames=255` ⇒ 整屏触摸被当手丢掉，回放器复现）。待定：game 预设要不要 `hand_enabled=0`；注册 `ABS_MT_TOOL_TYPE` 后 InputReader 会不会给触摸屏加 `SOURCE_STYLUS`—— **10-09 构建机核实：不会**。`mStylusMtToolSeen` 只在某个触点真报 `ToolType::STYLUS` 时置位（crDroid `frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp:115-116`，`hasStylus()` 在 `:225-227`），只注册轴或报 PALM 不加 SOURCE_STYLUS；palm 撤回路径在 `TouchInputMapper.cpp:1889` | 10-09 已在 PR 留言；作者改完（上限 / game 预设 `hand_enabled=0`），10-10 复审无新问题、**已合入 main（`8ea2b6f`）**，随下一个 RC 发。`hx_hand_reset()` 只编译验证过。与 #27 只在 KPATCHES 尾部冲突，按 0079→0082 排；先合 #30，位置锁定写在 0082 之上；合后 `evdev-strokes.py` 要认 `MT_TOOL_PALM`（撤回的不算点击）；合后开发机（CSOT）上纯手指 `hand_lands` 恒 0、手掌平放有撤回 |
| #28 BOE / CSOT 面板自动区分 | ✅ 不靠 `DA DB DC`：UEFI 读主板的 ID 引脚（GPIO 102、91，0/0 = CSOT，0/1 = BOE，见 `hw-inventory.md` 的「面板」），两种屏各实测一台。面板驱动在 probe 时读这两根脚（patches/0123、0124），BOE 不用再改 boot 镜像 | ⬜ 带这两个补丁的第一个发布构建：CSOT 机器上看一眼 `panel ID pins: 0 0`、画面正常（安装器用同一个内核，重新打包就带上了） |
| #33 无限暖暖 2.9.1 启动即崩（v0.7.1） | UE 5.4.4，`SecondsSinceStart=0` 致命错误 "Global shader library is missing" —— 包里没有所选 RHI 的全局着色器 ⇒ **推断** UE 选了 GLES、而游戏只带 Vulkan 着色器。CrashContext 的 GPU 品牌 = `ANGLE (Qualcomm, Vulkan 1.3.335 (Turnip Adreno (TM) 690 …`：UE 拿 GL_RENDERER 当 GPU 家族匹配机型档案，`ANGLE (` 前缀可能匹配不上 Adreno 规则（UE 规则本地无源码，**未核实**）。对照：本机三角洲（UE4）走 Vulkan（有 `VulkanProgramBinaryCache/`），卡拉彼丘（UE4）走 GLES（`ProgramBinaryCache/GLSL_ES3_1_ANDROID_*`）—— 各游戏规则不同。ANGLE 支持用 `debug.angle.gl_renderer` / `gl_vendor` 整串覆盖（ANGLE `ab6a82e0` 的 `Context.cpp:3500-3533`；本机 `libGLESv2_angle.so` 含这两个属性名），adb shell 不用 root 就能设 | 请报告者在 rc.1 上 `setprop debug.angle.gl_renderer "Adreno (TM) 690"` + `setprop debug.angle.gl_vendor Qualcomm` 后重试做 A/B；有效再议进不进镜像（全局伪装 Adreno 可能让游戏走针对高通驱动的规避分支，要回归） |
| #34 鸣潮每次重编着色器、加载掉帧、无 Vulkan 选项（v0.7.1） | 锁 GLES 推测同 #33。重编 / 掉帧另有一层：GLES 应用的 EGL blob cache 默认单文件，总量 2 MiB、单条 64 KiB（AOSP `egl_cache.cpp:33-35`、开关 `:237`，android16-release），ANGLE 的程序与管线缓存放不下；本机卡拉彼丘 1.6 MB、明日方舟 0.98 MB 已近上限。`debug.egl.blobcache.multifile=true` 在 rc.1（发布构建）上实测生效（出现 `.multifile/` 目录，已撤回）。已改镜像：`ro.egl.blobcache.multifile=true`（device.mk，多文件单条 8 MiB、总量 32 MiB） | ⬜ dev.11 后：同一游戏第二次进场景是否不再全量重编；同请报告者做 gl_renderer A/B。「划后台丢、正常退出保留」更像 UE 自己的 ProgramBinaryCache 只在正常退出时落盘（**推断**） |

---

## A. 用户能感觉到的缺口

### A9. ✅ 内核可重建性已修好并上机验证通过
结论：`patches/0016`（ashmem）/ `0017`（xt_quota2）入库、`kernel-apply-patches.sh` 的指纹判据加固；重建验收判据已落地为 [`release-checklist.md`](release-checklist.md) D1。
过程记录见 [archive/TODO-history-2026-10.md#a9](archive/TODO-history-2026-10.md#a9)。

### A0. ✅ 侧滑返回手势失效 —— 根因：这台机器从来没有过导航栏
结论：overlay 设 `config_showNavigationBar=true`，#86 装机验收生效，用户 2026-09-23 确认。过程记录见 [archive/TODO-history-2026-10.md#a0](archive/TODO-history-2026-10.md#a0)。

### T1. ✅ 触摸手感 —— 已随 v0.6.2 发布，手掌碎块随rc.2解决（[#114](stage4-findings.md)–[#116](stage4-findings.md)）
结论：T1a 幽灵触摸（跳点限速线）、T1c fuzz=0 与触点面积轴、T1d 驱动六个缺陷、T1e 可观测性（`patches/0037`–`0047`）都已发。过程记录见 [archive/TODO-history-2026-10.md#t1](archive/TODO-history-2026-10.md#t1)。
还剩（都不阻断 1.0）：
* ✅ 手掌碎成多触点（`max_contacts` 打满 10）：PR #30（`patches/0081`、`0082`，手的位置图 + `MT_TOOL_PALM`撤回）随rc.2发布，见[`palm-rejection.md`](palm-rejection.md)；
  BOE机器上实测过（回放里手掌误报281 → 1）。剩手掌加另一只手同时在屏上时偶尔误触。
* ⬜ T1b 三道形同虚设的噪声闸门：`patches/0039` 已做成旋钮 `iso_nbr_ratio_q8` / `edge_min_area`、默认值不变 —— 没有证据说幽灵走这两条路，留作边缘幽灵复现时的工具。
* ⬜ 触摸 IDC（`dumpsys input` 里 `ConfigurationFile: <none>`）：面积轴已有，可以写 `touch.size.calibration`（DISP-6，polish）。
* ✅ Parts 里的触摸手感开关（2026-10-25 用户要求，有人反馈 game 预设手感差）：「设置 → 系统 → 触摸手感」+ 控制中心「游戏触摸」磁贴，改 `persist.sys.gaokun3.touch_mode`（开 = game、关 = daily）。随rc2e发布；10-25 上机在设置页切换两次，`touch_mode`、`hand_enabled` 跟着变，无相关 avc（上面总表 rc.2 段）。磁贴与设置页互相同步没有单独记录。

### T2. ⬜ Google 未认证（"设备未经 Play 保护机制认证"）
修法是把本机的 Android ID 登记一次：工具 `bash scripts/google/gsf-android-id.sh`（新版 GMS 的 ID 在 `shared_prefs/Checkin.xml`，不在 `gservices.db`）、
用户文档在 `docs/INSTALL.md` 的 "This device isn't Play Protect certified" 一节。⬜ **剩用户动作**：去 <https://www.google.com/android/uncertified/> 登记，然后 `pm clear com.android.vending`（恢复出厂后 ID 会变，要重登）。
⬜ INST-14：Parts 加"Google 认证"页（root oneshot 服务读 ID），不用电脑。⚠️ Play Integrity（银行类）仍然过不了，那是 UEFI 解锁的取舍。
过程记录（含 v0.6.2 起修掉的 incremental 不一致）见 [archive/TODO-history-2026-10.md#t2](archive/TODO-history-2026-10.md#t2)。

### A6b. ⬜ WPA3(SAE) 连上即断 —— [issue #2](https://github.com/vahiru/gaokun-android/issues/2)
结论：本机 SAE 栈端到端可用（#107：WPA3-SAE 一次连上、3 分钟带流量 0 掉线；#100 是密码记错）；issue #2 在本地不复现。
⬜ 下一步（NET-11）：请报告者在**当前版本**上跑 `scripts/wifi/wpa3-probe.sh "<SSID>" "<密码>"` 并贴输出。过程记录（已排除的三层、候选、`wpa_cli` 的用法）见 [archive/TODO-history-2026-10.md#a6b](archive/TODO-history-2026-10.md#a6b)。

### A1. 音频与蓝牙长期运行后死锁 ⚠️ 次高优先
> 现状（2026-10-06）：从未复现。hangdump 自 AV-6（dev.2 起）另记 remoteproc 状态变化（`/data/vendor/gaokun3/rproc-events.log`）；靠 B7 的 72 小时狗粮去碰。下面是原条目。

用户实机报告，我未复现、未定位（[#38](stage4-findings.md)）。
两者共用同一条到 DSP 的 QRTR/FastRPC 通路，而这条通路上**已经实测到过**
会话级卡死（使能光感会污染整个 SSC 会话）。

★ **取证看门狗已随 v0.2.0 起的镜像发布**（`bin/gaokun3-hangdump.sh`）：
现实是死锁时用户只会重启、证据就没了，所以证据必须自动留下。它 60 秒采一次
`/proc` 线程状态（刻意不跑 dumpsys，很便宜），判据是**同一个 tid 连续三次都在 D**
（≥2 分钟），命中后把 stack/wchan/QRTR 服务表/PCM 状态/binder 日志/logcat
写到 `/data/vendor/gaokun3/hangdump-<uptime>/`。

**第一步**：下次死锁后把那个目录整个要过来 —— 不必再追问"多久、什么负载"。
若目录是空的，说明它没判定成死锁（比如卡的不是 D 状态），那本身就是线索。
手工对照仍可用 `gaokun3-qrtr-lookup` 比服务表：少了哪个服务就指向哪个 DSP。
⚠️ 别把 `Handover signaled` 当崩溃证据，那是良性噪声（#37 已用对照实验证明）。

### A2. 硬件视频【编码】—— 已查明并【故意关闭】（1.0 之后）
结论：`v4l2_codec2` 的 `EncodeComponent` 不做 RGBX → NV12 转换，开着比关着更糟（rank 压过软编、应用直接失败），所以编码属性故意注释掉、`verify-hw-codec2.sh` 断言"必须没有编码组件"；录屏 / 录像走软编。
1.0 只在 AV-1 出片后做一次测量（AV-13）。过程记录见 [archive/TODO-history-2026-10.md#a2](archive/TODO-history-2026-10.md#a2)。

### A3. 自动亮度（环境光）—— ✅ #54
tcs3701 的驱动给 `ambient_light` 注册了两个 SUID，默认的那个（变化时上报）是驱动内部的主传感器，一使能 SLPI 就崩溃（`sns_stream_service.c:436` 的 `req_found` 断言）；连续上报的那个可用。传感器 HAL 只用后者（`SscHub::FindStreamingLight`），overlay 打开自动亮度。细节见 [`device/huawei/gaokun3/sensors-hal/README.md`](../device/huawei/gaokun3/sensors-hal/README.md) 的"环境光"一节。
✅ 随rc.3（A17）发布，rc.3上自动亮度可用（rc.3 发版说明）。
2026-09-23 之前"芯片不应答"的定性与 `scripts/ssc/` 实验记录见 [archive/TODO-history-2026-10.md#a3](archive/TODO-history-2026-10.md#a3)。

### A5. 恢复出厂设置不起作用 —— ◐ 已交给统一启动入口
用户 2026-10-04 定（D4）：恢复出厂（BCB `--wipe_data`）与 `fastboot -w` 都落到统一启动入口的执行端上。1.0 镜像默认动作模式 + 分派开（`ca55b5c`），QEMU exec-wipe 过；**用户 2026-10-06 定 E10 不测** ⇒ 真机未验，已知限制照旧。
现成的替代：图形安装器"重新安装 + 清除数据"；或救援里 `mkfs.ext4 -F /dev/disk/by-partlabel/userdata`。过程记录见 [archive/TODO-history-2026-10.md#a5](archive/TODO-history-2026-10.md#a5)。

### A6. USB-C 口（port0）角色 / UCSI ★ 待机后回插仍会坏
* 0048（`qcom,select-utmi-as-pipe-clk`）修好了常规角色切换（#118 §8：来回切三次 `-110` / `-524` 各 0 次）；角色策略用用户态 `follow`（电气探测，不信 EC）。
* ❌ **待机 / 唤醒之后回插 port0 仍会坏**（v0.7.0 第 8 项：`-524` / `-110`，间歇，第一个候选版同样步骤过过一次）⇒ 1.0 只止损：PWR-4 通知（`5dfe7dd`）+ 已知限制；根因（dwc3 / QMP PHY 的 resume 路径）放 1.0 之后。
  ◐ 2026-10查到的线索：组合PHY退出时vreg_l9d断电，port0的dwc3软复位超时（`-110`）、xhci和gadget起不来直到重启，
  device角色挂起整板复位（#52）也是它。`patches/0086`（PR #36，vreg_l9d常开）随rc.2发布；修好以后"待机后回插"没有单独复测，复测过再改已知限制。
* ◐ rc.2起（PR #36–#39）：UCSI修好（`TYPEC_MUX_GPIO_SBU=y`、0087–0093），port0能跑USB 3；数据角色改由USB端口HAL按供电方向判断
  （EC只报供电方向，不报对面是主机还是设备，各种外设都量过），U盘插上约1秒后以USB 3枚举。下面两条是rc.2之前的情况。
  另有偶发的开机时UCSI初始化失败（`PPM init failed`，`/sys/class/typec`为空，重启恢复），还没查。
* UCSI 在当前内核上是活的，但给的数据角色是反的（PC 插着时 `data_role=[host]`），靠 `init.gaokun3.usb.rc` 硬写 device 盖回来（#112）；EC 的 `partner_type` 插不插都是 2，可能是常数（#118 §6）。
* 2026-10-05 新发现：插 Mac 有时落成**我方供电**（USB-1，`ccd54f9` 弹通知请重插）；host 时每秒重绑 UDC（USB-2，`d511d18`）；port0 = 靠近电源键的口（STOR-3 ✅）。
* ⬜ 下一步：① 复现 —— 待机前后各切一次角色看哪一步坏，开 dwc3 / QMP 的 dyndbg；② **要用户插拔**：U 盘 / 纯充电器 / 扩展坞各跑一次 `scripts/usb/ucsi-snapshot.sh`，看 `partner_type` 与 `pwr_dir`，再定 quirk（扩展坞会 DR_Swap，不能盲用"受电 ⇒ 对方是主机"）；③ hub / U 盘 / 充电器的真机场景（V11）。

过程记录（UCSI 旧状态、#52 的取舍、总表旧行）见 [archive/TODO-history-2026-10.md#a6](archive/TODO-history-2026-10.md#a6) 与 [`#tier1`](archive/TODO-history-2026-10.md#tier1)。

### A8. ✅ 设备的 WAN 吞吐"只有 PC 的 1/20" —— 2026-09-14 结案：不成立
见 [archive/TODO-history-2026-10.md#a8](archive/TODO-history-2026-10.md#a8)（★ 对照组要先确认两边走的是同一条链路）。

### A7. 摄像头 —— 前后摄可用，电源域缺陷已根治（#105）
已收口：电源域（`patches/0031`）、基于 libcamera（simple 流水线 + 软件 ISP）的相机 HAL、后摄 OV13B10（0032 / 0034 / 0035）、闪光灯与手电筒、相机 ID 稳定、后摄方向 180（用户目视；画面其实还转了90°，见下面AV-12）、PR #6（AF / JPEG 旋转 / 崩溃）、后摄隐私灯（0074）。
过程记录（#81–#112 的全过程、已否的候选修复）见 [archive/TODO-history-2026-10.md#a7](archive/TODO-history-2026-10.md#a7)。还剩：
* ✅ **AV-12 前摄方向**：设备树的 0 / 180 按横屏取景是对的；Android 的自然方向是竖屏，HAL 少算了这 90°，前后摄画面都转了90°。
  PR #83在HAL里补上（不改设备树，后摄ov13b10的H+V翻转照旧），实机前后摄预览和照片都正，下一版起。
* ◐ **AV-1 录屏出片**（录像已经出片）、⬜ Aperture 后摄闪光 ON 拍一张看 LED 亮灭各一次 —— 都要用户在场。
* ⬜ **T3 画质**（1.0 之后）：闪光帧下发 `ExposureValue` 负补偿；libcamera AWB 剔饱和像素（已做：`patches/libcamera/0009`，值得投上游）；前摄CCM要色卡（现在是Windows驱动里高通的通用矩阵，白点只在一台机器上量了两种光源，见`hi846.yaml`；后摄已用华为Windows驱动里这款模组的调校，见`ov13b10.yaml`）；手电筒亮度档位（HAL 声明 `FLASH_INFO_STRENGTH_*`、实现 `turnOnTorchWithStrengthLevel`）；`kDarkLuma=50` 是启发式（要 softisp IPA 把曝光 / 增益写进结果元数据）；AV-11变焦/EV。
* ⬜ libcamera：`camera_sensor_properties` 加 ov13b10、增益模型 helper、`ov13b10.yaml`；生成源码改成 Soong `genrule`（`patches/libcamera/README.md:34`）。上游：`ov13b10.c` 补 `get_selection`；读 EEPROM@0x50；camcc `Mark RCGs shared` 投稿 + 等待值 `0027`。
* ⬜ `patches/0022`（unbind / rebind 的 genpd 注销）在 camss **健康**时补一次验证。⚠️ camss 已 `runtime_error` 时不要 unbind（会拖死整机）。

### A′. 已关闭（只留索引，细节在案卷里）
s2idle 待机（`dr_mode="otg"`，#52–#57）· 耳机口与内置麦（#40）· 普通应用能 panic 内核（`patches/0013`，#58 / #62）· 硬件视频解码（#41）· 亮度调节（真 lights HAL）·
扬声器音量偏小（PA 21 / dig 84，#78 / #79 / #86）· 插着键盘时屏幕键盘不弹（`show_ime_with_hard_keyboard`）。原文见 [archive/TODO-history-2026-10.md#a-closed](archive/TODO-history-2026-10.md#a-closed)。

---

## B. 工程债与正确性

### B12a. ◐ 中文输入法（DISP-3 / D10）：fcitx5-android 0.1.3 随rc.1预装，⬜ 上机核对打字
2026-10-06 用户定从 GitHub release 取：APK sha256 `8e5de1036aea…` 与 GitHub 资产 digest 一致；`device.mk` 改成 APK 在就带（`GAOKUN3_WITH_FCITX5=false` 显式不带）；`sync-device-tree.sh` 3b2 断言；NOTICE 记 LGPL 源码 tag；已知限制中英改成"已预装、要自己打开一次"（没设成默认输入法）。
⬜ 上机：设置里能启用、拼音能出字、GK-W7X 键盘盖上怎么中英切换（与 Android 自己的 Ctrl+Space 冲不冲突）、候选窗正常。
10-05 的选型调研（fcitx5-android vs Trime）见 [archive/TODO-history-2026-10.md#b12a](archive/TODO-history-2026-10.md#b12a)。

### B12. ⬜ 释放 R2 桶的前提：国内可达的下载镜像
2026-09-14 v0.6.0 把清单 `download` 指到 GitHub Release 附件，用户当天反馈更新失败：附件 302 到
`release-assets.githubusercontent.com`，国内不可达；`raw.githubusercontent.com` 同样。已全部换回 R2。
出路：同一 Cloudflare 域名（`ota.072172.xyz`）下建 Worker 反代 GitHub Release 附件与仓库里的清单（⚠️ 2026-10-04 起仓库里**没有**清单了：`ota/gaokun3.json` 停在 v0.6.1、没有任何东西读它，已删（1.0 计划 REL-14）；权威清单只在 R2，由 `release.sh` 上传 —— 走这条路要先让 `release.sh` 把清单回写进仓库），
桶只留存储为零的转发层；或干脆保留桶（成本很低：出站免费）。**要用户定**，且改完要在国内网络实测下载。

### B14. ⬜ 息屏 USB adb（1.0 之后；[#112](stage4-findings.md)）
脚本折中（`bin/gaokun3-usbrole.sh` v2：插着主机息屏不切 host、不放行挂起，拔线再切）已在镜像里（2026-10-04 在 v0.7.1 上 md5 与仓库一致）；开发机 `allow_suspend` 持久为 0，1.0 起可用 Parts 开关（PWR-16）。
源码定性：device 模式挂起无条件 `dwc3_core_exit()`、gadget 总 soft disconnect ⇒ 上游 dwc3 没有"adb 穿越睡眠"。◐ device 角色下挂起会整板复位（#52）：查到是vreg_l9d断电，`patches/0086`随rc.2发布，PR #39审查时device角色挂起61次无复位（A6）；⬜ UCSI 角色修好（A6）后去掉脚本的 host 切换。
原文见 [archive/TODO-history-2026-10.md#b14](archive/TODO-history-2026-10.md#b14)。

### B13. ✅ `install-ota-local.sh` 装前先算 ESP 空间（2026-09-14）
见 [archive/TODO-history-2026-10.md#b13](archive/TODO-history-2026-10.md#b13)。

### B0. ⚠️★★★ 让构建机的设备树【就是本仓的 checkout】—— 已经咬了六次（1.0 之后）
现状：`~/crdroid/device/huawei/gaokun3` 是普通目录、靠同步脚本与本仓对齐，必然双向漂。探测器已有：编内核前 `kernel-apply-patches.sh <树> --verify`（绿了再 make）、
同步只用 `sync-device-tree.sh`（断言 adb_keys / firmware / hexagonrpcd-root / prebuilt-boot 这些不入库但构建必需的输入在）、同步一律排除 `._*` / `.DS_Store`。★ 参照物必须是"构建需要什么"，不是"本机有什么"。
⬜ 第一步：把构建机上那个目录换成本仓的 git checkout（或 symlink 到 checkout），换之前先做一次清单比对（`.gitignore` 掉的固件 / 传感器配置 / 预编译内核不能被覆盖）。
⚠️ 构建机 `prebuilts/build-tools` 里 `date` / `tar` 共 6 个文件被删、理由不明，只记录不编码；干净树构建出 `tar` / `date` 报错就是它。
六次事故的经过见 [archive/TODO-history-2026-10.md#b0](archive/TODO-history-2026-10.md#b0)。

### B1. SELinux 转 enforcing（= SEC-4 / G9）
现状以 [#129](stage4-findings.md)（第七轮）为准：块设备标签改按 by-name、相机 allocator 等 20 余处已修（`572a060` / `d885275` / `4d93d4c`）；**2026-09-30 第一次真 enforcing 开机成功**
（vendor 域零 denial，解锁 / GPU / 相机 / 音频 / 硬解 / 传感器 / Wi-Fi / 蓝牙 / usbrole 全过）。工具在 [`scripts/selinux/`](../scripts/selinux/README.md)。
* ✅ 用户10-06定D5：dev.10起默认enforcing，随rc.1发布；enforcing → enforcing OTA在rc.1、dev.11上过了（①表G9）。⬜ enforcing下拔线真实待机与回插没有单独测过。恢复出厂那一项随 E10 不测。
* ⬜ 新服务（PWR-16 开关、sscrecover、wlanap、Histen 开着放音 + 录音）在 enforcing 下各跑一次看 avc（`hal_audio_default` 自调用的规则已在 `hal_audio_default.te:34`，`572a060`）。
* ⬜ 非阻塞：rproc-kick / hexagonrpcd / bootctl 规则收窄；tinymix 的 vendor 变体（有了它 audioroute 就不用挂 `vendor_executes_system_violators`）；genfscon 前缀匹配盖住 UCSI `power_supply` 下的 `wakeupN`（语义不对、当前无后果，别当成有功能在等着修）。
* ⚠️ 切 enforcing 后 ESP 名字不标准的机器 OTA 会失败（OTA-5）；**不要用 `-user` 变体构建**（#117 §15）。
* ✅ 已收口：`/dev/dri/card*` 0666 → 0660（SEC-14，dev.2）；`vendor_init` 写 printk 的 denial（09-29 删掉那行写入）；allow_suspend 的 UI（PWR-16）。

第一至第七轮的过程（含两个"结构性阻塞"的旧处理、属性改名）见 [archive/TODO-history-2026-10.md#b1](archive/TODO-history-2026-10.md#b1)。

### B2. ✅ 真温控 HAL —— 已随 v0.6.0 进镜像
⚠️ 留着的教训：AOSP mock 的 skin / battery SHUTDOWN 阈值只有 36 °C，只换 HAL 不改阈值 = 开机几分钟自动关机。原文见 [archive/TODO-history-2026-10.md#b2](archive/TODO-history-2026-10.md#b2)。

### B3. 自研 EFI 加载器 —— ◐ 已被统一启动入口覆盖
`gk3boot.efi` 读 misc 选槽、扣 tries 自动回落、分派 BCB、从 `boot_x` 分区直接启动（E3–E8 真机过，[`boot-entry-design.md`](boot-entry-design.md)、`tools/gk3boot/`）——原条目"读 `bootloader_control` 选槽 + 解析 boot 镜像、让 BCB 能被消费"已做到。
⬜ 余项（1.0 之后）：退役 postinstall 与 ESP 上的派生文件；AVB / verified boot。原文见 [archive/TODO-history-2026-10.md#b3](archive/TODO-history-2026-10.md#b3)。

### B4. LiveCD 图形安装器（Flutter + Debian）
现状与里程碑见 [`stage7-flutter-debian.md`](stage7-flutter-debian.md)；1.0 的真机验收是总表阻断项 **B4 / B5**。原文（含 2026-08-23 搁置时的记录）见 [archive/TODO-history-2026-10.md#b4](archive/TODO-history-2026-10.md#b4)。

### B5b. ⬜ UBWC 压缩：仓库仍写着关（[#85](stage4-findings.md)）
`device/huawei/gaokun3/device.mk:263`（行号又漂了）仍是 `vendor.minigbm.debug=nocompression` —— Stage 2 为 SwiftShader 加的，turnip 之后没有存在理由。已知：设备上开着跑过 13 天，SMMU fault / `a6xx_recover` / GMU error 全 0；未知：**一次测量都没有**。
⬜ 做法：并进批 4 的 PERF-2（DISP-18）—— 删掉那一行并带一次实测（帧率 / 合成耗时 / 显存带宽三选一），别凭"理应更好"直接改。原文见 [archive/TODO-history-2026-10.md#b5b](archive/TODO-history-2026-10.md#b5b)。

### B6. GPU SMMU 中断根治
实际 DT 是全局 672/673、context bank 从 678 起；而硬件拉的是 675/680，
其中 680 被分给 CB2、675 整张表里根本没有。很像 CB 起始偏移就错了。
⚠️ 但只凭"675/680 挂起"推不出正确映射，而且**改错了没有任何征兆**
（只是继续收不到 fault）。做成之后可以丢掉常驻的 `smmu-nostall.sh` 轮询。

### B7. 用轻量系统替掉救援 Ubuntu —— 新装机已由安装器覆盖
新装机：安装器写 `gk3rescue` 救援条目（INST-6 / INST-17，见 stage7）。⬜ 开发机：Ubuntu 救援分区（24.6 GiB）仍在，回收要用户点头。2026-08-23 Alpine M0 的记录见 [archive/TODO-history-2026-10.md#b7](archive/TODO-history-2026-10.md#b7)。

### B8. ✅（查明、不修）`invalid volume index range in the curve` ×12
AOSP legacy 音量配方的启动顺序噪声，与本机无关。见 [archive/TODO-history-2026-10.md#b8](archive/TODO-history-2026-10.md#b8)。

### B9. ✅ SLPI handover 噪声 —— 结案
它是 SSC 向 AP 投递一批数据的门铃（#119 §4），`patches/0014` 的 ratelimit 是正解；"没订阅者时仍 50 Hz 常开"的空转由 PWR-3 修掉（dev.1 V9 实测），#119 §4 的更正已补（`b0133ab`）。见 [archive/TODO-history-2026-10.md#b9](archive/TODO-history-2026-10.md#b9)。

### B11. root（ReSukiSU）—— ✅ 随 ROM 常驻，D6 保留并披露
披露已写（known-limitations：授权过的 adb 直接是 root）。⬜ V7b 预不预装管理器 APK（`ksud` 就在 APK 的 `libksud.so` 里，ROM 侧不用加东西）；建议**不给** `kernelsu.allow_shell=1`。
原文见 [archive/TODO-history-2026-10.md#b11](archive/TODO-history-2026-10.md#b11)。

### D5. PR #3 待回复（已审完，等你定措辞）
线上那个 PR 动的正是内核预编译这一块。我把要问的整理好了，**没有发到 GitHub**
—— 对外发言等你。三个问题：`dr_mode=host` 是不是有意为之（那正是我们 #52 的
取舍另一半）；他们刷完之后 USB adb 还通不通；能不能公开那份内核 `.config`
（我们这边的断言是 52 条 MUST_Y + `VIDEO_QCOM_IRIS` MUST_N，可以对一遍）。

### D6. 把 `drm_crtc` 那个 `BUG_ON` 报到 dri-devel（对外动作，等你定）
`patches/0013` 修的是**上游 mainline master 现存**的缺陷：普通应用查一次
present fence 的名字就能把整机 panic 掉。稿子照那个补丁的 commit message
改一改就能发。我不代发对外邮件。

### D8. ✅ v0.6.1-alpha 已发布（2026-09-14）
见 [archive/TODO-history-2026-10.md#d8](archive/TODO-history-2026-10.md#d8)；那里列的"闪光 EV 负补偿与 AWB 剔饱和"归 T3（A7）。

### D7. ✅ v0.2.0-alpha 的 R2 产物要不要删 —— 2026-10-11 并入 R2 滚动机制、已删
用户 10-11 定滚动规则：正式版留 OTA 清单指向的当前版 + 前两版；staging 留最新 2 组；installer 留最新 2 个；
最新正式 release / 最新 pre-release 发布页链接到的另外保留。实现 `scripts/r2-prune.py`（默认只打印计划，`--go` 才删；
取不到 GitHub 发布页 / OTA 清单、或清单指向的 zip 不在就整体不删），`release.sh` 上传完自动跑（`--stage-only` 只滚 staging，
`GK3_R2_PRUNE=0` 跳过）。10-11 首次执行：38.6 GiB / 81 个对象 → **15.8 GiB / 38 个**，删 11 组 22.8 GiB ——
正式版 0820 / 0821 / 0822 / 0824 / 0913 / 0914 与 `install/v0.2.0-alpha/`，staging 0913 / 0914 / 0916 与 `dev11-1791554509`；
公开域名抽查当前版、rc.2 payload、installer 0.2.0-rc.1 都 200，删掉的 404。
⚠️ 后果：**v0.2.0–v0.5.0 四个 GitHub 发布页正文里的 R2 链接现在 404**（v0.6.0 起的正文本来就没有 R2 链接）。
⬜ 要不要在那四页正文里加一句"R2 下载已下线"（改公开页面，等用户点头）。

---

## E. 明确搁置（记录理由，不是忘了）

* **recovery** —— 启动即复位循环（[#39](stage4-findings.md)），仍搁置；它原本要承担的恢复出厂与 fastboot 已改由统一启动入口的执行端承担（D4）。
* **原神 1080×1728 的渲染上限** —— 游戏按设备白名单给的档位，要突破只能伪装机型、有账号风险，留给用户决定。
* 原 E 节的另两条已不成立：**fastboot**（统一启动入口 + `gk3-fastbootd` 执行端，E6 真机过）、**GMS / Play 商店**（D7：只发 GApps 版）。

原文见 [archive/TODO-history-2026-10.md#e](archive/TODO-history-2026-10.md#e)。
