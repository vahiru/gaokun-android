# `gk3boot`：统一启动入口与Fastboot执行端

`tools/gk3boot/`实现了gaokun3在UEFI固件（无Android ABL）下的统一启动入口`gk3boot.efi`、主机与设备侧`misc`分区工具`gk3-misc`、用户态Fastboot与恢复出厂执行端`gk3-fastbootd`及配套initramfs（`fastboot.img`）。

相关系统与模块文档：

- [../../docs/dev/android-bringup.md](../../docs/dev/android-bringup.md)（A/B分区、OTA与SELinux整体架构）
- [../../device/huawei/gaokun3/prebuilt-gk3boot/README.md](../../device/huawei/gaokun3/prebuilt-gk3boot/README.md)（ROM侧预编译产物放入方式）
- [../../scripts/live/README.md](../../scripts/live/README.md)（Live安装器初始化`misc`与ESP布局）

## 1. 架构与目录结构

华为MateBook E Go的UEFI固件仅加载ESP上的PE/COFF应用（`systemd-boot`），不读取Android的`misc`或`boot_a`/`boot_b`裸分区。`gk3boot.efi`作为`systemd-boot`的默认加载项运行在UEFI阶段，直接读取GPT与`misc`分区完成A/B选槽、`tries_remaining`递减、BCB（Bootloader Control Block）分派、双系统一次性跳转，并通过Linux EFI Stub直接从`boot_a`/`boot_b`分区加载内核或拉起`fastboot.img`执行端。

```text
tools/gk3boot/
├── core/                  # libgk3core：纯C11、零堆分配、宿主/UEFI/Android共用的盘上格式与决策库
│   ├── include/gk3/       # 公共头文件（types/endian/crc32/sha1/gpt/bcb/bcab/vab/gk3rec/bootimg/cmdline/select/dispatch/dual）
│   ├── src/               # 实现文件
│   └── Android.bp         # sync-device-tree.sh同步至device/huawei/gaokun3/gk3core/供boot_control HAL静态链接
├── misc/
│   └── gk3-misc.c         # 命令行工具：dump / select / set-active / verify-boot / init / mark-poweroff / set-next / set-default
├── efi/
│   ├── include/           # 最小UEFI ABI声明（零外部头文件依赖）
│   ├── lib/               # UEFI控制台、日志、BlockIo选盘、文件读写、变量读写、Linux EFI Stub交接（H2）
│   ├── probe/             # gk3probe.efi：只读硬件探针（枚举BlockIo、GPT、misc、boot_x、Watchdog与ResetSystem）
│   └── boot/              # gk3boot.efi：统一启动入口（observe观察模式 / action动作模式）
├── fastbootd/             # gk3-fastbootd：Fastboot协议守护进程与--wipe-data / --clear-bcb子命令
├── initramfs/             # fastboot.img构建脚本、PID 1 (/init) 与按键/状态/字体辅助工具gk3-fbi
├── test/                  # 单元测试（libgk3core）、AOSP对照测试、HAL宿主测试、postinstall测试、fastbootd测试与实机向量
└── qemu/                  # AAVMF + systemd-boot + NVMe整盘QEMU端到端测试夹具
```

### 代码约束（`libgk3core`）

- 纯C11（`-std=c11`），仅依赖`<stdint.h>`、`<stddef.h>`、`<stdbool.h>`，可在宿主（glibc/musl/macOS）、UEFI（`-ffreestanding -mno-red-zone`）与Android vendor（`cc_library_static`）间直接共用。
- 无动态内存分配（不调用`malloc`/`free`），所有缓冲区由调用方在栈或静态区提供。
- 所有盘上结构采用显式小端字节序读写（`gk3/endian.h`），并附带`_Static_assert`尺寸断言。
- 对外纯函数输入输出均为`const uint8_t *` / `uint8_t *`，不直接发起I/O。

## 2. 盘上格式与选槽规则

### 2.1 `misc`分区64 KiB布局

| 字节范围 | 大小 | 结构 | 读写方 |
|---|---|---|---|
| `0..2047` | 2 KiB | `bootloader_message`（BCB） | Android `init`/`recovery`、`gk3boot.efi`（动作模式分派）、`gk3-fastbootd` |
| `2048..4095` | 2 KiB | `bootloader_control`（BCAB，前32字节有效） | Android `boot_control` HAL、`gk3boot.efi`（动作模式扣减`tries_remaining`）、`gk3-fastbootd`（`set_active`） |
| `4096..8191` | 4 KiB | 保留 | 不触碰 |
| `8192..10239` | 2 KiB | `gk3_record` v1（GK3私有记录） | `gk3boot.efi`、`boot_control` HAL、`gk3-fastbootd`、`gk3-misc` |
| `10240..32767` | 22 KiB | 保留 | 不触碰 |
| `32768..65535` | 32 KiB | `MiscVirtualAbMessage`（`@32768`，64字节）等系统区 | Android `libsnapshot`、`gk3boot.efi`（只读）、`gk3-fastbootd`（只读，`snapshot-update cancel`收尾时置`NONE`） |

### 2.2 `bootloader_control`（BCAB，`@2048`，32字节）

与AOSP `hardware/interfaces/boot/1.1/default/boot_control/include/private/boot_control_definition.h`逐字节一致：

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| `0..3` | 4 | `slot_suffix[4]` | 固定`"a\0\0\0"`或`"_a\0\0"`（实现只读槽数组，写回时保留原字段；初始化写`"_a\0\0"`） |
| `4..7` | 4 | `magic` | 小端`0x42414342`（`"BCAB"`） |
| `8` | 1 | `version` | `1` |
| `9` | 1 | `nb_slot` | `2` |
| `10..11` | 2 | `recovery_tries_remaining` | 保留 |
| `12..15` | 4 | `slot_info[0]`（`_a`） | `priority`（0..15）、`tries_remaining`（0..7）、`successful_boot`（0/1）、`verity_corrupted`、`reserved` |
| `16..19` | 4 | `slot_info[1]`（`_b`） | 同上 |
| `20..27` | 8 | `reserved` | 保留 |
| `28..31` | 4 | `crc32_le` | 前28字节的IEEE 802.3 CRC32（`zlib::crc32(0, buf, 28)`） |

槽可启动条件：`priority > 0 && (successful_boot != 0 || tries_remaining > 0)`。

### 2.3 `gk3_record` v1（`@8192`，2048字节）

| 字节范围 | 大小 | 字段 | 说明 |
|---|---|---|---|
| `0..7` | 8 | `magic[8]` | `"GK3BOOT\0"` |
| `8..9` | 2 | `version` | 小端`1` |
| `10..11` | 2 | `size` | 小端`2048` |
| `12..15` | 4 | `flags` | bit0=`migrated`（已完成BCB首跑迁移），bit1=`fallback_pending`，bit2=`clean_poweroff` |
| `16..19` | 4 | `boot_streak` | 连续未收到Android开机完成确认的启动次数（入口动作模式`+1`，HAL开机完成清零） |
| `20` | 1 | `last_slot` | 上次入口选择的槽（`0`=无，`1`=`_a`，`2`=`_b`） |
| `21` | 1 | `last_why` | 上次决策原因（`gk3_why_t`） |
| `22` | 1 | `next_kind` | 一次性意图（`0`=无，`1`=`sdboot-menu`，`2`=`slot`，`3`=`windows`） |
| `23` | 1 | `next_arg` | `next_kind=slot`时的目标槽（`1`=`_a`，`2`=`_b`） |
| `24..25` | 2 | `dispatch_ver` | 完成首跑迁移时的分派规则版本（`GK3_DISPATCH_VER = 1`） |
| `26` | 1 | `bcb_wipe_tries` | 同一份`wipe` BCB已分派给执行端的次数（上限3次） |
| `27` | 1 | `ok_streak` | 已标成功槽连续未开机完成的次数（达`GK3_BOOTLOOP_THRESHOLD = 5`进执行端菜单） |
| `28..47` | 20 | `bcb_wipe_sha1[20]` | 当前计数对应的BCB前2 KiB SHA-1摘要 |
| `48..79` | 32 | `entry_ver[32]` | 最近一次运行的`gk3boot`版本字符串（NUL结尾） |
| `80..335` | 256 | `note[256]` | 最近一次异常或回退摘要（ASCII，NUL结尾） |
| `336..351` | 16 | `event_seq` / `event_notified_seq` | 事件环形缓冲写序号与HAL已通知序号（各`uint64_t`小端） |
| `352..355` | 4 | `bcb_seen_crc` | 分派关闭时最近一次已记录`bcb_ignored`事件的BCB CRC32 |
| `356..359` | 4 | `bcb_disp_crc` | 分派开启时最近一次分派的BCB CRC32 |
| `360` | 1 | `set_default` | 待由入口应用的默认系统请求（`0`=无，`1`=Android，`2`=Windows） |
| `361` | 1 | `default_os` | 入口最近一次观测到的默认系统（`0`=未知，`1`=Android，`2`=Windows） |
| `362..511` | 150 | `reserved` | 置零 |
| `512..2043` | 1532 | `events[16]` | 16条×92字节事件环 + 60字节置零保留 |
| `2044..2047` | 4 | `crc32_le` | 前2044字节的CRC32 |

### 2.4 选槽与BCB分派规则

1. **强制槽（`gk3.slot=a|b`）**：仅用于调试，直接启动指定槽，不修改`misc`（等效于观察模式）。
2. **正常选槽（`gk3_select`）**：
   - 若BCAB CRC或头字段无效：使用`hint`槽（来自加载条目`gk3.hint=`或条目名，无则`_a`），不写BCAB，标记`event=misc_invalid`。
   - 若BCAB有效：选`priority`最高的可启动槽（平手取`_a`）。
   - 若最高优先级槽不可启动，但另一槽可启动：
     - 当VAB `merge_status == MERGING (3)`时，拒绝回退到旧槽，报`GK3_SEL_MERGING`（转执行端或fail-open）。
     - 否则回退到另一槽，记`event=fallback`。
   - 若两槽均不可启动：报`GK3_SEL_NOSLOT`。
3. **扣减`tries_remaining`**：选中的槽若`successful_boot == 0 && tries_remaining > 0`，在动作模式下将`tries_remaining`减1并重算CRC32写回BCAB（写后读回校验）。已标成功的槽BCAB逐字节不动。
4. **BCB分类与分派（`gk3_bcb_classify` / `gk3_dispatch_decide`）**：
   - `command`为空：正常启动Android。
   - `bootonce-bootloader` / `boot-fastboot`：分派至执行端（`why=bootloader` / `why=fastboot`），进入前清空BCB。
   - `boot-recovery`：解析`recovery`字段行（首行跳过程序名）：
     - 含`--fastboot` → `why=fastboot`（进入前清空BCB）
     - 含`--wipe_data` → `why=wipe`（保留BCB交由执行端擦除完成后清理；同一SHA-1连续分派满3次仍未清除则第4次由入口清空并记`wipe_failed`）
     - 含`--prompt_and_wipe_data` → `why=prompt_wipe`（保留BCB交由执行端RescueParty界面处理）
     - 其他参数 → `why=recovery`（进入前清空BCB）
   - `boot-quiescent`或未知非空命令：清空BCB并记`bcb_dropped`，正常启动Android。
   - **首跑迁移（`gk3_rec_migrate`）**：当`gk3.dispatch=1`首次启用（GK3记录无`migrated`位）时，若`misc`中存在历史遗留BCB，只清空不执行，置`migrated`位并记`bcb_dropped`，防止历史残留的`--wipe_data`在升级后意外触发擦除。

## 3. `gk3boot.efi`引导模式与交接机制

### 3.1 运行模式与选项（`options`）

`gk3boot.efi`从`LoadedImage->LoadOptions`解析以空格分隔的键值（未识别的键忽略）：

| 选项 | 取值 | 说明 |
|---|---|---|
| `gk3.observe` | `0` / `1` | `1`=观察模式（只读`misc`，仅写ESP日志，不改BCAB/BCB/GK3记录）；`0`=动作模式（缺省） |
| `gk3.dispatch` | `0` / `1` | 是否启用BCB分派与异常进执行端（编译期缺省`0`） |
| `gk3.hint` | `a` / `b` | 当前条目的槽提示（用于判定是否发生了回退，以及BCAB损坏时的回退目标） |
| `gk3.slot` | `a` / `b` | 强制启动指定槽（强制观察模式） |
| `gk3.action` | `fastboot` / `menu` | 直接进入执行端（由`gk3boot-tools.conf`使用，不受`gk3.dispatch`开关限制） |
| `gk3.hold` | `0..30` | fail-open复位前屏幕停留秒数（缺省`1`） |
| `gk3.log` | `0` / `1` | 是否强制每回启动写ESP日志（缺省`0`，正常无事件启动实现ESP零写入） |
| `gk3.fbtcp` | `0` / `1` | 透传给执行端以开启TCP 5554调试端口 |

### 3.2 内核交接（H2：Linux EFI Stub + `LoadFile2` + `EfiDtbTableGuid`）

1. **读取并校验`boot_x`**：从唯一定位的NVMe目标盘读取`boot_a`或`boot_b`分区，校验Android Boot Image Header v2，提取kernel、ramdisk、dtb切片，并计算SHA-1（3段各非空时哈希内容，为0时哈希4字节小端0；后面补5个32位0达8段）与头部的`id[0..19]`比对。
2. **安装DTB与initrd**：
   - 将dtb拷贝至`EfiACPIReclaimMemory`并通过`InstallConfigurationTable(&EfiDtbTableGuid, ...)`注册。
   - 在新句柄上安装`EFI_DEVICE_PATH_PROTOCOL`（`LINUX_EFI_INITRD_MEDIA_GUID`）与`EFI_LOAD_FILE2_PROTOCOL`，由内核EFI Stub回调拉取ramdisk（Android启动时为`boot_x`的ramdisk；进入执行端时为`\EFI\gk3boot\<ver>\fastboot.img`）。
3. **拼接cmdline**：
   - Android路径：以`boot_x`内置cmdline为底，剔除冲突键后追加`androidboot.slot_suffix=_<a|b> androidboot.bootloader=gk3boot-<ver> androidboot.gk3boot.mode=<observe|action> androidboot.gk3boot.event=<...> androidboot.gk3boot.entry=<...> androidboot.gk3boot.streak=<N> [androidboot.gk3boot.dispatch=1] panic=10`。
   - 执行端路径：剔除`androidboot.*`与`init=`，追加`panic=10 gk3.mode=fastboot gk3.why=<...> gk3.slot=<a|b> gk3.bootver=<ver> gk3.disk=<misc PARTUUID> gk3.esp=<ESP PARTUUID> [gk3.dispatch=1] [gk3.fbtcp=1]`。
4. **启动内核**：对kernel切片调用`LoadImage( FALSE, ImageHandle, NULL, kernel_buf, kernel_sz, &kimg )`，设置`LoadedImage->LoadOptions`为UTF-16LE cmdline，在交接前调用`SetWatchdogTimer(0, ...)`撤除UEFI看门狗，然后调用`StartImage(kimg, ...)`。

### 3.3 Fail-Open故障退回机制

若`gk3boot.efi`在选盘、读取`boot_x`、SHA-1校验或`LoadImage`/`StartImage`任一环节失败，或者遇到`MERGING`/`NOSLOT`且无可用执行端：

1. 将诊断日志写入ESP `\EFI\gk3boot\log\boot-0.txt`（轮转保留`boot-0.txt`..`boot-3.txt`）。
2. 在ESP `\loader\entries\`下查找目标槽的直连条目`<32位hex>-android-<a|b>.conf`（若不存在则尝试另一槽）。
3. 向UEFI非易失变量写入`LoaderEntryOneShot`（GUID `4a67b082-0a4c-41cf-b6c7-440b29bb8c4f`，属性`NV|BS|RT = 0x7`）并读回核对。
4. 调用`RT->ResetSystem(EfiResetCold, ...)`冷复位，由`systemd-boot`消费`LoaderEntryOneShot`直接引导ESP上的直连内核副本。
5. 此外，部署的默认入口条目名带有`systemd-boot`启动计数后缀（`gk3boot-android-{a,b}+3.conf`，`sort-key 0gk3`）。若连续3次启动未进到Android完成祝福（重命名为不带`+N`的`.conf`），`systemd-boot`在第4次启动时会自动跳过`+0-3`条目，落到排序次之的直连条目`<mid>-android-{a,b}.conf`（`sort-key 1android`）。

## 4. Android侧集成

### 4.1 组件与职责

| 组件 | 路径 | 职责 |
|---|---|---|
| `boot_control` HAL扩展 | [../../device/huawei/gaokun3/boot_control/Gk3Boot.cpp](../../device/huawei/gaokun3/boot_control/Gk3Boot.cpp) | `sys.boot_completed=1`后清零GK3记录`boot_streak`、提取未通知事件、祝福（bless）本次启动条目、按属性对齐ESP文件、处理双系统与菜单请求、关机时写`clean_poweroff`标记 |
| OTA postinstall部署 | [../../device/huawei/gaokun3/bin/gaokun3-ota-postinstall.sh](../../device/huawei/gaokun3/bin/gaokun3-ota-postinstall.sh) | OTA写入新槽后同步直连条目与内核副本，并按开关部署或暂存（`.staged`）新槽`/vendor/boot/gk3boot/{gk3boot.efi,fastboot.img}` |
| 系统设置与通知 | `GaokunParts`（`BootEntryNotifier` / `BootOptionsActivity`） | 开机异常通知（回退旧槽、丢弃遗留BCB、跳过入口等）与「启动选项」页面（重启到Windows、默认系统切换、重启到引导菜单） |

### 4.2 开关属性`persist.vendor.gaokun3.gk3boot`

| 属性值 | 开机完成时（HAL） | OTA时（postinstall） |
|---|---|---|
| `off`或未设置 | 删除ESP上全部`gk3boot-android-*`、`gk3prev-android-*`、`gk3boot-tools.conf`及无引用的`EFI/gk3boot/<ver>/`目录 | 删除`gk3boot`相关条目，保留直连条目 |
| `observe` | 部署本槽`/vendor/boot/gk3boot/`至`EFI/gk3boot/<ver>/`；将已祝福的旧版条目保存为`gk3prev-android-{a,b}.conf`，写入新版`gk3boot-android-{a,b}+3.conf`（`gk3.observe=1`） | 若ESP尚无入口则直接部署`+3`条目；若已有其他版本则写入`EFI/gk3boot/<ver>/`与`.staged`条目，待新槽首次开机完成时由HAL激活 |
| `action` | 同`observe`，条目参数为`gk3.observe=0 gk3.dispatch=1`；若`fastboot.img`在ESP上就绪，同时维护`gk3boot-tools.conf` | 同`observe`（`gk3.observe=0 gk3.dispatch=1`） |

HAL在正常开机时先以**只读**方式挂载ESP比对文件状态；若条目已祝福且二进制一致，则直接卸载，保持日常开机ESP零写入。

### 4.3 状态与请求属性（`vendor.gaokun3.bootentry.*`）

- 只读导出属性：`via`（`gk3boot`/`gk3prev`/`direct`）、`mode`（`off`/`observe`/`action`）、`version`、`event`、`notify`、`bypassed`、`streak`、`windows`（`0`/`1`）、`default`（`android`/`windows`）、`default_pending`、`menu`（`0`/`1`）、`error`、`done`。
- 写入请求链路：`GaokunParts`设置`sys.gaokun3.bootreq=<next_windows|default_windows|default_android>` → `init`触发器转发至`vendor.gaokun3.bootentry.request` → HAL请求线程更新`misc`中的GK3记录并回写`vendor.gaokun3.bootentry.ack`。

## 5. Fastboot与恢复执行端（`gk3-fastbootd`与`fastboot.img`）

### 5.1 分工与生命周期

`fastboot.img`由`gk3boot.efi`搭配当前槽`boot_x`中的Android内核拉起。系统内职责划分为两层：

- **`/init`（PID 1，POSIX shell + `gk3-fbi`静态辅助程序）**：挂载`proc`/`sys`/`devtmpfs`/`configfs`/`functionfs`，持有`wake_lock`防止休眠，配置USB角色（`/sys/class/usb_role/a600000.usb-role-switch/role`）与ConfigFS Gadget（`0x18d1:0x4ee0`），加载Terminus 32×16字体并在`tty0`渲染文本菜单，监听按键（音量上/下、电源键、键盘盖方向键/回车/Esc），管理`gk3-fastbootd`进程生命周期，并在无主机连接且无按键操作满30分钟（`GK3_IDLE_TIMEOUT=1800`）时自动关机。
- **`gk3-fastbootd`（C11静态二进制）**：通过`--usb-nosetup`向`$GK3_FFS/ep0`写入FunctionFS描述符（`/init`检测到`ep1`生成后绑定UDC `a600000.usb`），处理Fastboot协议命令、分区擦写、LP元数据解析与ESP同步；同时提供`--wipe-data [--confirm]`与`--clear-bcb`子命令供`/init`调用。

### 5.2 `/init`与`gk3-fastbootd`退出码约定

常驻实例`gk3-fastbootd --usb-nosetup`不直接调用`reboot(2)`，而是通过退出码将生命周期操作交还给`/init`：

| 退出码 | 触发条件 | `/init`动作 |
|---|---|---|
| `0` | `fastboot reboot`；或`gk3.dispatch=1`时的`reboot-bootloader`/`reboot-fastboot`/`reboot-recovery`（守护进程已先写入对应BCB） | 解绑UDC → `sync` → `reboot -f` |
| `10` | `fastboot shutdown` / `powerdown` | 解绑UDC → `sync` → `poweroff -f` |
| `11` | `gk3.dispatch=0`时的`reboot-bootloader` / `reboot-fastboot` | 原地重启`gk3-fastbootd`并重新绑定UDC（软重新枚举），不写BCB |
| `12` | `gk3.dispatch=0`时的`reboot-recovery` | 停止协议并切回主菜单 |
| `1` / 其他 | 传输初始化失败或异常退出 | 自动拉起，60秒内连续失败3次则停止并在界面显示`STOPPED` |

子命令`gk3-fastbootd --wipe-data`退出码：`0`=已擦除并清空BCB；`3`=需用户在界面二次确认（随后由确认页调用`--wipe-data --confirm`）；`4`=被VAB守卫拒绝；`5`=I/O或盘唯一性校验失败。

### 5.3 支持的Fastboot命令与安全守卫

- **查询变量（`getvar`）**：支持标准变量（`version`、`version-bootloader`、`product`、`serialno`、`secure`、`unlocked`、`is-userspace=yes`、`max-download-size`、`slot-count`、`current-slot`、`has-slot:*`、`slot-successful:*`、`slot-unbootable:*`、`slot-retry-count:*`、`partition-size:*`、`partition-type:*`、`is-logical:*`、`super-partition-name`、`snapshot-update-status`、`battery-voltage`、`battery-soc`、`battery-soc-ok`）及诊断变量（`gk3-fastbootd-version`、`gk3-why`、`gk3-disk`、`gk3-disk-ok`、`gk3-disk-error`、`gk3-entry`、`gk3-esp-default`）。
- **刷写白名单（`flash:<part>`）**：仅允许写入`boot_a`、`boot_b`（或`boot`自动补当前槽）、`super`、`userdata`、`metadata`，支持raw与Android sparse格式（先整包校验chunk边界与总块数再写入分区，写后`fsync`、`POSIX_FADV_DONTNEED`并逐字节/逐chunk读回比对）。任何不在白名单内的分区（如UEFI固件分区`xbl`、`tz`、`hyp`、`modem`或逻辑分区名）一律拒绝。
- **`flash:boot_x`自动同步ESP**：写入`boot_a`/`boot_b`前校验Boot Image Header v2及SHA-1；写入分区并读回校验后，在同一条命令内挂载ESP并原子更新`<mid>/android/slot_<x>/{Image,ramdisk.img,gaokun3.dtb,cmdline.txt}`及直连条目`options`，保证直连回退路径与`boot_x`分区同步。
- **`flash:super`与`set_active`守卫**：
  - 刷写`super`首片（偏移`< 1 MiB`）后解析LpMetadata，若当前active槽无逻辑分区而另一槽有，则自动按`set_active`规则切换active槽。
  - `set_active:<a|b>`要求：①VAB状态非`MERGING`；②目标槽`boot_x`为SHA-1校验通过的有效`boot.img`；③`super`的LpMetadata中目标槽至少含一个后缀为`_<x>`的逻辑分区；④目标槽当前可启动或本会话刚刷写过`boot_x`；⑤ESP上`slot_<x>`内核三件套齐全。
- **恢复出厂与擦除（`erase:userdata|metadata` / `--wipe-data`）**：
  - VAB守卫：当`merge_status == MERGING`或处于待验证的新槽`SNAPSHOTTED`状态时拒绝擦除，清空BCB中的wipe请求并在GK3记录中写入`refused_merging`事件。
  - 免确认条件：`gk3.why=wipe`、GK3记录已有`migrated`标记、BCB确为`wipe`且其SHA-1与`gk3boot.efi`分派时记录的`bcb_wipe_sha1`一致。满足时自动擦除并重启；从菜单手动进入、未迁移或摘要不匹配时均返回退出码`3`，由`/init`弹出二次确认界面（默认高亮`Cancel`）。
  - 擦除顺序：对`userdata`执行`BLKDISCARD`并将首尾各1 MiB写零读回，对`metadata`整块写零读回；两分区均确认完成后最后清空BCB，保证中途断电下次开机可重入完成擦除。

## 6. 双系统引导（Android + Windows）

在保留Windows的双系统布局下，Windows Boot Manager（`EFI/Microsoft/Boot/bootmgfw.efi`）与Android共用同一ESP。`gk3boot`通过UEFI变量与GK3记录协同维护默认启动系统与跨系统重启：

1. **`LoaderEntryDefault`合法值约束**：仅允许不设置（默认走`loader.conf`的`default *-android-<x>.conf`进入`gk3boot`）或指向Windows条目（`auto-windows` / `gk3-windows.conf`）。若`LoaderEntryDefault`被意外设为某条Android条目ID，`gk3boot.efi`在动作模式下会将其删除并记录`default_reset`，防止绕过选槽或固定在单一槽位。
2. **默认系统为Windows时的预置OneShot机制**：
   - 当用户将默认系统设为Windows时，`LoaderEntryDefault`设为Windows条目ID。
   - 每次进入Android时，`gk3boot.efi`在交接内核前预先将`LoaderEntryOneShot`写为`*-android-<当前槽>.conf`。因此，在Android运行期间发起普通重启（`adb reboot`、系统更新重启、内核panic重启）都会消费该OneShot回到`gk3boot.efi`并继续进入Android，不会在更新或重启中途误进Windows。
   - 当用户在Android中执行**正常关机**时，`init`在`on shutdown`阶段调用`/vendor/bin/hw/android.hardware.boot-service.gaokun3 --gk3-mark-poweroff`，在GK3记录中置位`clean_poweroff`。下次按电源键冷开机时，`gk3boot.efi`经预置的OneShot被拉起，检测到`clean_poweroff`且默认系统为Windows、同时无Android待办事项（无BCB分派、非正在扣减`tries_remaining`的新槽、上次已正常开机完成），则清除`clean_poweroff`、写入`LoaderEntryOneShot=<Windows条目>`并冷复位进入Windows；再下一次冷开机则直接由`LoaderEntryDefault`进入Windows。
3. **从Android一键重启到Windows**：在「设置 → 系统 → 启动选项」中点击「重启到Windows」，HAL将GK3记录的`next_kind`设为`GK3_NEXT_WINDOWS (3)`并触发重启；`gk3boot.efi`检查无Android待办事项后清零`next_kind`，写`LoaderEntryOneShot=<Windows条目>`并冷复位进入Windows（若正处于OTA新槽验证或有待执行BCB，则自动作废该一次性意图并记`intent_dropped`，优先完成Android启动）。
4. **从Windows重启到Android / 切换默认系统**：由Windows侧的工具修改固件NVRAM中的`LoaderEntryOneShot`或`LoaderEntryDefault`。原先的Windows脚本已删除，这项功能由Windows安装器（[`installer/windows/`](../../installer/windows/README.md)）的管理页接替，尚未实现。
5. **安装器初始化**：安装器（LiveCD的`gk3l`、Windows的`gk3w`）在写入分区后用libgk3core的`gk3_misc_build_install`（与`gk3-misc init <misc> --slot a --default <windows|android|none>`相同）一步初始化空BCB、槽`_a`（`15/6/未成功`）及带`migrated`标记与`set_default`请求的GK3记录。

## 7. 构建与测试

### 7.1 宿主单元测试与脚本测试（无需Docker）

```bash
# 编译libgk3core与gk3-misc并运行全部C单元测试（466条断言）
make -C tools/gk3boot check

# 运行gk3-misc init与子命令独立校验测试
make -C tools/gk3boot misc-test

# 运行fastbootd协议辅助单元（SHA-256 / sparse / LP元数据，32条断言）
make -C tools/gk3boot fbd-unit

# 运行boot_control HAL (Gk3Boot.cpp) 宿主场景测试（94条场景，含ASan+UBSan）
make -C tools/gk3boot hal-test

# 运行gaokun3-ota-postinstall.sh多shell（mksh/dash/ksh）与HAL交叉测试（163条断言）
make -C tools/gk3boot postinstall-test
```

### 7.2 UEFI、容器与QEMU端到端测试

```bash
# 编译gk3probe.efi与gk3boot.efi并在AAVMF + systemd-boot + NVMe下跑只读探针测试
bash scripts/gk3boot/test-probe.sh

# 运行gk3boot.efi观察模式与动作模式QEMU端到端测试
bash scripts/gk3boot/test-boot.sh

# 在arm64容器内运行gk3-fastbootd协议、白名单、sparse、LP与VAB守卫测试（静态版 + ASan/UBSan版各252条）
bash scripts/gk3boot/test-fastbootd.sh

# 打包fastboot.img并在QEMU下运行11个initramfs界面、USB枚举与按键场景
bash scripts/gk3boot/test-initramfs.sh

# 运行gk3boot.efi + fastboot.img + gk3-fastbootd + 双系统全套25个QEMU端到端场景
bash scripts/gk3boot/test-executor.sh all
```

### 7.3 生成预编译产物供ROM打包

验证全套测试通过且工作树干净（版本号不含`.dirty`）后，将构建产物同步至`device/huawei/gaokun3/prebuilt-gk3boot/`：

```bash
cp tools/gk3boot/build/efi/gk3boot.efi device/huawei/gaokun3/prebuilt-gk3boot/gk3boot.efi
cp tools/gk3boot/build/fastboot/fastboot.img device/huawei/gaokun3/prebuilt-gk3boot/fastboot.img
strings tools/gk3boot/build/efi/gk3boot.efi | sed -n 's/^gk3boot-//p' | head -n1 > device/huawei/gaokun3/prebuilt-gk3boot/version
```
