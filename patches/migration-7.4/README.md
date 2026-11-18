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
- 补丁链198个（buildbot层19个，dev 179个），丢弃23个（见`drop.tsv`）。
- 在v7.4-rc3上：163个可直接打上，35个冲突待解决。其中20个属于相机HW ISP补丁栈，冲突主要集中在`camss-vfe-17x.c`。
- ReSukiSU：BakaSU主线尚未合入tiann/KernelSU `9ae6e65a`（7.3起arm64首个系统调用参数的寄存器修正），需在`add/resukisu/`中回移。
