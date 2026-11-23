# 迁移至Linux 7.4的暂存区

> **本分支以草稿PR的形式跟踪迁移进度，不会合并，也不要把本目录合并进`dev`。** 本目录只存在于`staging/7.4`分支，用来在7.4.0发布前积累迁移所需的改动。分支定期合并`upstream/dev`；决定迁移时，由工具从当时的`dev`新开分支，生成整理并重新编号后的补丁队列，再以此另开PR。

本目录不受[`../README.md`](../README.md)中编号与“补丁须接入消费脚本”规则的约束：这里的补丁由[`migrate.py`](migrate.py)消费，`override/`中的文件是队列中已有补丁在新基线上的版本，按slug命名，不带编号。

## 目录结构

| 路径 | 内容 |
|---|---|
| [`migration.conf`](migration.conf) | 新基线（`BASE_TAG`/`BASE_COMMIT`）与现行基线（`OLD_BASE_*`，仅供`prime`使用） |
| [`base/`](base/) | buildbot层的19个提交，在所有子系统之前应用；合并前按正交性规则分配到各子系统目录 |
| [`drop.tsv`](drop.tsv) | 新基线上丢弃的补丁及原因（上游对应提交或等效实现） |
| [`override.tsv`](override.tsv)、`override/<子系统>/<slug>.patch` | 在新基线上需要改写的补丁；`override.tsv`记录所基于的dev补丁的sha256 |
| [`add.tsv`](add.tsv)、`add/<子系统>/NNNN-<slug>.patch` | 只有新基线才需要的新补丁，以及插入位置 |
| [`bakasu/`](bakasu/) | KernelSU改为钉住BakaSU主线`7b65bde0`后需要的补丁，已包含`patches/resukisu/`中的strncpy兼容层，迁移时取代`patches/resukisu/` |
| [`kernel-config-7.4.sh`](kernel-config-7.4.sh) | 新基线需要的配置改动，在`kernel-config-android.sh`之后运行；合并时并入该脚本 |
| [`migrate.py`](migrate.py) | 生成补丁链、重建内核树、写回改动、检查与`dev`的差异 |

补丁以“键”标识：dev补丁为`<子系统>/<slug>`，buildbot层为`base/<slug>`，新增补丁为`add/<子系统>/<slug>`。slug即文件名去掉`NNNN-`前缀与`.patch`后缀，因此`dev`重新编号不影响本目录的任何记录。

## 用法

内核树须是一个git工作树，并已包含`BASE_COMMIT`与`OLD_BASE_COMMIT`。工具会把它切换到分离HEAD并重置，未提交的改动会丢失。

```sh
M=patches/migration-7.4
python3 -I $M/migrate.py prime  <内核树>            # 首次使用：在现行基线上打一遍现行补丁链，生成三方合并所需的blob
python3 -I $M/migrate.py tree   <内核树> --survey   # 统计：打不上的补丁记下后跳过
python3 -I $M/migrate.py tree   <内核树>            # 逐个打，遇到冲突停下
#   解决冲突后 git add 并 git am --continue（放弃该补丁则 git am --skip），然后：
python3 -I $M/migrate.py tree   <内核树> --resume
python3 -I $M/migrate.py record <内核树>            # 把与来源不同的补丁写回override/、base/、add/
python3 -I $M/migrate.py check                      # 合并upstream/dev之后运行
```

- 编译修正应以`git commit --fixup`并入对应补丁（`git rebase -i --autosquash`），再运行`record`。`record`按Subject对应补丁，提交的SHA变化不影响。
- `check`报告“override所基于的dev补丁已改动”时，需要在新基线上重新解决该补丁并`record`。
- 内核树建议开启`rerere.enabled`，以便重复解决同一冲突。

## 合并前的整理（尚未实现）

决定迁移时，由工具读取当时的`dev`与本目录，生成新的补丁队列：按`drop.tsv`删除，用`override/`替换，插入`add/`；把`base/`的补丁分配到各子系统目录；每个目录从`0001`起重新连续编号；重新生成`KPATCHES`；同时修改`kernel-apply-patches.sh`、`kernel-config-android.sh`、`kernel-setup-resukisu.sh`与`release.sh`中的基线信息，并把KernelSU钉住的版本改为BakaSU。产出为一个基于`upstream/dev`的新分支，并附新旧编号对照表。

## 当前状态

- 新基线：v7.4-rc4。
- 补丁链221个（buildbot层19个，dev 172个，override 24个，新增6个），丢弃25个（见`drop.tsv`）。
- 在v7.4-rc4上从头重建：221个全部可以打上，以rc.4发布配置加`kernel-config-android.sh`、`kernel-config-7.4.sh`编译通过。rc4改写了UCSI的初始化重试（只重试-EPROBE_DEFER与新的-EAGAIN，并在开启通知之前设置`ucsi->connector`），`ec-usb`的两个超时重试补丁因此改用override。
- v7.4-rc3上机（rc.4用户空间，不含KSU）：Android完整启动，init无属性写入被拒；前后摄像头均注册，未装配的备选前摄在绑定后立即跳过；Type-C端口、板载热敏电阻温区、Wi-Fi、GPU与显示正常；iris可以探测。尚未测试视频解码（`iris/0019`须搭配含c2 0007的ROM）、待机唤醒与其余外设。
- v7.4-rc3上机：Android可以完整启动，不含KSU与含BakaSU两种构建都已验证；显示、触控、音频正常。
- fastrpc：改用上游v4系列，1/3、2/3为`add/core/0003`、`0004`，3/3为`core/misc-fastrpc-fix-context-leak-and-hang-on-signal-interrupted-invoke`的override，内容与上游一致（3/3只适配了一处上下文）。本地修正单独放在`add/core/0005`，各项改动及原因见其补丁说明；v4的补丁单独应用并不安全，须与它一起应用。尚未上机验证。
- KernelSU：改为钉住BakaSU主线`7b65bde0`。它的UAPI为6，与目前发布的管理器（ReSukiSU v4.2.0-rc3，UAPI 4）不兼容，切换时用户需要同时更换管理器；可用的管理器目前只有BakaSU在GitHub Actions上用官方证书签名的构建。

## 7.4启动所需的改动

以下问题都会使Android无法完成启动，在纯主线上同样存在：

- `add/core/0001`、`0006`：自v7.3起（14adbd5341），PID 1拥有独立的fs_struct；PID 1切换到新的fs_struct时，`validate_fs_switch()`会把旧的fs_struct的根目录重置到nullfs。Android init先以`CLONE_FS`启动属性服务线程，再在主线程中`unshare(CLONE_NEWNS)`，这些线程因此离开真实根目录，libselinux无法打开`/sys/fs/selinux/access`，所有属性设置都被判定为拒绝。两个补丁是vfs维护者针对这一问题发出的上游修复（v1，尚未合入，标有`Cc: stable # v7.3+`）：0006不再重置，内核线程和usermodehelper继续使用PID 1留下的根目录，与v7.3之前相同；0001让`pivot_root()`同时更新这个fs_struct，是0006的前提。进入主线后改为对应提交的backport；若7.4.y已包含，则移入`drop.tsv`。
- `add/core/0002`：v7.4（d99c4a302c）不允许mmap回调在由内存管理核心管理的映射上清除`VM_MAYWRITE`。ashmem据此实现只读保护，只读映射因此失败，system_server无法建立字体表，启动循环。补丁改为在`do_mmap()`调用驱动之前去掉该标志，与memfd的写封印相同。
- binder：v7.4删除了C版binder（fd2bc059ed），只保留`ANDROID_BINDER_IPC_RUST`，构建因此需要Rust工具链（rustc ≥ 1.85、bindgen ≥ 0.71.1、rust-src组件、libclang）。`kernel-config-7.4.sh`会断言`CONFIG_RUST`未被olddefconfig关闭。
- `SCHEDSTATS`：关闭时`task_struct`中的`struct sched_statistics stats`是带64字节对齐的空结构体，bindgen生成的Rust类型丢失这一对齐，`stats`之后的字段在Rust中整体错位，Rust binder因此读错`mm`等字段。内核以`--no-layout-tests`调用bindgen，编译期不会报错。开启`SCHEDSTATS`后结构体非空，布局一致。
- KernelSU：v7.3起arm64原生系统调用从`orig_x0`取第一个参数，见`bakasu/0002`；v7.4已删除`strncpy()`，见`bakasu/0001`。

## 与7.2.9相比的行为变化（待上机核对）

- Wi-Fi WAKE#：v7.4把PERST、WAKE移到了PCIe端口节点。`wake-gpios`仍只放在`wifi@0`，从`pcie4_port0`删除，保持唤醒中断归Wi-Fi设备所有，与7.2.9一致。
- NVMe：v7.4的`pcie2a_port0`带有`wake-gpios`，根端口会注册WAKE#唤醒中断，7.2.9中该属性不生效。
- 相机：RDI线路改用上游的线路与写主控一一对应（fc02156a3e），像素通路固定使用写主控3、4；VFE复位改由上游在最后一条线路停止时执行（e5d4b0e69a）；传感器超时回退同时支持独立PHY节点的布局。
- remoteproc：handover中断首次触发后即关闭（上游改动），原限流补丁已丢弃。
- 温控：CPU冷却映射挂在`cpuN-0-thermal`（tsens0，与7.2.9的`cpuN-thermal`为同一传感器）；v7.4新增的`cpuN-1-thermal`（tsens1）只有临界温度。
- DSC宽度：保留按`dce_bytes_per_line`计算的算法，未采用上游的向上取整。
- selinuxfs：v7.4中`/sys/fs/selinux/policy`的权限不可更改，init.rc中的`chmod 0400`会失败。
- binder：Rust版binder下，`AidlLazyServiceRegistrar`注销apexservice时报告“仍有已知客户端”，C版下未出现。
- Wi-Fi板级数据：dev中设备树的校准变体为`HW_GK3`（#135），需要搭配含该变体的ROM；在此之前的ROM上会退回通用数据，信号明显变差。
