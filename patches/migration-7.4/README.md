# 迁移至Linux 7.4的暂存区

> **不要从此分支开PR，也不要把本目录合并进`dev`。** 本目录只存在于`staging/7.4`分支，用来在7.4.0发布前积累迁移所需的改动。分支定期合并`upstream/dev`；决定迁移时，由工具从当时的`dev`新开分支，生成整理并重新编号后的补丁队列，再以此开PR。

本目录不受[`../README.md`](../README.md)中编号与“补丁须接入消费脚本”规则的约束：这里的补丁由[`migrate.py`](migrate.py)消费，`override/`中的文件是队列中已有补丁在新基线上的版本，按slug命名，不带编号。

## 目录结构

| 路径 | 内容 |
|---|---|
| [`migration.conf`](migration.conf) | 新基线（`BASE_TAG`/`BASE_COMMIT`）与现行基线（`OLD_BASE_*`，仅供`prime`使用） |
| [`base/`](base/) | buildbot层的19个提交，在所有子系统之前应用；合并前按正交性规则分配到各子系统目录 |
| [`drop.tsv`](drop.tsv) | 新基线上丢弃的补丁及原因（上游对应提交或等效实现） |
| [`override.tsv`](override.tsv)、`override/<子系统>/<slug>.patch` | 在新基线上需要改写的补丁；`override.tsv`记录所基于的dev补丁的sha256 |
| [`add.tsv`](add.tsv)、`add/<子系统>/NNNN-<slug>.patch` | 只有新基线才需要的新补丁，以及插入位置 |
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

决定迁移时，由工具读取当时的`dev`与本目录，生成新的补丁队列：按`drop.tsv`删除，用`override/`替换，插入`add/`；把`base/`的补丁分配到各子系统目录；每个目录从`0001`起重新连续编号；重新生成`KPATCHES`；同时修改`kernel-apply-patches.sh`、`kernel-config-android.sh`、`kernel-setup-resukisu.sh`与`release.sh`中的基线信息。产出为一个基于`upstream/dev`的新分支，并附新旧编号对照表。

## 当前状态

- 新基线：v7.4-rc3。
- 补丁链197个（buildbot层19个，dev 162个，override 16个），丢弃24个（见`drop.tsv`）。
- 在v7.4-rc3上从头重建：196个可直接打上；以rc.4发布配置加`kernel-config-android.sh`编译通过（尚未接入ReSukiSU、未含fastrpc）。
- 待处理：`core/misc-fastrpc-fix-context-leak-and-hang-on-signal-interrupted-invoke`。改用上游v4系列（2026-11-17，3个补丁），并重新加入dev版本中仍然适用的修改（如回调在硬中断上下文中取`fl->lock`的问题，v4仍未处理）。
- ReSukiSU：BakaSU主线尚未合入tiann/KernelSU `9ae6e65a`（7.3起arm64首个系统调用参数的寄存器修正），需在`add/resukisu/`中回移。

## 与7.2.9相比的行为变化（待上机核对）

- Wi-Fi WAKE#：v7.4把PERST、WAKE移到了PCIe端口节点。`wake-gpios`仍只放在`wifi@0`，从`pcie4_port0`删除，保持唤醒中断归Wi-Fi设备所有，与7.2.9一致。
- NVMe：v7.4的`pcie2a_port0`带有`wake-gpios`，根端口会注册WAKE#唤醒中断，7.2.9中该属性不生效。
- 相机：RDI线路改用上游的线路与写主控一一对应（fc02156a3e），像素通路固定使用写主控3、4；VFE复位改由上游在最后一条线路停止时执行（e5d4b0e69a）；传感器超时回退同时支持独立PHY节点的布局。
- remoteproc：handover中断首次触发后即关闭（上游改动），原限流补丁已丢弃。
- 温控：CPU冷却映射挂在`cpuN-0-thermal`（tsens0，与7.2.9的`cpuN-thermal`为同一传感器）；v7.4新增的`cpuN-1-thermal`（tsens1）只有临界温度。
- DSC宽度：保留按`dce_bytes_per_line`计算的算法，未采用上游的向上取整。
