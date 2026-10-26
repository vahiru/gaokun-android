# MateBook E Go 移植 HyperOS 调研（修订版）

> 依据：7 个角度的调研，每条结论都经过独立审查。审查判为 refuted 的结论没有采用；判为 weakened 的，按修正后的说法写；判为 unverifiable 的和只有二手来源的，都在文中标了出来。
>
> 实机数据来自 2026-10-26 的两轮只读 adb 查询：一轮是初稿时做的，一轮是这次修订时补的。两轮都没有重启、没有写分区、没有 setprop。
>
> 本次修订按评审意见改了第 1、2、4、5、6、7、8 节，逐条处理结果见末尾「评审处理记录」。

---

## 1. 一句话结论

**技术上有路可走，但不值得作为项目交付，也不能公开分发。就算只做私下实验，也要排到 1.0 发版之后。**

- **能做吗？** 有可能，但还没证实。
  - 设想中阻力最小的组合是：保留我们自己的主线内核和 A17 vendor，只把 system 一侧的分区换成 HyperOS 基于 A17 的版本，也就是类似 GSI 的做法。要换的分区是 system、system_ext、product、mi_ext；HyperOS 4 还要加 mi_product。
  - 版本基线**可能**对得上：
    - 本机 A17 的构建号实测是 `CP2A.260605.016`（10-26 `getprop ro.build.id`）。
    - HyperOS 4 system 的构建号也是 `.016`，但这只来自审查者解析 OTA metadata 的转述，属于单一来源，没有原文，置信度 low。
    - 即使构建号相同，也只说明 AOSP 基线相同。小米是否带了 `mapping/202604.cil`，是否改了 sepolicy 的类型、属性和 public API，都要等阶段 0 解包后才知道。
  - 所以"阻力最小"只是**待阶段 0 核实的假设**。
- **难在哪？**
  1. **SELinux 只能在 enforcing 下首次启动。** HyperOS 是 user 构建，SELinux 初始化在它自带的 `/system/bin/init` 里完成。离线把整套策略编过，是一道真正的硬门槛。
  2. **图形栈。** GLES 驱动只有 ANGLE；turnip 上的 skiavk 现在用不了；小米的模糊效果在 GL 路径上有已知的渲染缺陷。
  3. **vendor 没有完全自洽。** 有几样设备必需的东西放在平台侧。
  4. **小米私有服务。** 账号、云服务、查找设备都依赖 TEE，Millet 依赖内核。
  5. **调试通道。** 换上 user 版的 HyperOS 后，adb 默认是关的，预置的密钥也没了。这台机又没有串口，必须专门设计一条拿日志的路。
- **能分发吗？** 不能。
  - 小米用户协议第 2.3 条不允许装到非小米设备上；第 6.1.6 条不允许复制、修改，也不允许做兼容类衍生品。
  - 所以项目的 GitHub Release、R2 和 OTA 都不能带 HyperOS。
  - 改成"只发脚本、用户本地合成"，OTA 体系就作废了；而且违约责任只是转到了用户身上，并没有消失。
- **推荐路线：**
  - 主线不做 HyperOS 移植。
  - 用户真正想要的平板体验，走这条路：crDroid 上做 HyperOS 风格，接键盘时启用桌面模式，再单独装跨品牌的小米应用。
  - 如果用户想留一个技术探索：先做不碰设备的离线评估（阶段 0）。启动实验放到 1.0 发版之后，而且优先选"另建一套独立 super、userdata、metadata"的方案（见第 6 节），不动开发机现有的 super 和 /data。全程私下进行，不发布。
  - 顺带的收获：把 mesa 上游的"模拟第二图形队列"提交 backport 过来，可能让 crDroid 本身也用上 skiavk。这件事和 HyperOS 无关。

---

## 2. 地基评估：我们的 Treble / vendor / 内核 / 图形能承载外来 system 到什么程度

图例：✅ 已具备 · ◐ 部分具备或需改造 · ❌ 不具备。只有二手来源的项一律不标 ✅。

| 项目 | 状态 | 现状与说明 | 出处 |
|---|---|---|---|
| 动态分区 / Virtual A/B | ✅ | super 12 GiB（12884901888 字节），设计上只放 system、system_ext、product、vendor 四个逻辑分区，A16 和 A17 两棵树这部分相同。实机现状见下面"super 容量"一行 | `.claude/worktrees/a17/device/huawei/gaokun3/BoardConfig.mk:171-175、:222-227`；A16 树 `device/huawei/gaokun3/BoardConfig.mk:164-166`；实机 lpdump |
| odm / mi_ext / *_dlkm 分区 | ❌ | 都没有。设备上的 /odm、/vendor_dlkm 等只是 system 根下的空目录，/mi_ext 不存在。HyperOS 的 mi_ext 需要合并进别的分区，或者新增分区。system_dlkm 是 GKI 模块，直接丢弃 | 同上；实机 /proc/mounts |
| fstab | ◐ | 写法是 `first_stage_mount + slotselect`，文件系统类型写死 ext4，不带 avb。同一份文件同时装进 ramdisk 和 /vendor/etc。super、userdata、metadata 按 by-name（PARTLABEL）引用。HyperOS 的镜像是 erofs，要么重打成 ext4，要么两份 fstab 都改（得重编或重打 boot 和 vendor） | `fstab.gaokun3:3-6、:20-23`；`device.mk:16-18` |
| 可另指 super / fstab | ◐ | fs_mgr 支持两个 bootconfig：`androidboot.force_super_partition` 可以把 super 指到另一个 by-name 分区；`androidboot.fstab_suffix` 可以选另一份 fstab。这两个是"另建独立 super"方案（第 6 节）的基础，没有上机验证过 | `refs/lineage-system-core/fs_mgr/fs_mgr.cpp:2257-2266`；`refs/lineage-system-core/fs_mgr/libfstab/fstab.cpp:548-558`；`refs/a17/system/core/init/first_stage_mount.cpp:143、:398`；low |
| AVB | ✅ | 没开：树里没有 `BOARD_AVB_*`，cmdline 是 veritymode=disabled，外来镜像不用重新签名。verifiedbootstate 的出入见本节末尾的说明 | `BoardConfig.mk:131、:135`；`lineage_gaokun3.mk:355` |
| boot 结构 | ✅ | boot header v2，没有 vendor_boot 和 init_boot，第一阶段 init 来自我们自己的构建。**但第一阶段 init 只负责挂载，随后就 exec system 分区里的 `/system/bin/init selinux_setup`，所以 SELinux 初始化和第二阶段都由 HyperOS 自带的 init 完成**（见下一行）。HyperOS 4 的 init_boot 已经归到系统侧（missi），这一点出自审查者的转述 | `BoardConfig.mk:95-106`；`refs/a17/system/core/init/first_stage_init.cpp:571-572`；piano OS4 OTA metadata（审查者转述，low） |
| SELinux 模式 | ❌ | HyperOS 是 user 构建，它的 init 编译时 `ALLOW_PERMISSIVE_SELINUX=0`（只有 debuggable 构建才是 1），`IsEnforcing()` 恒为 true，cmdline 里的 `androidboot.selinux=permissive` 会被忽略。CLAUDE.md 记的"-user 的 init 强制 enforcing、装上去起不来（#117 §15）"是同一个坑。**首次启动只能在 enforcing 下过。** 唯一的绕法是把 HyperOS system 镜像里的 init 换成我们 userdebug 版的同基线 init，这属于改 system，兼容性没有验证 | `refs/a17/system/core/init/selinux.cpp:116-121`；`refs/a17/system/core/init/Android.bp:124、:141-148`；CLAUDE.md「发布与构建」 |
| Treble / VNDK | ✅ | VNDK 被构建系统强制清空，`ro.treble.enabled=true` | `BoardConfig.mk:308-315` |
| sepolicy plat/vendor 拆分 | ✅ | /vendor/etc/selinux 里有 vendor_sepolicy.cil、plat_pub_versioned.cil 和 precompiled_sepolicy | 实机 |
| SELinux 版本 | ◐ | vendor 的 `plat_sepolicy_vers=202604`（A17）。A16 的 system 里没有 `mapping/202604.cil`，而 init 会无条件拼出这个路径交给 secilc。**所以按源码推断，A16 版 HyperOS 配现行 vendor，在 SELinux 阶段就起不来（没有上机）。** A17 版 HyperOS 理论上带 202604 映射，需要解包核实 | `refs/a17/system/core/init/selinux.cpp:274-279、:335-342、:351-358`；`refs/lineage-sepolicy/private/compat`（最高 202504）；`refs/a17/build/make/core/config.mk:911-913` |
| lineage sepolicy 类型 | ◐ | vendor_sepolicy.cil 里有 235 行涉及 lineage 类型，但都在 vendor 自己的 cil 里声明过；init 编译时带 `-m -M true -G -N`，"找不到声明"这条失败路径基本可以排除。剩下两个风险：① 小米策略里有同名但种类不同的声明；② 小米的 202604 映射没覆盖我们 vendor 里 1011 处 `_202604` 引用。**因为首次启动只能 enforcing（见上），离线用 secilc 编过是硬门槛**（工具链问题见第 6 节阶段 0） | 实机 grep；`plat_pub_versioned.cil:8021、:8027、:8029`；`selinux.cpp:351-353` |
| VINTF | ✅ | device 兼容矩阵是空的，target-level 202504。A17 vendor 声明的 health 5、power 7、allocator 3 只出现在 202604 矩阵里，但这只影响构建期的 check_vintf，开机时不检查。在 A17 的 system 上没有冲突 | `compatibility_matrix.xml:27-28`；`manifest.xml:44`；`refs/a17/hardware/interfaces/compatibility_matrices/compatibility_matrix.202504.xml / .202604.xml` |
| HIDL | ❌ | 没有任何 HIDL 服务，hwservicemanager 和 vndservicemanager 都没在运行（/system/bin/hwservicemanager 是悬空软链）。原因是 hwservicemanager 列在只对 shipping API ≤34 生效的包表里，而我们的 shipping API 是 36。HyperOS 侧剩下的 HIDL 客户端会拿到 nullptr。可以用 `PRODUCT_HIDL_ENABLED` 加显式的 PRODUCT_PACKAGES 装回来 | `refs/a17/build/make/target/product/base_system_ext.mk:29-33`；`refs/a17/build/make/core/main.mk:1007`；`product.mk:337-342`；`lineage_gaokun3.mk:328` |
| vendor 自洽性 | ◐ | 下面几样东西在平台侧，换 system 就没了：<br>① system_ext 属性：发布构建里有 5 条，真正影响功能的只有 `media.c2.hal.selection=aidl`，不设就没有解码器。`ro.bpf.kver_override` 是 Lineage 专有的覆盖属性，HyperOS 多半不读，挪分区也解决不了。<br>② 编进 framework-res 的静态 overlay：刷新率、导航栏、power_profile、SettingsProvider defaults 等。<br>③ 用 platform 签名的 Gaokun3Parts（sharedUserId=system）。<br>④ crdroid-tree-fixes 对平台代码的补丁：Wi-Fi 软件 PNO、MAC 随机化、KeyGesture 兜底等。<br>⑤ 音频路由脚本直接执行 `/system/bin/tinymix`，vendor 里没有这个变体。脚本用的 `/system/bin/devmem` 只是 toybox 软链，影响小 | `lineage_gaokun3.mk:163-164`；`device.mk:339-340、:350-351、:738`；`refs/a17/build/make/core/package_internal.mk:113-152`；`parts/Android.bp:6-28`；`.claude/worktrees/a17/scripts/crdroid-tree-fixes.py:1112-1150、:429-470、:739-791`；`sepolicy/gaokun3_scripts.te:194-196`；`bin/audio-route.sh:32-34` |
| 刷写路径 | ◐ | fastboot 执行端：<br>· 拒绝逻辑分区操作和 update-super，拒绝 gsi；<br>· **也拒绝 fetch / upload，所以只能往设备里写，不能从设备读出来，不能用来备份**；<br>· 支持整块刷 super，自带 sparse 解析；刷完 super 后如果新 super 不服务当前 active 槽，会自动切槽。<br>备份只能走 live 安装器、Ubuntu 救援用 dd，或者在已开机的 Android 里用 `data-snapshot-android.sh` | `tools/gk3boot/fastbootd/cmds.c:358、:364-418（post_super，切槽见 :409-412）、:880-886`；`vars.c:131-136`；`fastbootd/sparse.c`；`.claude/worktrees/a17/scripts/data-snapshot-*.sh` |
| super 容量 | ◐ | **实机 lpdump（10-26）**：只有 metadata 槽 1 的 `_b` 一组（system_b、system_ext_b、product_b、vendor_b，合计 6,726,336 扇区，约 3.21 GiB），另外挂着 `cow` 组的 4 个 `*_b-cow`（合计 6,752,656 扇区，**实际占用约 3.22 GiB**）。`snapshotctl dump` 显示 `Update state: none`。super 已分配到第 13,481,040 扇区，剩余约 5.57 GiB。<br>piano OS4 中国版的 system 一侧合计约 9.0 GB（product 6.62G + system 977M + system_ext 847M + mi_ext 516M；审查者解析 OTA 清单，二手，low），加上我们的 vendor 约 0.26 GB。<br>⇒ **不可能和现有 `_b` 并存于现役 super**；整块替换后单槽能装下（约 8.6 GiB / 12 GiB），但留给 Virtual A/B COW 的空间很少。<br>刷整块 super 会用镜像里的 LP 元数据整体覆盖，残留的 cow 自然就没了；post_super 本身只读元数据、不专门清理 cow。cow 为什么残留，没有查（评审认为是合并后的残留，low） | 实机 lpdump、snapshotctl；`cmds.c:364-402` |
| NVMe 空闲空间 | ◐ | **实机（10-26）**：boot_b（p6）结束于第 814,606,336 扇区，metadata（p10）起于第 948,561,920 扇区，中间有 133,955,584 扇区，约 **63.9 GiB 未分配**。hw-inventory 8quinquies 记的 p9 `userdata-old`（64 GiB）已经不在 /proc/partitions 里，删除的时间没有查。这块空间可以用来另建一套独立的 super、userdata、metadata | 实机 `/sys/block/nvme0n1/nvme0n1p*/start、size`；`docs/hw-inventory.md:594-606` |
| 内核：AOSP 核心需求 | ✅ | binder/binderfs、ashmem、memfd、PSI、cgroup v1/v2、CGROUP_BPF、netfilter、DM_VERITY、UCLAMP、4K 页、39 位地址窗口都具备。**HyperOS 额外需要什么还没核实**（置信度中） | 实机 `/proc/config.gz`（第 135、210、457、1517、5684、6851、7020、7134 行等）；`.claude/worktrees/a17/scripts/kernel-config-android.sh` |
| 内核：EROFS | ◐ | 开了 EROFS_FS、ZIP（LZ4）和 ZIP_LZMA，没开 ZSTD、DEFLATE。底包用的压缩算法和块大小还没验证；也可以重打成 4K 块的 lz4 | 实机 config:7134-7138 |
| 内核：ACK 专有特性 | ◐ | 没有 USERFAULTFD、LRU_GEN、INCREMENTAL_FS、DM_BOW、DM_USER、DM_DEFAULT_KEY、BLK_INLINE_ENCRYPTION；/data 不加密（`ro.crypto.state=unsupported`）。开机不需要这些，但 HyperOS 的账号、查找设备、隐私空间是否依赖加密，还没核实 | 实机 config:884、:1048、:1049 |
| 内核：小米私有接口 | ❌ | 没有 Millet（它依赖 ACK 的 vendor hook，主线移植不了），也没有 /proc/millet、/dev/iorap_dev、thermal_message 节点和 KGSL。后果是功能降级或者服务反复重启，需要做能力门控补丁 | 实机 ls；社区内核树（二手） |
| GLES | ◐ | 唯一的 GLES 驱动是 /system/lib64 下的 ANGLE，vendor 里没有 egl/ 目录。EGL Loader 遇到 angle 后缀时只在 system 路径下找，所以换 system 后必须把 ANGLE 注入进去。另一条路是给 ANGLE 换个后缀放进 vendor（low，需验证） | `frameworks/native opengl/libs/EGL/Loader.cpp:504-506、:516-521`；`device.mk:288-304` |
| HWUI Vulkan（skiavk） | ◐ | **现在用不了，原因是 mesa 版本旧。** 上游 mesa 从 26.2 起对 "android framework" 默认开启模拟第二队列（提交 57c69c99），我们用的是 26.1.0-devel，没有这个功能。backport 这个提交就能补上，不用给小米的 libhwui 打补丁（还需上机验证） | `VulkanManager.cpp:245-253`；mesa `tu_device.cc:2104-2105`、`00-turnip-defaults.conf:65-69`；`refs/a17/external/mesa3d/VERSION`；`docs/stage5-freedreno.md:495` |
| HWUI 渲染器属性 | ◐ | 我们靠 vendor 属性 `debug.hwui.renderer=skiagl` 压住 skiavk。但 HyperOS 的 product 分区加载顺序更靠后，可以覆盖它，init 里的运行时 setprop 也能覆盖 | `Properties.cpp:245-246`；`property_service.cpp:1203-1225`；`device.mk:303-304` |
| composer | ◐（二手，low） | 据报小米 SurfaceFlinger 配通用 AIDL composer3 在 AVD 上能跑；我们的 drm_hwcomposer 也是 composer3。没有在我们的硬件上验证过 | HyperOS-AVD `scripts/patch_composer.py`（二手） |
| 安全 / DRM / 振动 HAL | ❌ | keymint 和 gatekeeper 都是 nonsecure 的软件实现；没有 DRM HAL，没有振动 HAL；指纹没完成 | `device.mk:106-116`；实机 service list |
| 调试通道 | ❌ | 见第 6 节"调试通道"。要点是：adbd 只认 `/adb_keys` 和 `/data/misc/adb/adb_keys`；本机的 `/adb_keys` 是指向 `/product/etc/security/adb_keys` 的软链，而这个文件在发布构建里本来就不存在；HyperOS 是 user 构建（`ro.adb.secure=1`，adb 默认关） | 实机 `ls -l /adb_keys`；`refs/a17/packages/modules/adb/docs/dev/keystore.md:17-28`；`refs/a17/packages/modules/adb/daemon/main.cpp:226` |
| 32 位 | ◐ | 本机是 zygote64_32，但 turnip 只编了 64 位。donor 很可能只有 64 位（未核实），对 arm64 手游影响不大 | `.claude/worktrees/a17/docs/mesa-a17.md:136` |
| GSI 前置测试 | ❌ | 项目从没做过 GSI 启动、VTS 或 Treble 合规测试 | 在 `docs/` 和 a17 worktree 里 grep 的结果 |

**关于 verifiedbootstate 的说明：** 实机读到的情况是：`/proc/cmdline` 里是 `androidboot.verifiedbootstate=orange`，`/proc/bootconfig` 里没有这一项，`getprop ro.boot.verifiedbootstate` 却读到 `green`。

最可能的原因是用户自装的 KSU、TrickyStore 一类模块在运行时改了属性。CLAUDE.md 记过 ro.build 指纹被运行时改动的先例。这只是推测，置信度 low，没有逐个排查模块。

这件事和移植 HyperOS 基本无关，不算待解的技术问题。但它涉及第 7 节的游戏原则（不伪造证明）：HyperOS 实验里不要把 green 当成"本来就有"的状态去依赖。

---

## 3. HyperOS 的硬依赖清单、后果与对策

后果分三档：
- **起不来**：system_server、SELinux 或 init 层面失败；
- **卡流程**：能开机，但开机向导或首屏卡住；
- **功能缺失**。

| # | 依赖 | 后果 | 对策 | 依据与置信度 |
|---|---|---|---|---|
| 1 | Android 版本对齐：A16 system 配 A17 vendor | 起不来（缺 202604 映射，也缺 genfs cil） | 选 A17 版 HyperOS（HyperOS 4，或 Xiaomi 17 上基于 A17 的 HyperOS 3）；坚持用 A16 版就得用 A16 树编 vendor | 源码推断，medium；没有上机验证 |
| 2 | split sepolicy 能否编过（小米 plat 策略 + 我们的 vendor cil），而且**必须在 enforcing 下** | 起不来，没有 permissive 退路 | 阶段 0 离线跑 secilc，必须过；不过就补映射或调整 vendor 策略。最后的手段是把 HyperOS system 里的 init 换成 userdebug 版（改 system，low） | `selinux.cpp:116-121`；low，必须验证 |
| 3 | GLES 驱动（ANGLE） | 起不来（zygote preload abort / SurfaceFlinger） | 往 system 镜像注入我们验过的 ANGLE 三个库，或者把 ANGLE 放进 vendor | 机制 high；vendor 化方案 low |
| 4 | skiavk 被 HyperOS 打开（product 属性或 init setprop） | 所有 App 在 turnip 26.1 上中止 | 短期：在 product 或 init 里强制 skiagl；中期：backport mesa 57c69c99 | 机制 high；HyperOS 是否会打开 skiavk **未核实** |
| 5 | system_server 主线程上对 vendor.xiaomi.* 的 `waitForService` | 可能被看门狗杀掉（本机音频出过同类事故） | 解包后用 jadx 搜 `waitForService` + `vendor.xiaomi`，逐处打 smali 补丁 | low；已知唯一的例子在小米账号 App 里，不在 system_server |
| 6 | MiuiLightsService 找不到通知灯时 NPE（A03 / HyperOS 1） | 起不来（当时的环境） | 在 miui-services.jar 里加判空。"在 lights HAL 里多报几盏虚拟灯就能绕过"**只是待验证的假设** | 二手单一来源，low |
| 7 | device_features / cust_features 配置 | 平板、键盘、笔、刷新率等特性回落到默认值；个别开关可能导致崩溃（OnePlus 上的 CameraMR 是那台机特有的情况） | 拿到 OS4 包后搜出实际路径和键名，自己写一份平板配置（不带 FOD 等） | 路径各版本不统一，low；**不能断定是开机必做项** |
| 8 | 小米私有属性的 SELinux 标签（如 persist.vendor.camera.*.scenes） | 在 OS3 上导致 framework 中止；OS4 Pad 构建没加也能起 | 遇到再在 system_ext_property_contexts 里补；放进 vendor 是否违反 Treble 命名规则还没查 | medium（仅 OS3） |
| 9 | 查找设备的 Provider | 卡流程：开机向导黑屏 | 在 sysconfig 里禁用这个组件，或者跳过开机向导 | AVD 实证（二手），我们大概率也要照做 |
| 10 | Millet / Greeze | millet_monitor 循环、刷日志；后台冻结失效 | 探测到 EPROTONOSUPPORT 就停掉服务，再打 smali 门控；冻结退回 AOSP（本机 binder freeze_notification 可用；HyperOS 会不会自动退回**未核实**） | 循环现象 high；退回机制 low |
| 11 | QTI perf、GNSS 扩展、Uniperf、XQos、thermal_message 等 | 等待、重试，甚至无限等 latch；功能降级 | 按"HAL 没声明"打补丁走失败分支（AVD 有现成思路；高通底包要 stub QTI 这一组） | medium–high（二手） |
| 12 | `persist.sys.enhance_vkpipelinecache.enable`（在 product 里） | 不兼容的 GPU 上可能卡在首屏 | 卡首屏时先查它，也可以预防性注释掉；turnip 是否不兼容**没有证据** | 二手，low |
| 13 | 小米 libhwui / libgui 里的硬件钩子、模糊效果 | GL 路径下弹窗背景透出、渐变模糊丢失（AVD）；120Hz 下掉帧、耗电 | 关掉 gradient_blur_perf 这类专用路径；强开或降低设备档位；最终要靠 skiavk | medium；只能上机测 |
| 14 | 小米 Flutter Impeller（桌面玻璃效果、天气） | 渲染错误或崩溃（turnip 上从没跑过） | 实测后按 App 打补丁或降级 | medium |
| 15 | 小米账号 / 云服务 / 支付（mtdservice、SEDC、mlipay、tidaservice、soter） | 功能缺失：账号登录卡住，钱包类不可用 | 删掉钱包和支付类 App；账号 App 打补丁，或者写一个如实报"不支持"的桩服务。OS4 能不能登上账号**不确定**（AVD 没打通，只有 HyperOS 1 的 A03 登上过） | medium |
| 16 | displayfeature / touchfeature / MiuiCamera | 功能缺失（色彩模式、DC 调光、双击亮屏、小米相机） | 可选：写一个 AIDL 服务，映射到 SurfaceFlinger 的色彩矩阵；删掉 MiuiCamera，换通用相机 | medium（二手） |
| 17 | 平台侧的设备定制（静态 overlay、5 条属性、Parts、tree-fixes） | 解码器全失效（缺 c2 属性）、刷新率和功耗档案不对、设备设置项丢失、Wi-Fi 和按键兜底失效 | c2 属性改到 vendor（或 product、system_ext）；overlay 改成 vendor RRO；Parts 改造成不需要 system uid（vendor 服务 + 属性）；tree-fixes 改成对 HyperOS 打 smali 补丁，或者放弃。**这一步做 vendor 化需要构建机**（见阶段 1） | 现状 high；改造可行性 medium |
| 18 | 音频路由依赖 `/system/bin/tinymix` | 没声音（如果 HyperOS 不带 tinymix） | 装 tinymix 的 vendor 变体（A17 的 tinyalsa 已支持） | high |
| 19 | 音频 appname 私有参数 | 可能有约 200 ms 的 standby 卡顿 | 实测；必要时把调用点 NOP 掉 | low |
| 20 | 联发科 / 高通 telephony 钩子 | 没有 modem，相关 App 可能反复崩溃 | 选高通平台的 Wi-Fi 版平板作 donor；电话相关组件做裁剪 | medium（二手） |
| 21 | Widevine、硬件证明、Play Integrity | 功能缺失（和现在的 crDroid 一样） | 不处理，不伪造（见第 7 节） | high |
| 22 | 调试通道（adb 默认关、密钥丢失、无串口） | 起不来时无从定位 | 见第 6 节"调试通道" | high（现状）；方案 low |

**要特别说明的一点：** 现成的先例 HyperOS-AVD（HyperOS 4 Pad 跑在 ranchu vendor + GKI 6.6 上）**不能**推出"未打补丁的 HyperOS system 能在我们的 vendor 上起来"。原因有四个：
- 那个项目改了 vendor，也对 libhwui、libgui 做了二进制补丁，补丁按 sha256 钉在 yingtian 4.0.15.0 上；
- 开发者自己标的是 experimental；
- 开机向导只在测试实例上走通过；
- 内核是 GKI，不是主线内核。

目前找不到"主线内核 + Mesa"跑 HyperOS 的先例，但这个否定结论**无法证实**。

---

## 4. 路线比较

| 路线 | 收益 | 成本 | 风险 | 结论 |
|---|---|---|---|---|
| **A. 完整移植**（社区 port.sh 模式：小米底包 vendor + 移植包） | 最接近原厂体验 | 用不了。社区工具要求底包是目标机自己的小米官方 ROM：脚本从底包的 vendor 读代号和版本、拷贝 device_features。Python 版只"理论上"支持 GKI 5.10+ 的高通小米机，仓库已归档 | 我们没有小米底包，前提不成立 | ❌ 不可行 |
| **B. 类 GSI：HyperOS system 一侧 + 我们的 kernel/vendor** | 可能拿到 HyperOS 的平板界面和桌面；技术探索价值高 | 很高。第 3 节第 1–18、22 项要逐项处理；smali 和二进制补丁按版本钉死，小米每发一个新版都要重做；平台侧的设备定制要重构；需要独立的分区或整块替换 super | 法律上不能分发；GMS 不比现在好；性能（模糊效果）和稳定性未知；补丁维护负担没有尽头；首次启动只能 enforcing | ◐ 只适合 1.0 之后私下实验 |
| **B′. 用现成的 HyperOS GSI（Mystic GSI）** | 省掉自己解包 | 低 | 按第三方索引，Mystic 最新一版在 2026-01（low，没有核对下载页的原文），**没有看到 A17 / HyperOS 4 镜像**；A16 版配不上现行 vendor（见第 3 节第 1 项）；成品来源灰色 | ❌ 目前没有能配 A17 vendor 的现成 GSI |
| **C. crDroid 上做 HyperOS 风格 + 单装小米应用** | 低成本拿到"像小米"的外观；不碰小米系统；可以随项目发布 | 低：主题、图标、启动器。米家、小米运动健康这类跨品牌应用按设计能装（逐个兼容性**未核实**）；系统级应用（互联、互传、系统桌面、相册）多半装不上 | 低 | ✅ 推荐 |
| **D. 部分功能移植**（工作站、多窗口） | 用户最在意的平板生产力 | 小米的工作站模式长在 framework 和系统桌面里，搬不出来。替代方案是把 AOSP 桌面窗口化改成"接键盘时启用"。现在为了避免所有 App 都变成自由窗口而把它关掉了，运行时关不回去，可能要打 tree-fix 补丁 | 中：要改 framework | ✅ 推荐，和 C 一起做 |
| **E. 虚拟化**（在宿主里跑 HyperOS 客体） | 宿主不动 | 现行启动链下没有 /dev/kvm。要走 EL2，得改造 gk3boot、把 el2 补丁移植到 7.2.9、引入微软签名的闭源 tcblaunch.exe，还要另外处理 DSP（不然没声音）。AVF 只支持 Microdroid 和 Linux 客体，不支持完整 Android；客体只有 virtio-gpu | 工作量是一个子项目；HyperOS 没有面向虚拟设备的构建（推断）；GPU 性能撑不住游戏 | ❌ 不是完全做不到，但对目标（游戏、平板体验）没有意义 |

注：CLAUDE.md 写的"KVM/EL2 可用"，指的是硬件和社区层面能做到，现行 Android 启动链并没走 EL2。这句话容易误导，建议更正（见第 8 节）。

---

## 5. donor 机型与版本选择建议

### 5.1 版本匹配矩阵（我们的 vendor × HyperOS 的 system）

| | 我们的 A17 vendor（202604，现役） | 我们的 A16 vendor（202504，crDroid 16 旧树） |
|---|---|---|
| HyperOS 4（A17；构建号 `.016` 只有审查者单一来源） | ◐ 版本方向对（Treble 正向组合）；**能否起来要等阶段 0 核实映射、secilc 和 API 改动** | ◐ 理论上可行（新 system 配旧 vendor），但 A16 树已停止维护 |
| HyperOS 3 基于 A17（Xiaomi 17 / 17 Ultra 稳定版，2026-07，二手） | ◐ 版本对得上；但目前看到的是手机，没有平板 | 同上 |
| HyperOS 3 / 3.1 基于 A16 | ❌ 按源码推断在 SELinux 阶段失败（缺 202604 映射，没有上机）；硬补映射成本高、风险大 | ◐ 版本对得上，但 A16 树已停止维护，和 1.0 方向冲突 |
| A15 及更早（例如平板 6 Max 14） | ❌ | ❌ |

背景：
- 据审查者解析 piano 的 OTA metadata，小米自己也是"新 system 配旧 vendor"：HyperOS 4 是 A17 system 配 A15 vendor/odm。这是二手单一来源，low。
- 社区里没找到"vendor 比 system 新"的先例（二手）。
- Treble 只保证 vendor API 级别不高于 system 的组合（source.android.com api-flags 原文）；"反过来不支持"是推论。
- 另外注意：Google 官方 A17 GSI 的构建号是 `CP2A.260605.012`（2026-06-18，build 15430684），和本机的 `.016` 不同。阶段 1 用它做基线时，要把这个差异算进归因。出处：https://developer.android.com/about/versions/17/gsi-release-notes 。

**结论：用 A17 vendor 配 A17 版 HyperOS，不要为了 HyperOS 回到 A16 树。至于"能起来"，要等阶段 0 判断。**

### 5.2 候选 donor

| 优先级 | 机型（代号） | 优点 | 缺点 |
|---|---|---|---|
| 1 | **小米平板 8 Pro（piano）** | 据第三方索引已有 HyperOS 4 中国版卡刷包 OS4.0.5.0.XPYCNXM（A17；约 10.3 GB、2026-10-23 只出自 xmfirmwareupdater 索引，low）；官网写明工作站模式和 PC 级 WPS；高通平台（骁龙 8 Elite），要 stub 的是 QTI 那一组；可以从官方 CDN 按 Range 只取需要的分区 | 只有中国版、"Stable Beta"，没有 GMS；屏幕 3200x2136（3:2），和本机 2560x1600（16:10）不同；镜像体积大（system 一侧约 9 GB，low） |
| 2 | Redmi Pad 2 Pro（organ / flute） | 12.1 寸 2560x1600 16:10，和本机几乎一样；镜像较小（product 约 3.4 GB，low） | 现在只有 A16（配不上 A17 vendor）；HyperOS 4 中国版按第三方排期在 11-30 前；官网没提工作站模式 |
| 参考 | 小米平板 9 Pro Max（yingtian，XRing O3） | HyperOS-AVD 已经铺好一整套能力门控补丁，可以借鉴 | 不是高通平台（要 stub xr_* 节点）；补丁按 sha256 钉在特定版本 |
| 参考 | Xiaomi 17 系列 HyperOS 3（A17，国际版） | A17 + 国际版（可能有 GMS） | 手机界面，没有平板特性 |
| 排除 | 平板 6 Max 14（yudi） | — | 最新的 HyperOS 3 仍基于 A15（第三方索引） |

补充（下面几条都是二手，low）：
- 社区说法是版本号第 7 位字母对应 Android 版本：W=16、X=17。没有找到小米的官方说明。
- 同一机型不同区域的分区大小差别很大（据称 piano 欧洲版的 product 只有 4.04 GB）。
- system 和 system_ext 的设备名都叫 missi，按区域、日期分别构建；product 带机型定制。
- HyperOS 4 国际版的状态各家报道不一；piano 现在只看到中国版的 A17 包。

---

## 6. 若要做：分阶段实验计划

**总前提：**
- 只做私下实验，不发布任何含 HyperOS 的产物。
- **阶段 1 及以后排到 1.0 发版之后**（理由见下面的"与 1.0 主线的冲突"）。
- 每次重启、刷写、改分区表都要先征得用户同意，而且要有人在场能按电源键。
- 构建机：阶段 0 不需要（工具链要另外解决，见阶段 0）；阶段 1 的"vendor 自洽化"需要（见阶段 1），由用户决定。

**开发机现状（10-26 实机只读复核，以此为准）：**
- `ro.build.id=CP2A.260605.016`，`ro.build.date.utc=1792975257`，这是 A17 迁移 M4 装进 `_b` 的构建（本地载荷在 `out/a17-1792975257/`）；`ro.build.type=user`。
- cmdline：`androidboot.slot_suffix=_b`、`gk3boot.mode=action`、`event=none`、`streak=1`、`dispatch=1`。
- `bootctl get-active-boot-slot` 和 `get-current-slot` 都是 1（`_b`）。
- ⇒ **现在是正常走 active 槽 `_b` 起来的，不是回落。** CLAUDE.md 里 10-18 那条"回落到 `_b`"的记录，已经被 10-25 之后的安装取代了。
- `_a`：`bootctl is-slot-bootable 0` 和 `is-slot-marked-successful 0` 都返回非零（rc=70，解读为"否"，low）。super 里也没有 `_a` 的逻辑分区（lpdump 只有 metadata 槽 1 的 `*_b` 和 `*_b-cow`）。
- /data 已用 113 GB，其中 `/data/media` 42 GB、`/data/gk3-snap/a16-20261026-001616`（M4 前的 A16 快照）31 GB。
- `persist.sys.usb.config=adb`，`ro.adb.secure=1`，`/adb_keys -> /product/etc/security/adb_keys`（目标文件不存在）。

**与 1.0 主线的冲突：** 开发机是唯一一台目标机，正在承担 A17 1.0 的验收。a17-migration-plan.md 的 M4 在 10-26 刚跑完 release-checklist A 档，后面还有 M5（游戏环境）和 M6（`.claude/worktrees/a17/docs/a17-migration-plan.md:112-126`）。

如果在现役 super 和 /data 上做阶段 1、2，会有三个后果：
- 中断 1.0 验收；
- 让验收结论失效（装回去之后要重跑 A 档）；
- 丢掉用户自装的 TrickyStore、PIF、LSPosed、HMA 等测游戏的环境（CLAUDE.md「现在设备上跑的是什么」）。

所以建议：阶段 0 可以随时做（不碰设备）；阶段 1 及以后放到 1.0 发版之后，并且优先选下面的方案 Ⅲ。

**启动实验的三种落盘方案：**

| 方案 | 做法 | 优点 | 缺点 / 风险 |
|---|---|---|---|
| Ⅰ 在现役 super 里新建 `_a` 组 | — | — | ❌ 不可行：super 只剩约 5.57 GiB，HyperOS 一侧约 9 GB（见第 2 节）；执行端也拒绝逻辑分区操作（`cmds.c:880-882`） |
| Ⅱ 备份后整块替换 super、清空 /data | 执行端 `fastboot flash super`（支持 sparse） | 不用改分区表、不用改 fstab | 要整机备份（见下）；中断 1.0；回滚要刷回 super 并恢复 /data |
| **Ⅲ 另建独立的 super、userdata、metadata（推荐，low，需验证）** | 在 63.9 GiB 的未分配空间里建三个新分区，例如 `super_hos` 16 GiB、`userdata_hos` 约 47 GiB、`metadata_hos` 32 MiB，分区名都不同于现役的。用一个独立的 systemd-boot 直连条目启动，cmdline 带 `androidboot.force_super_partition=super_hos` 和 `androidboot.fstab_suffix=<新后缀>`；ramdisk 和 vendor 里各放一份新 fstab，指向 `_hos` 分区 | 开发机现有的 super、/data、metadata 完全不动，失败了重启回默认条目就行；/data 不用备份 | ① 要改 GPT（在 live 或 Ubuntu 救援里做，属于写分区表，要用户同意）；② 新 fstab 要进 ramdisk 和 vendor 镜像（重打 boot、改 vendor ext4，可以在 live Linux 上做，或者走构建机）；③ `force_super_partition` 和 `fstab_suffix` 的路径没有上机验证，A17 树里的实现要再 grep 核对（上面引用的是 lineage-system-core）；④ **misc（BCAB、BCB、GK3 记录）是共用的**，见下面的"gk3boot 交互" |

**整机备份（只有方案 Ⅱ 需要）：**
- fastboot 执行端读不出数据（`cmds.c:883-884`：fetch/upload 一律回 "not supported"）。
- super（12 GiB）只能在 live 安装器或 Ubuntu 救援里 dd 到外接盘；或者在 Android 里 root 读 `/dev/block/by-name/super`。核对字节数和 sha256，按 CLAUDE.md「运维坑 1」做。
- /data 可以用现成的 `data-snapshot-android.sh backup`（toybox tar --selinux + zstd，不含 media 和 ota_package）。但它默认存到 `/data/gk3-snap/`，**清空 /data 时会一起没掉，必须导出到外接盘**。参考量：M4 的 A16 快照，约 40 GB 源数据压到 31 GB。
- 加上 media 42 GB 和 super 12 GiB，**外接盘建议 ≥128 GB**。
- 耗时没有实测。live 里能否用 USB 主机模式接外接盘、能跑到什么速度，都没验证过。CLAUDE.md 提到 adb 方向只有 high-speed，按 USB 2.0 估算实际约 30–40 MB/s，约 85 GB 要 40–50 分钟。这是估算，low。
- 恢复走 `data-snapshot-live.sh restore`，这条路径还没在真机 live 里跑过（a17-migration-plan.md §8 末段）。
- 出处：`.claude/worktrees/a17/scripts/data-snapshot-android.sh`、`data-snapshot-live.sh`；`.claude/worktrees/a17/docs/a17-migration-plan.md:133-160`；`docs/release-checklist.md:115`（B22：先备份到外接盘）。

**gk3boot 计数、回落与共享 misc 的交互（实验设计的一部分）：**

1. **方案 Ⅱ，替换后仍从 `_b` 启动。** 实机 `_b` 已标记成功，属于"已确认的槽"，不扣 tries。HyperOS 每次没开机完成，`ok_streak` 加 1；连续 5 次（`GK3_BOOTLOOP_THRESHOLD`）、且 `dispatch=1` 时，gk3boot 记 bootloop 事件并进 fastboot 执行端（`tools/gk3boot/README.md:1404-1407`，QEMU 场景 exec-bootloop 在 `:1465`）。到这一步，就能在 Mac 上用 `fastboot flash super <备份>` 刷回（本机 `/opt/homebrew/bin/fastboot` 在）。
   - 开机完成后的 bless 和清 streak，由 vendor 里的 boot_control HAL 做：`.claude/worktrees/a17/device/huawei/gaokun3/boot_control/main.cpp:69-70`、`Gk3Boot.cpp:17-20`。这部分在 vendor 里，换 system 后还在。它靠 `sys.boot_completed=1 → vendor.gaokun3.boot.done` 触发，HyperOS 起不到 boot_completed 就不会清。
2. **如果把 `_b` 设成"未成功、带 tries"**（例如走 set-active 的新槽路径）：tries 耗尽后会回落到 `_a`（`README.md:815、:838` 的 action-tries 场景）。实机 `_a` 不可启动（rc=70，low），按 README 的表应当走 noslot 进执行端（`README.md` 15.1 节的表）。如果 `_a` 可启动，super 里却没有 `_a` 分区，第一阶段挂载就会失败，init 主动 reboot；之后 `_a` 作为已确认槽累计 ok_streak，到 5 次进执行端。这两条推断都没有上机，low。
   - 另外，刷 super 后执行端会检查新 super 服务哪个槽：如果 active 槽不被服务，会自动切过去（`cmds.c:409-412`）。
3. **方案 Ⅲ：** HyperOS 走独立的直连条目，不经 gk3boot，所以不扣 BCAB 的 tries；失败后重启回到默认条目（gk3boot → crDroid `_b`）。也可以给这个条目加 systemd-boot 的 `+N` 计数，自动退回。
   - ⚠️ **misc 是共用的：** HyperOS 里如果发起"恢复出厂"、`reboot recovery/bootloader`，BCB 会写进同一个 misc。下次开机由 gk3boot 分派（`dispatch=1`），执行端擦的是**现役的 userdata 和 metadata**（`README.md` 15.1 节表格：wipe → 执行端）。
   - 另外，我们 vendor 里的 boot_control HAL 在 HyperOS 下也会读写 BCAB 和 GK3 记录；HyperOS 的 update_engine 也可能尝试 OTA。
   - 实验期间要做到三点：在 HyperOS 里禁用恢复出厂、update_engine；先人工确认 HAL 在没有 `ro.boot.gk3boot.entry` 时不写入；或者实验期间临时把 `gk3.dispatch` 关掉（要改 ESP 上的条目，要用户同意）。

**调试通道（阶段 1 之前必须先建好）：**
- **前提：** 这台机没有串口；Android init 失败是主动 `reboot()`，pstore 抓不到（CLAUDE.md「已知坑」）。HyperOS 是 user 构建：`ro.adb.secure=1`（`adb/daemon/main.cpp:226`），adb 默认关。
- **adb 密钥：** adbd 只认两处密钥：`/adb_keys`（只读分区）和 `/data/misc/adb/adb_keys`（`refs/a17/packages/modules/adb/docs/dev/keystore.md:17-28`，这是文档，不是代码，medium）。本机的 `/adb_keys` 是指向 `/product/etc/security/adb_keys` 的软链，换上 HyperOS 的 product 就没了，现在的发布构建里这个文件本来也不存在。/data 清空或换成新的 userdata，`/data/misc/adb/adb_keys` 和 `persist.sys.usb.config=adb`、`persist.adb.tcp.port` 也都没了。
- **可选方案**（都没验证，low）：
  - ① 往 HyperOS 的 product 镜像里注入 `etc/security/adb_keys`；再用 vendor init rc 或 product 属性让 adbd 默认启动。vendor_init 能不能设 `persist.sys.usb.config` 要查 sepolicy；user 版 adbd 对 `/adb_keys` 的实际行为要读 adbd 的 auth 源码。
  - ② 方案 Ⅲ 下，在新 userdata 第一次开机前离线写好 `/data/misc/adb/adb_keys` 和 `/data/property/persistent_properties`。注意持久属性的文件格式和 SELinux 标签。
  - ③ 照 `docs/stage2-findings.md` 第 5.2–5.3 节，把 ramdisk 改造成"包装 init、把 kmsg 和 logcat 写进内置 ESP"。这是唯一一条在本项目里实测有效、而且不依赖 adb 的办法。
  - ④ 失败后在 Ubuntu 救援里挂载 HyperOS 的 userdata，读 tombstones 和 dropbox。
- **结论：** 这条通道没建好之前，阶段 2 估的工作量不成立。退出条件里的"logcat 拿不到"要先用 ③ 兜底。

| 阶段 | 内容 | 判据 | 退出条件 | 预估工作量（撰稿人估计，无实测依据） |
|---|---|---|---|---|
| **0 离线评估**（不碰设备，完全可逆） | ① 从官方 CDN 按 Range 下载 piano OS4 CN 的 system、system_ext、product、mi_ext、mi_product、init_boot（只读，放进隔离目录）。<br>② 解包 erofs，核实：`mapping/202604.cil` 在不在；system 的 `ro.build.id`（核对是否真是 `.016`）；`ro.llndk.api_level`；/system/lib64 里有没有 ANGLE；有没有 `ro.hwui.use_vulkan`；erofs 压缩算法和块大小；abilist；init 的构建类型。<br>③ 用 secilc 把小米 plat/system_ext/product cil 和本机 vendor cil 一起编，包括 neverallow 检查。<br>④ 用 jadx 搜 services.jar、miui-services.jar 里的 `waitForService` + `vendor.xiaomi`。<br>⑤ 搜 device_features 和 cust_features 的路径。<br>⑥ 合计 system 一侧的大小。<br>**工具链：** 本机实测 secilc、lpmake、lpunpack、simg2img、mkfs.erofs、fsck.erofs、extract.erofs 都没装（`which` 全部 not found），只有 jadx 和 fastboot、adb。Homebrew 有 `erofs-utils` 1.9.4 和 `libsepol` 3.11 的 arm64 bottle，但**没有 secilc、lpmake、lpunpack 的 formula**（`brew info` 实测）。secilc 和 lp 工具要么在 macOS 上自己编（没验证），要么改在 Linux 环境里做：colima 容器、设备上的 Ubuntu 救援或 live，或者构建机的 host 工具（路径没核实）。 | secilc 编过；system_server 路径上没有对 vendor.xiaomi 的阻塞等待；容量可行 | secilc 编不过且补映射不现实，或者主线程有大量阻塞等待 ⇒ 停止，写成结论 | 3–6 人日（含搭 Linux 工具链）。如果要用构建机，另计机时 |
| **0′ 调试通道与落盘准备**（1.0 之后；改分区表要同意） | 建好上面的调试通道（至少 ③）；如果选方案 Ⅲ：在 live 或救援里建 `_hos` 三个分区，准备新 fstab，重打 boot 和 vendor | 先用现役 crDroid 的 vendor + system 从 `_hos` 分区起一次（验证 `force_super_partition` 和 `fstab_suffix` 的路径），ESP 日志能拿到 | 路径走不通 ⇒ 退回方案 Ⅱ，或者停止 | 2–4 人日 |
| **1 vendor 自洽基线**（需重启） | 先做"vendor 自洽化"的 crDroid 构建：c2 属性挪到 vendor，换上 tinymix 的 vendor 变体，并逐项列出 overlay、Parts、tree-fixes 缺失后的预期症状。这一步**需要构建机**，和"阶段 0–2 不用构建机"的原前提冲突，由用户取舍。自测过后再上 Google 官方 A17 GSI（`CP2A.260605.012`，和本机 `.016` 不同），合成成 `super_hos`（方案 Ⅲ）或整块 super（方案 Ⅱ，要先完成整机备份） | 进 AOSP 桌面；出现的问题都能归到"平台侧定制缺失"或"GSI 与 .016 的差异"之一 | 起不来、ESP 日志也定位不到 ⇒ 回到默认条目（方案 Ⅲ）或刷回备份（方案 Ⅱ），先修 vendor 自洽性，HyperOS 搁置 | 3–5 人日 + 构建机时间 |
| **2 HyperOS 首次启动**（需重启） | 用阶段 0 的分区合成：mi_ext 合并进 product 或 system_ext，或者新增分区；注入 ANGLE；强制 skiagl；注入 adb 密钥（如果走方案 ①）。**只能在 enforcing 下启动**（user 版 init，见第 2 节），除非决定把 system 里的 init 换成 userdebug 版 | system_server 起来、SurfaceFlinger 出图、能看到开机向导或桌面 | 第一个致命错误按"首个失败 → 最小补丁"处理；连续若干轮（建议 ≤5 轮）没有进展 ⇒ 停止并回滚 | 5–15 人日（前提是调试通道已建好） |
| **3 进桌面与能力门控** | 禁用查找设备的 Provider；Millet 探测加停服务；给 QTI perf、GNSS 等打失败分支补丁；device_features 平板配置；禁用恢复出厂和 update_engine（共享 misc）；身份属性方案（保留 gaokun3，还是伪装成小米代号 ⇒ 需要用户先定） | 能稳定冷启动进平板桌面；没有循环重启的服务 | 补丁量失控（每个新版本都要重做大部分）⇒ 停止 | 10–20 人日 |
| **4 体验评估** | Wi-Fi、音频、触摸、键盘、手写笔、刷新率、待机；模糊效果在 120Hz 下的帧时间和功耗；arm64 手游 | 和 crDroid 对比没有明显退化 | 体验明显不如 crDroid ⇒ 结束，保留报告 | 3–5 人日 |
| **旁支（与 HyperOS 无关）** | 把 mesa 57c69c99（以及它依赖的队列模拟基础设施）backport 到 26.1，或者升级到 26.2+，在 crDroid 上试 skiavk | HWUI skiavk 不中止、渲染正确 | — | 需要构建机，用户决定 |

**恢复路径：**
- **方案 Ⅲ：** 重启回默认条目，或者删掉 `_hos` 条目。
- **方案 Ⅱ：** 进 fastboot 执行端，可以是 bootloop 自动进，也可以从开机菜单选 `Android fastboot / boot menu`。在 Mac 上 `fastboot flash super <备份>`（支持 sparse），再在 live 里恢复 /data。也可以用 ESP 上的 gaokun3 installer 或 Ubuntu 救援。
- super.img 是 sparse 格式，不能直接 dd，要先 simg2img（live 里的后端用 gk3-unsparse.py，`scripts/live/pkgs-common.txt:53-54`）。

---

## 7. 法律 / 分发 / GMS / 与项目游戏环境原则的冲突

**许可：《小米操作系统用户协议》**（terms.miui.com，10-26 抓取原页核对）
- 第 15.2 条写的是"本协议更新时间为2021年12月3日"，但第 1.1 条已经写了"Xiaomi HyperOS系统"，而 HyperOS 是 2023 年才发布的。⇒ **页面内容改过，标注的更新时间没跟着改**；引的是现行页面，不是 2021 年的旧版。英文版第 1.1 条没提 HyperOS。
- 第 2.2 条：只许在小米设备上非商业使用；商业性复制、散发须书面授权。
- 第 2.3 条原文："未经小米许可，用户不得将本软件安装在未经小米明示许可使用本软件及服务的其他终端设备上。"
- 第 6.1.2 条：禁止反向工程。
- 第 6.1.6 条：禁止未经书面同意"复制、修改……建立镜像站点、擅自借助本软件发展与之有关的衍生产品……兼容、互联等"。
- 适用中国法律。

**维权先例：**
- 小米在 GitHub 上发过 DMCA，但对象是公开托管的反编译源码。
- ANXCamera 是在小米机型之间移植，而且只下架了源码目录。
- github/dmca 里**没有**针对 HyperOS 移植或 GSI 的通知。toraidl/hyperos_port 仓库甚至直接带着 MiuiCamera.apk，至今没被下架。
- 这只说明小米的执法有选择性，**不能当作安全的依据**。

**项目处境：** 本仓库是公开的 GPLv3 仓库，有公开 Release、R2 和 OTA。现在已经在分发 MindTheGapps（D7），但那是社区长期默许的惯例；HyperOS 是整套闭源系统，风险量级不同。
- 结论：**任何含 HyperOS 的镜像都不进 Release、R2 或 OTA。**
- "只发脚本、本地合成"：live 环境缺 erofs-utils、Java/baksmali、lpmake；每次升级都要重新合成，OTA 链作废；用户本人仍然违反第 2.3 条。
- 移植脚本里"绕签名校验"一类补丁，可能落入《著作权法》第 49 条（禁止提供规避技术措施的工具）。条文原文没抓取成功，是否适用要律师判断，low。

**GMS**
- 中国版 HyperOS 没有 GMS，和 D7（只发 GApps 版）的定位相反。
- 国际版的 GMS 问题和现在的 crDroid 一样：可以靠用户登记 Android ID 解决"未认证"（TODO T2，`docs/TODO.md:147、:344`）。
- Play Integrity 过不了，是因为 KeyMint 是软件实现（`device.mk:115`）加上 UEFI 解锁。换 HyperOS 不会更好，也不会更差。

**游戏环境原则**（`docs/gaming.md:79-82` 的 A/B/C/D 四类）
- 伪造证明（B 类）和隐蔽工程（C 类）都不做，HyperOS 路线必须遵守。
- 开发机上 `ro.boot.verifiedbootstate` 读到 green、cmdline 却是 orange（第 2 节末尾），很可能是用户自装模块在运行时改的。HyperOS 实验里不要依赖这个值，也不要在移植脚本里复制这种改法。
- 真正的冲突点有三个：
  1. 小米自家服务（账号、云服务）可能需要伪装成小米机型（是否校验机型或证明**未核实**）。gaming.md:133 里"伪装机型交给用户决定"那一条，原本是针对原神的渲染上限，这里只能类比，同样应该交给用户定。
  2. 把小米的 platform 签名块移植到 Gaokun3Parts 上，属于灰色手段。
  3. 社区移植普遍要打"isBuildConsistent 恒真"和"绕签名校验"补丁，和项目"如实修正"的方向不一致。

---

## 8. 未决问题与需要用户定的决定

**需要用户定的：**
1. **做不做 HyperOS 探索？** 推荐不做交付，最多做阶段 0（离线，零风险），根据结果再决定要不要继续。
2. **排期：** 阶段 0′ 及以后会占用唯一的开发机。建议放到 1.0（A17）发版之后，不和 M5、M6 以及 rc 验收抢机器。同意吗？
3. **落盘方案：** 选方案 Ⅲ（在 63.9 GiB 空闲空间里另建 `_hos` 分区，要改 GPT），还是方案 Ⅱ（整机备份到 ≥128 GB 的外接盘后替换 super、清空 /data，会失去 TrickyStore、LSPosed 等测试环境，要重跑 1.0 验收）？每次重启都要有人在场。
4. **构建机：** 阶段 1 的"vendor 自洽化" crDroid 构建要用构建机；阶段 0 的 secilc 和 lp 工具也可能要借构建机的 host 工具。是否接受这部分机时？
5. **身份方案：** `ro.product.device` 保留 gaokun3、自己写平板配置，还是伪装成小米代号？后者会影响我们 vendor 和 OTA 里依赖代号的逻辑，也牵涉游戏环境原则。
6. **"只发脚本、本地合成"这个定位接不接受？**（推荐不接受。）要不要向小米申请书面授权？（成功概率很低。）
7. **路线 C/D 要不要排进 1.0 之后的计划？** 也就是桌面模式改成"接键盘时启用"，再加一套小米风格的主题和启动器。
8. **mesa skiavk backport（旁支）** 要不要单独立项？需要构建机。
9. **CLAUDE.md 修订：** "KVM/EL2 可用"要不要改成"硬件和社区层面可行，现行启动链没走 EL2，没有 /dev/kvm"？另外，「现在设备上跑的是什么」里 10-18 的回落记录已经过时（10-26 实机：A17 `1792975257` 正常从 `_b` 启动），要不要更新？

**技术上未决的（多数阶段 0 能回答）：**
- HyperOS 4 的 system 是否带 `mapping/202604.cil` 和 genfs cil？在 enforcing 前提下，secilc 离线编译（含 neverallow）能不能过？system 的构建号是否真是 `.016`？
- donor 的 system 里有没有 ANGLE、有没有设 `ro.hwui.use_vulkan`，erofs 的压缩算法和块大小，abilist。
- system_server 主线程上对 vendor.xiaomi.* 的阻塞等待有多少？
- OS4 的 device_features / cust_features 的实际路径和键名。
- 我们的 A17 第一阶段 init 挂载完之后 exec 小米 OS4 的 `/system/bin/init selinux_setup`，两边的接口是否兼容（OS4 的 init_boot 已归到系统侧）？
- `androidboot.force_super_partition` 和 `androidboot.fstab_suffix` 在 A17 树里的实现，以及在本机上能否让第二套 super 起来（方案 Ⅲ）。
- 我们的 boot_control HAL 在没有 `ro.boot.gk3boot.entry` 的直连条目下是否完全不写 misc；在共享 misc 下，怎样防止 HyperOS 触发现役 userdata 的擦除。
- user 版 adbd 对 `/adb_keys` 的实际读取逻辑；vendor 一侧能否让 adbd 默认启动。
- secilc 和 lpmake 能否在 macOS 上编出来，还是只能在 Linux 上做。
- 现役 super 里 `*_b-cow` 在 `Update state: none` 下仍然残留的原因。
- Greeze 失效后会不会退回 AOSP freezer；/data 不加密对账号、查找设备、隐私空间有没有硬依赖。
- 小米的键盘、手写笔、工作台特性是否依赖 vendor.xiaomi 私有 HAL；我们的标准 input 能不能被识别。
- 模糊效果在 ANGLE→turnip、1600x2560@120Hz 下的帧时间和正确性（只能上机测）。

---

## 9. 出处列表

**本地（仓库 / refs / 实机）**
- `.claude/worktrees/a17/device/huawei/gaokun3/BoardConfig.mk:95-106、:131、:135、:171-175、:184-188、:222-227、:308-315`；A16 树 `device/huawei/gaokun3/BoardConfig.mk:164-166`
- `.claude/worktrees/a17/device/huawei/gaokun3/fstab.gaokun3:3-6、:20-23`；`device.mk:16-18、:106-116、:134-142、:288-304、:339-340、:350-351、:738`
- `.claude/worktrees/a17/device/huawei/gaokun3/lineage_gaokun3.mk:163-164、:239-251、:319、:328、:355`
- `.claude/worktrees/a17/device/huawei/gaokun3/boot_control/main.cpp:69-70`；`boot_control/Gk3Boot.cpp:10-25`
- `.claude/worktrees/a17/device/huawei/gaokun3/manifest.xml:44`；`compatibility_matrix.xml:27-28`；`lights/Lights.cpp:135`；`camera/Provider.cpp:100-104`；`parts/Android.bp:6-28`；`parts/AndroidManifest.xml:19-24`；`sepolicy/gaokun3_scripts.te:184-196`；`bin/audio-route.sh:29-34`；`overlay/frameworks/base/core/res/res/values/config.xml:63-123`；`rro/Gaokun3SettingsProviderOverlay/Android.bp:25`
- `.claude/worktrees/a17/scripts/crdroid-tree-fixes.py:429-470、:739-791、:1112-1150`；`.claude/worktrees/a17/scripts/kernel-config-android.sh`；`.claude/worktrees/a17/scripts/data-snapshot-android.sh`、`data-snapshot-live.sh`；`.claude/worktrees/a17/docs/mesa-a17.md:23、:136`；`.claude/worktrees/a17/docs/a17-migration-plan.md:112-160`
- `tools/gk3boot/fastbootd/cmds.c:358、:364-418、:880-886`；`tools/gk3boot/fastbootd/vars.c:131-136`；`tools/gk3boot/fastbootd/sparse.c`；`tools/gk3boot/README.md:747-748、:815、:838-840、:1395-1411、:1465`；`docs/fastboot-design.md:32、:354`
- `refs/a17/system/core/init/first_stage_init.cpp:571-572`；`refs/a17/system/core/init/selinux.cpp:116-121、:274-282、:335-342、:351-358`；`refs/a17/system/core/init/Android.bp:124、:141-148`；`refs/a17/system/core/init/first_stage_mount.cpp:143、:398`；`refs/lineage-system-core/fs_mgr/fs_mgr.cpp:2257-2266`；`refs/lineage-system-core/fs_mgr/libfstab/fstab.cpp:548-558`；`refs/lineage-system-core/init/selinux.cpp:277`；`refs/lineage-sepolicy/Android.bp:121-134`、`private/compat/`；`refs/a17/system/sepolicy/private/compat/`
- `refs/a17/packages/modules/adb/daemon/main.cpp:226`；`refs/a17/packages/modules/adb/docs/dev/keystore.md:17-28`
- `refs/a17/build/make/core/config.mk:911-913`；`board_config.mk:1040-1046`；`main.mk:1007`；`product.mk:337-342`；`package_internal.mk:113-152`；`target/product/base_system_ext.mk:29-33`；`target/product/base_system.mk:439-440`；`target/product/generic_system.mk:117`；`refs/a17/build/make/core/build_id.mk:21`；`refs/a17/build/release/flag_values/cp2a/RELEASE_BOARD_API_LEVEL.textproto`
- `refs/a17/hardware/interfaces/compatibility_matrices/compatibility_matrix.202504.xml`、`.202604.xml`；`refs/a17/hardware/interfaces/audio/aidl/default/Stream.cpp:933-937`；`refs/a17/external/mesa3d/VERSION`
- `docs/stage2-findings.md` 第 5 节（5.2–5.3）；`docs/stage5-freedreno.md:433-510、:495`；`docs/TODO.md:147、:307、:344`；`docs/gaming.md:79-82、:99、:133、:135`；`docs/release-checklist.md:115`；`docs/hw-inventory.md:594-606`；`docs/v1.0-plan.md:319`；`docs/stage0-findings.md:147-158、:310-320`；`docs/stage4-findings.md:3093、:8776、:8898、:8966`；`docs/stage6-crdroid.md:310、:745、:859`；`README.md:338-344`；`scripts/live/pkgs-common.txt:53-57`；`scripts/live/installer-lib.sh:1521`；`patches/0004-*`、`patches/0005-*`
- `refs/gaokun-buildbot/docs/el2_kvm_guide_zh.md:13-17、:40`；`refs/gaokun-buildbot/tools/el2/README.md:1-15`
- 实机只读查询（2026-10-26，两轮）：getprop（ro.build.id、ro.build.date.utc、ro.build.type、ro.boot.verifiedbootstate、ro.adb.secure、persist.sys.usb.config、ro.board.api_level、ro.llndk.api_level、ro.vendor.api_level、debug.hwui.renderer 等）、`/proc/cmdline`、`/proc/bootconfig`、`bootctl get-active-boot-slot / get-current-slot / is-slot-bootable / is-slot-marked-successful`、`lpdump`、`snapshotctl dump`、`/dev/block/by-name/`、`/proc/partitions`、`/sys/block/nvme0n1/nvme0n1p*/{start,size}`、`ls -l /adb_keys`、`du /data/media /data/gk3-snap`、`/vendor/etc/selinux/*`、`/system/etc/selinux/mapping/`、`/vendor/etc/vintf/`、lshal、service list、`/proc/config.gz`、`/proc/filesystems`、dumpsys SurfaceFlinger / lights / display
- 本机工具实测（2026-10-26）：`which secilc lpmake lpunpack simg2img mkfs.erofs fsck.erofs extract.erofs`（全部 not found），`jadx`、`fastboot`、`adb` 在；`brew info erofs-utils libsepol secilc lpunpack`

**AOSP / 上游**
- https://source.android.com/docs/core/architecture/api-flags
- https://developer.android.com/topic/generic-system-image
- https://developer.android.com/about/versions/17/gsi-release-notes（A17 GSI `CP2A.260605.012`，2026-06-18）
- https://android.googlesource.com/platform/system/libhidl/+/refs/heads/main/transport/ServiceManagement.cpp
- frameworks/native `opengl/libs/EGL/Loader.cpp:504-521`（main）；frameworks/base `libs/hwui/renderthread/VulkanManager.cpp:153-155、:245-253`、`libs/hwui/Properties.cpp:245-246`、`Properties.h:138`、`HWUIProperties.sysprop:4-6`；system/core `init/property_service.cpp:1203-1225`（main）
- mesa main `src/freedreno/vulkan/tu_device.cc:1676-1686、:2104-2105`、`00-turnip-defaults.conf:65-69`；https://gitlab.freedesktop.org/mesa/mesa/-/commit/57c69c991233896912739f22d41c5180571837ca
- https://developer.android.com/google/play/integrity/verdicts
- https://source.android.com/docs/core/virtualization ；https://source.android.com/docs/core/virtualization/microdroid

**社区项目（二手）**
- https://github.com/NEORUAA/HyperOS-AVD（README、docs/hyperos4.md、hyperos4-pad.md、compatibility.md、os4-patches.md、scripts/*）
- https://github.com/toraidl/hyperos_port（port.sh、bin/port_config）；https://github.com/toraidl/HyperOS-Port-Python（已归档）
- https://github.com/w111user/hyperos-gsi-samsung-galaxy-a035f ；https://sourceforge.net/projects/hyperos-gsi-sm-a035f/
- https://github.com/yangFenTuoZi/OnePlus15-Port-Script
- https://sourceforge.net/projects/mystic-gsi-updates/files/HyperOS/
- https://github.com/lolipuru/device_xiaomi_sm8650-common（proprietary-files.txt）
- https://github.com/xiaoxindada/SGSI-build-tool（MIUI 时代）
- https://github.com/Fklearn/XiaomiFramework（MIUI 12 的 FeatureParser）
- https://github.com/ReChronoRain/HyperCeiler ；https://github.com/Enki013/hyperosunfcker
- https://github.com/OrbitingX/DroidDeckGpuFix ；https://github.com/Androidzu/HyperOS-GSIs
- 社区内核树（Millet）：Rohail33/Realking_xiaomi_xaga、Rohail33/Realking_kernel_sm8250

**版本与机型（多为二手新闻或第三方索引）**
- https://miuirom.org/updates/hyperos-4
- https://www.notebookcheck.net/Android-17-based-HyperOS-4-beta-rollout-begins-for-eight-Xiaomi-devices.1368569.0.html
- https://borncity.com/news/hyperos-4-xiaomi-startet-beta-mit-android-17-fuer-26-geraete/ ；https://borncity.com/news/xiaomi-startet-hyperos-3-3-beta-2-auf-android-17-basis/
- https://www.androidauthority.com/stable-android-17-xiaomi-17-ultra-3687634/
- https://www.gizbot.com/mobile/news/hyperos-4-based-android-17-rolling-out-globally-starting-xiaomi-17-series-128499.html
- http://xmfirmwareupdater.com/hyperos/piano/ ；http://xmfirmwareupdater.com/hyperos/organ/ ；http://xmfirmwareupdater.com/hyperos/yudi/
- https://www.mi.com/global/product/xiaomi-pad-8-pro/（及 /specs/）；https://www.mi.com/my/product/redmi-pad-2-pro/specs/
- https://wiki.postmarketos.org/wiki/Qualcomm_Snapdragon_7s_Gen_3/7s_Gen_4/6_Gen_4_(Milos)
- https://ximitime.com/?p=3994 ；XDA 帖子（只看到搜索摘要，正文 403，未核实）

**法律**
- https://terms.miui.com/doc/eula/zh_CN.html（第 1.1、2.2、2.3、6.1.2、6.1.6、14.1–14.2、15.2 条；10-26 抓取核对）
- github/dmca：2023/06/2023-06-30-xiaomi-camera.md 等
- https://www.ncac.gov.cn/xxfb/flfg/flfg_532/202103/t20210309_50530.html（《著作权法》第 49 条，原文没抓取成功）
- https://sspai.com/post/28840（2015 年的 Patchrom，历史参考）

---

## 评审处理记录

1. **备份手段：采纳。** 核实 `tools/gk3boot/fastbootd/cmds.c:883-884` 对 fetch/upload 回 "%s is not supported"；`docs/release-checklist.md:115`（B22）要求先备份到外接盘。
   - 第 2 节"刷写路径"和第 6 节都改了，并补上现成的 `data-snapshot-android.sh` / `-live.sh`（`.claude/worktrees/a17/docs/a17-migration-plan.md:133-160`）。
   - 也指出这个脚本默认存在 /data 上，清 /data 时会一起没掉。
   - 用实机数据估了容量：/data 已用 113 GB，media 42 GB，A16 快照 31 GB，建议外接盘 ≥128 GB。耗时标为估算（low）。
2. **permissive 不可行：采纳。** 核实三处：`refs/a17/system/core/init/first_stage_init.cpp:571-572`（exec `/system/bin/init selinux_setup`）、`selinux.cpp:116-121`（`IsEnforcing` 在 `ALLOW_PERMISSIVE_SELINUX` 为 0 时恒为 true）、`init/Android.bp:124、:141-148`（只有 debuggable 构建才置 1）。
   - 第 2 节加了"SELinux 模式 ❌"一行，把"boot 结构"一行改成"SELinux 初始化在 system 侧的 init 里"。
   - 删掉阶段 2 的 permissive 观察；把 secilc 离线编译改成硬门槛（第 1、3、6 节）。
   - 补了一个 low 的绕法：换掉 system 里的 init。
3. **调试通道：采纳。** 实机读到 `/adb_keys -> /product/etc/security/adb_keys`（目标文件不存在）、`ro.adb.secure=1`、`persist.sys.usb.config=adb`。adbd 只认 `/adb_keys` 和 `/data/misc/adb/adb_keys`，见 `refs/a17/packages/modules/adb/docs/dev/keystore.md:17-28`（文档，medium），`ro.adb.secure` 见 `daemon/main.cpp:226`。
   - 第 6 节新增"调试通道"小节，给出四个方案，其中 ESP 写日志引用 `docs/stage2-findings.md` 第 5.2–5.3 节。
   - 第 3 节加了第 22 项；阶段 2 的工作量改成以通道建好为前提。
   - user 版 adbd 的密钥读取逻辑标为未核实。
4. **开发机现状：采纳。** 10-26 实机复核结果和评审一致：`ro.build.id=CP2A.260605.016`、`ro.build.date.utc=1792975257`、cmdline `gk3boot.event=none streak=1 dispatch=1`、`get-active-boot-slot=1`。另外读到 `ro.build.type=user`；`_a` 的 `is-slot-bootable` 返回 rc=70。第 6 节的"开发机现状"按实机重写；第 8 节第 9 条建议更新 CLAUDE.md。
5. **和 1.0 主线的冲突：采纳。** 核实 `a17-migration-plan.md:112-126`：M4 在 10-26 跑完 A 档，M5、M6 还没做。第 1、6、8 节明确写了排期冲突、对游戏测试环境的影响，并建议放到 1.0 发版之后。
6. **另建独立 super：采纳，并补上实测。**
   - 实机 `/sys/block` 显示 p6 和 p10 之间有约 63.9 GiB 未分配空间；hw-inventory 8quinquies 记的 p9 `userdata-old` 已经不在了。
   - 源码有两个现成的 bootconfig：`androidboot.force_super_partition`（`refs/lineage-system-core/fs_mgr/fs_mgr.cpp:2257-2266`）和 `androidboot.fstab_suffix`（`libfstab/fstab.cpp:548-558`）。fstab 按 PARTLABEL 引用（`fstab.gaokun3:3-6`）。
   - 写成第 6 节的方案 Ⅲ（推荐，low），也写出了它的风险，尤其是共享 misc 下 HyperOS 触发的 wipe 会擦掉现役 userdata。
   - 顺带纠正：评审说"fstab 用 PARTUUID"，但这份 fstab 的 super、userdata、metadata 实际按 by-name（PARTLABEL）引用，这三个分区的标签是项目自己设的。
7. **gk3boot 计数和回落：采纳。** 核实 `boot_control/main.cpp:69-70`、`Gk3Boot.cpp:17-20`（开机完成后 bless、清 streak），`README.md:815、:838`（tries 耗尽后回落），`README.md:1404-1407`（已确认槽 `ok_streak` ≥5 进执行端，exec-bootloop 在 `:1465`），`cmds.c:409-412`（刷 super 后自动切槽）。第 6 节新增"gk3boot 计数、回落与共享 misc 的交互"，分三种情形写了回滚流程。`_a` 不可启动时走 noslot 这一推断标为 low。
8. **Mac 工具链：采纳。** 本机 `which` 实测，七个工具全部 not found，只有 jadx、fastboot、adb。`brew info` 显示 erofs-utils 和 libsepol 有 arm64 bottle，但没有 secilc、lpunpack 的 formula。阶段 0 补了工具链说明和替代环境，工作量从 2–4 人日改为 3–6 人日；也撤掉了"阶段 0–2 不需要构建机"的总前提。
9. **super 现状解读：采纳，并补上实测。**
   - lpdump 显示 4 个 `*_b-cow` 合计 6,752,656 扇区，约 3.22 GiB，确实占用空间；super 只剩约 5.57 GiB；`snapshotctl dump` 显示 `Update state: none`。
   - post_super（`cmds.c:364-418`）只读 LP 元数据、检查 VAB 状态和切槽，不专门清理 cow；刷整块 super 时，镜像里的元数据会把它整体覆盖。
   - 第 2 节容量一行重写，结论是"不可能和 `_b` 并存于现役 super"。残留原因标为未查。
10. **"逐字相同"：采纳。** HyperOS 一侧的 `.016` 改标为单一来源、low；"阻力最小"降成"待阶段 0 核实"（第 1、5.1 节）。抓取 developer.android.com 的 GSI 发布说明，确认 A17 GSI 是 `CP2A.260605.012`（2026-06-18），已写进第 5.1 节和阶段 1。
11. **没核实却写成事实：采纳。** composer 改成 ◐（二手，low）；Mystic 日期、piano 包的大小和日期、W/X 字母规则、"A17 system 配 A15 vendor"都标为二手或 low；图例下面加了"二手不标 ✅"；5.1 矩阵里原来的 ✅ 改成 ◐。
12. **法律一节的小矛盾：采纳。** 重新抓取原页核对：第 1.1 条确实含"Xiaomi HyperOS系统"，第 15.2 条写的是"更新时间为2021年12月3日"，第 2.3 条和第 6.1.6 条与原文一致。正文已注明页面内容改过、更新日期没跟着改，并引了第 2.3 条原文。
13. **阶段 1 判据偏乐观：采纳。** 阶段 1 改成先做"vendor 自洽化"的 crDroid 构建（c2 属性挪到 vendor、换上 tinymix 的 vendor 变体），再上 GSI。注明这一步需要构建机、和原前提冲突，交用户取舍（第 8 节第 4 条）；判据也改成"问题能归因"。
14. **verifiedbootstate 可以当场回答：采纳。** 实机核实：cmdline 是 orange，`/proc/bootconfig` 里没有这一项，getprop 读到 green。从第 2 节表格和第 8 节的技术未决项里拿掉了；改为第 2 节末尾的说明，原因写成"很可能是用户自装模块在运行时改的"（推测，low）；在第 7 节点明了它和"不伪造证明"原则的关系。