# 发版回归清单

> REL-6 / PERF-12（`docs/v1.0-plan.md`）。这份就是此前在 `docs/TODO.md:244`、`:1017`（10-06 之前的行号，那段现在在 archive/TODO-history-2026-10.md，按节内标注的原行号找）、`docs/project-log.md:494`、
> `docs/stage4-findings.md:4691`、`:8053` 里被引用、却一直不存在的「发版收尾清单」。
> 内容取 `docs/TODO.md` 里三份装机验收清单的并集 —— v0.6.3 候选版（`:118-127`）、v0.7.0（`:57-68`）、
> v0.7.1（`:133` 那段与 `docs/relnotes/v0.7.1-alpha.md` 末段"没测的项"）—— 再加上 1.0 计划的 G1–G11 与 PERF-12 的三档。
> 以前每版临时按本版改动挑项目，上一版测过的项下一版不一定再测；以后**每个候选版都从这里抄**，本版的结果记进 TODO 的本版一节。

## 怎么用

| 档 | 什么时候跑 | 谁 | 工具 |
|---|---|---|---|
| **A 无人值守** | **每个候选版**，装机 oneshot 起来、开机 ≥10 分钟后 | 无人值守（USB 或 TCP adb） | `scripts/accept.sh` 一键 |
| **B 要人在场** | 每个候选版跑和本版改动相关的项；**1.0 RC 全跑** | 用户（解锁、插拔、目视、听） | 下表的命令 |
| **C 长稳与测量** | 只在 1.0 前跑一次；内核 / mesa / HWC / 刷新率 / minigbm 有改动时跑游戏那几项 | 用户在场或拔线放置 | `scripts/perf/` |
| **D 发版收尾** | `release.sh --no-build` 之前 / 之后 | 维护者（宿主机） | 下表的命令 |

```bash
# 候选版装进 _b、oneshot 起来、开机 ≥10 分钟后：
SER=gaokun3 bash scripts/accept.sh --stamp <候选版的 ro.build.date.utc> --soak 600 --video <某个.mp4>
echo $?        # 0 = 没有 FAIL；1 = 有 FAIL；2 = adb 不通或中途掉线（汇总行照出、带一个 A0 FAIL）。别接 | tail 再取 $?（CLAUDE.md 运维坑 1）
```

* 报告在 `out/accept/<戳>-<时间>/report.txt`，同目录存着 logcat / dmesg / dropbox 列表 / 频率与温度快照，下一版拿来对比。
* `--profile dev`：开发构建（adb 免授权与 TCP 5555 是故意的）时 A3 只记录、不判。发布构建一律 `release`（默认）。
* `--readonly`：只做只读检查（跳过开相机、录音、真解视频），任何时候都能跑。
* **WARN 不挡发版，但要人看一眼**（denial 有没有新的大面积、测试工具自己崩了等）。
* 本清单里每个属性名 / 路径 / 字符串都是 2026-10-04 在 `1791053208` 上实测核对过的；新增一项也要先在实机上核对再写。
  ⚠️ **例外**：2026-10-05 补进来的 A23–A27、B14–B17、D9 来自 1.0 批 1 的待验证项（`docs/TODO.md` 总表第一节 V0–V17），对应的改动还没构建、设备当时也不在线，**命令与判据都没在实机上核对过**。第一次跑之前先在实机上核一遍，核过后删掉这条说明。
  ⚠️ **同样的例外**：2026-10-05 第二批补进来的 A28–A37、B18–B22、D10，以及 B7 / B12 的补充（1.0 dev.5–dev.8 的新东西）。其中启动入口几项
  （A28–A31、B12 的回滚、B21）的命令是 E5–E8 与执行端 E6 / 分派 E7 上机时真用过的（`docs/hw/gk3boot-*-20261005.txt`、`tools/gk3boot/README.md` §12.5 / §15.6），
  但"镜像默认就是动作模式 + 分派开"（`ca55b5c`）是 dev.8 才有的、dev.8 没装机；B21 的电源键确认、B22（E10）、B12 的 E11 那一跳**从没在真机上跑过**；其余几项（Parts 待机开关、充电上限、STA+AP、稳定 MAC、
  power_profile、cpuset）对应的改动只编译过、**没上过机**，判据抄自 `docs/TODO.md`「▶ 1.0 批 0/1」开头"批 2 ROM 侧五项"段与「▶ 1.0 批 2 · 网络」一节。
* **要重启的项**（A28–A31、A34）：不用人动手，但按 CLAUDE.md 的禁忌 3，重启前先征得用户同意、确认有人能长按电源键；
  在一个新候选版上**第一次**跑执行端（A30 / A31）时要人在场 —— 执行端卡住只能长按电源键。

---

## A 档：无人值守（`scripts/accept.sh`）

编号与 `accept.sh` 的输出一一对应。"来源"一列：v063#n / v070#n = TODO 里那一版清单的第 n 项，v071 = v0.7.1 那段，G = 1.0 计划 §1。

| # | 检查 | 命令 / 读什么 | 通过判据 | 来源 |
|---|---|---|---|---|
| A1 | 构建与内核 | `getprop ro.build.date.utc`、`ro.build.characteristics`、`/proc/version` | 戳 = 候选版的戳（`--stamp`）；`tablet` | v063#1、v070#1 |
| A2 | 开机状态 | `sys.boot_completed`、`/proc/uptime`、`persist.sys.boot.reason.history` | 1；开机 ≥10 分钟（不到判 WARN） | PERF-12 |
| A3 | 安全默认值 | `ro.adb.secure`、`ro.debuggable`、`persist.adb.tcp.port`、`/proc/net/tcp{,6}` 里 `:15B3` 的 LISTEN、`/product/etc/security/adb_keys` | 1 / 0 / 空 / 0 个 / 不存在（D1：debuggable 一起关） | G1 |
| A4 | 待机默认值（S1） | `grep allow_suspend /vendor/build.prop` | 镜像默认 = 1（设备当前值只记录：开发机持久 0） | v063#8 |
| A5 | 自研服务与域 | `getprop init.svc.<名>`；`ps -AZ` | `smmustall` `hangdump` `vendor.{usb,boot,camera-provider,sensors,light,thermal}-gaokun3` 都 running；`smmu-nostall.sh` 在 `gaokun3_smmustall`、`android.hardware.usb-service.gaokun3` 在 `hal_usb_default` 域 | v063#7 |
| A6 | 三颗 DSP | `/sys/class/remoteproc/*/{name,state}` | adsp / cdsp / slpi 都 running | v063#7 |
| A7 | 传感器 | `dumpsys sensorservice` 的 Sensor List | 有 `SH3001 Accelerometer`、`SH3001 Gyroscope` | REL-6 |
| A8 | Wi-Fi / 热点 | `cmd overlay lookup com.android.wifi.resources com.android.wifi.resources:string/config_wifi_tcp_buffers`；`dumpsys connectivity` 的 `TcpBufferSizes`；`dumpsys wifi` 的 `config_wifiSaeUpgradeEnabled`；`/vendor/bin/hw/hostapd` | 资源值与活动网络都含 `8388608`（没有活动网络时后者 SKIP）；SAE 升级 `false`；hostapd 在 | v063#5、v070#9、v071（#11） |
| A9 | USB 服务（#13） | `service check usb`；`pm list features` | `found`；有 `android.hardware.usb.host` | v071 |
| A10 | NTP | `settings get global ntp_server`；设备与宿主机 `date +%s` 之差 | `null`（没手动设）；≤5 秒（5–60 WARN，>60 FAIL） | v070#7 |
| A11 | 硬件解码 | `scripts/verify-hw-codec2.sh [片子]` | 小结失败 0；给了 `--video` 时第 5 步真解出帧 | v070#2 |
| A12 | 相机 | `/data/local/tmp/gaokun3-ncam-smoke` 与 `… front` | 两次最后一行都是 `RESULT: PASS` | v063#3、v070#3 |
| A13 | 麦克风 | `/data/local/tmp/micverify/gaokun3-mic-smoke -r 48000 -c 2 -t 5 -T`（不出声、不落文件） | 最后一行以 `RESULT: PASS` 开头（工具打的是 `RESULT: PASS (开头静音 N ms)`，`mic-smoke.c:511`）；logcat 有 `first capture block ready`、没有 `incomplete data received`。TIMING 行（开头静音、D ≈ 1 ms、E ≈ 21 ms）打进报告，人对照 v070#4 看 | v070#4 |
| A14 | 扬声器增强注册 | effect HAL（`android.hardware.audio.effect.service-aidl.example`）的 `/proc/<pid>/maps`；logcat | maps 里有 `libgaokunhisteneffect.so`；没有 effect 相关（`soundfx` / `histen` / `audio.effect`）的 `not accessible for the namespace`（App 自己的命名空间报错只记一笔） | v070#5（B2-0） |
| A15 | GPU | `scripts/verify-turnip.sh 600`（`--soak 600`） | 小结失败 0：GLES 行是 turnip、GMU 错误 / a6xx_recover 0、smmustall 心跳 `抓 fault=0`、帧读回有字节；浸泡前后三项不增、桌面进程 PID 不变 | PERF-12 |
| A16 | 崩溃 | `/data/system/dropbox` 里本次开机以来的条目 | `system_server_crash` 0；系统进程的 `SYSTEM_TOMBSTONE` 0。按 tombstone 头分类：`/data/local/tmp` 下的测试工具、`uid` ≥ 10000 或 `/data/app` 下的 App 只 WARN（跑过 B9 / C1 之后游戏自己的崩溃不算系统回归）；`system_server_anr` 0（否则 WARN） | PERF-12 |
| A17 | SELinux | `logcat -b all` + `dmesg` 里的 avc，`scripts/selinux/avc-summary.py`（permissive=1 / =0 各一遍）按（主体, 类型, 类, 权限）元组去重计数 —— 不依赖 `audit()` 戳，servicemanager 打的用户态 `service_manager` 拒绝也算进来 | 0 为 PASS；非 0 为 WARN，看有没有新的大面积 denial；`audit_lost` 0 | v063#6、v070#6、v071 |
| A18 | 待机计数与性能快照 | suspend_stats、qcom_stats、两簇 `time_in_state`、GPU `trans_stat`、thermal、cooling_device；频率与温度在 A15 前后各存一份（`freq-before.txt` / `freq-after.txt`），报告里出一行 A15 期间的平均频率增量 | 空闲时 cooling_device 全为 0；快照存进报告目录供下一版对比 —— **跨版本比 A15 期间的增量**，开机以来的累计值受开机时长影响 | PERF-12、REL-6 |
| A19 | GApps | `pm list packages com.google.android.gms` | 已装（D7：只发 GApps 版） | D7 |
| A20 | root | 经 adb root 或 `su -c` 的 `id -u` | 0（D6：保留 KSU；ro.debuggable=0 之后要验它还在） | D6、B1 |

A 档里不在 `accept.sh` 的这几项（要手动，但不用人在场；A28–A31、A34 要重启，见上面"要重启的项"）：

| # | 检查 | 命令 | 通过判据 | 来源 |
|---|---|---|---|---|
| A21 | iris 回归 15 项 | `out/iris-rc/rc-accept.sh`（⚠️ 不在仓库里，只在维护者本机的 `out/`） | 播完后 seek、seek、分辨率变化、stop、drain 全过 | v070#2 |
| A22 | 热点起得来（#11） | USB adb 下 `cmd wifi start-softap gk3test wpa2 <口令>` → `dumpsys wifi` 看 AP 起来 → `cmd wifi stop-softap` | AP 起来；停掉后 STA 自己重连。⚠️ 会断 STA，TCP adb 下别做 | v071 |
| A23 | USB 角色的开机路径（批 1） | `logcat -b all -d \| grep 'SVC_EXEC service gaokun3_role_device'`；`getprop vendor.gaokun3.usbrole.broken` | 有 `… started; waiting` 那一行，且 USB adb 照常起来；`broken` 为空或 0（为 1 说明这次开机里 port0 坏过，A6） | PWR-5、PWR-4、B1 |
| A24 | 诊断通路（发布构建） | `adb shell su -c id`、`adb shell dmesg`、`adb logcat -b kernel -d`、`adb bugreport` 各跑一次（`su` 要先在 KSU 管理器里给 Shell 授 root，一次性、要人在场） | `su -c id` 为 0；其余三条能不能用逐条记下 —— FAQ、`gsf-android-id.sh`、`accept.sh` / `standby.sh` 的 su 路径都靠它们，不能用就改 FAQ | B1、D1、D6、REL-9 |
| A25 | `/data` 保留块（STOR-5） | `su -c "tune2fs -l /dev/block/by-name/userdata" \| grep -iE 'Reserved block count\|Reserved blocks gid'`；`getprop vold.has_reserved` | 32768 与 1065；`1`（`vold.has_reserved` 只说明 fstab 有 reservedsize，vold `main.cpp:256-258`；保留块以 tune2fs 那两行为准）。开机日志里 fs_mgr 的 `Setting reserved block count` 只在第一次开机出现 | STOR-5 |
| A26 | Wi-Fi 软件 PNO 与 NTP 重试 | `dumpsys wifi` 的 WifiResourceCache；`dumpsys network_time_update_service` | `config_wifiSwPnoEnabled=true`，且 DeviceConfig `wifi/software_pno_enabled` 已被 tree-fixes 绕过（只记录它的值）；`mTryAgainTimesMax=-1`，`mServerUris` 第一项是 `ntp.aliyun.com` | NET-1、NET-12 |
| A27 | 传感器按订阅启停 + 亮灭屏 50 次 | 息屏、没有订阅者时隔 10 秒看两次 `/proc/interrupts` 的 smp2p-slpi 与 q6v5 handover 两行（10-04 实机是 IRQ 19 / 212）和 debugfs `qcom_stats/slpi`；然后 TCP adb 下 `input keyevent 26` 亮灭屏 ≥50 次，每次亮屏后看 `dumpsys sensorservice` 的 Recent events | 息屏时两行 IRQ 不涨、`slpi` 的 Count 涨、logcat 有 `SscHub: accel 停用`，5 分钟内没有看门狗的"15 秒无读数""60 秒没有读数，重建"；每次亮屏后都有 accel 新读数、Z≈9.8，SscHub 不重建会话；dropbox 里 `system_server_crash` 0；`logcat -s gaokun3-lights` 里的写失败行记下 errno（不判） | PWR-3、LIVE-2、HW-3、LIVE-1 |
| A28 | 启动入口：条目部署与 bless（S9；要重启） | 装上候选版后开机**两次**（升上来的第一次走直连条目，第二次起才经入口）。每次开机完成后 `getprop \| grep vendor.gaokun3.bootentry`；用私有挂载点（不叫 `/mnt/esp`）**只读**挂 ESP 看 `EFI/gk3boot/` 与 `loader/entries/` | 第一次 `via=direct`；第二次起 `via=gk3boot`、`mode=action`、`version` = `/vendor/boot/gk3boot/version`、`error` 空、`done` 非空；`getprop ro.bootloader` = `gk3boot-<ver>`；ESP 上 `EFI/gk3boot/<ver>/{gk3boot.efi,fastboot.img}` 与 `/vendor/boot/gk3boot/` 下的逐字节相同（sha256）；当前槽的条目已祝福成 `gk3boot-android-<x>.conf`（不再带 `+N`）、options 含 `gk3.dispatch=1`；有 `gk3boot-tools.conf`；`logcat -b kernel` 里正常开机是 "nothing written"（ESP 零写入） | S9、E5、`ca55b5c` |
| A29 | 启动入口：streak 清零 | A28 第二次开机后：`getprop ro.boot.gk3boot.streak`、`getprop vendor.gaokun3.bootentry.streak`、`bash scripts/misc-dump.sh` | 两个 streak 都是 1（入口 +1）；misc 的 GK3 记录 `boot_streak=0`（开机完成清零）；BCAB 当前槽 successful；`vendor.gaokun3.bootentry.notify` 为空（本版刚回滚过除外） | S9、dev.5 |
| A30 | fastboot getvar（执行端；要重启） | USB 线插 port0（靠近电源键）接宿主机；`bash scripts/misc-dump.sh --raw before.bin` → `adb reboot bootloader` → `fastboot devices` → `fastboot getvar all` → `fastboot oem device-info` → `fastboot reboot`；回到 Android 后再 `misc-dump.sh` | `fastboot devices` 有 `gaokun3`；`version-bootloader` = A28 的 `<ver>`、`current-slot` = 当前槽、`gk3-why: bootloader`、`gk3-disk-ok: yes`、`gk3-fastbootd-version` 不带 `-dirty`；回来后 BCB 全零、BCAB 没被扣 tries、`boot_streak` 只在回 Android 那次 +1 | E6、E7、`docs/hw/gk3boot-e6exec-e7dispatch-20261005.txt` |
| A31 | BCB 分派（要重启） | 接 A30：`adb reboot fastboot` → `fastboot getvar gk3-why` → `fastboot reboot`；`adb reboot recovery` → 同样看 why、`fastboot reboot` 回去；`adb reboot bootloader` 后 `fastboot reboot bootloader` 再 `fastboot reboot` | why 依次是 `fastboot`、`recovery`、`bootloader`；`fastboot reboot bootloader` 冷重启后仍回执行端；每次回 Android 后 BCB 全零、BCAB 不动。⚠️ 别手写 wipe 类 BCB（那是 B22 / E10，要另行同意） | E7、D4 |
| A32 | 充电上限：只读检查（PWR-14；**不写 EC**） | `cat /sys/class/power_supply/gaokun-ec-battery/{battery_adaptive_charge,charge_control_start_threshold,charge_control_end_threshold,smart_charge_delay}`；`ls -lZ` 这四个节点；`dumpsys vendor.lineage.health.IChargingControl/default`；`pm list packages org.lineageos.lineageparts` | 四个值记进报告（下一版对比；充电控制关着时使能位应为 0）；属主 `system:system`、标签 `sysfs_batteryinfo`；HAL 服务在、只报 LIMIT；LineageParts 已装（「电池 → 充电控制」的入口靠它）。写 EC 的那半在 B19 | PWR-14、D17 |
| A33 | STA+AP 的静态部分（NET-2） | `ls /sys/class/net`；`logcat -b all -d \| grep gaokun3-wlan`；`dumpsys wifi` 里 HalDeviceManager 的 chip mode / 接口组合 | 有 `wlan1`（开机预建）；HalDeviceManager 报了 STA+AP 的组合。真开热点连手机的那半在 B7 / B20 | NET-2 |
| A34 | 稳定 MAC（NET-4；要重启） | `cat /sys/devices/soc0/serial_number`；`ip link show wlan0` 记下地址，重启两次各再看一次 | 序列号非空；三次地址相同、首字节含 0x02（本地管理位）、不是固件给的 `00:03:7f:…`；`dumpsys wifi` 里随机化总开关仍为 false（1.0 不开，`508b395`） | NET-4 |
| A35 | power_profile（LIVE-5） | `dumpsys batterystats` 里 power profile 相关的输出（参数先 `dumpsys batterystats -h` 查，别凭记忆） | 2 个 CPU 簇（policy0 / policy4）、21 / 18 个频点、电池容量 4483；「电池用量」页能出应用排行（这半要人看一眼，可并进 B 档任一项） | LIVE-5 |
| A36 | cpuset（PERF-6） | `cat /dev/cpuset/{background,system-background,foreground,top-app}/cpus`（路径待实机核对）；任挑一个后台进程看 `/proc/<pid>/status` 的 `Cpus_allowed_list` | background 与 system-background 为 `0-3`，foreground 与 top-app 为 `0-7`；后台进程 `0-3`。帧率回归看 C1 | PERF-6 |
| A37 | 待机开关的属性镜像（PWR-16） | `getprop persist.sys.gaokun3.allow_suspend`、`getprop persist.vendor.gaokun3.allow_suspend` | 前者设过时两者相同（`init.gaokun3.rc` 的字面值触发器镜像）；前者没设过时只看后者（镜像默认 1，开发机持久 0）。拨开关的那半在 B18 | PWR-16 |

---

## B 档：要用户在场

| # | 检查 | 怎么做 | 通过判据 | 来源 |
|---|---|---|---|---|
| B1 | 前后摄目视 | 解锁后开 Aperture，后摄、前摄各看一次，点按对焦 | 取景是正的（后摄朝向 180，#119 §7）；点按能锁焦 | v063#2 |
| B2 | USB 拔插 | 拔插一次 USB 线；port0 / port1 各插一次 U 盘，亮屏、息屏各一次 | USB adb 能回来（0048 + follow）；U 盘能挂上。⚠️ 待机后回插 port0 失败是已知问题 A6 | v063#4、批 1 |
| B3 | USB 授权弹窗 | 发布构建插 USB | 弹 RSA 授权框；不授权时 `adb devices` 是 unauthorized | G1 |
| B4 | 真实待机 | 临时 `setprop persist.vendor.gaokun3.allow_suspend 1` → 拔 USB → 息屏等它睡 → 电源键唤醒 → 插回 USB → 改回原值 | `suspend_stats/success` 涨了；TCP adb 回来；插回后 USB adb 回来。⚠️ 别插着线直接 `echo mem`（device 角色挂起会整板复位，#128 §17） | v070#8 |
| B5 | 扬声器增强开关 | 设置 › 提示音和振动里的开关：开 / 关两向，各放一段音乐；有耳机时插耳机再来一遍 | 默认关；关着时音频正常（bypass）；开着时 logcat 有 `Histen chain up`；耳机不走扬声器链 | v070#5（B2-1…B2-5） |
| B6 | 出声音频 | 扬声器、耳机、双麦录音、蓝牙耳机各放 / 录一段；音游跑一遍出声回归（#130 §7） | 都有声、无爆音；音游卡顿后下一声即恢复。⚠️ 回环工具 `gaokun3-loopback` 10-04 自己崩过一次，回归前先修稳 | Stage 4、v071、PERF-12 |
| B7 | 热点让真设备连上 | **先连着 Wi-Fi**，再开热点，手机连上并打开网页 | 平板自己的 Wi-Fi **不断**（NET-2 起；断了说明 STA+AP 没生效，已知限制里那条不能删）；手机连得上、能上网 | v071 未测、NET-2 |
| B8 | 混合 WPA2/WPA3 路由器 | 连一台 WPA2/WPA3 混合模式的路由器 | 连得上、不反复断 | v071 未测 |
| B9 | App 冒烟 | 解锁后各开一次：明日方舟、三角洲、卡拉彼丘、Phigros、Arcaea、QQ、网易云、Brave | 0 崩溃（之后 `accept.sh` 的 A16 里 `data_app_crash` 不新增） | v071 |
| B10 | 1.0 RC 追加 | 合盖三种姿态的 getevent（DISP-1）；屏幕方向 A/B（DISP-2）；录屏与录像（AV-1）；开机菜单不接键盘能否操作（INST-18）；金融类 App（APP-4）；首次开机向导完整走一遍并截图（OTA-13） | 按各条目的判据；结果记进 1.0 计划对应条目 | 1.0 计划批 1 / 批 3 |
| B11 | 安装路径（G4） | 外接盘 / 真 NVMe 上：整盘清空、双系统、清除数据重装、从 U 盘启动 | 各通过一次，记进 `docs/stage7-flutter-debian.md` §M4a | G4 |
| B12 | OTA 与回落（G6） | 在 `_b` 上从上一版保留数据 OTA 一次；让新槽故意起不来（E8 的做法：新槽连续 panic，`docs/hw/gk3boot-e8-20261005.txt`）。1.0 RC 另做一次 **0.7.x → 1.0 的那一跳**（E11：升上来第一次走直连、开机完成后 HAL 部署入口，第二次起经入口） | 数据原样；6 次 tries 用完后 gk3boot 自动回旧槽、`vendor.gaokun3.bootentry.notify` 含 `fallback`、Parts 弹"已自动退回旧版本"；E11 那跳 A28 / A29 照样过 | G6、E8、E11 |
| B13 | live 安装器写盘安全（G5） | 启动进 live（开机菜单的 `gaokun3 installer`，要重启 ⇒ 要用户同意、在场），ssh 进去跑 `systemctl is-active sleep.target`；写盘期间按电源键、合盖各一次（用测试盘）；本机 `cd live/installer-flutter && flutter test test/flow_test.dart` | `sleep.target` 是 `masked`、写盘不被打断；flow_test 里有"下载失败 → 重试"用例且通过。2026-10-05 两样都已合进 main（`3608a52`：`build-rootfs.sh` mask 5 个睡眠 target，`flow_test.dart` 有"下载失败（盘没动）… 重试"用例，本机 flutter test 72/72）；⬜ mask 还要等 `build-live.sh` 重建后在真 live 里验 | G5 |
| B14 | adb 开关与 root（发布构建） | 开发者选项里打开「USB 调试」→ 关掉再开 → 重启；「无线调试」用配对码配对一次；KSU 管理器给一个 App、再给 Shell 授 root | 打开时 USB 枚举并弹授权框；关了再开不断连；**重启后开关保持原状**（开着重启仍开、关着重启仍关：#41 起有 UsbDeviceManager，开关记进 `persist.sys.usb.config`；没有 0096 的旧内核上仍是复位成关）；切「USB 用途」或开关 USB 调试时 adbd 会重启、TCP / 无线调试断一下再回来（AOSP 原样行为）；无线调试能配对、能连；App 拿得到 root、`adb shell su -c id` 为 0。全新装机另看：开机后 adbd 不在跑、`settings get global adb_enabled` 为 0 | B1、D1、D6 |
| B15 | U 盘与外设细项（B2 之外） | port1 插 FAT32、exFAT 的 U 盘各一次：`sm list-disks`、`sm list-volumes all`；port0 插 U 盘或鼠标、allow_suspend=1，息屏→亮屏 5 次后换插 PC；开机时插着 U 盘，开机后拔掉换插 PC | `disk:8,0`、`public:8,N mounted`，「文件」里看得到；port0 每次仍是 host、外设还在、`logcat -s gaokun3-usbrole` 有"保持 host"，换插 PC 后 USB adb 回来；开机插着 U 盘那条，切回 device 后 `/sys/class/udc/a600000.usb/function` 为 g1、`/config/usb_gadget/g1/UDC` 非空。⚠️ port0 可能坏到要重启（A6），全程用 TCP adb | STOR-1、PWR-5、STOR-2 |
| B16 | 亮屏后的方向、陀螺仪与亮度 | 亮屏后转动机器；游戏里只开 gyro、accel 已开再开 gyro、切后台再回来各一次；目视亮屏后的亮度 | 旋转及时跟上，亮屏后那段 UNRELIABLE 零向量不误转、不卡住；陀螺仪三种情形与 Game Rotation Vector / Gravity 正常；亮度和熄屏前一致 | PWR-3、LIVE-2、LIVE-1 |
| B17 | Wi-Fi 断开后回连（软件 PNO） | 息屏但系统醒着（插电或持锁）时让 AP 断开再恢复；allow_suspend=1 时再做一次 | 2 分钟内重新连上；定时唤醒后能重新连上 | NET-1 |
| B18 | Parts 待机开关（PWR-16） | 解锁后进「设置 → 电池 → 待机（睡眠）」，看位置、图标、文案；拨关 → 拔 USB、息屏 2 分钟；拨开 → 拔 USB、息屏等它睡；重启后再看开关。全程 TCP adb（⚠️ 插着电脑 USB 时本来就不睡，别插着线测） | 开关在「电池」页；拨关：`persist.vendor.gaokun3.allow_suspend` = 0、`/sys/power/suspend_stats/success` 不变；拨开：= 1、下一次息屏后 success 增长；重启后保留；**设备上以前 setprop 的 0 要如实显示成"关"**。enforcing 试跑（oneshot）时再拨一次，avc 为 0 | PWR-16、S1 |
| B19 | 充电上限：写 EC（PWR-14；**要用户同意，D17**）。⚠️ 2026-10-06 用户定 D17「保持现状」：**不做这项实测**，留作将来参考，发版不跑 | 先做 A32 记下原值；插原装充电器、电量 > 80% 时在「电池 → 充电控制」打开并选"限制"；然后关掉 | 打开后四个节点 = 1 / 70 / 80 / 0，`current_now` 掉到 ≈0 或转负、`status` 不再是 Charging；关掉后使能位回 0、恢复充电。⬜ 没证据的两点要记下实测：EC 停充时是否旁路供电；EC 对 start = 0 / end = 100 的行为 | PWR-14、D17 |
| B20 | 热点 5 GHz / WPA3（NET-7） | 热点设置里选 5 GHz、WPA3 各开一次，手机连上；`cmd wifi get-country-code` | 能选、能连、能上网；国家码非 null。⚠️ STA 5 GHz + AP 2.4 GHz 的双信道组合驱动报了支持，固件实际表现没验证过 ⇒ 记下 STA 当时的频段 | NET-7、NET-2 |
| B21 | fastboot 执行端：目视与按键 | 接 A30（或开机菜单选 `Android fastboot / boot menu`）：看屏幕；音量上 / 下移动、电源键确认一项（如 `Reboot to Android`） | `FASTBOOT MODE`、原因、槽、USB 一行（`state=configured`）都在，画面方向正；音量键移动选项（E6 过）；**电源键确认能用（⬜ E6 没测到这条，第一次跑时补进 `docs/boot-entry-design.md` 的 E6 结果行）** | E6、S7 |
| B22 | 恢复出厂（E10；**1.0 RC 跑一次，要用户另行明确同意**） | 先把 `/data/misc/adb/adb_keys`、Wi-Fi、KSU 等备份到外接盘（发布构建擦掉 adb_keys 就连不上 adb）；设置 → 系统 → 重置选项 → 清除所有数据；步骤见 `tools/gk3boot/README.md` §15.6 的 E10 | gk3boot `why=wipe` → 执行端免确认 → 擦 userdata + metadata → 自动重启 → fs_mgr 重建 → 开机向导；BCB 清零。过了：已知限制 / INSTALL / FAQ（中英）的恢复出厂一节与 1.0 发版说明草稿的已知问题改成"可用"；没过：改回"不起作用" | E10、B6、D4 |
| B23 | USB 用途：文件传输 / PTP / 网络共享 / MIDI（#41） | 解锁后插电脑：设置 → 已连接的设备 → USB 里依次选文件传输、PTP、USB 网络共享、MIDI；文件传输时往平板拷一个 1 GB 以上的文件并比哈希，中途息屏一次；关掉 USB 调试、只开文件传输时 port0 插 U 盘、切一次 USB 用途、拔 U 盘再插电脑；插着电脑息屏时用 adb 切 5 次 USB 用途；开着 USB 调试时跑一遍 `scripts/game-mode.sh` 的 on / off / 定时恢复 / 重启兜底 | `ls -l /sys/class/android_usb/android0/state` 是 `root system -rw-rw-r--`，插着电脑时为 `CONFIGURED`、拔线当场 `DISCONNECTED`；`dumpsys usb` 有 `kernel_state` / `current_functions`；电脑上分别出现 MTP 设备、PTP 设备、RNDIS 网卡（Windows 自动装驱动、拿到地址、能上网）、MIDI 端口；拷完哈希一致、全程不挂起；U 盘那条换插电脑后 3 秒内枚举到（0112）；息屏切功能时 `logcat -s gaokun3-usbrole` 不切 host；游戏模式四条路径后 adb 能回来、`persist.sys.usb.config` 不被写空。⚠️ 插着电脑时息屏不睡（PWR-11），别拿它测待机 | PWR-6、#41 |
| B24 | 手写笔设置 | 设置 → 已连接的设备 → 手写笔设置，中文和英文各看一次：笔迹平滑从弱往强逐档拖，每档停一下（一次连续拖动只写一次），各在 OneNote 里放大画几条快线、写几个字；双击笔身功能的三个选项各双击几次；最后选一个非默认档位，重启 | 入口在"已连接的设备"页；滑块上有四个刻度，两端下方是"弱""强"；每次改动后 `dmesg` 有 `settings: filter beta …, side …`，beta 依次是 300、200、125；越强线条越平滑、笔迹越落后；选"关"时双击不切换，笔的电量照常显示；重启后页面和 `getprop vendor.gaokun3.pen_beta` 仍是重启前的值 | `stylus/gk3pend.rc` |

---

## C 档：长稳与测量

| # | 检查 | 怎么做 | 判据 / 记录 | 来源 |
|---|---|---|---|---|
| C1 | 游戏性能基线 | 用户解锁、进固定场景后：`SER=gaokun3 bash scripts/perf/game-perf.sh -p com.tencent.tmgp.dfm -t 1200`（三角洲 20 分钟）；`-p com.idreamsky.klbqm -t 600`（卡拉彼丘 10 分钟）；`-p com.hypergryph.arknights -t 1800`（明日方舟挂机 30 分钟）。之后马上 `accept.sh --readonly` 看 A15 / A16 | 每个记平均 fps、1% low、CPU / GPU / 内存最高温、温控压频秒数、GPU 频率分布；GMU / fault 0。**和上一版比，平均 fps 降超过 10% 或最高温升超过 5 °C 判失败**。先看 `header.txt` 确认帧率上限是 60 还是 120（PERF-2 复核）。**UBWC 的 A/B 同时做**（DISP-18 并入 PERF-2，`docs/v1.0-plan.md:141`、`:229`；TODO B5b）：同一场景在 `vendor.minigbm.debug=nocompression`（`device.mk:224`，历版发布值）与 UBWC 开着各跑一次，两份 `summary.txt` 并排记 —— 切换要改镜像或 overlay，按 TODO B5b 的做法来，别凭"理应更好"直接改 | PERF-2、PERF-12、DISP-18 |
| C2 | 拔线待机 8 小时 | 插着线 `SER=gaokun3 bash scripts/perf/standby.sh start` → 用户临时把 allow_suspend 设 1 → 拔 USB、息屏放 8 小时 → 插回 `pull` → `stop` → 改回原值 | 不设合格线，但必须量过（G8）：掉电点数 / 每小时、charge_now 的 mAh、挂起次数、`cxsd` / `aosd` / `ddr` 增量写进案卷与发版说明。`cxsd` 仍为 0 时按 1.0 计划 B7 的顺序排查。软件 PNO（NET-1）进镜像后，每小时的唤醒次数和掉电与上一版并排记，看 PNO 有没有让待机多醒、多耗电 | B7、PERF-3、G8、NET-1 |
| C3 | 亮屏硬解 1 小时 | 固定亮度，`HB=300 … standby.sh start`，拔线连续硬解播放 1 小时，`pull` | 掉电点数写进案卷（G8） | B7、G8 |
| C4 | 72 小时狗粮 | allow_suspend=1、拔线日常使用，`standby.sh start` 全程开着；结束 `pull` | 真实挂起 ≥30 次；`system_server_crash` 0、tombstone 0；心跳行里关键进程 PID 不变、RSS / fd 无单调增长；每次唤醒后 `wlan=up` 能回来；结束后 `accept.sh` 全过（G7） | B7、PERF-4、G7 |
| C5 | 挂起 / 恢复循环（#16） | 内核改动时：`scripts/s2idle/android-ath11k-s2loop.sh`（用法见脚本头；先切 host） | 全部恢复、Wi-Fi 每次回来；恢复期间 GFP_DMA 高阶分配 0（#131） | v071（#16） |
| C6 | 60 / 120 Hz 电流对比 | 同一场景各测一次 `current_now` | 数据交给 D16（刷新率选项）决定 | LIVE-7 |

---

## D 档：发版收尾（宿主机侧）

| # | 检查 | 怎么做 | 通过判据 | 来源 |
|---|---|---|---|---|
| D1 | 内核能从仓库重建 | 设备 `/proc/config.gz` 与照本仓配方重建出的 `.config` 做 diff；两个内核镜像做全字符串差集，dtb 比 sha256 | config 无差；字符串差集无未解释项；dtb sha256 一致（字符串差集看不见 DTS） | `TODO.md:244`（10-06 之前的行号，那段现在在 archive/TODO-history-2026-10.md，按节内标注的原行号找）、`stage4-findings.md:4691` |
| D2 | 文档不过时 | `grep -rnE "cannot suspend\|不能待机\|=m\|❌" README.md README.zh-CN.md docs/INSTALL.md docs/TODO.md`，再对 README 状态表与本版实测；设备树里的成段注释也会变质（`init.gaokun3.rc`、`etc/usbrole.rc`、`device.mk` 都出过事），另跑 `grep -rnE "^[[:space:]]*#.*(cannot suspend\|不能待机\|❌)" device/huawei/gaokun3/` 只看注释命中 | 每一处命中都确认仍然成立 | `project-log.md:494`、`TODO.md:1017`（10-06 之前的行号，那段现在在 archive/TODO-history-2026-10.md，按节内标注的原行号找）、`stage4-findings.md:8053` |
| D3 | 发的就是验过的那一版 | `release.sh --no-build`；变体是 `lineage_gaokun3-cp2a-userdebug`；各分区 build.prop 的 `date.utc` 对一遍（system / system_ext / product / vendor / odm，vendor 的曾停在 09-28 —— OTA-11 / SEC-13） | 构建戳 = A1 验过的戳；各分区 `date.utc` 一致（G6 / OTA-11：`2bf59e1` 起 `release.sh` 第 2 步断言各分区的 `ro.<part>.build.date.utc` 与 system 相同，⬜ 还没在真构建上跑过，第一次跑时仍手动对一遍；构建前要 `rm -f $OUT/vendor/build.prop`，构建机的 `~/iris-work/rom-build.sh` 还没加） | CLAUDE.md、#117 §15、G6 |
| D4 | 产物齐、字节对 | GitHub release 附件逐个核对服务端字节数；R2 清单最后传、设备侧抓取 200；`install-artifacts.sha256` | 字节数与本地一致（别信上传命令的输出，CLAUDE.md 运维坑 1）。`2bf59e1` 起每版多出 `kernel-source.txt`、`kernel-config.txt`、`kernel-base-patches.tar.gz`，`repo manifest -r` 能跑成时还有 `crdroid-manifest.xml` —— 都要传、都要核字节数（它们不进 `install-artifacts.sha256`，sha256 记在 `kernel-source.txt` 里），发版说明的 Files 一节要链到 `kernel-source.txt` | v0.7.0 / v0.7.1 发版记录、REL-7 |
| D5 | 安装器与 ROM 一致 | `release-installer.sh --artifacts <本版 install-artifacts.sha256>`（2026-10-11 起必填，带 `--payload` 时默认取发布目录里的）：`--boot-img` 的 sha256 必须等于清单里 `boot.img` 那一行，镜像内核再与 `--boot-img` 比（`GK3_BOOTIMG_SHA256`） | 两道都过 | G11 |
| D6 | 发版说明与披露 | 已知问题 / 不支持一节：root（D6）、AOSP test-key 签名（D2）、`/data` 不加密（D3）、专有组件、Widevine、U 盘 / MTP / 指纹 / 手写笔等；GPL 对应源码清单 | 每项都写了 | G2、G3、G10、REL-7 |
| D7 | 验收记录存档 | A 档报告目录、B / C 档结果 | `report.txt` 汇总行 FAIL 0；B / C 档结果写进 TODO 的本版一节 | G11 |
| D8 | SELinux 的去留（G9） | 二选一：做完 #129 §6 剩下的 3 项、默认切 enforcing（`getenforce` = `Enforcing`，A17 在 enforcing 下跑一遍 `avc-summary.py --enforcing`）；或者书面接受 permissive | 有用户的决定（1.0 计划决定 D5）**加一份验收记录**：切了就附 enforcing 下的 A 档报告；没切就在发版说明里披露、并把决定记进 TODO 的本版一节 | G9、SEC-4 |
| D9 | 发的是发布构建（B1） | 构建时**不设** `GAOKUN3_DEV_BUILD`；同一棵 `out/` 从开发构建换过来要先 `m installclean`；`release.sh --dry-run --no-build` 看第 2 步 | 每份 build.prop 都没有 `ro.adb.secure≠1`、`ro.debuggable≠0`、`persist.adb.tcp.port`，至少一份写了 `ro.adb.secure=1`；`product/etc/security/adb_keys` 不存在或为空。`GAOKUN3_DEV_BUILD=1` 的构建只能 `--stage-only`。装机后由 A3 复核 | B1、D1、G1 |
| D10 | 发的入口与执行端就是测过的那一份 | `release.sh --dry-run --no-build` 第 2 步；`cat device/huawei/gaokun3/prebuilt-gk3boot/version`（构建机） | 第 2 步"vendor 里的入口与执行端 = prebuilt"通过；版本串不带 `dirty`（dev.7 的执行端带过 `-dirty`，`29f4614` 之后只看 `tools/gk3boot` 与 `scripts/gk3boot`）；版本串 = A28 / A30 在设备上读到的；`grep gk3boot <out>/vendor/build.prop` 有 `persist.vendor.gaokun3.gk3boot=action`（1.0 起镜像默认，`device.mk:836`、`ca55b5c`；只在带了入口时设） | S9、S7、REL-13 |
