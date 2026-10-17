<!--
  维护说明（不显示）：
  * 这份清单写的是"装上这个 ROM 之前应当知道的事"：长期存在的限制、不支持的功能、安全与许可上的取舍。
    某一版特有的缺陷写在那一版发版说明的 Known issues 里，不写在这里。
  * 每条后面的注释里写着它在 docs/v1.0-plan.md 里的条目 id。发版前逐条核对；修好了就删掉那一条，
    取舍变了就改写那一条（例如 1.0.0-rc.1 时 SELinux 切成 enforcing、中文输入法真正预装进镜像，这两条就是那时改写的）。
  * 中英两份（known-limitations.md）内容必须一致，改一份就改另一份。
  * 依据（文件:行号 / 实机）写在注释里，不写在正文里。
-->
# 已知限制与不支持的功能

[**English → known-limitations.md**](known-limitations.md) · [常见问题](FAQ.zh-CN.md) · [安装](INSTALL.zh-CN.md)

这个 ROM 是在一台**没有任何厂商 Android 支持**的机器上从零搭起来的。下面这些是装之前应当知道的事：
有些是我们有意做的取舍，有些是还没做出来，有些在这台机器上做不到。

每一条都按同一个格式写：**现象**（你会看到什么）、**原因**（一句话）、**替代办法**（现在能怎么办）。

某一版特有的问题见那一版的[发版说明](relnotes/)。

---

## 一、安全与隐私（请先读这一节）

### 到 v0.7.1 为止，网络 adb 不要授权就能连
<!-- B1 / D1（docs/v1.0-plan.md:40-47 的 SEC-1、OTA-1 等）：⬜ 未构建、未上机，发版前核对。
     v0.7.1 的状态（tag v0.7.1-alpha = 21e24fd）：device/huawei/gaokun3/device.mk:43 persist.adb.tcp.port=5555；
     lineage_gaokun3.mk:64 WITH_ADB_INSECURE := true（ro.adb.secure=0）、:89 system_ext 的 ro.debuggable=1、
     :143 PRODUCT_ADB_KEYS := device/huawei/gaokun3/adb_keys（维护者公钥）。
     "所有网卡、含热点口"：v1.0-plan.md:41-42（B1 复核：热点下同样暴露，#11 之后 hostapd 已进镜像）；adbd 的监听地址
     没从源码核实（packages/modules/adb 不在本地 refs），待构建机核实。
     发布构建（B1 改动，91df0b7）：lineage_gaokun3.mk:49-130（及 :190-192 的 PRODUCT_ADB_KEYS）只在 GAOKUN3_DEV_BUILD=1 时给这几样；
     device.mk:27-43 同理。
     "老用户 OTA 后自动生效"：build.prop 里的默认值不落盘 —— refs/lineage-system-core/init/property_service.cpp:424-425
     只有经 setprop（socket）且 persist 属性已载入后才 WritePersistentProperty。自己 setprop 过 persist.adb.tcp.port
     的人 /data/property 里会留着它 —— 清除办法 ⬜ 待上机核实（v1.0-plan.md:46 的"还要验证"）。
     「USB 调试」开关：patches/0096 补上 /sys/class/android_usb 以后有了 UsbDeviceManager，它按 persist.sys.usb.config
     记住这个开关，重启后保持原状（AOSP 原生行为）。 -->
* **现象**：在 v0.7.1 及以前的版本上看不出任何异常 —— 问题恰恰在这里。
* **原因**：v0.7.1 及以前的镜像会在所有网卡（包括 Wi-Fi 热点那个口）上监听 TCP 5555 端口的 adb，连接时**不弹**
  "允许调试吗？"的授权框。镜像是可调试的（`ro.debuggable=1`），连上后再 `adb root` 就是 root。镜像里还带着维护者的
  adb 公钥。连在同一个 Wi-Fi 上、或者连上你热点的任何人，都能接管这台机器。
  从下一个发布版（计划是 1.0）起：adb 默认关闭，每台电脑都要你授权，不再监听 5555，不能 `adb root`，镜像里也不带
  维护者的公钥。老用户通过 OTA 更新后自动生效，不用做什么：原来的 5555 设置来自镜像里的默认值，Android 从不把这种
  默认值存进 `/data`。例外是自己 setprop 过 `persist.adb.tcp.port` 的人。
* **替代办法**：
  * 下一个发布版出来后尽快更新。在那之前，不要连不信任的 Wi-Fi，也不要让陌生人连你的热点。
  * 自己设过 `persist.adb.tcp.port` 的：更新后怎么清掉它，会补在这里（⬜ 待上机核实）。
  * 想通过网络用 adb，请用 **无线调试**（[常见问题](FAQ.zh-CN.md#无线调试)）。

### 系统用 AOSP 公开的测试密钥签名
<!-- B2 / SEC-2（用户 2026-10-04 定 D2：保留 test-key、披露）。证据：实机 otacerts.zip 只有 testkey.x509.pem，
     与 refs/aosp-build/target/product/security/testkey.x509.pem 逐字相同；framework-res 的证书 = platform.x509.pem；
     scripts/release.sh 没有签名步骤；ro.build.tags=release-keys 只是标签。 -->
* **现象**：看不出来。系统版本信息里写的是 `release-keys`，但那只是一个标签。
* **原因**：系统、系统应用和 OTA 更新包都用 AOSP 源码里**公开**的测试密钥签名，对应的私钥人人都能下载。
  所以**任何人**都能做出一个本机会当成正版接受的 OTA 更新包，或者一个以系统身份运行的 App。
  这台机器的安全因此取决于两件事：下载渠道没被人控制，以及你不装来路不明的"系统组件"。
* **替代办法**：
  * 只通过 设置 里自带的系统更新，或本项目的 GitHub Releases 页面更新。
  * 不要安装别人发给你的、自称"系统补丁 / 系统组件"的 APK 或 zip。
  * 全新安装时，安装器会先核对 `install-artifacts.sha256` 再写盘，别跳过这一步。

### `/data`（你的全部个人数据）没有加密
<!-- B3 / SEC-3（用户定 D3：1.0 不加密、如实披露）。证据：实机 ro.crypto.state=unsupported；
     device/huawei/gaokun3/fstab.gaokun3:27 的 userdata 行没有 fileencryption=；
     scripts/live/build-rootfs.sh:145 救援/安装系统 root 无密码（网络侧只认公钥）。 -->
* **现象**：锁屏密码只能挡住"开机后直接用"，挡不住"拿到机器的人"。
* **原因**：数据分区是明文的 ext4。Secure Boot 必须关着；开机菜单里有安装器和救援系统，本地控制台登录 root 不需要密码；
  插一个 Linux U 盘也一样。拿到机器的人可以直接读出照片、聊天记录、浏览器登录状态、Wi-Fi 密码等全部数据。
* **替代办法**：
  * 把这台机器当成"谁拿到谁就能看全部数据"的设备来用，不要在上面存放敏感资料。
  * 卖机、送修、借给别人之前，用图形安装器的 **重新安装 Android**（默认清除数据），见[常见问题](FAQ.zh-CN.md#恢复出厂--卖机前清除数据)。
  * 以后要上文件级加密，只能清空数据重装，老用户无法通过 OTA 直接获得加密。

### 默认带 root（KernelSU / ReSukiSU）
<!-- SEC-12 / REL-10（用户定 D6：保留 root、充分披露；1.0 前不出无 root 变体）。证据：实机 /proc/config.gz CONFIG_KSU=y、
     CONFIG_KSU_MULTI_MANAGER_SUPPORT=y；scripts/kernel-config-android.sh:363-420；TODO B11。
     ⚠️ 维护：v0.7.1 及以前【不预装】管理器 APK（TODO B11："ksud 就在 APK 里，装 App 即到位"）。1.0 若预装，
     下面"没有管理器时"那句仍然成立，但要在第一句写明"系统里带着 ReSukiSU 管理器"。 -->
* **现象**：内核里内置了 root 实现 KernelSU（ReSukiSU 分支）。某些银行、支付 App 和带反作弊的游戏可能检测到它，
  提示"设备存在风险"或拒绝运行。
* **原因**：开发和排错离不开 root，我们决定发布版也保留它。哪些 App 能拿到 root，由 **ReSukiSU 管理器** App 决定：
  只有你在管理器里批准过的 App 才能拿到；没有安装管理器时，没有任何 App 能拿到 root。
* **替代办法**：
  * 不需要 root 就不要在管理器里给任何 App 授权。
  * **adb 也是 root**：你授权过 USB 调试或无线调试的电脑，`adb shell` 进来直接就是 root（经 KernelSU，
    尽管 `ro.debuggable=0`）。只授权你信任的电脑，不用时把 USB 调试 / 无线调试关掉。
    <!-- 2026-10-05 在 1.0.0-dev.1/dev.2 发布构建上实测：adbd 在 u:r:ksu:s0，`adb shell id` = uid 0。 -->
  * 目前没有不带 root 的版本，内核里的 root 能力关不掉。
  * 冒烟测试里，带 ACE 反作弊的《三角洲行动》和《卡拉彼丘》能正常运行；银行和支付类 App 还没有系统地测过，欢迎反馈。
<!-- 冒烟测试出处：docs/TODO.md:136（10-06 之前的行号，那段现在在 archive/TODO-history-2026-10.md，按节内标注的原行号找）（v0.7.1 候选版 1791053208，App 冒烟 8/8）。APP-4 的金融 App 测试做完后在这里补结果。 -->

### 从 1.0.0-rc.1 起 SELinux 是 enforcing —— 手动分区的盘可能要给 EFI 分区改名
<!-- SEC-4 / D5（用户 2026-10-06 定：切）。提交 137e8eb：device/huawei/gaokun3/BoardConfig.mk 的 cmdline androidboot.selinux=enforcing（:120-126 是注释），
     1.0.0-dev.10 起默认，第一个对外发布的是 1.0.0-rc.1。证据：docs/TODO.md 总表 ① G9 行（dev.9 经 OneShot 真 enforcing 开机：普查只剩两类已知上游缺口
     —— system_server 读键盘 country、com.android.se 的 oat 缓存；显示 / 触摸 / Wi-Fi / 传感器 / 音频 / iris 15/15 / 前后摄 / A 档 44 /
     s2idle 11/11（含 USB 角色切换与回插）全过，out/sel-d5-*）；总表"设备现状"（dev.10 在 enforcing 下经 OTA 装上）。
     v0.7.1 及以前：BoardConfig.mk 当时是 androidboot.selinux=permissive，实机 /sys/fs/selinux/enforce=0。
     OTA-5（v1.0-plan.md:177）：BoardConfig.mk:125-126；sepolicy/file_contexts:155-160 只给 by-name 下的 esp 与 "EFI system partition" 打 ESP 标签
     （整盘安装的 PARTLABEL 是 esp，双系统复用 Windows 的 ESP）；enforcing 下 postinstall 扫全盘找 ESP 被拒（通用 block_device，domain.te:705）；
     安装器只警告不改名（scripts/live/installer-lib.sh:958-965）。HAL 的 ESP 认法另修了一处（309f735，进 rc.1）。
     ⚠️ INSTALL 中英「更新」一节"从 v0.6.1 起两个组件会退而按内容找 ESP"在 enforcing 下不再成立，待另行更正。
     ⚠️ 原条目里"任何 App 都能使用内核的性能计数器"（perf_event_paranoid=-1）在 enforcing 下是否仍成立没核实，新条目不写。
     ⚠️ 维护：哪天策略又放宽（改回 permissive）就改写这一条；组件在 enforcing 下也能认出别的名字的 ESP 时，删掉 OTA-5 那一半。 -->
* **现象**：大多数机器上看不出来。v0.7.1 及以前 SELinux 是 `permissive`（宽容）模式，规则不允许的事只记录、不拦截；
  从 1.0.0-rc.1 起是 `enforcing`，会真的拦下来。**在你自己手动分区的盘上**，如果 EFI 分区的 GPT 分区*名*既不是 `esp`
  也不是 `EFI system partition`，系统更新和切换槽位都会失败。
* **原因**：Android 的应用沙箱有两层，一层是普通的用户权限，另一层是 SELinux；SELinux 拦截之后，一个 App 即使找到了漏洞，能做的事也少得多。
  规则只允许更新组件和启动控制组件按这两个名字打开 EFI 分区：`esp` 是安装器"清除整个磁盘"建的名字，`EFI system partition`
  是 Windows 建的名字（双系统用的就是它）。以前的版本找不到时会退而扫描所有块设备，enforcing 下不允许这样做。
* **替代办法**：
  * 如果你的 EFI 分区叫别的名字，在任意一个 Linux 里改一次名（安装器的终端或者救援系统都行）：
    `sgdisk -c <N>:esp /dev/nvme0n1`，N 是 EFI 分区的分区号。UEFI 只看分区类型，改名无害。见[安装指南的"更新"一节](INSTALL.zh-CN.md#更新)。
    安装器看到这种名字时会提醒。
  * 这套规则是新的。如果某个功能在 v0.7.1 上好用、现在坏了，请报告，并附上 `adb shell dmesg | grep avc` 的输出。

### 启动链未上锁，系统分区不做完整性校验
<!-- BoardConfig.mk:128 androidboot.veritymode=disabled；实机 ro.boot.verifiedbootstate=orange、ro.boot.flash.locked=0；
     docs/INSTALL.md 第 4 节"What this does not fix"。 -->
* **现象**：开机时不会检查系统有没有被改动过。Play Integrity 永远过不了（见下面的"Google 认证"一条）。
* **原因**：这台机器用 UEFI + systemd-boot 启动，Secure Boot 必须关着，内核没有签名，也没有开 dm-verity。
  能在这台机器上装别的系统，靠的就是这条开着的启动链。
* **替代办法**：没有。这是在这台机器上运行 Android 的前提。

---

## 二、随镜像一起分发的专有组件

<!-- SEC-10 / REL-15（D20：披露 + 准备不带 Histen 的构建开关）。证据：device/huawei/gaokun3/firmware/README.md 清单表与
     "不可公开再分发"一句（:36）；docs/TODO.md:86-96（10-06 之前的行号，那段现在在 archive/TODO-history-2026-10.md，按节内标注的原行号找）（Histen，用户 2026-09-28 定"带着发"）；TODO B23（zap shader 随
     live 镜像发，用户 2026-09-27 定）；device/huawei/gaokun3/lineage_gaokun3.mk:238-250（MindTheGapps）。
     ✅ NOTICE 已覆盖二进制发布（本次，2026-10-05）：NOTICE 的 "Third-party proprietary components in the binary
     releases" 一节逐个列了文件名（以 firmware/README.md:26-31 的表为准）、hexagonrpcd-root、Histen、MindTheGapps。 -->
本项目的源码按 GPL 等开源许可发布（见 [NOTICE](../NOTICE)）。但**发布的系统镜像、OTA 包和安装器镜像**里还带着下面这些
**不属于本项目、也不是开源软件**的组件。没有它们，GPU、Wi-Fi、蓝牙、声音、传感器都无法工作：

| 组件 | 在系统里的位置 | 来源 | 说明 |
|---|---|---|---|
| 华为专有固件：GPU 安全着色器（zap shader）、ADSP / CDSP / SLPI 固件、音频拓扑、pd_mapper 服务表 | `/vendor/firmware/qcom/sc8280xp/HUAWEI/gaokun3/` | 华为的 Windows 驱动包 | 未获华为的再分发授权。安装器镜像里也带着 GPU 那一份 |
| 传感器 DSP 配置（SLPI 的 JSON 与注册表） | `/vendor/etc/hexagonrpcd-root/` | 同上（高通参考配置） | 同上 |
| Histen 音效引擎 `libhw_histen_processing.so` | `/vendor/lib64/soundfx/` | 华为 Windows 驱动 | 华为专有，未获再分发授权，而且做过二进制修改。只有打开"扬声器增强（实验性）"时才处理声音 |
| Google 应用与服务（MindTheGapps：Play 商店、Play 服务等） | `/system_ext`、`/product` | Google | 闭源，按 Google 的条款使用 |
| GPU 微码、Wi-Fi、蓝牙固件 | `/vendor/firmware/` | linux-firmware | 高通的可再分发许可，不在上面的问题之内 |

这意味着：如果权利方提出要求，下载页面有可能被下架。你自己转发或二次分发镜像时，也要知道里面带着这些东西。

---

## 三、不支持或不完整的功能

### 设置里的"清除所有数据"（恢复出厂）不起作用
<!-- ⬜ 2026-10-05：1.0 构建起镜像默认 persist.vendor.gaokun3.gk3boot=action、条目带 gk3.dispatch=1（device.mk / Gk3Boot.cpp），Settings 写的 --wipe_data 会由统一启动入口分派给 fastboot 执行端去擦（QEMU exec-wipe 过、E7 分派真机过）。但 E10 真机恢复出厂还没做（要用户同意 + 备份）⇒ 这一节先不改；E10 过了再改成"1.0 起可用"，并写明首次开机后才生效（0.7.x 升上来的机器第一次开机走直连）。 -->
<!-- B6 / A5（用户定 D4：将来由 fastboot 承接，设计进行中）。证据：docs/INSTALL.md 的 Recovery 一节；#39。
     ⚠️ 维护：fastboot 落地并验收后改写这一条（替代办法换成新路径）。 -->
* **现象**：点了之后机器会重启，但**数据全部还在**，系统也没有任何提示。
* **原因**：这个功能要靠 recovery 来执行，而 recovery 在这台机器上启动不了，重启后的清除请求没人执行。
  1.0 起这个请求改由统一启动入口的 fastboot 环境执行（见[安装指南](INSTALL.zh-CN.md)里"启动入口与 fastboot"一节），
  但这条路还没在真机上验证过 —— 请当它不起作用。
* **替代办法**：用图形安装器的 **重新安装 Android**，它默认清除数据。步骤见[常见问题](FAQ.zh-CN.md#恢复出厂--卖机前清除数据)。

### 用 USB 线传文件：不弹通知，要到设置里选；只有 USB 2.0 的速度
<!-- PWR-6 / BKUP-7 / STOR-7。有了 UsbDeviceManager（patches/0096 补上 /sys/class/android_usb，连接状态由 USB 端口 HAL 写）
     以后，MTP / PTP 走 FunctionFS（init.gaokun3.usb.rc），协议由 MtpService 在用户态实现。
     不弹通知：键盘接口的 MCU（12d1:10b8）一直挂在 USB 上、带 HID 接口，UsbDeviceManager 看到这类设备就隐藏 USB 连接通知。
     速度：靠近电源键的口（a600000.usb）在设备模式下只有 high-speed。
     同一页的 USB 网络共享（RNDIS）和 MIDI 见 init.gaokun3.usb.rc 的"USB 网络共享（RNDIS）、MIDI"一段。 -->
* **现象**：插上电脑后，平板上不会弹出"USB 用途"的通知。要传文件，到 **设置 > 已连接的设备 > USB** 里选
  "文件传输"或"PTP"，电脑上随后会出现"MateBook E Go"这个便携设备。传输速度只有 USB 2.0 的水平（约 30 MB/s）。
  同一个页面里还有"USB 网络共享"和"MIDI"。
* **原因**：键盘接口在系统里是一个一直插着的 USB 键盘，Android 看到这类设备时不显示 USB 连接的通知。
  靠近电源键的 USB-C 口在设备模式下只能跑 USB 2.0。
* **替代办法**：
  * 按上面的路径在设置里选"文件传输"。
  * 插着电脑时平板不会进入待机（这是有意的设计，避免待机时整机复位），所以息屏后传输不会中断；传完记得拔线。

### 插 U 盘没有反应
<!-- STOR-1 / BKUP-6：fstab 的 voldmanaged 行已写（8da5974，device/huawei/gaokun3/fstab.gaokun3 末尾），⬜ 未构建、未上机。
     ⚠️ 维护：上机通过后把这一条改写成"port1 可用、port0 要等 usbrole 切到 host"，并写明哪个物理孔是 port1
     （STOR-3：2026-10-05 用户确认 port0 = 靠近电源键的口，所以 port1 = 另一个）。在那之前正文不改。 -->
* **现象**：U 盘、移动硬盘插上后，文件管理器里看不到。
* **原因**：内核其实认到了设备，但系统的分区表配置里没有登记"可移动存储"，Android 不会去挂载它。
* **替代办法**：暂时没有。需要拷文件请用网络。

### 指纹不可用（开发中）
<!-- HW-5 / T6（D12：不是 1.0 门槛）。证据：docs/fingerprint-driver-design.md:3-5、104-112（M1 完成，M2 暂停）；
     实机 pm list features 没有 android.hardware.fingerprint。 -->
* **现象**：电源键上的指纹在 Android 里完全不存在，设置里没有指纹选项。
* **原因**：指纹比对在华为签名的安全固件里运行，命令协议需要从 Windows 驱动逆向。目前已经能把华为的指纹程序加载进安全环境，
  还差内核驱动和 Android 的指纹服务。
* **替代办法**：用 PIN 或密码解锁。指纹就算做出来，支付类 App 的指纹支付大概率仍然用不了。

### 手写笔（华为 M-Pencil）不支持
<!-- DISP-13。证据：refs/gaokun-buildbot/drivers/touchscreen-hx83121a/himax-spi-core.c:1074 只上报 MT_TOOL_FINGER；
     实机 getevent -pl 没有 BTN_TOOL_PEN。推迟到 1.0 之后（要逆向原始帧，XL）。 -->
* **现象**：手写笔完全没有反应，没有压感，也没有悬停。
* **原因**：触摸屏的触点是内核驱动从原始电容数据里自己算出来的，这套算法只认手指。笔的信号格式还没有人逆向过。
* **替代办法**：没有。

### 没有自动亮度
<!-- DISP-8 / HW-4 / A3（#121）。证据：实机 dumpsys display mAutoBrightnessAvailable=false。 -->
* **现象**：设置里没有"自适应亮度"，只能手动调。
* **原因**：光线传感器在总线上能应答，但一打开它，负责传感器的 DSP 就会崩溃，所以只能先关着。
* **替代办法**：手动调节亮度。

### 定位基本不可用（没有 GPS）
<!-- APP-13 / NET-5。证据：实机 pm list features 只有 android.hardware.location 与 .network，没有 .gps；dumpsys location 的
     network provider 来自 com.google.android.gms 且 enabled=false。本机是否有 GNSS 硬件未核实（无 modem）。
     国内 App 自带 Wi-Fi 定位 SDK 能否工作：未实测（批 4）。 -->
* **现象**：地图、天气、外卖、打车等 App 拿不到位置，或者一直显示"定位中"。
* **原因**：系统里没有 GPS。"网络定位"由 Google Play 服务提供，而它在国内连不上 Google。
* **替代办法**：
  * 在天气等 App 里手动选择城市。
  * 自带定位 SDK 的国内 App（例如用 Wi-Fi 定位的地图 App）也许能用，我们还没有测过。
  * 导航请用手机。

### 无法播放受 DRM 保护的视频（没有 Widevine）
<!-- AV-9 / APP-6（D11：不带，并披露）。证据：实机 service list 没有 android.hardware.drm.IDrmFactory；
     /vendor/etc/vintf/manifest/ 没有 drm；media_codecs_c2.xml:25。国内视频 App 的 VIP 内容受不受影响：未实测。 -->
* **现象**：Netflix、Disney+、Prime Video 等无法播放正片，会报 DRM 或"设备不支持"之类的错误。
  国内视频 App 的部分会员或版权内容可能也受影响（还没测过）。
* **原因**：系统里没有任何 DRM 模块。Widevine 是 Google 的闭源组件，需要授权和认证，我们没法带。
* **替代办法**：用别的设备观看这类内容。

### 蓝牙耳机在通话、语音时麦克风不可用
<!-- AV-2（1.0 只披露；HFP 软件数据通路推迟到 1.0 之后）。证据：device/huawei/gaokun3/audio/audio_policy_configuration.xml:25-31
     只 include 了 primary / r_submix / bluetooth_with_le_audio；实机 dumpsys media.audio_policy 没有任何 *BLUETOOTH_SCO* 设备；
     device.mk 的 bluetooth.profile.hfp.ag.enabled=true。"通话声音从扬声器出来"是推断，未实测。
     AV-3：A2DP 放音从没在真机上完整验过。AV-10：有线耳机麦克风也不可用（批 2 计划修，修好后删掉那半句）。 -->
* **现象**：用蓝牙耳机打微信语音、开腾讯会议、开游戏语音时，耳机上的麦克风不工作，通话声音也可能不走耳机。
  另外，有线耳机上的麦克风目前也不能用。
* **原因**：蓝牙通话需要一条专门的音频通路，这台机器没有高通给手机准备的那一套，系统里还没有搭出替代方案。
* **替代办法**：通话时用平板自带的麦克风和扬声器。用蓝牙耳机听音乐（A2DP）走的是另一条路，不受这条影响，
  不过它还没有在真机上完整测过。

### 待机时收不到消息推送
<!-- APP-5 / NET-9 / PWR-12（1.0 先测量后披露；WoW 推迟到 1.0 之后）。证据：docs/stage4-findings.md #131 §1（ath11k 在本机只走断电挂起）；
     device/huawei/gaokun3/device.mk 默认 persist.vendor.gaokun3.allow_suspend=1；1.0 起关待机走 Parts 的开关（PWR-16，
     parts/…/StandbySettingsActivity.java + init.gaokun3.rc 的镜像触发器），⬜ 开关在「电池」页的位置与文案待上机核对。
     实际延迟没量过（批 4 测完把数字补进来）。v0.7.1 发版说明：每次唤醒 Wi-Fi 约 2 秒回来。 -->
* **现象**：屏幕关掉、机器进入待机后，微信、QQ 等的新消息不会实时提醒，要等你点亮屏幕（或系统定时唤醒）才一起到。
  每次唤醒后，Wi-Fi 要过几秒才连上。
* **原因**：待机时 Wi-Fi 芯片整个断电，网络上的数据没法唤醒机器。国内 App 在这个系统上也没有厂商推送通道可用。
* **替代办法**：
  * 需要及时收消息时，让屏幕保持常亮，或者用手机收消息。
  * 也可以彻底关掉待机，代价是息屏时耗电明显增加：**设置 → 电池 → 待机（睡眠）**，关掉「允许待机」。
    关闭立刻生效，重启后仍然有效；打开从下一次息屏起恢复待机。

### 未通过 Google 认证
<!-- T2 / INST-14 / APP-4。证据：docs/INSTALL.md 第 4 节（登记流程与 Play Integrity 说明）；实机 verifiedbootstate=orange。 -->
* **现象**：Play 商店提示"设备未经 Play 保护机制认证"，有些 App 装不上。依赖 Play Integrity 的 App（例如 Google 钱包、
  部分海外银行）用不了。
* **原因**：这个 ROM 不在 Google 的认证设备名单上。Play Integrity 还要求上锁的启动链和 Google 签名的系统，这一条在本机永远满足不了。
* **替代办法**：按 [安装指南的"此设备未经 Play 保护机制认证"](INSTALL.zh-CN.md#此设备未经-play-保护机制认证)
  把设备登记一次，Play 商店就能正常用。Play Integrity 没有办法解决。

### 系统里带的是 Google 应用，国内用不上，也没有国内应用商店
<!-- APP-12（用户定 D7：只发 GApps 版、不发 vanilla）。证据：lineage_gaokun3.mk:238-250；LIVE-6（GMS 开机后常崩 4 次，用户是否看得到未确认）。
     GMS 在国内不断重试联网的耗电：未测。 -->
* **现象**：不翻墙时，Play 商店和 Google 服务都连不上，Google 服务还会在后台反复尝试联网（耗电没测过）。系统里没有国内的应用商店。
* **原因**：我们只发带 Google 应用的版本。
* **替代办法**：用浏览器从各 App 的官网，或者国内应用商店的网页版，下载 APK 安装。

### 其它不支持的硬件
<!-- 无 modem：CLAUDE.md 关键约束；无磁力计：README.zh-CN.md 传感器一行。 -->
* **没有蜂窝网络和 SIM 卡**：这台机器没有基带。
* **没有指南针**：没有磁力计。自动旋转正常，它用的是加速度计和陀螺仪。

---

## 四、其它常见问题（计划在后续版本改进）

这几条属于缺陷，不属于取舍，修好就会从这里删掉。

### 长时间运行后，音频和蓝牙可能一起卡死
<!-- A1 / AV-6（v1.0-plan.md:126）。docs/stage4-findings.md #38：用户报告，我们从未复现。
     取证看门狗：device/huawei/gaokun3/bin/gaokun3-hangdump.sh（同一 tid 连续三次采样在 D 状态，即 ≥2 分钟，
     就写到 /data/vendor/gaokun3/hangdump-<uptime>/，:63；盘上留最近 5 份，:28-34）、etc/hangdump.rc、device.mk:567。
     目录 0770 root system ⇒ 要 root 才能读；抓取命令在 FAQ.md / FAQ.zh-CN.md 的"有电脑、机器能开机时"一节。
     "没有目录也是线索"：docs/TODO.md:515-516（10-06 之前的行号，那段现在在 archive/TODO-history-2026-10.md，按节内标注的原行号找）。 -->
* **现象**：机器连续运行很久以后，声音和蓝牙都不工作了，只有重启才能恢复。这是用户报告的，我们自己还没复现过。
* **原因**：还不清楚。音频和蓝牙共用一条到 DSP 的通路，怀疑是这条通路卡住了。
* **替代办法**：重启。系统里有一个看门狗，发现卡死时会自动把证据存到 `/data/vendor/gaokun3/hangdump-*`，重启后还在。
  报告问题时请附上它（需要 root，命令见[常见问题](FAQ.zh-CN.md#有电脑机器能开机时)）。如果没有这个目录，也请说明，
  那本身就是线索。

### 插电脑时平板可能反过来给电脑充电
<!-- USB-1（2026-10-05 开发机接 Mac 实测）：port0 落成供电方，对端不支持 USB PD，内核随之定成 host ⇒ 两边都不枚举；
     usbfollow 只纠正我方受电的那种情况。 -->
* **现象**：把平板（靠近电源键的那个口）插到电脑上，USB 调试 / 电脑都看不到它，平板电量反而在**往下掉**。
* **原因**：平板的口和很多电脑的 USB-C 口都既能供电也能受电。电脑那边不支持 USB PD 时，谁供电在插上那一刻决定，
  偶尔会反过来。这时平板当了主机，电脑又不会当 USB 设备，两边就互相看不到。
* **怎么办**：拔下来重插一次（通常一次就好），或者换电脑上的另一个口。自动重新协商的修复在计划中。
* 只有**靠近电源键**的那个 USB-C 口能做 USB 调试；另一个口只能当主机。

### USB-C 口回插后可能坏掉，直到重启
<!-- A6 / PWR-4 / HW-14（v1.0-plan.md:111）。"之后整机不再待机"是按代码推断的（role_host 一直失败、持有 wakelock），
     不是实测；README.md:64 同口径。PWR-4 的止损（失败时置 vendor.gaokun3.usbrole.broken，11ace25）未上机；
     Parts 通知不在本批。 -->
* **现象**：往 USB-C 口上拔下再插回东西（多见于待机之后），这个口可能就不工作了：USB adb 连不上，插 USB 设备也认不到，
  一直到重启为止。按代码推断，在重启之前整机也不会再进入待机，息屏时电量照样往下掉。
* **原因**：USB 控制器这一侧的根因还没找到。口坏着的时候，负责切换这个口角色的服务会一直失败，并一直让机器保持唤醒。
* **替代办法**：重启。重启之前，adb 和传文件请走 Wi-Fi。

### 手掌压在屏上会碎成好几个触点
<!-- DISP-7 / T1（v1.0-plan.md:221、:359：要在驱动里做跨帧形态判断，推迟到 1.0 之后）。v0.7.1 发版说明 Known issues 同口径。 -->
* **现象**：手掌或手的侧面搭在屏幕上，会被认成好几个分开的触点，可能误点、误缩放或误滚动。
* **原因**：触点是内核驱动从原始电容数据里自己算出来的，它还分不出手掌和手指，于是一大块接触面被拆成了几个触点。
* **替代办法**：操作时别把手掌搭在屏幕上。

### 《英雄联盟手游》（Wild Rift）点开就退
<!-- #15（docs/TODO.md:150（10-06 之前的行号，那段现在在 archive/TODO-history-2026-10.md，按节内标注的原行号找）：没有日志，不再并入 #12）；README.md:57。 -->
* **现象**：游戏一打开就退出。
* **原因**：还不知道。这是 [#15](https://github.com/vahiru/gaokun-android/issues/15) 里报告的，我们还没拿到日志。
* **替代办法**：如果你也遇到，请在它退出后马上抓崩溃日志（见[常见问题](FAQ.zh-CN.md#有电脑机器能开机时)），附到那个 issue 上。

### 用低功率电源时显示"正在充电"，电量却在掉
<!-- BATT-1（BATT-2 已并入，v1.0-plan.md:109、:386）。README.md:56 同口径（09-14 实例）。 -->
* **现象**：接在电脑的 USB 口、小功率手机充电器这类弱电源上时，电池显示"正在充电"，百分比却一直往下掉。
  这时 Android 不会做低电量自动关机，到 0% 机器会直接断电，没保存的东西会丢。
* **原因**：电池状态来自华为的嵌入式控制器（EC）。要么是驱动把其中一些状态值解错了，要么是控制器自己就报了"正在充电"，
  是哪一种还没查清。
* **替代办法**：用真正充得进电的充电器（例如原装充电器）。接弱电源时看百分比而不是看充电图标，电量低之前保存好东西。

### 打开 Wi-Fi 热点时，平板自己会断开 Wi-Fi
<!-- NET-2（批 2）。v0.7.1 发版说明里"芯片只能二选一"的说法不准确：iw 显示驱动支持 STA+AP，是软件配置没开。
     发版说明已加勘误（docs/relnotes/v0.7.1-alpha.md 的 Wi-Fi 一节）。
     2026-10-05 源码核实：ath11k 给 WCN6855 hw2.1 报的接口组合里 STA 与 AP 同组（ath11k mac.c:10327-10356、
     core.c:525-529/:572 @7.2.9）。修法已写（BoardConfig 的 WIFI_HAL_INTERFACE_COMBINATIONS + gaokun3-wlan-ap.sh 预建 wlan1），
     未编译、未上机；哪一版过了"热点与 Wi-Fi 同时开"的验收就删掉这一条。 -->
* **现象**：打开热点后，平板自己的 Wi-Fi 连接会断开。机器没有基带，所以热点没有网络可分享。
* **原因**：目前的软件配置不支持同时开 Wi-Fi 和热点，不是芯片的限制。
* **替代办法**：暂时没有。请让别的设备直接连路由器。

### 中文输入法要自己打开一次
<!-- DISP-3（D10，用户 2026-10-06 定从 GitHub 取 fcitx5-android 0.1.3，prebuilt-apps/fcitx5/README.md；提交 f2fd400 / 7eb356c）。
     从 1.0.0-rc.1 起预装 —— 它是第一个真正带上它的构建：1.0.0-dev.10 的镜像里，构建系统给 PRESIGNED 的 APK 解压 .so、重对齐，弄坏了 v2 签名，
     PackageManager 拒装（docs/TODO.md 总表"设备现状"②）；5c48d08 改成原样安装（LOCAL_REPLACE_PREBUILT_APK_INSTALLED），dev.11 / rc.1 起带。
     开发机上的 0.1.3 是作为普通应用装的，不能当镜像里带着它的证据。
     ⬜ 还没上机核对：rc.1 的系统应用列表里有它、下面的菜单名、实体键盘怎么切中英文（与 Ctrl+Space 冲不冲突）；核对后改写这一条。 -->
* **现象**：默认的键盘不支持中文。
* **原因**：从 1.0.0-rc.1 起预装了中文输入法（fcitx5，自带拼音 / 双拼 / 五笔），但没有设成默认。v0.7.1 及以前没有。
* **替代办法**：设置 → 系统 → 键盘 → 屏幕键盘 → 打开 *Fcitx5*，再切换过去（菜单名还没在本机核对）。以后从项目的 GitHub release 更新
  （F-Droid 版签名不同，装不上覆盖）。实体键盘怎么切中英文还没核对过。
