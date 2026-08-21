#!/vendor/bin/sh
# 修正 /sys/fs/bpf 各子目录的 SELinux 标签。
#
# ★ 为什么需要（2026-08-19 实测定案）：
#
# Android 靠 sepolicy 的 genfscon 给 bpffs 子目录打标签：
#     genfscon bpf /net_shared u:object_r:fs_bpf_net_shared:s0    （等 11 条）
# genfscon 是【惰性】标注 —— inode 首次被访问时按路径匹配。
#
# 但主线内核的 bpffs（kernel/bpf/inode.c）在三处 inode 创建点都调用了
#     security_inode_init_security(inode, dir, &dentry->d_name, ...)
# 也就是【创建时急切赋标签】，从父目录继承 —— genfscon 根本不会被查。
# 于是 /sys/fs/bpf 下所有子目录和文件全都拿到根标签 u:object_r:fs_bpf:s0。
#
# 对照实验证明这不是 genfscon 整体失效：
#     /proc/sysrq-trigger  → proc_sysrq            ✅ 子路径匹配正常
#     /sys/kernel/tracing  → debugfs_tracing_debug ✅
#     /sys/fs/bpf/*        → fs_bpf                ❌ 只有 bpffs 这样
#
# 后果：system_server 崩溃循环。ClatCoordinator 注册时会
# 【逐字比对标签字符串】（不是权限检查，所以 permissive 也救不了）：
#     packages/modules/Connectivity/service/jni/
#         com_android_server_connectivity_ClatCoordinator.cpp:564 verifyClatPerms()
#   E jniClatCoordinator: context of '/sys/fs/bpf/net_shared' is
#       'u:object_r:fs_bpf:s0' != 'u:object_r:fs_bpf_net_shared:s0'
#   → ALOGF 置 fatal → abort() → SystemServer.startOtherServices 挂掉
#
# 讽刺的是，打破 genfscon 的那个内核特性（bpffs 支持 security.* xattr，
# 见 kernel/bpf/inode.c 的 bpf_fs_security_xattr_handler）恰好让我们
# 能用 chcon 直接把标签设回去。
#
# ⏱ 时序：本脚本挂在 init 的 bpf-progs-loaded 触发器上 ——
#    system/core/rootdir/init.rc:576-584 的顺序是
#        trigger post-fs-data → trigger load-bpf-programs
#        → trigger bpf-progs-loaded → （然后才 start zygote）
#    所以一定在 bpfloader 完成之后、system_server 起来之前跑完。
#
# 标签命名规律：/sys/fs/bpf/<name> → u:object_r:fs_bpf_<name>:s0
# （逐条核对过 system/sepolicy/private/genfs_contexts:347-357）

# ★★ 2026-08-22：先检查，再动手 —— 内核带上 patches/0007 之后这个脚本已经多余。
#
# 实测证据（不是推断）：本机 /sys/fs/bpf 下 9 个子目录的标签**本来就是对的**
# （fs_bpf_net_shared / fs_bpf_netd_shared / …），而临时造一个 zz_probe 会拿到
# 根标签 fs_bpf —— genfs_contexts 里没有它的条目，回落正是惰性匹配该有的行为。
# 更直接的一条：`chcon -R` 被拒的那些 denial 里，**tcontext 已经是
# fs_bpf_netd_shared 而不是 fs_bpf** —— 要是标签靠这个脚本设，它走进去时看到的
# 应该是错的那个。
#
# ⚠️ 代价是真的：无条件 chcon -R 递归进 10 个目录，一个人贡献了
# **全系统约 1/3 的 avc denial（363/988）**，把日志和 pstore 都搅浑。
# 而 #58 那两次 panic 的调用栈正是被日志洪水挤掉的。
#
# 所以不删（老内核/没打 0007 的树上它仍是 system_server 不崩溃的唯一依靠），
# 改成：标签已经对 → 立刻退出；只有真的错了才动手。
PROBE=/sys/fs/bpf/net_shared
WANT=u:object_r:fs_bpf_net_shared:s0
CUR=$(ls -Zd "$PROBE" 2>/dev/null | awk '{print $1}')

if [ "$CUR" = "$WANT" ]; then
    log -t bpfrelabel "标签已正确（$CUR）—— 内核惰性 genfscon 正常，不做任何事"
    exit 0
fi

log -t bpfrelabel "标签不对（实际 [$CUR] 期望 [$WANT]）—— 内核多半没带 patches/0007，开始修正"

for d in cputimeinstate loader memevents net_private net_shared netd_readonly netd_shared tethering uprobestats vendor; do
    p="/sys/fs/bpf/$d"
    [ -d "$p" ] || continue
    # -R：genfscon 是最长前缀匹配，子路径继承同一标签，所以递归是对的
    chcon -R "u:object_r:fs_bpf_$d:s0" "$p" 2>/dev/null ||
        log -t bpfrelabel "chcon 失败: $p"
done

log -t bpfrelabel "bpf 标签修正完毕: $(ls -Zd /sys/fs/bpf/net_shared 2>/dev/null)"
