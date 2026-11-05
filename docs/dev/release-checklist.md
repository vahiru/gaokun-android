# 发版回归清单

> 每个候选版与正式版发布前按本清单执行回归：第一部分由[`scripts/accept.sh`](../../scripts/accept.sh)自动执行并配合少量手动只读/重启检查，第二部分需人在场交互验证，第三部分为长稳与功耗/性能基线测量，第四部分为宿主机发版前后的产物核对。

## 1. 使用方式

| 类别 | 执行时机 | 执行方式 | 工具 |
| --- | --- | --- | --- |
| **无人值守检查** | **每个候选版**，装入备用槽位启动且开机满10分钟后 | 无人值守（USB或TCP adb） | `scripts/accept.sh` + 手动补充项 |
| **交互验证** | 每个候选版跑与本版改动相关的项；**大版本RC全跑** | 人在场操作（解锁、插拔、目视、试听） | 下表命令 |
| **长稳与测量** | 大版本发布前，或内核 / Mesa / HWC / 刷新率 / minigbm有改动时 | 拔线放置或固定场景实测 | `scripts/perf/` |
| **发版核对** | `release.sh --no-build`前后 | 宿主机侧核对 | 下表命令 |

```bash
# 候选版装入备用槽位启动、开机满10分钟后：
SER=gaokun3 bash scripts/accept.sh --stamp <候选版的ro.build.date.utc> --soak 600 --video <测试视频.mp4>
echo $?        # 0 = 无FAIL；1 = 有FAIL；2 = adb不通或中途掉线（不要接管道后再取$?）
```

* 报告保存在`out/accept/<戳>-<时间>/report.txt`，同目录保存`logcat`、`dmesg`、`dropbox`列表及频率与温度快照，供跨版本对比。
* `--profile dev`：面向开发构建（故意开启adb免授权与TCP 5555）时安全默认值检查只记录不判失败；发布构建一律使用默认的`release`。
* `--readonly`：仅执行只读检查（跳过开相机、录音、硬解视频），可在任意时刻运行。
* **涉及重启的检查项**（启动入口部署与祝福、fastboot执行端查询、BCB分派、稳定Wi-Fi MAC）：重启前需确认有人可在设备旁长按电源键恢复；新候选版首次跑fastboot执行端时需人在场。

## 2. 无人值守检查

### 2.1 `scripts/accept.sh`自动检查项

| 检查项 | 读取内容 | 通过判据 |
| --- | --- | --- |
| 构建与内核 | `getprop ro.build.date.utc`、`ro.build.characteristics`、`/proc/version` | 时间戳等于`--stamp`；`tablet` |
| 开机状态 | `sys.boot_completed`、`/proc/uptime`、`persist.sys.boot.reason.history` | `1`；开机满10分钟（不足判WARN） |
| 安全默认值 | `ro.adb.secure`、`ro.debuggable`、`persist.adb.tcp.port`、`/proc/net/tcp{,6}`中`:15B3`的LISTEN、`/product/etc/security/adb_keys` | `1` / `0` / 空 / 0个 / 不存在 |
| 待机默认值 | `grep allow_suspend /vendor/build.prop` | 镜像默认值为`1` |
| 自研服务与域 | `getprop init.svc.<名>`；`ps -AZ` | `smmustall`、`hangdump`、`vendor.{usb,boot,camera-provider,sensors,light,thermal}-gaokun3`均为`running`；`smmu-nostall.sh`位于`gaokun3_smmustall`域、`android.hardware.usb-service.gaokun3`位于`hal_usb_default`域 |
| 三颗DSP | `/sys/class/remoteproc/*/{name,state}` | `adsp`、`cdsp`、`slpi`均为`running` |
| 传感器 | `dumpsys sensorservice`的Sensor List | 含`SH3001 Accelerometer`、`SH3001 Gyroscope` |
| Wi-Fi / 热点配置 | `cmd overlay lookup com.android.wifi.resources com.android.wifi.resources:string/config_wifi_tcp_buffers`；`dumpsys connectivity`的`TcpBufferSizes`；`dumpsys wifi`的`config_wifiSaeUpgradeEnabled`；`/vendor/bin/hw/hostapd` | 资源值与活动网络均含`8388608`；SAE升级为`false`；`hostapd`存在 |
| USB服务 | `service check usb`；`pm list features` | `found`；含`android.hardware.usb.host` |
| NTP时间同步 | `settings get global ntp_server`；设备与宿主机`date +%s`差值 | `null`；差值≤5秒（5–60秒WARN，>60秒FAIL） |
| 硬件解码 | `scripts/verify-hw-codec2.sh [视频]` | 失败数为0；传入`--video`时第5步解出有效帧 |
| 相机冒烟 | `/data/local/tmp/gaokun3-ncam-smoke`与`… front` | 前后摄末行均为`RESULT: PASS` |
| 麦克风冒烟 | `/data/local/tmp/micverify/gaokun3-mic-smoke -r 48000 -c 2 -t 5 -T` | 末行以`RESULT: PASS`开头；`logcat`有`first capture block ready`且无`incomplete data received` |
| 扬声器音效库加载 | effect HAL进程的`/proc/<pid>/maps`；`logcat` | `maps`含`libgaokunhisteneffect.so`；无音效相关的链接命名空间报错 |
| GPU浸泡 | `scripts/verify-turnip.sh 600`（`--soak 600`） | GLES渲染器为turnip、GMU错误与`a6xx_recover`为0、`smmustall`心跳`fault=0`、帧读回非空；浸泡前后错误计数不增、桌面进程PID不变 |
| 系统崩溃记录 | `/data/system/dropbox`本次开机以来的条目 | `system_server_crash`为0；系统进程`SYSTEM_TOMBSTONE`为0（测试工具与第三方应用崩溃记WARN）；`system_server_anr`为0 |
| SELinux拒绝 | `logcat -b all` + `dmesg`中的`avc`，经`scripts/selinux/avc-summary.py`去重统计 | 0为PASS；非0为WARN并核查是否有新增拒绝；`audit_lost`为0 |
| 待机计数与性能快照 | `suspend_stats`、`qcom_stats`、两簇`time_in_state`、GPU `trans_stat`、`thermal`、`cooling_device`（GPU浸泡前后各采一份） | 空闲时`cooling_device`全为0；保存GPU浸泡期间的平均频率增量供跨版本比对 |
| GApps | `pm list packages com.google.android.gms` | 已安装 |
| Root通路 | 经`adb root`或`su -c`执行`id -u` | 返回`0` |

### 2.2 手动补充的无交互检查项

| 检查项 | 命令 | 通过判据 |
| --- | --- | --- |
| 视频解码边界回归 | 运行本地`rc-accept.sh`视频回归集 | 播放完毕后seek、连续seek、分辨率切换、stop、drain全部通过 |
| 热点启停 | USB adb下执行`cmd wifi start-softap gk3test wpa2 <口令>` → `dumpsys wifi` → `cmd wifi stop-softap` | AP正常启动；关闭后STA自动重连 |
| USB角色开机路径 | `logcat -b all -d \| grep 'SVC_EXEC service gaokun3_role_device'`；`getprop vendor.gaokun3.usbrole.broken` | 服务正常启动且USB adb枚举正常；`broken`为空或`0` |
| 发布版诊断通路 | `adb shell su -c id`、`adb shell dmesg`、`adb logcat -b kernel -d`、`adb bugreport` | 在KernelSU管理器授权Shell后`su -c id`返回`0`；核对各诊断命令可用性 |
| `/data`保留块 | `su -c "tune2fs -l /dev/block/by-name/userdata" \| grep -iE 'Reserved block count\|Reserved blocks gid'`；`getprop vold.has_reserved` | 保留块数为`32768`、GID为`1065`；`vold.has_reserved`为`1` |
| Wi-Fi软件PNO与NTP重试 | `dumpsys wifi`的`WifiResourceCache`；`dumpsys network_time_update_service` | `config_wifiSwPnoEnabled=true`；`mTryAgainTimesMax=-1`，首选NTP为`ntp.aliyun.com` |
| 传感器按订阅启停与亮灭屏50次 | 息屏无订阅者时相隔10秒采样`/proc/interrupts`的`smp2p-slpi`与`q6v5 handover`及`qcom_stats/slpi`；随后在TCP adb下用`input keyevent 26`循环亮灭屏50次 | 息屏期间两路IRQ不增长、`slpi`休眠计数增长、日志有`SscHub: accel 停用`且无看门狗重建；每次亮屏后均有新读数且`system_server_crash`为0 |
| 启动入口部署与祝福（需重启） | 安装候选版后重启两次，每次检查`getprop \| grep vendor.gaokun3.bootentry`并只读挂载ESP核对`EFI/gk3boot/`与`loader/entries/` | 首次开机`via=direct`，第二次起`via=gk3boot`、`mode=action`、`error`为空；`ro.bootloader`为`gk3boot-<ver>`；ESP文件与`/vendor/boot/gk3boot/`的sha256一致；当前槽位条目已去除`+N`后缀 |
| 启动计数清零 | 第二次开机后执行`getprop ro.boot.gk3boot.streak`、`getprop vendor.gaokun3.bootentry.streak`、`bash scripts/misc-dump.sh` | 两个属性均为`1`；`misc`分区GK3记录中`boot_streak=0`，BCAB当前槽位标记为`successful` |
| fastboot执行端查询（需重启） | port0连接宿主机：`bash scripts/misc-dump.sh --raw before.bin` → `adb reboot bootloader` → `fastboot devices` → `fastboot getvar all` → `fastboot oem device-info` → `fastboot reboot` → `misc-dump.sh` | `fastboot devices`识别出`gaokun3`；`version-bootloader`、`current-slot`、`gk3-why: bootloader`、`gk3-disk-ok: yes`正确，版本串不带`-dirty`；返回Android后BCB全零且BCAB未被扣减tries |
| BCB分派（需重启） | 依次验证`adb reboot fastboot`、`adb reboot recovery`、`adb reboot bootloader` + `fastboot reboot bootloader` | `gk3-why`依次对应`fastboot`、`recovery`、`bootloader`；每次返回Android后BCB清零 |
| 充电控制只读检查 | 检查`/sys/class/power_supply/gaokun-ec-battery/{battery_adaptive_charge,charge_control_start_threshold,charge_control_end_threshold,smart_charge_delay}`权限与标签；`dumpsys vendor.lineage.health.IChargingControl/default` | 属主`system:system`、标签`sysfs_batteryinfo`；HAL服务在线且LineageParts已安装 |
| STA+AP静态接口 | `ls /sys/class/net`；`logcat -b all -d \| grep gaokun3-wlan`；`dumpsys wifi`中HalDeviceManager接口组合 | `wlan1`已预建；HalDeviceManager报告支持STA+AP组合 |
| 稳定Wi-Fi MAC（需重启） | 核对`/sys/devices/soc0/serial_number`非空；重启两次比对`ip link show wlan0`的MAC地址 | 三次开机MAC地址一致、首字节含本地管理位`0x02`，而非固件默认`00:03:7f:…` |
| `power_profile`配置 | `dumpsys batterystats`中的power profile信息 | 包含2个CPU簇（`policy0` / `policy4`）、21与18个频点、电池容量4483 mAh |
| `cpuset`分组 | `cat /dev/cpuset/{background,system-background,foreground,top-app}/cpus` | `background`与`system-background`为`0-3`，`foreground`与`top-app`为`0-7` |
| 待机开关属性镜像 | `getprop persist.sys.gaokun3.allow_suspend`、`getprop persist.vendor.gaokun3.allow_suspend` | 设置过前者时两者保持一致 |

## 3. 人在场交互检查

| 检查项 | 操作步骤 | 通过判据 |
| --- | --- | --- |
| 前后摄取景与对焦 | 打开Aperture相机，横竖屏切换查看前后摄取景，在画面各处点按对焦 | 取景方向正确（后摄Orientation 90、前摄270）；后摄点按能锁焦 |
| USB线与U盘拔插 | 拔插USB线；port0与port1在亮屏、息屏下分别插入U盘 | USB adb正常恢复；U盘自动挂载 |
| USB调试授权弹窗 | 发布构建首次连接电脑USB | 弹出RSA公钥授权框；未授权前`adb devices`显示`unauthorized` |
| 拔线深度休眠唤醒 | 设`allow_suspend=1` → 拔除USB → 息屏静置等待入睡 → 按电源键唤醒 → 插回USB | `suspend_stats/success`计数增长；唤醒后Wi-Fi与USB adb正常恢复 |
| 扬声器音效开关 | 在“设置 → 提示音和振动”中切换扬声器增强开/关并播放音乐，插耳机重复验证 | 默认关闭；关闭时直通播放正常，开启时`logcat`输出`Histen chain up`；耳机通道不经过扬声器音效链 |
| 音频播放与录制 | 测试扬声器、3.5mm耳机（经Type-C转接）、双麦录音、蓝牙耳机及音游发声 | 播放与录音正常、无爆音 |
| STA+AP热点并发 | 保持平板连接Wi-Fi，开启个人热点并用手机连接访问网页 | 平板自身Wi-Fi不断线，手机可正常联网 |
| WPA2/WPA3混合路由 | 连接开启WPA2/WPA3混合模式的路由器 | 连接稳定，无反复断连 |
| 常用应用与游戏冒烟 | 依次打开明日方舟、三角洲行动、卡拉彼丘、Phigros、Arcaea、QQ、网易云音乐、Brave | 全部正常启动运行，系统崩溃记录中无新增崩溃 |
| 姿态与录屏等综合体验 | 验证键盘盖合盖/反折磁铁响应、自动旋转、屏幕录制与相机录像、不接键盘时的启动菜单按键操作、首次开机向导 | 各项交互符合预期 |
| Live安装器四条安装路径 | 在测试盘或实机NVMe上验证：整盘清空、双系统并存、保留/清除数据重装、外接U盘启动 | 四条路径均正常走通并启动进桌面 |
| OTA升级与自动回滚 | 从上一版本OTA升级；构造新槽连续启动失败触发回滚 | 保留数据升级成功；连续失败6次后`gk3boot`自动回退至旧槽位，系统弹出回退提示 |
| Live安装器防休眠与重试 | 进入Live安装器检查`systemctl is-active sleep.target`；写盘期间测试按电源键与合盖；运行`flutter test` | `sleep.target`为`masked`，写盘不被休眠打断；单测全部通过 |
| 开发者选项ADB与Root | 开关“USB调试”并重启验证状态保持；测试“无线调试”配对码连接；使用KernelSU管理器授权应用与Shell | 重启后USB调试开关状态保持原样；无线调试可配对；Root授权正常生效 |
| U盘文件系统与角色切换 | port1分别插入FAT32与exFAT U盘；port0插入U盘经过多次亮灭屏后换插电脑；带着U盘冷启动后拔下换插电脑 | FAT32/exFAT均正常挂载；port0在host与device角色间切换后USB adb可在3秒内枚举恢复 |
| 亮屏后旋转、陀螺仪与亮度 | 亮屏后旋转机身；游戏中测试陀螺仪与切后台恢复；观察亮屏瞬间背光亮度 | 自动旋转及时响应，亮屏瞬间无误转；陀螺仪正常；背光亮度与息屏前一致 |
| Wi-Fi断线自动回连 | 息屏状态下（含`allow_suspend=1`休眠期间）重启路由器或断开AP后恢复 | 系统唤醒后2分钟内自动重连Wi-Fi |
| 电池页待机开关 | 在“设置 → 电池 → 待机（睡眠）”中切换开关并拔线息屏验证，重启后核对状态 | 开关状态与`persist.vendor.gaokun3.allow_suspend`及实际休眠行为一致，重启后保留 |
| 充电控制实机测试（按需） | 在“设置 → 电池 → 充电控制”中启用限制（需用户明确确认测试写EC操作） | 4个sysfs节点值更新，`current_now`降至0附近且停止充电；关闭后恢复充电 |
| 5 GHz与WPA3热点 | 热点设置中分别切换5 GHz频段与WPA3加密并用手机连接 | 手机正常连接并上网，`cmd wifi get-country-code`非空 |
| fastboot执行端界面与按键 | 进入fastboot执行端界面，用音量上/下键移动光标、电源键确认执行`Reboot to Android` | 屏幕显示状态信息且方向正确，音量键与电源键操作正常响应 |
| 恢复出厂设置（RC跑一次） | 备份`/data/misc/adb/adb_keys`后在“设置 → 系统 → 重置选项”执行“清除所有数据” | `gk3boot`识别`why=wipe`并转交执行端格式化`userdata`与`metadata`，重启后进入开机向导且BCB清零 |
| USB文件传输/PTP/RNDIS/MIDI | 连接电脑依次切换文件传输（拷贝>1 GB文件并校验哈希）、PTP、USB网络共享、MIDI，并测试`scripts/game-mode.sh` | 四种USB功能在电脑端均正常识别工作，大文件传输哈希一致；游戏模式开启与恢复后adb正常重连 |
| 手写笔设置 | 在“设置 → 已连接的设备 → 手写笔设置”中调节笔迹平滑档位与双击笔身功能，重启后复查 | 每次调节后`dmesg`输出对应`beta`值（300/200/125），双击功能与电量显示正常，重启后设置保留 |

## 4. 长稳与性能/功耗测量

| 检查项 | 操作命令 | 判据与记录要求 |
| --- | --- | --- |
| 游戏性能基线 | 固定场景下运行`SER=gaokun3 bash scripts/perf/game-perf.sh`：三角洲行动20分钟（`-p com.tencent.tmgp.dfm -t 1200`）、卡拉彼丘10分钟（`-p com.idreamsky.klbqm -t 600`）、明日方舟30分钟（`-p com.hypergryph.arknights -t 1800`），随后跑`accept.sh --readonly` | 记录平均帧率、1% low、最高温度、温控降频时长与GPU频率分布；GMU错误与SMMU fault为0。与上一版相比，平均帧率下降不得超过10%、最高温升不得超过5 °C |
| 拔线待机8小时 | `SER=gaokun3 bash scripts/perf/standby.sh start` → 设`allow_suspend=1` → 拔除USB息屏静置8小时 → 插回执行`pull`与`stop` | 记录每小时掉电百分比、mAh消耗、挂起次数及`cxsd` / `aosd` / `ddr`睡眠统计增量 |
| 亮屏硬解1小时 | 固定亮度下启动`HB=300 … standby.sh start`，拔线连续硬件解码播放视频1小时后`pull` | 记录1小时耗电百分比与温升 |
| 72小时日常综合测试 | 设`allow_suspend=1`并开启`standby.sh start`，拔线日常使用72小时后`pull`并跑`accept.sh` | 累计挂起≥30次；`system_server_crash`与系统tombstone为0；关键守护进程PID不变、无内存或fd泄漏；每次唤醒后Wi-Fi自动恢复 |
| 挂起/恢复压力循环 | 内核休眠路径有改动时运行[`scripts/s2idle/android-ath11k-s2loop.sh`](../../scripts/s2idle/android-ath11k-s2loop.sh) | 全部循环正常唤醒、Wi-Fi每次恢复、恢复期间无`GFP_DMA`高阶内存分配失败 |
| 60 / 120 Hz功耗对比 | 同一静止与滑动场景下分别采样60 Hz与120 Hz的`current_now` | 存档作为刷新率功耗基线 |

## 5. 发版核对（宿主机侧）

| 检查项 | 操作方法 | 通过判据 |
| --- | --- | --- |
| 内核可复现性核对 | 比对设备`/proc/config.gz`与仓库配方生成的`.config`；比对内核镜像字符串差集与DTB sha256 | `.config`无差异，DTB sha256一致 |
| 文档与注释同步检查 | 检查`README*.md`、`docs/INSTALL*.md`、`docs/known-limitations*.md`、`docs/TODO.md`及`device/huawei/gaokun3/`中的配置注释 | 状态描述与本版实测结果一致 |
| 构建时间戳一致性 | 运行`release.sh --no-build`核对`system`、`system_ext`、`product`、`vendor`、`odm`各分区`build.prop`的`date.utc` | 各分区时间戳与验收时的时间戳完全一致 |
| 发布产物与校验和核对 | 核对GitHub Release与R2上所有附件的服务端字节数及`install-artifacts.sha256`、`kernel-source.txt`、`kernel-config.txt`、`kernel-base-patches.tar.gz` | 服务端文件字节数与本地完全一致 |
| 安装器与ROM镜像匹配 | 运行`release-installer.sh --artifacts <install-artifacts.sha256>` | `--boot-img`的sha256与清单中`boot.img`及安装器内置内核完全一致 |
| 发版说明与合规披露 | 检查发版说明中的已知限制、Root说明、签名说明、专有组件说明及GPL源码对应清单 | 披露完整且与当前版本行为一致 |
| 验收报告归档 | 归档`out/accept/`下的`report.txt`与交互验证、长稳测量数据 | 无人值守检查FAIL为0 |
| SELinux Enforcing核验 | 确认`getenforce`为`Enforcing`，运行`avc-summary.py --enforcing` | 无未解释的SELinux拒绝 |
| 确认非开发构建 | 构建时不设`GAOKUN3_DEV_BUILD`；运行`release.sh --dry-run --no-build`检查第2步断言 | 所有`build.prop`均满足`ro.adb.secure=1`、`ro.debuggable=0`、无`persist.adb.tcp.port`，且无内置`adb_keys` |
| `gk3boot`预编译产物一致性 | 核对`release.sh --dry-run --no-build`第2步及`device/huawei/gaokun3/prebuilt-gk3boot/version` | `vendor`内打包的`gk3boot`与`prebuilt-gk3boot`一致、版本号不带`-dirty`且等于实机读数，`persist.vendor.gaokun3.gk3boot=action`已配置 |
