#!/usr/bin/env bash
# 把 ReSukiSU 的内核驱动接进一棵内核树（built-in，非 GKI）。
#
#   bash scripts/kernel-setup-resukisu.sh <内核树>            # 接上
#   bash scripts/kernel-setup-resukisu.sh <内核树> --cleanup  # 拆掉
#   bash scripts/kernel-setup-resukisu.sh <内核树> --check    # 只报告状态
#
# ★ 为什么不把那一万多行代码 vendored 进本仓：
#   它是独立上游、GPL-3.0、更新很快。我们只钉一个 commit，
#   本仓自己的改动一律走 patches/resukisu/*.patch —— 这样"我们改了什么"
#   永远一眼可见，而不是混在一万多行别人的代码里。
#
# ⚠️ 本机内核是 mainline（2026-10-05 起 v7.2.9 stable，此前 v7.2-rc2），不是 Android common kernel。
#   ReSukiSU 自带 tools/kernel_compat.mk —— 它【grep 内核源码】来探测每个 API
#   在不在，所以对非 ACK 的树友好得多。对照组很说明问题：SukiSU-Ultra 直接
#   引用了 Android 私有的 policydb 字段（sepolicy.c），在主线上当场编不过；
#   ReSukiSU 同一处代码是用 KSU_COMPAT_HAS_* 宏包起来的。
#
# ★ 钩子方式选 tracepoint（上游默认），不是手工钩子：
#   Kbuild:104-114 的 KERNEL_TYPE 只按版本号判 —— 7.2 会被判成 "GKI 2.0"，
#   所以那道 "TP hooks are incompatible with Non-GKI" 的门我们本来就过得去，
#   内核源码里【一个钩子都不用插】。代价只是 CONFIG_FTRACE_SYSCALLS=y
#   （sys_enter tracepoint 由它提供），已加进 kernel-config-android.sh。
set -uo pipefail

# 钉死的上游 commit（v4.2.0-rc3 标签，2026-09-23；内核 UAPI 4，与同名管理器一致 —— 管理器的 UAPI 比内核高就只提示"需要更新内核"、不能授权）。要升级就改这一行，别改成分支名 ——
# 分支名会让"同一份脚本"在不同时间产出不同内核。
RESUKISU_PIN=239e1e8871b8fcd51a6e5b3002e0ba522fdd99fb
RESUKISU_URL=https://github.com/ReSukiSU/ReSukiSU

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TREE=${1:-}
MODE=${2:-apply}

[ -n "$TREE" ] || { echo "用法: $0 <内核树> [--cleanup|--check]" >&2; exit 2; }
[ -f "$TREE/Makefile" ] && [ -d "$TREE/drivers" ] || {
    echo "✗ $TREE 看着不像内核树（缺 Makefile 或 drivers/）" >&2; exit 2; }

TREE=$(cd "$TREE" && pwd)
KSU_DIR="$TREE/KernelSU"
LINK="$TREE/drivers/kernelsu"
DMAKE="$TREE/drivers/Makefile"
DKCONF="$TREE/drivers/Kconfig"

status() {
    echo "== 状态 =="
    [ -L "$LINK" ] && echo "  drivers/kernelsu -> $(readlink "$LINK")" || echo "  drivers/kernelsu: 没有"
    [ -d "$KSU_DIR/.git" ] && echo "  KernelSU commit: $(git -C "$KSU_DIR" rev-parse HEAD 2>/dev/null)" || echo "  KernelSU: 没有"
    grep -q 'CONFIG_KSU' "$DMAKE" 2>/dev/null && echo "  drivers/Makefile: 已改" || echo "  drivers/Makefile: 未改"
    grep -q 'drivers/kernelsu/Kconfig' "$DKCONF" 2>/dev/null && echo "  drivers/Kconfig: 已改" || echo "  drivers/Kconfig: 未改"
}

if [ "$MODE" = "--check" ]; then status; exit 0; fi

if [ "$MODE" = "--cleanup" ]; then
    echo "== 拆除 ReSukiSU =="
    [ -L "$LINK" ] && rm -f "$LINK" && echo "  已删符号链接"
    if grep -q 'CONFIG_KSU' "$DMAKE" 2>/dev/null; then
        sed -i '/kernelsu/d' "$DMAKE"; echo "  drivers/Makefile 已还原"
    fi
    if grep -q 'drivers/kernelsu/Kconfig' "$DKCONF" 2>/dev/null; then
        sed -i '\#drivers/kernelsu/Kconfig#d' "$DKCONF"; echo "  drivers/Kconfig 已还原"
    fi
    rm -rf "$KSU_DIR" && echo "  KernelSU/ 已删"
    status; exit 0
fi

# ---------- 1. 取源码（按 SHA 浅取，不拉整部历史） ----------
if [ -d "$KSU_DIR/.git" ] && [ "$(git -C "$KSU_DIR" rev-parse HEAD 2>/dev/null)" = "$RESUKISU_PIN" ]; then
    echo "== 源码已是钉住的 commit，跳过拉取 =="
else
    rm -rf "$KSU_DIR"
    echo "== 取 ReSukiSU $RESUKISU_PIN =="
    # ⚠️ 故意【不用 --depth 1】：ReSukiSU 的 Kbuild:75-77 用
    #    `git rev-list --count HEAD` 算版本号，而且发现是浅克隆就会在
    #    【构建过程中】自己跑 `git fetch --unshallow`。构建期偷偷联网是坏事：
    #    离线构建会挂，而且产物的版本号取决于当时能不能上网。
    git clone -q "$RESUKISU_URL" "$KSU_DIR" || {
        echo "✗ clone 失败（网络？）" >&2; exit 1; }
    git -C "$KSU_DIR" checkout -q "$RESUKISU_PIN" || {
        echo "✗ 检出 $RESUKISU_PIN 失败" >&2; exit 1; }
fi
GOT=$(git -C "$KSU_DIR" rev-parse HEAD)
[ "$GOT" = "$RESUKISU_PIN" ] || { echo "✗ commit 对不上：拿到 $GOT" >&2; exit 1; }

# ---------- 2. 我们自己的 7.x 修复 ----------
shopt -s nullglob
FIXES=("$REPO"/patches/resukisu/*.patch)
if [ ${#FIXES[@]} -gt 0 ]; then
    echo "== 应用本仓的 ReSukiSU 修复（${#FIXES[@]} 个）=="
    for p in "${FIXES[@]}"; do
        n=$(basename "$p")
        if git -C "$KSU_DIR" apply --reverse --check "$p" >/dev/null 2>&1; then
            echo "  跳过（已打）: $n"; continue
        fi
        if git -C "$KSU_DIR" apply "$p" 2>/dev/null; then
            echo "  ✓ $n"
        else
            echo "  ✗ $n 打不上" >&2; exit 1
        fi
    done
else
    echo "== patches/resukisu/ 是空的（还没有 7.x 修复）=="
fi

# ---------- 3. 接进 drivers/ ----------
ln -sfn ../KernelSU/kernel "$LINK"
echo "== drivers/kernelsu -> ../KernelSU/kernel =="

if ! grep -q 'CONFIG_KSU' "$DMAKE"; then
    printf 'obj-$(CONFIG_KSU) += kernelsu/\n' >> "$DMAKE"
    echo "== drivers/Makefile 已加 obj-\$(CONFIG_KSU) =="
fi

if ! grep -q 'drivers/kernelsu/Kconfig' "$DKCONF"; then
    # ⚠️ 上游 setup.sh 用 sed '/endmenu/i' —— 那会插到【第一个】endmenu 之前。
    #    drivers/Kconfig 现在只有一个 menu，但这种写法早晚会咬人。
    #    这里显式插到【最后一个】endmenu 之前。
    awk '
        { lines[NR] = $0 }
        /^endmenu[[:space:]]*$/ { last = NR }
        END {
            for (i = 1; i <= NR; i++) {
                if (i == last) print "source \"drivers/kernelsu/Kconfig\""
                print lines[i]
            }
        }' "$DKCONF" > "$DKCONF.ksu" && mv "$DKCONF.ksu" "$DKCONF"
    grep -q 'drivers/kernelsu/Kconfig' "$DKCONF" || {
        echo "✗ drivers/Kconfig 没插进去（找不到 endmenu？）" >&2; exit 1; }
    echo "== drivers/Kconfig 已加 source =="
fi

# ---------- 4. 体检 ----------
[ -f "$LINK/Kconfig" ] || { echo "✗ 符号链接指过去没有 Kconfig" >&2; exit 1; }
echo
status
echo
# Kbuild:62-69 的 LOCAL_GIT_EXISTS 硬性要求 KernelSU 仓库根有 .git，否则 $(error)
[ -e "$KSU_DIR/.git" ] || { echo "✗ $KSU_DIR/.git 不在 —— Kbuild 会直接报错" >&2; exit 1; }
[ -f "$KSU_DIR/.git/shallow" ] && echo "⚠️ 这是浅克隆，构建期 Kbuild 会自己联网 unshallow" >&2

echo "下一步：跑 scripts/kernel-config-android.sh —— 它检测到 drivers/kernelsu 就会"
echo "自动 --enable KSU / KSU_TRACEPOINT_HOOK / FTRACE_SYSCALLS 并加进 MUST_Y 断言。"
