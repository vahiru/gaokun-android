# SELinux策略普查与验证工具

本目录包含3个脚本，分别用于开机AVC拒绝汇总、运行期策略查询以及Enforcing模式试运行：

| 用途 | 脚本 | 运行位置 |
|---|---|---|
| 汇总并去重开机以来的AVC拒绝日志 | `avc-summary.py` | 宿主机（读取导出的`logcat` / `dmesg`） |
| 查询设备当前加载策略是否放行指定访问 | `policy-query.sh` | 设备（root） |
| 在不重启的情况下临时切换Enforcing试运行 | `enforcing-trial.sh` | 设备（root，带看门狗自动恢复） |

## 1. 完整开机普查

```bash
adb shell 'logcat -b all -d -v monotonic' > logcat.txt
adb shell dmesg > dmesg.txt
python3 scripts/selinux/avc-summary.py logcat.txt dmesg.txt
```

采样注意事项：

* **`dmesg`环形缓冲可能滚掉开机早期日志**；`logd`的`kernel`缓冲区从开机起持续读取`kmsg`，通常保留自0秒起的记录。分析前先用`head`核对首行时间戳。
* **审计限速**：`logd.rc`在`boot_completed`时将限速设为5条/秒，开机完成瞬间容易丢弃审计记录（可在`dmesg`中搜索`audit_lost`）。开发机上可执行`setprop persist.logd.audit.rate 1000`解除限速。
* **Permissive去重机制**：Permissive模式下同一`(scontext, tcontext, tclass, perm)`四元组只记录一次。若某类问题需要通过修改文件/节点标签（而非添加`allow`规则）修复，日志中只会暴露第一个触发节点，需自行通过`/proc/<pid>/maps`、`/sys/class/wakeup/*`、`getprop -Z`或`vendor/build.prop`枚举全部同类对象。

## 2. 查询现行加载策略

```bash
adb push scripts/selinux/policy-query.sh /data/local/tmp/
printf '%s\n' "kernel vendor_file system firmware_load" "p vendor_init vendor_init capability2 block_suspend" \
  | adb shell 'su -c "sh /data/local/tmp/policy-query.sh"'
```

脚本通过`/sys/fs/selinux/access`直接查询内核当前加载的二进制策略。行首加`p `表示目标为进程上下文（`u:r:`）；源域处于Permissive状态（如KernelSU的`ksu`域）时输出会标注`[permissive]`。

## 3. 运行期Enforcing试跑

```bash
adb push scripts/selinux/enforcing-trial.sh /data/local/tmp/
adb shell 'su -c "setsid sh /data/local/tmp/enforcing-trial.sh </dev/null >/dev/null 2>&1 &"'
# 约3分钟后查看结果
adb shell cat /data/local/tmp/enf-trial.log
python3 scripts/selinux/avc-summary.py --enforcing logcat.txt dmesg.txt
```

由于本机`adbd`与`su`运行在Permissive的`ksu`域，全局切换Enforcing不会锁死adb连接；脚本内置300秒看门狗超时恢复。运行期试跑无法覆盖仅在开机或服务首次启动时发生的访问，后者需结合第1步或带`androidboot.selinux=enforcing`启动测试。
