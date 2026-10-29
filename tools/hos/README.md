# HyperOS 离线适配工具

仅处理本地文件。镜像、策略、专有文件和执行结果放 `out/`，不提交、不发布。
当前进展与未完成项见 [适配执行记录](../../docs/hyperos-adaptation-progress.md)。

## SELinux：分开验证运行时编译与 neverallow

```sh
python3 tools/hos/check-sepolicy.py \
  --root out/hyperos-r2/probe-files/hos-selinux-root \
  --vendor out/hyperos-r2/probe-files/gaokun3-ref/vendor_selinux \
  --secilc out/hyperos-r2/probe-tools/bin/secilc \
  --out out/hyperos-r3/sepolicy-user
```

加 `--userdebug` 选择 donor 的 `system_ext/etc/selinux/userdebug_plat_sepolicy.cil`。
输出目录必须不存在。每次输出 `runtime.log`、`neverallow.log`、`report.json`，成功的编译另输出
对应 `.policy`；JSON 留存输入文件、工具哈希及实际 argv。退出码只有两次编译均通过才为 0。
R3 原始输入为 runtime PASS、neverallow FAIL；R4 修复组合的两项均应 PASS，见下文。

运行时模式按 A17 init 使用 `-N`；neverallow 模式去掉 `-N`。两者成功都不能证明设备启动、
上下文标签、HAL 行为已通过验证。默认 policy version 30 来自原始实测；另有 `--policy-version`
供后续与目标 init/kernel 核对后使用。工具要求显式提供 genfs version 文件，避免静默猜测。

## 生成隔离 ramdisk 原型

先用现有工具从本地全量 payload 解出并校验 boot，再拆出 ramdisk：

```sh
mkdir -p out/hyperos-r3
python3 tools/hos/payload-extract.py out/a17-1792983426/payload.bin out/hyperos-r3 boot
python3 scripts/live/gk3-bootimg.py out/hyperos-r3/boot.img out/hyperos-r3/boot-1792983426
python3 tools/hos/prepare-ramdisk.py \
  --input out/hyperos-r3/boot-1792983426/ramdisk.img \
  --out out/hyperos-r3/ramdisk-1792983426-erofs --system-fs erofs
```

`--system-fs ext4` 供原系统独立 super 基线实验，`erofs` 供 donor system/system_ext/product；
vendor 保持 ext4。输入支持 gzip 和裸 newc；直接处理归档，保留 uid/gid、mode、符号链接、
硬链接字段及设备号，避免 APFS 提取造成元数据丢失。拒绝路径穿越、重复路径、截断归档及
尾部非零数据；输出目录必须不存在。

修改仅包括：

- 原位置的 `fstab.gaokun3`：metadata/userdata/misc 全部引用 `_hos`；保留 logical 与 slotselect。
- 根目录 `force_debuggable` 和 `adb_debug.prop`，包含 `debug.gk3.dbgramdisk=1` 观察标记。

输出 `ramdisk.cpio.gz`、实验 vendor 必须同步使用的 `fstab.gaokun3`、所需额外参数片段
`cmdline.required.txt`、`manifest.json`。参数片段需追加到完整命令行，不可替换原命令行。
R4 已制作配套隔离 HAL 和禁用 OTA 的镜像；完整 HyperOS 尚未处理 mi_ext/mi_product，
不能直接用于启动。`force_debuggable` 不会把 HyperOS user init 变成 userdebug 可执行文件，
不会让它响应全局 permissive 参数；ADB、root 及持久化后的 USB 行为均待上机验证。

## 验证

```sh
python3 -m unittest discover -s tools/hos -p 'test_*.py'
```

当前 8 项测试覆盖归档元数据/软链接往返、非法归档、物理分区隔离、CIL 匿名属性修复及
neverallow 保留、HAL 前置守卫和拒绝 OTA 写入。HAL 测试以 host C++ stub + ASan/UBSan 运行，
不替代目标设备验收。硬件准入仍以执行记录中的未完成项为准。


## R4 严格策略修复

```sh
python3 tools/hos/prepare-debug-policy.py \
  --root out/hyperos-r2/probe-files/hos-selinux-root \
  --out out/hyperos-r4/debug-policy
python3 tools/hos/check-sepolicy.py \
  --root out/hyperos-r4/debug-policy/root \
  --vendor out/hyperos-r3/device-20261026-203434/vendor_selinux \
  --secilc out/hyperos-r2/probe-tools/bin/secilc \
  --out out/hyperos-r4/debug-policy-check --userdebug
```

输出目录均须不存在，已有 R4 产物请换新目录复现。修复隔离 debug/user 匿名属性编号，
导入原 user 匿名定义供 split CIL 引用，删除唯一一条 llkd 广域 ptrace allow；
5,217 条 neverallow 保留，没有添加 permissive。配套 ramdisk 禁用 llkd。
`check-sepolicy.py` 的 runtime 与 neverallow 两次检查均应通过。

## R4 独立模块和镜像工具

`prepare-boot-hal.py --source device/huawei/gaokun3/boot_control --tinymix-source <A17 tinymix.c>
--out <新目录>` 生成隔离模块，按源码 manifest 留存输入；只同步该生成目录到构建机的
独立 vendor 项目，使用项目的正常 A17 userdebug lunch。模块名分别为
`android.hardware.boot-service.gaokun3_hos`、`tinymix_hos_vendor`，安装 stem 也与生产模块不同。
生产 device tree 不变，实际编译结果放本地后再传给 vendor 工具。

```sh
docker build -t hos-tools:alpine3.24 tools/hos
docker run --rm -v "$PWD:/work" -w /work hos-tools:alpine3.24 \
  python3 tools/hos/prepare-vendor.py \
  --image out/hyperos-r4/vendor.img \
  --fstab out/hyperos-r4/ramdisk-ext4/fstab.gaokun3 \
  --boot-hal out/hyperos-r4/binaries/boot-hal \
  --tinymix out/hyperos-r4/binaries/tinymix \
  --out out/hyperos-r4/vendor-final-ext4
```

EROFS 路线换用 `ramdisk-erofs/fstab.gaokun3` 及新输出目录。vendor 始终为 ext4。
工具在副本上改写、回读并检查文件系统；同时导出最终 CIL 和上下文文件，须核对它们与实际
策略检查输入相同。最终 vendor 移除原 precompiled policy 与三份 hash marker，避免旧缓存
绕过新增 tinymix 类型。生成 manifest 的 remaining 列表是制作时的后续门槛，跨产物完成情况
以执行记录及最终审计为准。

`repack-erofs.py` 在 Linux 容器内临时目录提取并保留 xattr，避免在 APFS 中间落地树。
此工具需要 `docker run --privileged` 以保留 security xattr，只接受镜像文件。
单项替换使用 `--replace <镜像内相对路径> --with-file <本地文件>`，多项使用
`--plan <JSON>`（对象键为镜像内相对路径，值为容器内可访问的替换文件路径）。共同参数为
`--image <输入镜像> --out <新目录>`。输出 LZ4HC EROFS，并将全部条目的内容、mode、uid/gid、
符号链接、提取出的 xattr 与预期树比较；不核验时间戳及硬链接拓扑。

`disable-baseline-ota.py --image <原始 A17 system ext4> --out <新目录>` 生成基线 system，
禁用原 updater/verifier rc；HyperOS system 的同名 rc 使用 EROFS plan 整份替换成注释。

```sh
docker run --rm -v "$PWD:/work" -w /work hos-tools:alpine3.24 \
  python3 tools/hos/make-baseline-super.py \
  --system out/hyperos-r4/baseline-system/system.img \
  --system-ext out/hyperos-r4/system_ext.img \
  --product out/hyperos-r4/product.img \
  --vendor out/hyperos-r4/vendor-final-ext4/vendor.img \
  --out out/hyperos-r4/baseline-super-final
```

此工具仅合成原 A17 ext4 基线：12 GiB raw super_hos、仅逻辑槽 `_a`，lpunpack 后比较全部
四分区的大小和哈希。尚不生成完整 HyperOS super，也不执行 GPT、刷写、挂载设备或重启。
`vendor-ext4`、`vendor-erofs`、`baseline-super`、`baseline-super-no-ota` 是旧过程产物，
不要混入最终组合；当前只使用 `vendor-final-*` 和 `baseline-super-final`。


## R5 更新：隔离块设备上下文

当前 `prepare-vendor.py` 还会从原四条精确 by-name 块设备规则派生 `_hos` 规则。
这项修复不改变 CIL，但对 enforcing 下的块设备访问必要。现为 11 项测试。
上面的 R4 命令保留作历史复现；当前应将 vendor 输出改为 `out/hyperos-r5/vendor-ext4`
或 `vendor-erofs`，baseline super 使用 R5 ext4 vendor，输出到 `out/hyperos-r5/baseline-super`。
**R4 vendor-final-* 与 baseline-super-final 已过时**，完整设备检查、备份范围及剩余门槛见执行记录。


## R6 早期日志与安装包

`early-log.c` 在 Ubuntu 原生 `cc -static` 构建，必须用 `-DHOS_MISC_UUID='"<R6 misc GUID>"'`
绑定固定计划。生产构建不可定义 `HOS_NAMESPACE_TEST`；该宏仅供独立 loop/namespace 演练。
构建记录在 R6 logger-build.json。使用前还须通过严格策略检查，vendor 使用
`prepare-vendor.py --early-log-policy`，不能混入未包含日志权限的 R5 vendor。

```sh
python3 tools/hos/prepare-logged-ramdisk.py \
  --input out/hyperos-r4/ramdisk-ext4/ramdisk.cpio.gz \
  --logger out/hyperos-r6/early-log-production \
  --plan out/hyperos-r6/partition-plan.json --out <新目录>
python3 tools/hos/read-early-log.py --input <完整4MiB-misc_hos普通文件副本> --out <新目录>
```

日志不会触及 misc_hos 前 64 KiB；超过容量时回绕，默认最多 180 秒。头 CRC 不保证断电时
数据尾部完整。PID 1 流程已在救援系统 namespace + 临时 loop 镜像演练通过；Android enforcing
下的实际记录仍需验收。`test-early-log-namespace.sh` 需要 Linux root 和 cc/sgdisk/losetup/unshare，
只修改其创建的临时 loop 磁盘；参数为源码、固定 UUID 和 decoder 路径。

`install-baseline.py --bundle <目录> --disk <磁盘>` 默认只读，核对 GPT 及资产。
`--rehearse-gpt` 只对普通镜像应用分区计划，无格式化。真实写入需要显式 `--apply`，应在用户
批准后由救援系统执行；它不重启、不设置一次性/default。不要直接重跑部分失败的安装。
当前可审阅包为 `out/hyperos-r6/install-bundle`，设备暂存位置与证据见执行记录。
单元测试在 Mac 上会跳过 3 项原生 Linux 测试，须在 Ubuntu 补跑 `test_early_log.py`。

## R7：HyperOS 扩展分区与六分区 super

```sh
python3 tools/hos/prepare-ramdisk.py \
  --input out/hyperos-r3/boot-1792983426/ramdisk.img --system-fs erofs \
  --donor-fstab out/hyperos-r2/probe-files/vendor_boot/fstab.qcom \
  --out <新目录>
```

`--donor-fstab` 保留 mi_ext/mi_product 为独立 EROFS 逻辑分区，挂载到
`/mnt/vendor/mi_ext`、`/mnt/vendor/mi_product`，供 donor init 读取原有属性和 rc。
仅导入 donor 的只读 overlay 行，保留 lowerdir 顺序，不导入 donor 的物理设备、AVB 或格式化
参数。overlay 显式补 `first_stage_mount`，否则会被第一阶段 fstab 筛掉。当前 donor 共 22 行。
扩展分区本身挂载失败应停止启动；overlay 仍是 nofail，且 first-stage init 忽略其挂载返回值，
所以生成成功不能证明 overlay 生效。尚须核对完整镜像的路径、标签和真实挂载结果。
本实现未新增 `/mi_ext` bind 别名，其客户端依赖仍须审计。

生成的 fstab 必须通过 `prepare-vendor.py --early-log-policy` 同步到新 vendor；
再用 `prepare-logged-ramdisk.py` 与 R6 固定计划、生产 logger 包装 ramdisk。
R7 仅替代 donor 分支的配套文件，R6 A17 安装包仍保持原组合。

`make-baseline-super.py` 默认仍只接受四分区 ext4 A17 基线。新增 `--hyperos` 必须同时指定
`--system`、`--system-ext`、`--product`、`--mi-ext`、`--mi-product`（均 raw EROFS）和
`--vendor`（raw ext4），以及 `--out`。沿用 12 GiB super_hos、逻辑槽 `_a`，打包后逐一
lpunpack 比较六分区哈希。镜像魔数检查不替代 EROFS 压缩特性、SELinux 和框架兼容检查。

R7 已生成配套 vendor/日志 ramdisk；六分区流程已用无专有内容的合成镜像演练通过。
完整 donor product/mi_ext/mi_product 输入尚未找回，尚未生成真实 HyperOS super。
Mac 共 22 项测试，19 通过、3 项既有 Linux 原生日志测试跳过；新增 3 项在 Linux 容器也通过。

## R8：恢复镜像、挂载审计与 donor 配置

```sh
python3 tools/hos/fetch-donor.py \
  --url-file out/hyperos-r2/url.txt --payload-offset 4983 \
  --metadata-size 357827 --metadata-hash lV/HfpJBvrDzFZDMsHjgUysWbBoBBBi7ORbUq665Omg= \
  --out out/hyperos-r8/donor mi_product mi_ext product
```

以上 offset/hash 仅用于已记录的 piano OS4.0.5.0.XPYCNXM OTA，不适用于其他包。
中断后复用同一输出目录，校验已完成镜像和每个缓存块；整分区哈希通过才把 .partial 改为 .img。
不会覆盖已有不匹配的最终镜像。仅校验与留存元数据一致性，不增加 OTA 签名验证。

`audit-donor-mounts.py --system <SAR EROFS> --system-ext <EROFS> --product <EROFS>
--mi-ext <EROFS> --mi-product <EROFS> --fstab <生成的fstab> --dump-erofs <工具> --out <新目录>`
逐项查询 overlay target/lowerdir；输出路径、输入哈希与缺失目录清单。返回 0 仅表示目录检查
通过，不证明 SELinux 或实际 mount 成功。

R8 的 ramdisk 生成器补上 `/mi_ext` first-stage 只读 bind，修复 FeatureParser 访问原路径的问题。
`prepare-donor-init.py --system-ext-rc <原rc> --mi-ext-rc <原rc> --out <新目录>` 生成精确范围的
rc 替换：移除四条引用不存在 mi_ext/root 的 debug 挂载和两条已由 fstab 覆盖的重复 overlay。
先核对输入与实际镜像内容相同，再使用 repack-erofs.py，不能直接假定 R2 提取文件未过时。

`prepare-graphics-props.py --system-ext <原build.prop> --product <原build.prop> --out <新目录>`
生成 skiagl、增强 Vulkan 缓存禁用和背景模糊禁用的属性替换文件；拒绝待改属性重复或预期
donor 属性缺失。system_ext/product 的实际重打包与最终属性覆盖顺序须单独验证，已有 /data
里的 persist 值也可能覆盖镜像默认值，因此首次运行使用独立空 userdata_hos。

框架依赖初查见 [HAL 审计](../../docs/hyperos-hal-audit.md)，当前有反编译错误且覆盖范围有限，
尚不能据此判定首次启动不会阻塞。

`repack-erofs.py --add-plan <JSON>` 可在已有目录中新增普通文件，且强制提供镜像内的
元数据模板。例如：

```json
{
  "etc/device_features/gaokun3.xml": {
    "source": "tools/hos/config/gaokun3.xml",
    "metadata_from": "etc/device_features/piano.xml"
  }
}
```

可与 `--replace/--with-file` 或 `--plan` 同用。拒绝覆盖已有路径、路径穿越、模板软链接、
越界的父目录和替换/新增目标重叠。新文件继承模板 uid/gid、mode、xattr 与时间戳，镜像
回读仍比较整棵树的内容和元数据（时间戳/硬链接拓扑不在整体校验范围内）。该工具及新增
文件测试依赖 Linux xattr API，Mac 上跳过，须在 Linux 运行。

`config/gaokun3.xml` 是实验能力配置，不拷贝 piano 硬件认证声明；选择依据及未实现范围见
HAL 审计。完成 product 下载后，应先核对原 build.prop 与生成 graphics-props 的输入一致，
再同时替换 `etc/build.prop`、新增上述 XML，最后运行完整 mount audit 和六分区打包。

## R9：真机基线与 root ADB

R6 A17 隔离基线已实际安装并启动到 `sys.boot_completed=1`、Enforcing；独立 data/metadata
挂载已确认，misc_hos 日志回读覆盖约 181 秒。完整证据见执行记录 R9，HyperOS 尚未启动。

该 A17 adbd 的 `adb root` 依赖缺失的 adbroot_service，只有 debug 标志并不足以切到 root。
新增显式 `prepare-ramdisk.py --root-adb`，在实验 adb_debug.prop 增加 `ro.secure=0`，使 adbd
首次启动按自身 drop_privileges 逻辑保留 root。默认行为不变，不修改 SELinux enforcing。
必须先生成普通实验 ramdisk，再使用 UUID 绑定的生产 logger 包装；R9 使用独立 ESP 路径与
一次性 entry 验证，不覆盖 R6 或生产启动项。其运行验收结果以执行记录为准。

### R9 真机启动修复（首次设置界面，尚未稳定）

`prepare-vendor.py --media-profiles config/media_profiles_bootstrap.xml` 可将实验媒体配置
加入 `media_profiles_vendor.xml` 和 donor 实际选择的 `media_profiles_piano.xml`。
文件为 root:root/0644/vendor_configs_file；生成器读回数据、权限、标签并执行 e2fsck。
这个低规格单摄配置已使 donor MediaProfiles 通过初始化，但不代表相机录像通过验收。
空 MediaSettings 在 donor 会触发 cameraIds.size()>0 断言，不能用空 XML 替代。

`prepare-graphics-props.py --system <system-build.prop> ...` 另生成 system.build.prop，
将实测导致 WifiDisplayAdapter 崩溃的 `persist.debug.wfd.enable=1` 改为 0。
仅覆盖 config_enableWifiDisplay 资源不足以阻止这个强制属性。已有实验 userdata 的持久
值仍需单独处理，不能假设更新只读镜像会覆盖它。

运行时复现用 `runtime-startup-probe.sh`（在设备上执行），入参是已经推送到
`/data/local/tmp/` 的候选 XML。脚本要求 root、gaokun3、super_hos、p12 userdata 和
Enforcing，先关闭强制 WFD、临时关闭 RescueParty 自动恢复并关闭背景模糊，再用带
正确标签的 tmpfs/lowerdir overlay 增加 XML。它不写 vendor/super，重启后挂载消失；
属性写在 userdata_hos。故障稳定后需恢复 `persist.sys.disable_rescue=false`。
不可把这份诊断脚本当作最终安装方案；它不会验证录像能力或解决电话/显示 HAL 问题。

R9 产物 `vendor-media/vendor.img` 与 `system-wfd/image.erofs` 已离线验证、尚未刷入；
`startup-fix-audit.json` 证明原 vendor 替换项和策略完全不变，system 仅修改 WFD 属性。
真机已在 Enforcing 下达到 boot_completed 并显示 HyperOS 首次设置界面，但电话进程
ExtTelephonyManager 类解析失败、drm composer 原生崩溃仍待解决。详情见进度文档。

R9 composer 越界修复位于 `patches/drm-hwcomposer-client-index.patch`，应用根目录为
AOSP `external/drm_hwcomposer`。它将 client target 的原始 Z 值转为排序后 plan 的实际
下标，并拒绝越界缓存索引。测试直接编译打补丁后的生产函数，覆盖真机值 0x10000001。
上机临时 bind 替换的二进制 SHA 为
`08d81e878d8a0f21f22f41ff4c5cf635cbd0a08db85d15b9508e9e8f5cf698b5`；
多次息屏/唤醒及超过原故障周期后 PID 不变，证据 composer-runtime-observations.txt。
`prepare-vendor.py --composer-apex <rebuilt.apex>` 支持固化：要求原/新 apex_manifest.pb
及 apex_pubkey 完全一致，保留原 APEX 文件权限/xattrs。另需验证构建签名、payload 和
真实启动；不因 ZIP 检查通过就宣称安装成功。

`--64-bit-apps` 同时修改 vendor `/build.prop` 与其嵌套 `/odm/etc/build.prop`，因为 init 的
ABI 来源优先级为 product > odm > vendor > system。真实 donor vendor/odm 都为 arm64-only。
这会让仅包含 arm64 native 库的 multiArch `LyraSdkSpp.apk` 被正常扫描，从而提供
continuity_sdk_app；此前库文件并非缺失，而是包扫描拒绝它，连带清空 Phone 的全部共享库路径。
此行为仍须通过更新后的真实启动验收，不能以属性文本修改代替。

全量 APEX 构建若被设备树无关策略错误挡住，可用 `repack-composer-apex.py` 在 Linux
构建机上仅替换原 APEX 内的 composer。它要求原匹配密钥/证书，验证原/新 AVB 哈希树、
APK 签名与证书一致、ZIP 对齐，保留执行文件标签/所有者/权限与其余 ZIP 成员字节。
本轮没有修改策略以绕过构建错误；实际输出与命令记录在 R9 composer-repacked/。

后续已有实验数据的更新使用 `update-hyperos-stream.py`，而不是会格式化 data 的首次安装器。
在 rescue 预检/完整 hash 检查通过后，仅写 super_hos。默认 preview；--apply 才从 stdin 读取
完整 12 GiB。需明确提供新 SHA，更新非原始 R8 super 时还需 --old-image-sha256。
中断后保留 stream-started.json，不得删除标记盲重试。脚本不会自动重启或修改 ESP。
# R10 runtime follow-up

Use `prepare-vendor.py --wifi-optional-pmf` for new HyperOS images: the donor
framework selects `ieee80211w=3` (global default), while the baseline supplicant
global PMF default is disabled. With SAE this produced AP association rejection
31. `pmf=1` enables optional PMF without requiring it on all WPA2 networks.
R11 reboot verification found vendor init copies `/vendor/etc/wifi/wpa_supplicant.conf`
to `/data/vendor/wifi/wpa/wpa_supplicant.conf` on every post-fs-data. Therefore the
R10 data-only PMF fix survived a Wi-Fi restart but NOT a device reboot. The vendor
image must include this option for persistence. R10 verified SAE/PMF, DHCP and
Android VALIDATED; R11 temporarily reapplied it after reboot. No passwords changed.

`prepare-graphics-props.py` also restores the device's system_ext default
`persist.sys.gaokun3.touch_mode=game`. Existing vendor init services apply this
property with Enforcing enabled. Runtime readback must confirm smoothing=0,
start debounce=1, debounce_base=1, jump_dist2=0, pressure_enabled=1 and hand_enabled=0.

R10 sets `min_refresh_rate=60.0` and `peak_refresh_rate=60.0` in experimental
userdata as a provisional workaround for the donor scheduler staying at 20 Hz.
This is not a measured 120 Hz performance claim. Original settings were absent;
delete both settings to restore the original policy. Subjective touch/scroll
acceptance and full reboot verification are still pending. Evidence is in
`out/hyperos-r10/`.

## R12 Codec2 compatibility

`patch-codec2-legacy-map.py` accepts only the inspected piano OS4 platform and
swcodec APEX library hashes. It restores `allocation->map()` dispatch at the two
linear-block mapping branches. The original `0xfc0000` BLOB pool mask is retained:
changing the mask fixed software encoding but regressed hardware AVC drain.
Custom mapper callbacks are not supported in this compatibility mode; DRM and
secure codecs are not covered by the current test results.

The R12 image replaces the platform library and adds the separately patched APEX
library as a read-only system file. A guarded system init post-fs-data action binds
it over the activated swcodec library before zygote/class main start. The original
APEX container and signatures remain unchanged. `prepare-vendor.py
--codec-map-overlay` adds only init's file mounton permission for system_lib_file;
run split-policy compilation AND the neverallow check before deployment.

Runtime evidence under `out/hyperos-r12/` includes 16-frame software AVC roundtrip,
60-frame hardware AVC drain with EOS, both JPEG outputs, loaded-library maps, and
the failed global allocator comparisons. Reboot acceptance must also verify the
actual loaded hashes and repeat media tests, not merely read the library paths.


### R14 后续诊断与候选兼容修复

`bash tools/hos/trace-suspend-ucsi.sh start|collect|status [serial]` 使用独立
`hos_suspend_ucsi` trace 实例，采集休眠阶段、失败的设备 PM 回调及 UCSI 事件。
所有命令先检查 `super_hos`，不切换全局 tracer，不重绑 USB。`start` 拒绝覆盖现存
实例；物理拔线休眠并重新接线后，`collect` 导出统计/事件并删除本次实例。
实例不会自动停止，应在测试完成或取消后 collect，保留输出以核查丢事件统计。

`prepare-libc-property-contexts.py` 只追加四个 donor libc 调试属性的 exact 标签，
复用原 `libc_debug_prop` 读取权限，不修改属性值或 CIL allow。必须通过 `--context
PARTITION=PATH` 或 `--absent PARTITION` 逐一交代 plat/system_ext/product/vendor/odm，
并提供 `--plat-policy` 与新的 `--out` 目录；生成器拒绝冲突及重复映射。需重启才可
验证实际属性区权限，不能用单纯 bind 文件代替。详见
[属性审查](../../docs/hyperos-libc-property-contexts.md)。

`prepare-startup-compat.py --kind product-props` 可显式生成
`persist.sys.powmillet.enable=false` 候选；默认 init 模式不变。该开关影响 OEM 后台
策略及 Google freezer 资格，尚未自动接入镜像，必须做后台应用/通知/休眠回归。
详见 [剩余日志审查](../../docs/hyperos-r14-residual-log-audit.md)。

UCSI 自动重试候选因早期错误清理和 PM 生命周期缺口已标记 BLOCKED，不得部署。
`ucsi-gaokun-init-diagnostics.patch` 是互斥的纯诊断替代，只增加失败阶段日志，
不改变重试或设备生命周期；详见 [UCSI 审查](../../docs/hyperos-ucsi-init-review.md)。

`prepare-vendor.py --recents-overlay PATH` 可在既有 `/overlay` 目录新增
`HosRecentsOverlay.apk`，不修改其他 R14 选项或增加 CIL 规则。输入必须是无 dex/native
代码、ZIP 校验通过且包含编译后 AndroidManifest.xml/resources.arsc 的 APK。
APK 生产方仍须用 aapt2/apksigner 核对签名、静态 RRO、targetSdk28、android 目标和
唯一的 config_recentsComponentName 覆盖值；容器校验不替代这些语义检查。
生成器要求原 Gaokun3WifiOverlay.apk 为 0644 root:root，并继承其实际 SELinux 标签，
新 APK 同样是 0644 root:root；目标已存在时拒绝覆盖。内容、权限和 xattr 均使用原有
debugfs 回读验证。固化后须完整重启，检查 SystemUI 绑定以及 system_server 的
RecentTasks UID/组件；单独重启桌面不能刷新后者的启动缓存。

`prepare-vendor.py` 可附加 `--thermal-hal PATH` 与
`--usb-device-only-dir DIR`。USB目录必须同时包含
`android.hardware.usb-service.gaokun3`、`gaokun3-usbrole.sh`、`usbrole.rc`；
缺任一成员或ELF架构/程序段校验不通过时，在镜像复制前拒绝。
这些选项只替换既有vendor对应文件，继承并读回校验mode/uid/gid及全部xattr，
输入哈希记入manifest，不写入或默认启用device_only持久属性。
重建时仍须保留此前R15所用的其他兼容选项；此工具不是对已补丁镜像重复运行的增量更新器。
测试：`python3 -m unittest discover -s tools/hos -p 'test_vendor*.py'`。
