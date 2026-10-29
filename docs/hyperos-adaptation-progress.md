# HyperOS 适配执行记录

2026-10-30 更新：当前 R18 已部署（R17 内核），R19 只做了离线修复与只读诊断、未上机；最新状态见文末 R15–R19 各节（R14 节及以前是历史）。10-30 数据分区重排见 [hyperos-repartition-20261030-review.md](hyperos-repartition-20261030-review.md)。早期记录保留用于复现历史基线。综合报告位于
`out/hyperos-r2/hyperos-port-comprehensive-report.md`；本文只存工程方法和实测结论，
不含小米文件。新增工具用法见 [tools/hos/README.md](../tools/hos/README.md)。

## 本轮实际完成

1. 固化 `check-sepolicy.py`，显式输入 donor 根目录、原始 vendor 策略目录、编译器和输出目录，
   分别运行 init 等效编译与 neverallow 检查，记录工具/输入/产物 SHA-256、参数和日志。
2. 固化 `prepare-ramdisk.py`，直接读写 newc，无需 root，无需在 APFS 上提取固件与属主元数据。
   用本地 A17 `1792983426` payload 的 boot 生成 ext4 与 EROFS 两份实验 ramdisk。
3. 将三项物理挂载统一改为 `metadata_hos`、`userdata_hos`、`misc_hos`；生成供实验 vendor 同步
   使用的 fstab。增加调试文件与观察标记，保持原 init 不变。
4. 四项归档/隔离测试通过。工具成功处理真实 ramdisk 的 36 个原始条目，其中 35 项保持原样，
   改写 1 项并新增 2 项。无 ADB 连接，未改设备分区、ESP、启动配置或运行属性。

## R3 历史基线：运行时可编译，neverallow 未通过

本轮使用原始 vendor 策略 `probe-files/gaokun3-ref/vendor_selinux`，没有使用
`sepol-test/vperm` 全域 permissive 变体。编译器为留存的上游 secilc macOS arm64 产物。
输入逐文件哈希、完整 argv 在 `out/hyperos-r3/sepolicy-{user,userdebug}/report.json`。

| 平台策略 | init 等效（`-N -c 30`） | 开启 neverallow | 产物字节数 |
|---|---|---|---|
| donor user plat | rc=0 | rc=255，4 项检查失败 | 1,147,297 |
| donor userdebug plat + 原 system_ext/product | rc=0 | rc=255，91 项检查失败 | 1,223,767 |

这复现了第二轮产物大小，但原来的 `-N` 本来就关闭 neverallow，不能称为“含 neverallow 通过”。
失败不等于内核 enforcing 必然拒绝加载；它说明离线安全准入还没通过，也不能证明运行权限正确。

正式 plat 的冲突集中于三个方面（具体原始位置都在 `sepolicy-user/neverallow.log`）：

- `vendor_sepolicy.cil:1439、1576、1591、1606` 等：debug 的 HAL → `su_202604` TCP 权限。
- `vendor_sepolicy.cil:3134、3135`：`gaokun3_smmustall` 的 `sys_rawio`。
  A17 设备树 `sepolicy/gaokun3_scripts.te:290-300` 明确把它放在 userdebug/eng 条件中；
  正式 donor plat 不具备同一豁免。不能为通过编译直接删掉它：相应 SMMU 辅助服务的行为也须评估。
- `vendor_sepolicy.cil:720`：atrace 对 `debugfs_tracing_debug_202604` 的文件访问。

userdebug 组合还存在跨分区内部属性编号不匹配的直接证据：

- 正式 `system/etc/selinux/plat_sepolicy.cil:84443-84444` 的 `base_typeattr_732`
  是 telephony server 的排除集；
- `system_ext/etc/selinux/userdebug_plat_sepolicy.cil:88069-88070` 同名属性变成 OMX
  client/server 的排除集；
- 正式 `system_ext/etc/selinux/system_ext_sepolicy.cil:17680` 仍用这个属性约束 telephony
  服务注册。`base_typeattr_734` 同样从 telephony find 排除集变成 power client/server 排除集
  （正式 plat:84439-84440，debug plat:88065-88066，system_ext:17683）。

因此“仅替换 userdebug plat”即使 `-m -N` 编过，也不能据此保证策略语义匹配。
下一步应对比 donor 的成套 debug 策略及生成方式，或在正式 plat 路线上移除明确的调试依赖，
同时保留原始策略和失败日志。暂未生成声称可交付的 precompiled policy。

## 调试与隔离原型

本轮 boot 来自 `out/a17-1792983426/payload.bin`，按 payload 的 new_partition_info.hash
校验通过，29,351,936 字节，SHA-256：
`e93c5a07ebcd122f56547ac4922c475bd1c1d0179df8eb381213eb45cc31355f`。

使用现有 `scripts/live/gk3-bootimg.py` 拆分后，原始 ramdisk SHA-256：
`3658b0bd89b9986d95089aab83dd6fa10d0006bdf966b00951f3ee790b6ffaa7`。

| 原型 | 本地产物 | SHA-256 |
|---|---|---|
| donor EROFS 系统侧 | `out/hyperos-r3/ramdisk-1792983426-erofs/ramdisk.cpio.gz` | `a55faeb46cab8d05d197d949e9a9e5b26182b109d65e73afd0e5bec6db159f97` |
| 独立 ext4 基线 | `out/hyperos-r3/ramdisk-1792983426-ext4/ramdisk.cpio.gz` | `e44ed9abb06f08c0a2d70d7c4972a33f84bbc3ef594d22f699679b68cdf04a18` |

`force_debuggable` 的源码入口在 `refs/a17/system/core/init/first_stage_init.cpp:504-522`；
debug 策略选择在 `selinux.cpp:207-222`。`IsEnforcing()` 在 `selinux.cpp:114-120` 仍受
编译期 `ALLOW_PERMISSIVE_SELINUX` 控制，因此加文件不等于更换 user init，也不等于全局 permissive。
原报告的“拿到 adb root”和独立评审的“拿不到”都未经本机 HyperOS 验证；ADB daemon 本地源码
`refs/a17/packages/modules/adb/daemon/main.cpp:67-89` 还读取调试状态及 root 请求，最终行为待测。

没有设置固定 TCP ADB，也未修改原始策略或增加 permissive 域。`debug.gk3.dbgramdisk=1`
用于将来验证 debug 属性文件确实加载，免授权 USB ADB 仍需连同 gadget、持久属性和框架行为验证。

## R3 当时的工程顺序（完成情况见 R4）

1. 解决 SELinux 正式/debug 策略组合：保存两条原始失败基线，避免用 `-N` 掩盖准入失败。
2. 制作实验 vendor 副本：同步完全一致的 fstab；实际落实 C2 属性、音频工具、RRO；
   隔离 boot HAL 的 ESP 写入和 update_engine。仅改 `misc_hos` 还不够：
   `device/huawei/gaokun3/boot_control/BootControl.cpp:114-142` 会更新 ESP 默认入口。
3. 固化 mi_ext/mi_product 的挂载或静态展开规则，保留其属性、init rc、文件标签及优先级。
4. 在上述检查完成后才合成 `super_hos`、核实逻辑槽位与镜像容量，准备早期日志捕获。
5. 上机前核实当前 GPT、救援入口和可恢复备份；安排人在场的 GPT/启动实验。当前未进行这些操作。

补充：根工作区当前在 `hyperos-port`；按 CLAUDE.md 尝试切 `dev` 时，Git 报该分支已由
`.claude/worktrees/rc2-relnotes` 占用。没有强制切分支、提交或推送；本轮新增源码仍为待审阅文件，
后续提交须按仓库约定整合到 `dev`。


## 设备连接后的只读复核（2026-10-26 20:34，Asia/Taipei）

原始采集：`out/hyperos-r3/device-20261026-203434/`。`collection.json` 记录命令及退出码，
`followup.json` 按单条命令记录槽位状态；前面的“本轮实际完成”描述的是连接设备之前的离线阶段。
本次仅查询、拉取文件及读取 boot_a 镜像前缀，没有设备写入、setprop、挂载或重启。

| 项目 | 本次结果 | 对适配的影响 |
|---|---|---|
| 设备/构建 | gaokun3，A17 `CP2A.260605.016`，戳 `1792983426`，Enforcing | 离线 ramdisk 使用的构建号与当前系统一致 |
| 当前/active 槽 | 均为 0（`_a`）；`_a` bootable/successful；`_b` 两项均为 0，查询 rc=70 | 第二轮 `_b` 记录已过时；不能把 `_b` 当作可用回退槽 |
| 启动路径 | cmdline 的 initrd 指向 ESP `android/slot_a/ramdisk.img`；无 gk3boot entry/event 参数 | 与直连槽条目一致；没有实际读取 ESP 条目或验证救援入口 |
| 解锁状态 | cmdline `verifiedbootstate=orange`，运行时属性 `green`；无 `/proc/bootconfig` | 不用运行时 green 推断真实启动参数；这不证明 HyperOS 的 debug 加载已经可用 |
| boot_a 内容 | 读取前 29,351,936 B，与 payload 的 boot.img SHA-256 完全一致 | 原型输入与设备 boot_a 对齐；不等于验证了 ESP 上的派生副本 |
| vendor 策略 | 拉回的全部文件逐项 SHA-256 与 R2 原始副本相同 | 前轮策略编译结果继续适用，无需因本次连接重复编译 |
| LP 元数据 | lpdump 默认输出 slot 0、四个 `_a` 分区及四个 `_a-cow`；super 12 GiB | 合成实验 LP 的槽位须显式确定，不能照抄旧 `_b` 假设 |
| Snapshot | `Update state: none`，Using snapuserd/userspace snapshots 均为 0 | COW 仍存在，不能据此擅自清理 |
| 独立分区 | 没有任何 `_hos` by-name 节点 | 隔离原型尚无对应物理分区 |
| 分区空隙 | 内核分区表显示 p6 结束 814606336、p10 开始 948561920，512 B 扇区 | 中间 68,585,259,008 B = 63.875 GiB；落盘前仍要核实磁盘 GPT 和备份 |
| 图形 | 当前 `skiagl`、EGL `angle`、Vulkan `freedreno` | 当前系统基线值已确认，不代表 donor 同样可运行 |
| C2 属性 | `media.c2.hal.selection=aidl` 仍在 `/system_ext/etc/build.prop:25`，vendor build.prop 未找到 | 实验 vendor 需要显式迁入，不能标记已完成 |
| 音频工具 | `/system/bin/tinymix` 存在，`/vendor/bin/tinymix` 不存在；音频脚本第 32-34 行先找 system 再找 vendor | donor 自带 tinymix 可减少一项风险，但 vendor 自洽化尚未完成 |
| 内核文件系统 | EROFS、LZ4/LZMA、xattr/security、OVERLAY_FS、SELinux 均启用；EROFS ZSTD/DEFLATE 未启用 | donor/重打包必须遵守压缩能力；这里只核对配置，未挂载新镜像 |

关键哈希见 `boot-a-comparison.json`；全部 vendor 策略比对见 `vendor-policy-comparison.json`。
boot_a 校验不保存设备分区镜像副本，仅在本地内存中计算摘要，**不构成可恢复备份**。

本次还确认实验 vendor 的 boot HAL rc 带开机完成属性中转及 shutdown 的
`--gk3-mark-poweroff` 动作（`vendor_init/android.hardware.boot-service.gaokun3.rc:10-11、40-41`）；
它们必须与 HAL 本身的 ESP 写入一起隔离。不能仅改 fstab 的 misc 路径就宣布实验环境不会影响现役启动链。

接下来的离线适配输入已核实。优先处理正式/debug 策略组合，再制作具备 C2 属性、音频工具和
启动写入隔离的实验 vendor；目前无需重启设备。


## R4：严格策略检查通过，隔离基线已合成

本轮全部为本地镜像操作及独立模块构建，没有设备分区、ESP、运行属性写入或重启。
收尾检查时 `adb devices -l` 列表为空，基线策略直接从校验过的 A17 镜像提取，不依赖设备在线。

### SELinux 修复与最终镜像的对应关系

`prepare-debug-policy.py` 将 debug plat 的匿名属性编号迁入独立范围，补回原 user plat
供 system_ext/product 引用的 2,340 条匿名属性定义。删除唯一一条 llkd 广域 ptrace allow；
原有 **5,217 条 neverallow 全部保留**，没有增加 permissive 域。R4 ramdisk 的调试属性
设置 `ro.llk.enable=false`，其 rc 门控见 donor `system/system/etc/init/llkd.rc:2-7、34-35`。
这是一份实验 debug 组合，正式 user plat 的 R3 失败组合并未被修复或交付。

| 组合 | runtime | 严格 neverallow | 证据目录（相对 out/hyperos-r4） |
|---|---|---|---|
| 修复后 donor debug + 原 vendor | PASS | PASS | debug-policy-check |
| 修复后 donor debug + tinymix vendor 策略 | PASS | PASS | vendor-policy-check |
| 原 A17 平台 + tinymix vendor 策略 | PASS | PASS | baseline-policy-check |

最终两份 vendor 导出的 CIL 均与检查输入逐字节一致，SHA-256 为
`6383b99b9a8b3391a1859aa17dfe12a35fce13e82d20bdc8a7fea8e52991ef9b`。
原 A17 system_ext 没有额外 userdebug plat，基线使用原 plat；HyperOS 则必须实际加载修复后的
system_ext debug plat。离线通过不能替代设备上的策略选择、属性加载和 enforcing 行为验收。

### Vendor 自洽化与启动写入隔离

`prepare-boot-hal.py` 生成独立构建模块，未修改生产设备树。在调用可能写 BCB 的
`impl_.Init()` 之前，强制校验 `ro.boot.force_super_partition=super_hos`、所选 misc 的真实路径
等于 `misc_hos`，并拒绝它与生产 misc 互为别名。`markBootSuccessful` 只更新隔离 BCB；
切换槽、标记不可启动和修改 snapshot merge status 均返回失败。模块不链接 EspSlot/Gk3Boot，
不启动原后台 worker；实验 rc 移除 GK3 属性中转及 shutdown 动作。
依据：`refs/a17/hardware/interfaces/boot/1.1/default/boot_control/libboot_control.cpp:196` 起。

另用同一 A17 tinyalsa 源码构建 vendor tinymix，并为其分配专用执行标签，只授权音频脚本执行。
实验 vendor 同步隔离 fstab、增加 C2 AIDL 属性，音频脚本仅调用 vendor tinymix。
移除旧 precompiled_sepolicy 及三份平台 hash marker，强制基线重新编译策略，避免缓存绕过新增类型。
镜像修改后回读内容、mode、uid/gid 和 xattr，`e2fsck -fn` 通过。

独立模块已在 D16 构建机成功构建（08:38）；产物大小与 SHA-256 已核实，日志为
`out/hyperos-r4/module-build.log`。确认无其他构建后已停机，状态为 deallocated。
8 项测试通过，覆盖归档隔离、CIL 修复，以及使用 ASan/UBSan 的 HAL 前置守卫和禁用写入路径。
它们不替代目标设备上的动态链接和 HAL 启动验证。

### 本地最终产物

以下路径均相对 `out/hyperos-r4/`，SHA-256 全值见各目录 manifest 和 `final-audit.json`。

| 产物 | 字节数 | SHA-256 前 16 位 | 验证 |
|---|---:|---|---|
| binaries/boot-hal | 以文件为准 | ac4f95250caed6e9 | arm64 原生构建、传输哈希 |
| binaries/tinymix | 52,096 | 5ed20f07502358b6 | arm64 原生构建、传输哈希 |
| vendor-final-ext4/vendor.img | 402,653,184 | 029424660fb6e796 | 基线 vendor，ext4 系统侧 fstab |
| vendor-final-erofs/vendor.img | 402,653,184 | 3f7cbc102ea21787 | donor vendor，EROFS 系统侧 fstab |
| system-ext/image.erofs | 830,148,608 | 40b0f7e9b4712492 | 3,737 条目内容/元数据回读一致 |
| hyperos-system/image.erofs | 958,152,704 | cb60be4da4de2c08 | 4,805 条目内容/元数据回读一致 |
| baseline-system/system.img | 1,069,793,280 | 36a56a18da30fa82 | A17 OTA rc 禁用、ext4 回读检查 |
| baseline-super-final/super_hos.raw.img | 12,884,901,888 | 533476594e1fdfec | lpunpack 四分区大小/哈希一致 |

HyperOS system_ext 仅替换修复后的 debug CIL；system 仅将 update_engine.rc 和
update_verifier.rc 改成注释文件，覆盖 donor 的常驻、lazy 和 merge 触发服务定义。
EROFS 统一使用当前内核支持的 LZ4HC，保留提取出的 SELinux/capability xattr；回读核验
未包含时间戳或硬链接拓扑，不宣称整镜像每个元数据字段均等价。

最终 super 为 **A17 隔离基线**，包含原 A17 system_ext/product、禁用 OTA 的 A17 system
和最终 ext4 vendor；LP 仅 `*_a`，super 名称 `super_hos`，不启用 virtual-ab。
文件是普通 raw 镜像（宿主文件可带稀疏区），不是 Android sparse 格式。尚未写入设备。

配套最终 ramdisk：

- ext4：`ramdisk-ext4/ramdisk.cpio.gz`，SHA-256
  `2f29968febf39924ee01d9e3faff9a928637ef28da989a23200aed8af8e96123`。
- EROFS：`ramdisk-erofs/ramdisk.cpio.gz`，SHA-256
  `87a9fa08f5825f1bc62d66a0cafa02cf8070bdf6e0029fdd86418e884791e52a`。

**已过时，勿用于实验组合**：`vendor-ext4/`、`vendor-erofs/` 保留旧策略缓存；
`baseline-super/`、`baseline-super-no-ota/` 引用旧 vendor。最终以 `vendor-final-*` 和
`baseline-super-final` 为准。旧产物保留作为过程证据，不覆盖或静默删除。

### 尚未准出的项目

1. 完整 HyperOS super：mi_ext/mi_product 的属性、init rc、overlay/pangu 优先级尚未集成；
   当前 HyperOS system/system_ext 只是已核验的中间镜像。
2. gaokun3 device_features、RRO、图形属性和阻塞 HAL 等待分类尚待实现及框架审计。
3. GPT、救援入口和可恢复备份尚未核验；设备没有 `_hos` 分区，`_b` 不能作为回退槽。
4. 早期失败日志、独立 ESP 条目和有人在场的基线启动尚未准备及执行。

所有产物维持 `ready_to_boot=false`。完整固件未发布、未提交专有内容；设备尚未启动 HyperOS。


## R5：设备重连、恢复资产检查与隔离块设备标签修复

采集目录：`out/hyperos-r4/device-20261026-221538/`。设备仍为 A17 `1792983426`、当前/active
`_a`、Enforcing；`_a` bootable/successful，`_b` 两项均为 0，不能作为已验证回退槽。
没有重新分区、刷写、setprop、启动项/EFI 变量修改或重启。

### 只读备份与救援文件检查

- `sgdisk --verify` 无 GPT 错误；本地另验主备 header CRC、分区数组 CRC 和两份数组一致性。
  唯一连续空隙为扇区 **814606336–948561919**（512 B/扇区），63.875 GiB。
- 本地备份主 GPT 34 扇区、尾部 GPT 33 扇区，以及完整 ESP（300 MiB）、misc、boot_a、boot_b。
  四份完整分区镜像均与设备端 `sha256sum` 一致；哈希见 `backup/manifest.json`。
  这些是启动资产备份，**不含 userdata、super 或整个救援根分区备份**。
- 从本地 ESP 镜像用 mtools 提取文件，9 个 loader entry 的所有 linux/initrd/devicetree/efi 引用
  均存在；slot_a 的 kernel/ramdisk/dtb 与已校验 payload 拆分结果逐字节相同。
- Ubuntu 救援 entry 的根 UUID 与 p3 的 ext4 UUID 一致。对 p3 使用临时私有挂载点、
  `ro,noload,nosuid,nodev,noexec`，确认 Ubuntu 26.04、systemd 和 live.squashfs 存在，随后卸载并
  清理临时目录。没有执行 journal replay。live.squashfs 为 212,566,016 B，SHA-256
  `cea4580f0e808f5077563d7dd229a92bb270a1ddf1fe0749893888be9b16aa72`。
- 当前 pstore 后端显示 efi_pstore、参数 N，目录为空。没有触发 panic 或切换后端；空目录不能证明
  启动失败时可以保留日志。历史已记录 EFI panic 写入失败，早期 kmsg 日志仍是独立门槛。

证据：`gpt-validation.json`、`esp-validation.json`、`rescue-filesystem.json`、`logging.txt`。
此次只验证救援文件可读与引用正确，**没有实际启动救援系统**。

### 修复 R4 遗漏的 `_hos` 块设备标签

R4 改了 fstab，但原 vendor_file_contexts 仅有 misc/metadata/super/userdata 原名精确规则。
`refs/a17/system/core/init/devices.cpp:452` 对节点及别名执行 best-match 标签查找；
`:567-570` 从 GPT 分区名生成 by-name 别名。新名字缺少规则会落入通用块设备类型，
即使 CIL 严格检查通过，也可能无法在 enforcing 下正常访问。

`prepare-vendor.py` 现从原 vendor 的四条精确 `-b` 规则复制上下文，分别新增
misc_hos/metadata_hos/super_hos/userdata_hos 精确规则。拒绝原规则缺失、重复、无块设备限定
或已存在隔离规则的输入。没有新增类型或 allow；CIL 与 R4 严格检查输入保持相同哈希。
新增三项回归测试，全部 **11 项测试通过**。

R5 两份 vendor 已完成改写回读、xattr/权限/属主核验和 e2fsck；新 super 已用 lpunpack
逐分区比较大小和 SHA-256。以下替代 R4 的最终 vendor 和 baseline super：

| 产物（相对 out/hyperos-r5） | SHA-256 |
|---|---|
| vendor-ext4/vendor.img | 87a8873cecf986d89bbc6b1c08087ce4dd4f21b51a726fc426e927b8a5322bcc |
| vendor-erofs/vendor.img | c725445e1aa6cc62a6497d4f8effb9c0f1266d074a2e7970ae9772f227325478 |
| baseline-super/super_hos.raw.img | fdeb81801b738dcfa46175aac23e0d687ba0dbe01e8bd80239d71d77134ccb3f |

**R4 的 vendor-final-* 与 baseline-super-final 也已被 R5 替代**，不再用于上机组合。
R4 的修复 donor system/system_ext、原生模块和 ramdisk 继续作为输入。

### 已准备的本地草案与剩余门槛

`out/hyperos-r5/partition-plan.json` 在已验证空隙内草拟 12 GiB super_hos、32 MiB metadata_hos、
4 MiB misc_hos、32 GiB userdata_hos，保留现有分区和剩余空隙。userdata 容量只是草案。
未执行任何 GPT 写命令。

`esp-baseline-package/` 包含独立路径的 kernel/dtb/隔离 ramdisk 和 A17 baseline entry，
29,333,242 B，低于当前 ESP 70,500,352 B 空余容量。参数继承实际 slot_a entry，追加隔离
super 选择，固定逻辑槽 `_a`；不覆盖现有 entry/default/oneshot。包仅在本地，未安装。

尚需早期失败日志、救援实际启动验证及有人在场的隔离分区/基线实验；完整 HyperOS 的
mi_ext/mi_product、RRO/设备特性和框架阻塞服务审计仍未完成。所有准入标记继续为 false。


R5 救援访问补查：只读确认 sshd 存在，ssh.service/ssh.socket 已启用，user 的
`.ssh/authorized_keys` 存在（未输出内容），NetworkManager/netplan 配置目录存在。
证据为同采集目录 `rescue-access.json`。这不证明重启后网络可达，实际救援启动仍须有人在场。


## R5 实际救援启动验证（用户明确同意后）

用户同意一次性 Ubuntu 救援启动。通过 `scripts/boot-oneshot.sh` 设置并回读
`8a29534fa802480d9fbb71aa18c01d7b-int-ubuntu.conf` 后执行一次 `adb reboot`。
已成功启动 Ubuntu 26.04，内核 `7.2.0-rc2-gaokun3+`；通过全网段扫描及 SSH 主机名/命令行
核验确认目标为 `gaokun3-rescue`，IP `192.168.10.225`，user 公钥登录和免密码 sudo 正常。
Android ADB 离线符合此次启动的预期。当前设备留在救援系统，尚未执行返回 Android 的重启。

`bootctl status/list` 确认当前条目是 Ubuntu；一次性变量已消耗。loader.conf 的内容与备份一致，
其 `*-android-a.conf` 模式当前解析出的有效默认条目为 **gk3boot-android-a.conf**，不是启动
此次实验前 cmdline 显示的直接 slot_a entry。默认设置未改写；不能据此声称已实测返回 Android。
所有原 entry、slot_a kernel/dtb/ramdisk 的 SHA-256 均与重启前备份一致。

救援环境的 sgdisk、partprobe、mkfs.ext4、e2fsck、rsync、zstd、busybox 均存在；
`sgdisk --verify` 通过，主备 GPT 原始区域哈希与备份一致。尚未分区、格式化或安装实验镜像。
系统为 degraded，失败单元为 Bluetooth MAC、modem setup 和 modules-load；SSH、sudo 和
磁盘检查工作正常，未尝试修改这些与当前验收无关的服务。

证据：`out/hyperos-r5/rescue-boot/{oneshot,reboot,ssh-identity,identity,boot,disk,recovery-tools,verification}.json`。
**救援实际启动与远程管理这一项通过**；早期实验日志及隔离分区/镜像安装仍待完成。
后续 GPT/格式化和实验启动不包含在此次仅进入救援验证的确认范围内。


## R6：早期日志实现、PID 1 演练与设备安装预检

本轮继续留在已经启动成功的 Ubuntu 救援系统。临时程序、loop 镜像和安装包暂存为本轮新增
设备文件；未执行真实 GPT 修改、分区格式化、实验 ESP 安装或新的重启。

### 原生早期日志

`tools/hos/early-log.c` 是固定 UUID 的 arm64 静态 PID 1 包装程序：

1. 先核验 cmdline 中唯一的 super_hos 选择及 `androidboot.hos_earlylog=misc-ring-v1` 标记。
2. 从 sysfs 识别唯一的 PARTNAME=misc_hos，UUID 必须等于 R6 分区计划，块设备大小必须为 4 MiB；
   不接受外部传入的块设备路径，找不到或冲突时停止交接，不回退生产 misc。
3. 前 64 KiB 完全保留给 Android bootloader/system space；两份带 CRC 的日志头位于 64 KiB 起，
   环形数据从 68 KiB 起，容量 4,124,672 B。每次提交执行 fdatasync；最多记录 180 秒。
4. 后台记录进程持有预先打开的 kmsg/隔离块设备 fd，追平已有 kmsg 后才允许父进程卸载全部
   临时挂载并 exec 原 init。没有 ESP 读写；主 ramdisk 的原 init 字节和所有 38 个原条目保留。

前 64 KiB 的依据为 `refs/lineage-bootable-recovery/bootloader_message/include/bootloader_message/bootloader_message.h:25-36`。
临时节点在策略加载前创建，所需追加权限仅为 kernel 对 tmpfs 字符节点 read、块节点 write，
及自身 syslog capability/system 权限；没有添加通用 block_device 访问或 permissive 域。
`prepare-vendor.py --early-log-policy` 才启用这些规则，未启用的 R5 路线保持原样。

`prepare-logged-ramdisk.py` 检查 ELF64/aarch64、无动态解释器/动态段、编译 UUID 与计划一致，
再包装现有隔离 ramdisk。`read-early-log.py` 只接受本地普通镜像文件，按日志头 CRC 选最新有效
副本并重组环形数据。CRC 保护的是头部元数据，断电仍可能留下不完整的数据尾部，不承诺日志事务原子性。

### 验证结果及边界

- Mac 单元测试发现 19 项，其中 16 项通过、3 项 Linux 原生编译测试跳过；Ubuntu 上日志相关
  5 项全部通过，补齐这 3 项，因此跨环境共覆盖 19 个不同单元测试。覆盖前缀保护、环形顺序、
  文件边界、损坏头部回退、错误/重复启动参数拒绝，以及安装计划的重叠/身份/编号/GUID 拒绝。
- `test-early-log-namespace.sh` 在救援系统的独立 PID/mount namespace 中，以临时 GPT + loop
  分区演练完整 PID 1 交接。捕获 107,645 B kmsg，交接前临时挂载点已消失，原 64 KiB 前缀不变。
  loop 与临时目录已清理。该测试专用二进制可替代 cmdline，生产二进制不包含该入口。
- 生产程序单独构建，source/binary SHA 与 argv 在 `logger-build.json`，无测试宏。
  编译器和静态 libc 来自已启动的 Ubuntu；未启动 Azure 构建机。
- 加入日志权限后的 A17 基线、修复 donor debug 两组均 runtime PASS、严格 neverallow PASS。
  最终两份 vendor CIL 的 SHA 与检查输入一致。日志版 vendor 的内容/权限/属主/xattr 回读及
  e2fsck 通过；super 的四分区 lpunpack 哈希一致。
- 目前没有在真正 Android enforcing 加载后验证持续记录。Ubuntu namespace 交接演练通过
  不能替代真实基线启动；该项正是下一次有人在场实验的验收内容。

### 当前产物（相对 out/hyperos-r6）

| 产物 | SHA-256 |
|---|---|
| early-log-production | 6b49fc6839c5b8e1563c5c5c3d976a5b48198e5b81b370533c90cd23c6cc5a61 |
| ramdisk-production-ext4/ramdisk.cpio.gz | 73d7f85a7b39458555ea04ad4b50735b90c9a25beab6b1ffbff2c821b85f5450 |
| vendor-ext4/vendor.img | 5a9d6bc63e2bb632d7b137ba9b9ef7a6d96a8e7d6cb0ed7eff6535d5219879e0 |
| vendor-erofs/vendor.img | 07b748830cb77db47ebc8882ec1f409cb83dc4a3b96924e274af58f45cab8e68 |
| baseline-super/super_hos.raw.img | 9340524de087721f7fc7e2df21d50d198046d5dfd40afcda8bd81ac44dfd704a |

只使用 `early-log-production` 与 `ramdisk-production-*`；其他 R6 ramdisk 目录是编译迭代过程，
不用于安装。带日志的最终基线包在 `install-bundle/`，绑定 R6 固定 GUID 分区计划与最终镜像哈希。
日志占用 misc_hos 的额外空间仅属于此次独立实验，不适用于原有 misc 的布局或大小。

### 安装包与 GPT 演练

`install-baseline.py` 默认只读预检；`--rehearse-gpt` 仅接受普通磁盘镜像，绝不格式化；
`--apply` 才允许 root 对真实块设备执行安装。它验证磁盘大小、主备 GPT 原始哈希及 CRC、
所有旧分区身份、新分区范围/大小/GUID、整套资产哈希和允许的独立 ESP 路径。
安装过程会另备 GPT、只格式化新 metadata_hos/userdata_hos、初始化新 misc_hos、写新 super_hos，
逐项回读，最后才发布新 entry；不会修改 default/oneshot 或自行重启。
执行途中失败即停止；GPT 一旦变化会拒绝直接重跑，避免无意重复格式化，需要先按日志诊断。

在只保存 GPT 内容的 512,110,190,592 B 本地稀疏磁盘镜像上，已经实际运行同一新增命令：
**原有 8 个 GPT entry 逐字节不变，新增 4 项均与计划一致，GPT verify 通过**。
该镜像不含原分区文件系统或用户数据，不是磁盘备份。

固定计划为：super_hos 12 GiB、metadata_hos 32 MiB、misc_hos 4 MiB、userdata_hos 32 GiB；
全部位于原空隙内，另留约 19.84 GiB。实际分区号、边界与 UUID 见 `partition-plan.json`。

安装包已暂存到救援系统 **`/home/user/hos-r6/install-bundle`**。12 GiB raw super 经 zstd 压缩为
1,660,347,183 B，传输后压缩文件 SHA 验证一致，再稀疏解压。真实设备上的默认只读预检 rc=0，
包括解压后的完整 super 和所有 ESP 资产哈希，以及当前 GPT 与备份一致的检查。
真实 GPT 仍无新分区，ESP 实验路径和 entry 尚不存在；尚未执行 `--apply`。

证据目录：`out/hyperos-r6/` 下 `early-log-tests.txt`、`namespace-test.txt`、`gpt-rehearsal.txt`、
`{baseline,donor}-policy-check/report.json`、`transfer.json`、`device-preview.json`、`final-audit.json`。
下一步需明确确认这套新增分区、安装及一次性 A17 基线启动；完整 HyperOS 的 mi_ext/mi_product
等集成仍是后续工作。当前 `ready_to_boot=false`，`ready_for_approved_installation=true`。

## R7：扩展分区挂载实现与六分区打包验证（2026-10-29）

本轮推进离线 donor 集成，没有执行设备写盘、GPT/ESP 修改或重启。R6 A17 基线包未改动。

- `prepare-ramdisk.py --donor-fstab` 新增 mi_ext/mi_product 的 EROFS logical/slotselect
  first-stage 挂载，保留 `/mnt/vendor/…` 原路径供 HyperOS init 读取属性和 rc；从 donor
  fstab 只导入 22 条只读 overlay，保留 mi_ext > pangu > 原目录的既有顺序。
- 修正报告历史判断：仅复制 donor overlay 行不足以在 first stage 挂载。
  `refs/lineage-system-core/init/first_stage_mount.cpp:149` 会过滤未带 first_stage_mount
  的条目，`:640` 才处理留下的 overlay。生成器显式补该标志，拒绝危险路径、重复目标和
  可写/非 lowerdir 选项。实际 A17 ramdisk 的挂载结果仍待上机确认。
- `make-baseline-super.py --hyperos` 支持五个 EROFS 系统侧分区加 ext4 vendor，严格要求
  六分区齐全、非空 raw 镜像、4K 对齐和容量不超组限额；默认 A17 四分区行为保持不变。
- 新 vendor 已同步完整 fstab，内容/属主/权限/xattr 回读和 e2fsck 通过。最终 CIL 与 R6
  donor 严格检查输入逐字节一致，SHA-256 为
  `662f05fef0e6f55ef1141ed47d04a0890ad61f3fa513f1c2ef38946e6c38be50`；未新增策略规则。
- 新 ramdisk 已用 R6 生产 logger 与固定分区计划包装，保留原 init；仍要求配套日志策略。
- Mac 22 项测试中 19 通过、3 个既有 Linux 原生日志测试跳过；新增 3 项在 Linux 容器通过。
  使用合成 EROFS/ext4 镜像执行真实 lpmake/lpdump/lpunpack，六分区大小和哈希全部一致。
  演练产物仅供测试，不可安装为系统。

产物位于 `out/hyperos-r7/`：

| 路径 | SHA-256 |
|---|---|
| vendor-erofs/vendor.img | 58cf35b8ec19fb5ba1df95de8369b08694fb38480df1abb17f0744f90679d36e |
| ramdisk-production-erofs/ramdisk.cpio.gz | 9adae56d9fb934648a848fb2c657d57a7a91c07deb50f570846c75cea4544043 |

`ramdisk-erofs/manifest.json` 记录 donor fstab 哈希；`input-audit.json` 记录策略一致性；
`super-smoke/super/manifest.json` 记录合成六分区拆包验证。

尚未完成完整 HyperOS 集成：本轮在项目与 /private/tmp 未找到原始 donor product、mi_ext、
mi_product 镜像。R2 仅有属性/rc/超级块记录等部分提取内容；R4 product.img 是 A17 基线，
不能冒充 donor 输入。需要重新提取并校验这三个分区，核验所有 overlay lowerdir/目标目录
及标签。当前未创建 `/mi_ext` bind 别名，仍需审计其调用方；donor mi_ext rc 与 init 自带
overlay 是否重复、userdebug 条件下 root/xbin 的行为也待核验。
overlay 挂载返回值在 first-stage init 中未作为致命错误处理，仍需通过真实挂载与日志验收。
框架 HAL 等待审计、设备特性/RRO、图形属性和 A17 隔离基线启动均仍未完成。
本轮产物全部维持 `ready_to_boot=false`。


## R8 进行中：恢复 donor 输入、补齐挂载别名与 init 修复（2026-10-29）

通过 R2 留存的官方 OTA 地址恢复镜像。新增 `tools/hos/fetch-donor.py`，要求精确 HTTP 206
范围响应，先以留存 METADATA_HASH 校验 payload 元数据，再逐操作核验 data_sha256，最后
核验整分区 new_partition_info.hash。仅接受全量 REPLACE/BZ/XZ/ZERO，拒绝 delta、重叠、
空洞、越界、解压长度错误；校验后的下载块保留用于续传，只有完整哈希通过才发布 .img。
这是与 R2 留存哈希一致性的检查，未新增 OTA 证书签名验证。

`out/hyperos-r8/donor/mi_product.img` 与 `mi_ext.img` 均已通过整分区哈希；product 正在同一
进程下载（exec session 99192，本次记录时到 600/3157 操作）。后续应先查询该进程，不能
仅凭记录或暂时无输出认定终止；终止后重跑同一命令会复用校验过的数据块。
完整 donor super 尚未生成，R7 的输入缺失状态正在消除，不能提前标完成。

新增 `audit-donor-mounts.py` 直接检查真实镜像内的每个 overlay 目录，注意 dump.erofs 对
不存在路径也可能返回 0，所以必须同时匹配正面的目录信息。mi_ext 的 21 个 lowerdir 路径
已全部存在（mi-ext-paths.json）；product 下载完成后需执行完整五镜像路径审计。

### R7 配套产物的修复

框架 FeatureParser 明确引用 `/mi_ext`，现新增 first-stage 只读 bind；donor system 根目录
确实有该目录。A17 源码 `init/first_stage_mount_android.cpp` 的 Create 会筛选 first_stage_mount，
MountOverlays 会遍历 overlay 且忽略返回值，进一步确认 R7 对标志的修复适用于 A17。

mi_ext 根不存在 root 子目录，`prepare-donor-init.py` 对四条精确匹配的 donor debug mount
作注释，保留其他 action；另外去掉已由 fstab 提前建立的两条重复 boot overlay。
生成器遇到额外/不同命令即报错；独立生成结果与实际替换文件逐字节相同。
新 system_ext 重打包 3,737 条目、新 mi_ext 重打包 188 条目的内容/元数据/xattr 均回读一致。
new vendor 的 fstab、权限/属主/标签回读与 e2fsck 通过；新 ramdisk 保留 R6 UUID 绑定日志。

| 产物（相对 out/hyperos-r8） | SHA-256 |
|---|---|
| system-ext/image.erofs | 1d82ba96940fc11afb41c107a0326c12a93dbffa6db149fd0c45a91292cf9161 |
| mi-ext/image.erofs | 0984849b182e09142ea4d0090a559d9e7c5684d9b43f95cfe3941aa1ef2ebd28 |
| vendor-erofs/vendor.img | d276351c10fc7db73ca4ea2bd482bd81353aac078a26ef8f179cc1e233445d68 |
| ramdisk-production-erofs/ramdisk.cpio.gz | 4fe8685d1cea1f07108ea70fa665ff88c3199c60041836e7258f4aa99d564e28 |

本轮 Mac 25 项测试：22 通过，3 个既有 Linux 原生日志测试跳过。初步 HAL 调用审计见
`docs/hyperos-hal-audit.md`；反编译存在错误，尚非完整的 DEX/调用链或启动验收。

R6 A17 安装包没有修改。只读 SSH 探测旧救援地址 192.168.10.225:22 超时，尚未重新定位设备；
没有设备写盘、GPT/ESP 修改或重启。完整镜像、图形配置、gaokun3 特性、框架门控和实际启动
均需继续推进，所有本轮产物仍为 ready_to_boot=false。


R8 图形配置补充：新增 `prepare-graphics-props.py`，system_ext 关闭
ro.surface_flinger.supports_background_blur，product 固定 debug.hwui.renderer=skiagl，关闭
persist.sys.enhance_vkpipelinecache.enable、persist.sys.background_blur_supported 和
persist.sys.background_blur_status_default。生成文件在 graphics-props/，product 尚待镜像下载
完成后回读输入并重打包，不能把生成配置当成最终镜像已生效。
system_ext 的 rc+属性组合重打包进行中（exec session 55748，输出 system-ext-graphics/），
完成后才替代本节表中的 system-ext/image.erofs。最终属性覆盖与实际渲染仍未验收。

R8 最新核验：system-ext-graphics 重打包已完成，3,737 条目全部回读一致，SHA-256 为
`196a51b4c7df88ce20a1b95e62e302fa9a9eb715f3816bc778bdc217fece82fb`，替代此前仅 rc 修复的 system-ext/image.erofs。
最终 vendor CIL 与 R6 严格检查输入仍逐字节一致。26 项 Mac 测试中 23 通过、3 跳过，
日志为 out/hyperos-r8/unit-tests.txt。product 下载进程 PID 28491 / exec session 99192
经 ps 与 session 轮询确认仍运行，最近输出 900/3157；尚无完整 product.img。


### R8 继续：设备特性文件、EROFS 新增路径与 DEX 复核

实际实验 vendor 的 `/build.prop` 回读为 ro.product.vendor.device=gaokun3，记录在
out/hyperos-r8/vendor.build.prop。新增 tools/hos/config/gaokun3.xml，共 44 个不重复配置项，
保留平板和实际 60/120 Hz 模式，禁用尚未实现的小米私有硬件协议与认证能力。
该文件尚未进入真实 product，不能声称 FeatureParser 已在设备加载它。

repack-erofs.py 新增 --add-plan，明确指定新路径、源文件和镜像内元数据模板。添加 XML
使用 etc/device_features/piano.xml 的 uid/gid/mode/xattr；拒绝已有目标、越界和模板软链接。
Linux 单元测试通过，另用带 security.selinux 标签的合成 EROFS 实际新增 gaokun3.xml 并
重打包，全部 5 个条目回读一致。证据在 addition-smoke/repacked/manifest.json。
当前待用添加计划为 product-add-plan.json，需与 product 图形属性替换一起实施。

Mac 全套 27 项测试：23 通过，4 跳过（3 个旧 Linux 日志测试和 1 个新增 Linux xattr 测试）；
新增 xattr 测试已在 Linux 容器补跑通过。日志为 unit-tests-with-additions.txt。

对 services.jar 继续审计：JADX 有 73 个错误，改用 apkanalyzer 直接反汇编 DEX 复核
BatteryService、BacklightAdapter、PowerManagerService 和 NotificationAttentionHelper
的四处灯对象调用，均有空值检查；排除了 JADX 重复控制流造成的一处假 NPE。未修改 DEX。
范围与证据见 docs/hyperos-hal-audit.md 和 framework/lights-dex-audit.json。

USB ADB 未发现设备，192.168.10.0/24 的只读 SSH 端口探测只发现路由器和本机，未定位到
救援系统；已询问用户设备当前状态和地址，离线工作可继续。没有设备写盘或重启。
product 下载 session 99192 / PID 28491 再次确认存活，最近输出 1600/3157。应继续查询同一
进程，不要重复启动；完成后校验原 product 属性、重打包属性和 gaokun3 XML，再审计所有
挂载目录与生成六分区 super。完整目标仍未完成。


### R8 完整 donor 输入到位，真实 product 集成启动

原下载进程正常结束，product 全部 3157 个操作及整分区 SHA-256 通过：
`603d21b82c534689c963af54671944af1f81d042ea749bacea1bfdb19c2e2415`。
三个完整 donor 输入均已保存在 out/hyperos-r8/donor，哈希见该目录 manifest.json。
原 product 的 etc/build.prop 与 R2 记录逐字节一致。

已对真实五份系统侧镜像进行 overlay 路径审计，22 条 overlay 的 47 个唯一目录全部存在，
报告为 mount-audit-original-product/report.json（尚使用原 product；修改完成后需再核对最终组合）。
从实际镜像回读的 7 份平台策略加最终 vendor CIL，共 8 项哈希与 R6 严格检查输入一致，
证据为 policy-image-audit.json。尚未重新验证真实设备 enforcing 下的标签访问行为。

product-final 正在 Linux 容器内重打包，替换图形属性并新增 gaokun3.xml，exec session 21848。
必须等待 manifest.json 和整树回读验证成功后才能用于最终 super，不能使用尚未完成的 image.erofs。


### R8 真实六分区 HyperOS super 离线集成通过

product-final 重打包成功，全部 **2,104 条目**回读一致；新增 gaokun3.xml 的权限为 0644、
uid/gid 为 0/0、SELinux 类型为 system_file，与模板一致。图形属性和 XML 内容均与生成输入
逐字节一致，product 两份策略仍与严格检查输入一致。见 product-final-audit.json。

最终五镜像目录审计再次通过：22 条 overlay、47 个唯一目录，无缺失目录；
mount-audit-final/report.json 包含最终镜像哈希。该检查不证明实际 overlay mount 成功。

**完整 HyperOS 六分区 super 已生成**：12 GiB raw super_hos，仅逻辑槽 `_a`，
包含 system/system_ext/product/vendor/mi_ext/mi_product。lpunpack 后六分区的大小和 SHA-256
均与输入一致；LP 属性均 readonly，所有 extent 位于 super_hos 内。

| 产物（相对 out/hyperos-r8） | 大小 | SHA-256 |
|---|---:|---|
| product-final/image.erofs | 6492934144 B | 497a463824c49effde23bf549e6af10db5a296aac9756f1b3f95150dfa6878c9 |
| hyperos-super/super_hos.raw.img | 12884901888 B | 1bfa8cdc00ec1dc617cfaf03ad42bc85a01db456086ceac5d616389d8c287755 |

组合清单以 hyperos-super/manifest.json 和 integration-audit.json 为准；配套 vendor 与
ramdisk 为 R8，不能混用 R6/R7 donor fstab。R6 A17 基线包保持原样。

另确认 donor 已提供 system_ext/bin/hwservicemanager 及对应 rc；system/bin 的软链接经
system/system_ext -> /system_ext 能找到该文件。证据 hidl-manager-audit.json。
因此“原 A17 平台缺 hwservicemanager”不等同 donor 镜像也缺该程序；实际服务运行仍待验。

没有设备写盘、安装或重启。新 super 仍为 ready_to_boot=false：设备重新定位、A17 隔离
基线启动、enforcing 早期日志、框架阻塞/空值审计、RRO 与硬件渲染等门槛尚未通过。
先前“product 下载中/完整 super 尚未生成”的段落为过程历史，以本节为最新结果。


## R9：用户授权上机，等待设备恢复连接

用户明确提出“上机吧”，已授权推进安装及启动实验。执行顺序仍为重新核对救援环境与磁盘，
安装/启动 A17 隔离基线，验收早期日志和隔离行为，再推进 HyperOS 启动。无需再次请求同一
上机操作授权，但不能跳过尚未完成的基线验收或把离线通过当作硬件通过。

当前 USB ADB 无设备，旧救援地址 192.168.10.225:22 超时；对原 192.168.10.0/24 网段
只读探测仅发现路由器 .1 与本机 .233，证据 out/hyperos-r9/ssh-discovery.json。
已请求用户将设备开机接网并告知当前系统/IP，或在 Android 下接 USB 数据线。
本轮没有设备写盘、GPT/ESP 修改或重启。后续恢复连接后必须重新验证设备身份、当前 GPT、
备份与安装包，不能沿用 R6 的旧设备在线状态。


### R9 真机安装与首次 A17 隔离启动

用户连接 USB 后确认原 A17 1792983426、_a、Enforcing。通过一次性 Ubuntu 入口重启，
SSH 确认 gaokun3-rescue；真实 GPT 与 R6 备份一致，安装程序和全部资产哈希通过。
用户上机授权下执行 install-baseline.py --apply：新增 p7 super_hos、p9 metadata_hos、
p11 misc_hos、p12 userdata_hos。原 GPT entry 字节保持不变，新文件系统 e2fsck 通过，
12 GiB super 写入并回读 SHA-256 一致，ESP 资产校验通过。安装日志为
out/hyperos-r9/install-baseline.txt。默认启动项没有改变。

一次性设置 gaokun3-hos-baseline-r6.conf 并回读 EFI 变量后重启，约一分钟后 ADB 上线。
实测 sys.boot_completed=1、Enforcing、debug.gk3.dbgramdisk=1；cmdline 确认 super_hos
和 misc-ring-v1，/data 为 nvme0n1p12，/metadata 为 nvme0n1p9。隔离 boot HAL 进程启动，
日志确认发布 android.hardware.boot.IBootControl/default。连续运行超过 180 秒。

发现调试缺口：ADB shell 正常，但 adb root 无效，日志明确 Failed to get adbroot_service
interface；因此 shell 无法读 misc_hos 或 dm table。首次 misc_hos-baseline.img 是失败读取的
空文件，不能用作日志证据。bootctl 的 shell 查询也失败，不能据此判 HAL 没有启动。
尚未验收 early logger 在 enforcing 下的持续落盘。

已通过普通 adb reboot 成功回到原 Android（生产 cmdline、root 恢复），再通过一次性入口
重启救援，准备读取 misc_hos 与核对生产资产。当前状态见 deployment-state.json。
针对已查明的 ADB 依赖，prepare-ramdisk.py 新增显式 --root-adb，用 debug 属性 ro.secure=0
让实验 adbd 首次启动不降权；不切 SELinux permissive。R9 新归档与 R4 相比仅
adb_debug.prop 多这一行，init/fstab 保持一致，四项归档测试通过；生产 logger 仍绑定 R6 GUID。
新 ramdisk 尚未上机验证。HyperOS 尚未安装或启动，不能把 A17 成功当作 HyperOS 成功。


R9 日志验收补充：第一次尝试回救援实际回到原 Android，原因未确证；已核对 cmdline，
没有把它误认作救援。利用原系统 root 读出完整 misc_hos 到 misc_hos-baseline-readback.img，
解码获得 362,564 B，两个 CRC 头均有效，未环绕、无 dropped notification，最后 kmsg 时间
181.327020 秒。包含 Enforcing 期间消息，证明本次实际 PID1 交接及持续落盘通道工作。
日志位于 baseline-early-log/，不再把早期日志硬件验证列为“完全未验”。
等待生产 sys.boot_completed=1 后再次设置救援一次性项并重启，仍需核验实际进入哪个系统。
R9 root ADB 实验 ESP 包已在 root-adb-esp/ 生成，仅新增 13,457,786 B ramdisk 与独立 entry，
复用 R6 kernel/dtb，尚未安装；不覆盖原 R6 基线或生产启动项。


### R9 root ADB 基线通过，HyperOS 写入进行中

第二次回救援成功，新增 R9 独立 root ADB entry 后实际启动：uid=0、su 域、Enforcing、
sys.boot_completed=1。dmctl 确认 system_a/vendor_a 指向 259:7（super_hos）；data/metadata
仍为 p12/p9。bootctl get-current-slot=0、is-slot-marked-successful 0=1，HAL 进程正常。
两份生产 boot 分区 SHA 与安装前记录一致。证据 root-baseline-verification.txt。
R9 root ADB 调试缺口已解决，不再是待验证项；首次 R6 日志覆盖 181 秒的证据继续有效。

现已再次返回救援，准备首次 HyperOS 启动。R9 donor root 日志 ramdisk 的 fstab 与 R8
vendor fstab 完全相同，只增加 ro.secure=0；独立 ESP 包为 hyperos-esp/，复用 R6 kernel/dtb。
因救援根分区仅余 5.5 GiB，不暂存 7.39 GiB 压缩包；使用经过预检的流式安装脚本，把本地
已校验 12 GiB raw 镜像直接送入 UUID 固定的 super_hos。脚本来源保存于
 tools/hos/install-hyperos-stream.py（R9 固定设备/固定哈希实验工具，非通用刷机工具）。

预检再次确认原 GPT entries 字节不变、四个 _hos GUID/边界/大小正确且未挂载、ESP 资产
一致和空间充足。脚本写入后必须验证传输哈希与整块回读哈希，随后仅重建实验 data/metadata、
清空已备份的 misc_hos，最后发布新 HyperOS entry；不自动改 default/oneshot 或重启。
传输在 exec session 99434 运行，日志 out/hyperos-r9/install-hyperos.txt；stream-started.json
阻止失败后盲目重跑。此刻 HyperOS 尚未启动。旧 A17 实验 entry 与新 super 不匹配，安装完成
后须停用旧实验菜单项，保留原生产 Android default。


### R9 HyperOS 已实际写入并发起首次启动

流式安装完成：12 GiB 输入哈希和 super_hos 整块回读均为
`1bfa8cdc00ec1dc617cfaf03ad42bc85a01db456086ceac5d616389d8c287755`。
metadata_hos/userdata_hos 重建并通过 e2fsck，旧 misc_hos 在救援根保存副本后清零，
独立 HyperOS ESP 文件逐项回读通过；原 production default 未改变。
日志 install-hyperos.txt 最后为 HYPEROS INSTALL VERIFIED。

旧 R6/R9 A17 基线 .conf 已改名 .conf.disabled，因为共享的实验 super 已换成 EROFS donor，
这些 ext4 基线 entry 不再适用；要重测 A17 必须先恢复对应 super。原生产 Android entry 未改。
一次性项 gaokun3-hos-hyperos-r9.conf 已设定并回读 EFI 内容一致，随后从救援执行重启。
当前处于首次 HyperOS 启动观察，尚不能宣称启动完成；实时结果见 deployment-state.json
和 hyperos-adb-first-observation.json。首次 HyperOS 与 A17 基线验收分开记录。


R9 首次启动观察修正：随后出现的 ADB 是原 crDroid 直连 _b，EFI Selected 也是原 Android B，
loader.conf 的 default 为 *-android-b.conf。用户确认在菜单里手动选择过 Android B／切槽。
因此本次不能算 HyperOS 成功，也不能判定 HyperOS 自动回退；实验 misc_hos 无有效日志头。
默认项变化来自用户切槽操作，不是安装程序修改。已保留用户当前 B 默认，不擅自改回 A。

已重新通过 Android root 设置 gaokun3-hos-hyperos-r9.conf，EFI 回读一致后重启，并告知用户
不要操作菜单，继续自动启动观察。结果仍待验证；hyperos-unattended-adb-observation.json
记录本次轮询。原失败读取 misc_hos-hyperos-first.img 作为无有效头证据保留。

### R9 实际 HyperOS 启动与两处框架崩溃（运行时诊断）

第二次无人操作启动已实证进入 HyperOS OS4.0.5.0.XPYCNXM：cmdline 指向
r9-hyperos ramdisk/super_hos，root ADB、Enforcing、mi_ext/mi_product 与 overlays
已工作，但最初 zygote 每约 5 秒 SIGABRT，system_server 尚未启动。
`out/hyperos-r9/zygote-strace.txt` 明确记录 donor libmedia 打开
`/vendor/etc/media_profiles_piano.xml` 返回 ENOENT，随后 fopen CHECK abort。
media.settings.xml 虽指向 media_profiles_vendor.xml，donor 按 ro.video.product.device=piano
选了专用路径；vendor 实际两个文件都不存在。UltraFramework ClassNotFoundException
出现在此前但执行继续，不能将它误判为本次致命原因。

诊断通过 tmpfs + 只读 lowerdir overlay 给 /vendor/etc 增加两个媒体 XML，正确标为
vendor_configs_file，未写 super/vendor。空 MediaSettings 被 donor 的 cameraIds.size()>0
断言拒绝；增加单路 640x480@15 H.264/AAC 候选后，zygote 成功启动 system_server
（PID 11531），执行到 boot phase 200 和首次 dexopt。该候选仅证明启动解析通过，
不是录像能力验收；文件为 media_profiles_candidate.xml，重启即丢失。

随后明确异常为 `WiFi display was requested, but there is no WiFi Direct feature`，
位于 WifiDisplayAdapter 构造路径，system_server 因此反复退出。证据
framework-first-logcat.txt、framework-crash.txt。尚未 boot_completed，未宣称桌面成功。

设备随后重启回默认 crDroid B，原因待用户观察及日志核对。诊断命令未在每次修改内
复查身份，导致 HosDisableWifiDisplay fabricated overlay 误建于生产 Android。
发现后先禁用，再通过 OverlayManagerTransaction.unregisterFabricatedOverlay 完整注销；
验证 list/dump 与 resource-cache 均不再存在该 overlay，清理证据
production-overlay-unregistered.txt。此次读取的 framework-res.apk 已改名
production-framework-res.apk，不能用作 donor 资源分析。后续所有实验修改须在同一
设备脚本内先验证 force_super_partition=super_hos 与 Enforcing，跨重启不能沿用身份。
已再次设定 HyperOS oneshot 并回读、重启，待第三次启动验证。

### R9 第五次启动达到首次设置界面，尚未稳定

第三次 early log 明确记录 system_server 请求 `reboot,RescueParty`，不是启动菜单自动
猜测回退。第四次证明仅关 config_enableWifiDisplay 的 RRO 不够：实际强制属性为 1。
现已查明来源是 `/system/build.prop:140`，而 donor framework-res 默认布尔值本身是 false。
DEX 方法读取资源 0x011101bd，JADX 错标成 config_hasPermanentDpad，不可据错名修补。

第五次在媒体 overlay 发布前设置 persist.debug.wfd.enable=0、
persist.sys.disable_rescue=true，root ADB/Enforcing 下得到 sys.boot_completed=1、
bootanim=stopped、system_server 正常创建，截图 `hos-first-boot.png` 为 Xiaomi HyperOS
欢迎页，activity 为 com.android.provision/.activities.StartupLanguageActivity。
这是首次设置界面验收，不是桌面或稳定性验收；用户屏幕观察尚未收到回复。

仍存在两项实际运行故障：
- com.android.phone 每约 250ms 因 ExtTelephonyManager 类解析失败重启。对应 donor
  extphonelib.jar 实际存在并含该类，权限 XML/usesLibraries 也有声明，不能简化为缺文件。
  pm disable-user/force-stop 未阻止持久进程重启，已用 pm enable 恢复，未作为修复保留。
- drm composer 每约 132 秒 SIGSEGV，引发 SurfaceFlinger/zygote/system_server 重启。
  二进制实际在 `/apex/com.android.hardware.graphics.composer/bin/hw/`，并非 /vendor/bin。
  root LLDB 在 Enforcing 下可附加，正在取崩溃现场；未放宽 SELinux。
  为排查息屏关联，将实验 screen_off_timeout 从 120000 调为 1800000，仍需恢复/验证。

新增工具 runtime-startup-probe.sh：每次修改在同一设备脚本内核对 super_hos、p12 data、
root 与 Enforcing。媒体候选放入 tools/hos/config/media_profiles_bootstrap.xml，
prepare-vendor.py 新增 --media-profiles；prepare-graphics-props.py 新增 --system。
离线生成 vendor-media/vendor.img 和 system-wfd/image.erofs，尚未刷入/打成新 super。
vendor 内容/权限/xattrs/e2fsck 均通过；system 全 4,805 项树校验通过；原 vendor 修改项和
CIL/contexts 字节保持一致。startup-fix-audit.json 记录新哈希，28 单测中 24 通过、4 Linux-only
跳过。新增测试覆盖真实空 XML 断言，录像本身未验证。

生产误建 overlay 的清理补充：注销后缓存未自动删除，后来已仅删除两条精确命名的 frro/
idmap 文件，并核对 list 中无该 overlay；最终证据 production-overlay-final-cleanup.txt。
设备当前仍在 HyperOS 实验，不改默认启动项；媒体修复仍依赖 tmpfs，重启后需要重新注入，
直到新 vendor 镜像真正安装。RescueParty 诊断开关仅留在实验 userdata，尚未恢复。

R9 composer 根因已由第二次 LLDB 现场确认：
`HwcDisplay::CreateFrameUpdateCommit +796` 从 `plan[client_z_order]` 越界读。
寄存器 x8=0xb400007cfda14820（vector base）、x23=0xb400008efda14940，
元素步长 0x120，反算索引恰为 0x10000001，即 HyperOS 稀疏 Z 顺序。
CreateLayerToPlaneJoiningPlan 用有序 map 压成稠密 vector，却把原 Z 值留作索引。
这比“息屏相关”的初步猜测更具体；延长息屏不是该问题的修复。
证据 composer-lldb-second.txt、composer-second-maps.txt、composer.elf。

源码补丁 tools/hos/patches/drm-hwcomposer-client-index.patch 在构建 vector 时记录
client_layer 的实际下标，并在消费缓存索引时检查边界；保持 SDM backend 的索引语义。
test_composer_client_index.py 编译打补丁后的真实生产函数、用假 DRM backend 承接，
覆盖 1/100/0x10000001/0xfffffffe 的 Z 值、设备层/光标层组合、无客户端层以及无效客户端
缓冲区。回归通过。编译机 CICD（原为 deallocated，D16）已启动；远端源码 clean 且两文件
哈希与本地参考完全一致，HEAD 87daba89b05fabc7f1e9316e3f283f19fd0c45cd。
已仅应用此补丁并发起 composer 模块构建，尚未上机验证；完成后需检查其他构建任务再停机。
LLDB 已 detach、目标未残留 debugger，ADB forward 已移除。尚未修复电话进程类加载问题。

电话崩溃进一步定位：PhoneLibraries 通过真实 IPackageManager.getApplicationInfo
(GET_SHARED_LIBRARY_FILES) 读出 sharedLibraryFiles/sharedLibraryInfos 均 null，尽管
pm list libraries 中 extphonelib 已注册且 APK usesLibraries 声明存在。
首次 framework 日志 11234 行直接报：`Package com.android.phone requires unavailable
shared library continuity_sdk_app; failing!`，com.miui.audiomonitor 同样失败。
缺少 continuity_sdk_app 导致整组共享库关联失败，ExtTelephonyManager 缺类只是后果。
证据 phone-application-libraries.txt；需追溯真实库及权限 XML 来源，不能塞空壳库冒充实现。

R9 composer 模块已编译并运行时验证：标准 vendor 模块 SHA
08d81e878d8a0f21f22f41ff4c5cf635cbd0a08db85d15b9508e9e8f5cf698b5，
原模块 8ff52b7682417b586858a96f2d922b98ae5b00f80487d7e3b12f68272847f4aa。
NEEDED 库集合一致。通过带哈希/隔离校验的 test-composer-fixed.sh 在 tmpfs 中保留原执行
标签、bind 到真实 APEX 路径，然后仅重启 vendor.hwcomposer-3（其 onrestart 会重启 SF）。
/proc/23783/exe 哈希确认实际运行新模块，system_server=24117。
恢复 screen_off_timeout=120000 后，完成多次手动息屏/唤醒，并至 15:08 保持两个 PID
不变、boot_completed=1、Enforcing；此前约 132 秒周期的 SIGSEGV 未复现。
截图 hos-composer-fixed.png 已显示可绘制的语言选择页，未自动接受设置条款。
修复仍为运行时 bind，重启失效；APEX 固化构建进行中。
首次 APEX 目标误用模块名 com.android.hardware.graphics.composer，构建明确报 unknown
 target；随后改为源码 Android.bp 的真实目标 com.android.hardware.graphics.composer.drm_hwcomposer。
电话依赖仍未修复，原生 vendor 镜像只离线提取/核查，没有刷入 donor vendor HAL。

### R9 固化更新准备：媒体/WFD/composer 与纯 64 位 ABI

电话根因闭环：扫描所有 APK manifest 找到 `/product/app/LyraSdkSpp/LyraSdkSpp.apk`，
包名 com.xiaomi.continuity.sdkapp，实际导出 continuity_sdk_app。它是 multiArch，但仅含
lib/arm64-v8a/libmicontinuity_sdk.so。初始日志明确因设备仍宣告 32 位 ABI 而拒绝扫描它。
经完整 payload/分区哈希验证提取的原 donor vendor/odm 都只宣告 arm64-v8a，ro.zygote=zygote64。
因此 prepare-vendor.py 新增显式 --64-bit-apps，以原 donor 合同修正 vendor ABI/zygote；
不伪造 SDK，不复制 donor 硬件 HAL。vendor/odm 镜像仅离线分析，来源验证见 donor-extra/manifest.json。

全量 composer APEX 目标触发设备树既有 `hal_lineage_health_default` 未定义策略错误。
未修改或放宽该策略。新增 repack-composer-apex.py 只在原 APEX payload 中替换已真机验证
的 composer，读回 mode/uid/gid/全部 xattrs、e2fsck 校验、用原匹配 AVB 私钥重签哈希树、
用原匹配 APK 证书重签包，并核验签名、4096 对齐与除 payload 外所有 ZIP 成员字节不变。
AVB key 与 APK certificate 均未改变。最终 APEX SHA：
4d9bf6821ddc3f99761890ab479f70927c177486a89f707e0e8b1158c4f0530d。
证据 composer-repacked/manifest.json、commands.json。失败的中间打包只操作离线副本。

最终 vendor-final/vendor.img 已同时含媒体 XML、64 位 ABI 和新 composer APEX；原厂
fstab/boot 隔离实现和现有 CIL/contexts 字节保持不变。system-wfd 只改强制 WFD 属性。
六分区 super-final 已完成 lpunpack 全部输入哈希核验，12 GiB SHA：
9b73a7f892914ba1999472ed8b7065288d0394a623ff937d529b327871ff7d04。
30 单测中 26 通过、4 平台条件跳过。

为保留实验 data，新增 update-hyperos-stream.py：验证固定 GPT/GUID/边界/原分区 entry、
无挂载与无 dm holders、当前整块 super 原 SHA、ESP 资产，然后仅更新 super_hos 并整块回读；
不格式化 data/metadata，不写 misc/ESP，不改默认项。开始标记阻止中断后盲重试。
已从 HyperOS 设置 Ubuntu oneshot 并回读后重启，实际 rescue hostname/根 p3/sudo 已验证；
新镜像上机预检进行中，尚未宣称固化启动通过。

最新 ESP 只读观察 default 为 *-android-a.conf（pre-update-boot-state.txt），与此前 B 不同；
本轮工具只写 oneshot，未写 default，变化原因未另行确证。更新以本次实际 default 字节为
保留对象，不擅自切回 B。CICD 编译机在取回并核验产物、确认无其他构建/登录后已发起 deallocate。

固化更新中的 ABI 优先级补充：A17 init 读取 ABI 的次序是 product > odm > vendor > system。
基线 vendor 镜像内 `/odm/etc/build.prop` 仍宣告 ARM32，因此仅修改 `/build.prop` 不够。
现已让 --64-bit-apps 同时更新嵌套 ODM 三个 ABI 字段，保留各自元数据与所有其他属性；
回归测试补充 ODM 情况，30 项仍 26 通过/4 跳过。生成 vendor-final-abi、super-final-abi。
首个 9b73... 镜像写入已在进行，不中断为半写状态；完成校验后留在救援，再更新完整 ABI
修正版，期间不启动这个中间版本。实验数据始终不格式化。CICD 已确认 PowerState/deallocated。

### R9 最终固化镜像写入完成，启动验证待回连

最终 super-final-abi（SHA256 251efec673b967a8f00c2fade5838cc014dc4f8698e530eb0fabbcce016f5079）
已完成 12 GiB 写入及整块回读，设备 installed.json 确认 verified=true，
userdata/metadata/misc 保留、ESP 与原 boot 分区未变。证据 update-super-final-abi.txt。
已在 gaokun3-rescue（根 nvme0n1p3）设置 HyperOS oneshot，UTF-16 EFI 变量回读
精确匹配 gaokun3-hos-hyperos-r9.conf，随后发出 reboot。当前等待 ADB 回连，
尚未验证固化修复的启动效果或 Phone 依赖恢复；不得把之前的运行时测试当作本次验收。

### R9 最终固化启动验证通过（有限时长验收）

cmdline 确认 force_super_partition=super_hos；OS4.0.5.0.XPYCNXM、root ADB、
Enforcing、boot_completed=1。ABI=arm64-v8a、abilist32 为空、zygote64。
媒体 XML 已正常读取，无 hos-media/hos-composer 运行时挂载。
实际 composer /proc/788/exe 与预装 APEX 哈希均匹配最终构建产物。
LyraSdkSpp 被正常扫描，continuity_sdk_app 注册；PhoneLibraries 返回完整的
sharedLibraryFiles/Infos，包含 extphonelib 和 continuity_sdk_app，原缺类崩溃消失。

已恢复 persist.sys.disable_rescue=false；WFD=0、屏幕超时 120000。
截至 uptime=213.29 秒、两次息屏唤醒后，composer=788、system_server=1039、
Phone=1746 均未改变。完整 logcat 未发现 FATAL EXCEPTION、Fatal signal、
NoClassDefFoundError。截图 hos-final-boot.png 为语言选择页，未自动完成条款或账号设置。
证据：final-boot-verification.txt、final-boot-wake.txt、final-boot-logcat.txt、
final-boot-properties-mounts.txt、final-boot-hardware-dumps.txt。
Wi-Fi 服务 enabled、camera 枚举两个设备仅为发现信息，不代表功能通过。
联网、音频、相机、长期稳定性及 keymint 安全补丁元数据仍待验证处理。

### R10 联网、帧率和触摸反馈处理

用户报告无法联网、掉帧、触摸不好用。现场证据保存在 out/hyperos-r10。
Wi-Fi 在关联阶段被 AP 以 status=31 拒绝；框架网络 ieee80211w=3，
supplicant 全局 PMF=0。先用单网络 PMF=2 证实 SAE 可握手，再将实验 data 的
wpa_supplicant.conf 备份到 /data/local/tmp/hos-r10-wpa-original.conf，追加 pmf=1。
重启 Wi-Fi 后读回全局 1、网络 3，自动连接 SAE/PMF，取得 192.168.10.182，
Android 网络能力 VALIDATED；外网 ping 三次全通（延迟约 440–488ms）。
prepare-vendor.py 新增 --wifi-optional-pmf；现有 data 配置不会被 vendor 更新自动覆盖。

SurfaceFlinger 调度记录在 20 Hz 停留约 6 分钟，应用/SF 时限约 50ms。
临时将实验 data 的 min_refresh_rate 和 peak_refresh_rate 设为 60.0，
读回 renderRate=60 Hz。两项原均未设置，删除即可回退；尚未量化修复后的掉帧率。

触摸坐标映射正确，但 donor system_ext 遗漏 persist.sys.gaokun3.touch_mode 默认值，
驱动停在 smoothing=1、debounce=2、hand_enabled=1。恢复本机原有 game 预设后，
原 vendor init 服务成功执行，读回 smoothing=0、两项 debounce=1、hand_enabled=0，
jump_dist2=0、pressure_enabled=1。prepare-graphics-props.py 已补入默认属性。
相关 7 项单测通过。三项调整均写入隔离实验 data；新镜像模板改动尚未重新刷入，
完整重启、用户触摸手感和滑动效果仍待验收。此次没有重启整机。

### R11 全功能测试启动，设备失联中断

用户提供指定 Wi-Fi 并授权全面测试。沿用已保存连接，密码未写入测试产物。
实际应用验证通过：HTTPS、16 MiB 存储 fsync/读回、四类传感器事件、
麦克风 144000 样本、音频播放接口、前后摄像头 YUV 连续出帧、BLE 广播扫描、
Keystore AES-GCM。听感、真实触摸、配对、拍照/视频和完整休眠重启不能据此认定通过。
电池遥测温度等显示 0，需排查。完整矩阵见 out/hyperos-r11/function-test-report.md。

亮度/旋转测试后、安装媒体补测 APK 前，USB ADB 与 Wi-Fi 同时失联；原因未知，
已询问用户是否有拔线/重启和屏幕状态。不得将本次全功能测试标为完成。
回连先确认隔离 cmdline，然后恢复：蓝牙关闭、accelerometer_rotation=1、
user_rotation=0；核对亮度恢复为 0.074743524。测试应用 org.gaokun.hosprobe
尚安装，完成测试后卸载。精确待恢复项见 out/hyperos-r11/test-state.json。

### R11 回连补测：JPEG 通过，编码与休眠暴露缺陷

已回连隔离 HyperOS，期间设备重启；用户确认移动到自习室，指定 Wi-Fi 不在范围。
game 预设与 60 Hz 设置跨启动保留；PMF data 修改未保留，原因是 vendor 的
wpa_supplicant.rc 在 post-fs-data 无条件复制 vendor 配置。必须将 PMF 固化至 vendor，
此前 README 关于 data 配置保留的推断已纠正。本次临时重新添加但未刷镜像。

前后摄 JPEG 缓冲区产出通过，未保存照片。AVC 编码输出映射报 22，copy buffer failed，
超时没有输出包；解码没有执行，视频仍不可验收。息屏唤醒不重启核心进程，但真实
suspend 计数仍 0，com.xiaomi.finddevice 持有三个 partial wake locks。
设置列表 220 帧：当前 janky 0.45%、legacy 27.27%，P95 22ms，不等于触摸手感通过。
设置旋转后 viewport 仍为 0，旋转未通过验收。详情与证据见 R11 功能报告新增节。

测试清理已完成：蓝牙关闭、自动旋转 1、user_rotation 0、亮度恢复请求并回读，
测试 APK 已卸载。当前 Enforcing、boot_completed=1。未擅自关闭查找设备服务。

### R12 Wi-Fi 与 Codec2 修复，固化更新进行中

Wi-Fi 已连接用户提供的 Xiaomi 17 热点；密码仅用于设备网络配置，不进报告。
vendor 构建启用 --wifi-optional-pmf，确保 post-fs-data 复制到 data 的默认配置含 pmf=1。

Codec2 A/B 定位：poolmask 从 0xfc0000 改成 0x50000/0xf50000 后，软件 AVC
16 帧编解码可通过，但硬件 AVC 仅 9–10 帧、无 EOS；恢复原 mask 则硬解 60 帧通过。
因此拒绝将全局分配器变更作为修复。平台与 swcodec APEX 内两份 libcodec2_vndk
都把非 ION 的 BLOB 线性块错误路由到 DMABUF mapper，返回 BAD_VALUE(22)。
新增 patch-codec2-legacy-map.py 对精确 donor 哈希的两个分支恢复 allocation->map()
虚调用路径（A16 语义），保留原分配器和 CFI，不支持自定义 mapper 回调。
两份库临时替换、重启 zygote/swcodec 后：软件编码/解码 16/16，硬件 AVC drain
60 帧含 EOS，前后 JPEG 输出通过。证据 dual-map-result.txt、hw-avc-dual-map.txt。

固化采用 system 库替换，加 system 中的只读 APEX 兼容库和 init post-fs-data bind；
原 APEX 文件/签名不修改。只补 init 对 system_lib_file 的 file mounton 权限；
runtime 与独立 neverallow 编译均通过。开机加载顺序仍待实际重启验收。
系统 4807 项内容/元数据核验、六分区 lpunpack 哈希匹配通过。新 super SHA256：
0bbcfa2a87671e225c9e362bc67d1bc2dbb7e246ad2290e34218b0e2ad20f1f6。

已进入 gaokun3-rescue，根 nvme0n1p3，救援新增当前热点配置。热点链路过慢，
首次流仅写 4194304 字节后暂停：精确核对前缀等于新镜像，再恢复旧前缀，整块旧
SHA256 251efec6...5079 核验通过，保留中断标记记录。随后创建临时 USB ECM 网络，
主机显式绑定 169.254.176.69，设备 169.254.77.1，改走 USB 重新执行完整更新。
当前写入进行中，尚未宣称固化启动成功；记录 out/hyperos-r12/deployment-state.json。

其他问题：FindDevice 线程栈及 APK 确认在无限等待缺失的
vendor.xiaomi.hardware.aidl.mtdservice.IMTService/default（及 HIDL fallback），
涉及状态/凭据存储，未伪造服务或关闭查找设备。iorapd 日志显示缺 /dev/iorap_dev，
每五秒退出重启；traced 也在重启，根因待查。HEVC/VP9 指定硬解组件创建失败，待查。

R12 最终更新与启动已验证：USB 完整写入/回读通过，实验 data/metadata/misc、ESP 和
原 boot 分区保留。重启后 Enforcing、boot_completed=1，热点自动连接且 VALIDATED，
PMF=1 跨重启保留。平台与 APEX 兼容库从 system 正常加载（maps/哈希匹配）；
软件 AVC 16/16 编解码含 EOS、硬件 AVC 60 帧 drain 含 EOS、前后 JPEG 均通过。
显式 init mounton CIL 规则在现有策略展开后冗余，编译策略哈希与原 R6 检查一致。

traced 根因是 product 的 traced.relay_producer_port=vsock://-1:2010 与无 AF_VSOCK
内核不兼容。清空该非持久属性并重启 traced 后 PID 5175 稳定，实际 2 秒 process_stats
trace 为 50184 字节。产品属性生成器已修正，**尚未刷入，所以此项仍仅运行时修复**。
iorapd 使用原 donor persist.sys.stability.PrereadEnable=false 开关停服，确认 stopped，
不再因缺 /dev/iorap_dev 重启；该属性已持久化，完整重启行为未再测试。
测试应用卸载，设备 trace 删除；核心 PID 786/803/1060 保持稳定，最后 uptime 254 秒。
当前结果与限制见 out/hyperos-r12/fix-report.md；休眠和 HEVC/VP9 硬解仍待修复。


### R13 运行时修复与 OOBE 定位（构建中）

- ccodec=4 并重启 media 刷新进程内 MediaCodecList 后，AVC/HEVC/VP9 硬解均可枚举。HEVC 与 VP9 各输出 60 帧且 EOS，证据 hardware-decode-drain.txt；保留 poolmask=0xfc0000。
- 用户批准停用 FindDevice；pm disable-user 被 MIUI 核心包保护拒绝，最终使用正常 pm hide --user 0。hidden=true、installed=true，APK 与数据保留，进程消失。回滚 pm unhide --user 0 com.xiaomi.finddevice（必须先核对 super_hos）。
- FindDevice 休眠锁消失；息屏后 framework 所有 suspend blockers 为 0。内核 gaokun3_usbrole 因接着 USB 主机持锁，内核 suspend success 仍 0；未绕过保护，不能宣称整机休眠通过。
- Wi-Fi 仍连接当前热点且 VALIDATED；密码不写报告。
- debug.power.fps.enable=false 的运行时门控使后续崩溃只剩 ThermalLogService，仍待镜像修复后完整观察。

## OOBE

用户反馈登录账号附近异常；已抓到精确 ANR：DefaultActivity.onActivityResult → FindDeviceState.getLastSessionUserId → ContentResolver.query，FindDevice Binder 阻塞 24.017 秒。见 oobe-anr.txt。
隐藏 FindDevice 后重新打开 Provision，可进入账号页；按返回不是原来的“前进”路径，不能当作通过。已请用户在账号页正常跳过/完成，复测下一页。没有写 device_provisioned/user_setup_complete 或清除引导数据。
字节码确认 provider 缺失时 null Cursor 分支安全返回；默认反编译 finally 的无条件 close 是伪影，没有据此修改 APK。

## R13 镜像内容

- vendor：显式 --all-codec2-components 固化 ccodec=4；--powerkeeper-thermal-read 限定 system_app 对 sysfs_thermal 目录/文件只读。没有写温控节点权限，system_app 是共享域，非按包授权。
- product：清空 traced.relay_producer_port，默认禁用不可用的 iorap 预读；删除错误声明的 fpsList，保留真实显示模式与 support_max_fps。关闭的是依赖缺失 displayfeature 的 donor FPS 控制器。
- system_ext：iorapd 服务增加 disabled，保留属性触发；触摸 game 默认值。
- 保留 R12 平台与 APEX 的 Codec2 mapper 修复、Wi-Fi PMF 配置。
- FindDevice hidden 状态由当前隔离 userdata 保留；新建/清空 userdata 不会自动应用此用户选择。

## 未关闭问题

实际休眠/唤醒、OOBE 前进路径、PowerKeeper 镜像修复后的稳定性、cgroup 迁移 EACCES、所有外设/触摸主观体验仍需验证。不得把本报告解释为“全部功能无问题”。

R13 OOBE 复测：19:30正常进入完成页，两个setup完成标志均1，未由工具写标志。原账号附近阻塞路径已通过，证据out/hyperos-r13/oobe-progress.txt。

R13 UI比例根因：donor两项440DPI覆盖gaokun3原240DPI，2560x1600横屏短边变成582dp。生成器已修正ro.sf.lcd_density/persist.miui.density_v2=240；product-density与super-density完整回读通过。最终待部署super SHA256：e4987222033d97c0da35731c5461b805fca48d9c590fff45268cc5c4c5345a37。救援USB流写入进行中，未宣称启动验证完成。


## R14 已部署：启动、媒体、调度与拔线休眠复测（2026-10-29）

本节取代上文“待部署/构建中”的当前状态描述。out 中的阶段性 deployment-state.json 和 fix-report.md 尚停留在部署过程中，不能代表最终状态；本节汇总实际启动后证据。

- 已完成 super 整块写入与回读，新 super SHA256 为 `313038b4773a7c9c8f0f98d03ee35b8158729c0106dd2592aabad736881628b3`。R14 EFI SHA256 为 `0b595a30926f04d4877ba15e7770b68c50750ca781db0ecea5e952eb641de771`；新增启动项 `gaokun3-hos-hyperos-r14.conf`，生产默认启动项保持不变。
- 本次复查 boot ID `1cf6b929-49cd-47cc-875a-0c34aa38d11e` 未改变，boot_completed=1，SELinux Enforcing，cmdline 明确隔离到 super_hos。
- 两项密度属性及 wm physical density 均240，无覆盖；分辨率1600×2560。保留用户字体比例1.25。此前已检查桌面及设置横竖屏截图。
- FindDevice 按用户选择隐藏，OOBE 由用户自然完成；没有强写setup标志。PowerKeeper PID3192、traced PID898保持，iorapd stopped。
- ccodec=4、poolmask=0xfc0000。永久部署后 AVC/HEVC/VP9 的 Surface 与 ByteBuffer 各60帧且EOS通过，后者实际读取全部帧像素。证据：`out/hyperos-r14/codec/{surface-after-boot,bytebuffer-pixels-after-boot}.txt`。自动硬件AVC编解码120/120也通过，测试APK已卸载。
- 新内核跨UID cgroup测试：具有CAP_SYS_NICE写入成功，不具有时仍EACCES；实际应用前台cpu/cpuset为top-app，Home后为background。证据：`out/hyperos-r14/kernel/{migration-after,app-cgroup-unlocked}.txt`。
- 用户拔线、息屏约一分钟、唤醒并接回USB后确认亮屏可操作。内核suspend success由0增至6，fail为2（EBUSY，suspend阶段，未记录具体设备），failed_resume及其子阶段均0。证明发生过成功休眠；不据此声称连续睡眠时长或所有尝试均成功。
- USB曾在接回后暂时未枚举，之后自动恢复；本次复查ADB在线、UDC configured、USB充电状态正常。没有进行USB控制器重绑。尚不能断言偶发重连问题根因已修复。
- Wi-Fi当前已连接家庭网络并有VALIDATED；网关3次ping全部成功。RSSI约-83dBm，信号较弱。没有将密码写入记录。
- 临时 `persist.adb.tls_server.enable=1` 没有启动TLS端口，现已恢复0并确认端口为空。

### 仍需处理

Type-C辅助驱动保持绑定，但/sys/class/typec为空；早期日志存在con2 alt modes注册失败及PPM init -ETIMEDOUT。这与当前USB数据连接正常可以并存，不能仅凭ADB连接宣称Type-C/DP功能完整。0092只重试GET_CONNECTOR_STATUS，尚未覆盖初始化失败。没有在缺少独立恢复通道时进行辅助驱动重绑。

此前 `out/hyperos-r14/ucsi-reprobe-review.md` 对num_ports清理的担忧未计入完整0088补丁；0088在ports_init赋值并修正数组边界，需以完整补丁链评估，不能将未打补丁的上游行为当作当前内核事实。

仍需确认两次EBUSY休眠失败的触发条件、USB偶发重连、外接DP及其他未覆盖外设。以上有限测试不代表“全部功能没有问题”。构建机已释放（deallocated），本次未重新启动。


### R14 后续修复准备与诊断（同日）

- 新增 `tools/hos/trace-suspend-ucsi.sh`，在独立trace实例记录失败PM回调、休眠阶段及UCSI命令，已在隔离系统启动。基线success=6/fail=2，pm_wakeup_irq=22对应pmic_pwrkey，只代表最近记录的唤醒源，不能归因历史两次失败。已请求用户再做一次拔线测试，尚未收到完成结果。全局tracing_on仍0。
- donor libc探测四个无精确标签的调试属性，使smmustall循环每次执行sleep/log时触发default_prop拒绝。新增严格生成器及5项测试通过；当前设备五分区实际上下文复核通过，四属性getprop均空。仅补exact libc_debug_prop标签，不新增allow或修改属性值；未部署。详见hyperos-libc-property-contexts.md。
- PowerMillet在无对应内核socket协议时不断重启。显式product属性生成模式及4项测试通过，尚未集成。关闭powmillet会影响OEM后台策略及Google freezer资格，不能当作纯日志开关；必须重启后做后台应用/通知/休眠回归。详见hyperos-r14-residual-log-audit.md。
- UCSI整轮超时重试候选虽然通过决策测试，深审发现GET_CONNECTOR_CAPABILITY早期失败路径引用清理遗漏，且Huawei未接入core PM回调，故已标记BLOCKED，未应用或部署。
- 改为只增加Huawei初始化transfer/ACK/reset失败日志的独立补丁。已在独立 `/home/vahiru/hos-r15-kernel-ucsi` 完整编译EFI，配置、内建cpio、版本对象与R14一致；原R14 EFI哈希未变。诊断EFI SHA256 `e8d4d91fb09d328da40e3ef67b4fe98e23908dfd00867bedae390ebb66316f00`，未刷入设备。它仅用于定位，不代表Type-C已修复。

本轮构建机已确认无其他构建后执行stop，服务端回读deallocated。诊断EFI本地下载哈希匹配；3799对象比较仅UCSI两对象及链接产物变化。当前设备仍R14，休眠计数6/2未变，独立trace实例保持开启等待已请求的物理测试；测试完成/取消后须collect清理。

## R15 已部署（2026-10-30）

当前状态以 [R15 部署与验收](hyperos-r15-deployment.md) 为准。永久导航 RRO 和触摸边缘钳位内核已部署且完整启动；用户确认应用内上划可回桌面。中央滑动仍不灵敏，新采样发现单指操作时短暂双接触，继续排查。之前诊断内核和 trace 等阶段状态均已过期；当前构建 VM 已停止，旧 trace 已清理。未部署 libc 属性标签、PowerMillet 关闭或 UCSI 重试候选。

## R16 已启动（2026-10-30）

参见 [R16部署](hyperos-r16-deployment.md)。面板物理尺寸纠正后SF实测244.82PPI，全局240DPI不变，控制中心面板大小恢复。设置按压背景仍在单独验证，不能将显示尺寸修复视为全部渲染问题解决。

### R16 扩容后回归与 Millet 处理

见 [本轮结果](hyperos-r16-regression-20261030.md)。无线ADB下确认扩容后的/data；存储、应用HTTPS、严格传感器、音频采样、双摄取帧、BLE扫描、AVC120帧编解码和三格式六条解码路径通过限定验收。停止缺失内核协议的Millet重启循环并持久关闭兼容开关，完整重启/后台策略回归仍待做。保留用户要求的临时USB device模式，不以此宣称休眠通过。

## R17 已部署（2026-10-30）

见 [R17部署与验收](hyperos-r17-deployment.md)。ADC-TM5内建、新真实皮温HAL和USB device-only模式已固化并完整启动验证；应用thermal API通过，host请求拒绝且息屏不切host，无线ADB持久启用。Millet在干净启动超过376秒未启动。保留新的扩容布局和生产默认启动项，数据未格式化。仍有OEM板温附加监视器、设置按压背景及物理外设/休眠等未关闭项，目标继续。

### R18 待集成候选

- Thermal HAL forecast负数参数与unsupported错误消息已按AIDL契约修复源码；实际方法的host编译/ASan/UBSan测试通过，尚未Android编译部署，见hyperos-r18-forecast-contract.md。
- libc四项诊断属性精确标签已生成system镜像候选并完成4807项元数据/内容核对，仅一文件变化，尚未部署。
- 设置双资源蓝色按压临时覆盖已分别验证首页、子页与取消恢复，静态APK通过vendor idmap校验；用户接受与持久部署待确认。
- OEM板温59号事件协议无unknown、消费者及策略校准未完整确认，真实SKIN不能直接映射旧等级；适配设计见hyperos-r18-oem-thermal-adapter-design.md，尚未改framework。

## R18 已部署（2026-10-30）

见 [R18冷启动验收](hyperos-r18-platform-deployment.md)。HAL负预测参数合约通过真机差分及冷启动Binder测试；四项libc诊断属性精确标签生效，同条件20秒重复拒绝80→0，SMMU轮询仍运行。继续使用R17内核/真实皮温/USB device-only，数据和默认启动项保留。设置双颜色仍为临时覆盖；OEM板温等剩余项未关闭。

### R19 光感与registry调查

现场确认自动亮度mode=1但无LIGHT provider，亮屏实际manual。已复现上游registry生成器同名组覆写导致非法JSON/丢字段，新增严格工具9测试及真实配置值保真验证。受限写backend原型3组C sanitizer测试通过，但基于不同fork且有默认错误码差异，尚未回移/Android构建/上机。没有启用会触发DSP崩溃的ALS会话，也未改校准数据。见hyperos-r19-auto-brightness-audit.md。

### R19 只读RPC诊断与生产回移准备

只读拒写日志版hexagonrpcd完成Android构建与临时运行，原shared lib/ABI不变；前、中、恢复后三轮严格IMU测试通过，已恢复原守护进程和自动恢复策略，未动DSP/ALS/registry数据。热重启定点采样未得到写路径，早期捕获窗口不足，不能作为否定证据。独立生产pin受限write回移五套离线C测试通过，但未Android构建或上机。见hyperos-r19-rpc-trace-runtime.md。
