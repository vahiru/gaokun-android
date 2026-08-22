#!/usr/bin/env bash
# 把本仓 patches/ 里【内核那一半】应用到一棵内核树。
#
# ★ 为什么需要这个脚本：patches/*.patch 长期以来【没有任何消费者】——
#   全靠人在构建机上手动 git apply。本会话已经为此付过一次代价：
#   两个 DTS 改动只活在构建机工作区里、从未入库（提交 ee5eca9 才补上）。
#   没有消费者的东西一定会漂。
#
# 用法：
#   bash scripts/kernel-apply-patches.sh /path/to/linux
#   bash scripts/kernel-apply-patches.sh /path/to/linux --check   # 只检查，不改动
#
# 幂等：已经打上的补丁会被跳过（用反向 --check 判定），所以可以反复跑。
set -uo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TREE=${1:-}
MODE=${2:-apply}

[ -n "$TREE" ] || { echo "用法: $0 <内核树路径> [--check]" >&2; exit 2; }
[ -f "$TREE/Makefile" ] && [ -d "$TREE/kernel" ] || {
    echo "✗ $TREE 看着不像内核树（缺 Makefile 或 kernel/）" >&2; exit 2; }

# ★ 上游 Venus 补丁集先打 —— 本仓的 0011 依赖 0019 提供的 `venus` label。
#   2026-08-22 实测：少了它 DTB 直接编不过（"Label or path venus not found"），
#   而在此之前这一套【只活在构建机工作区里】。详见 patches/upstream-venus/README.md。
#   ⚠️ 0014 故意不列：纯格式清理、主线已分叉（M14 就决定跳过）。
UPATCHES=(
    upstream-venus/0013-media-dt-bindings-Document-SC8280XP-SM8350-Venus.patch
    upstream-venus/0015-media-venus-hfi_venus-Support-only-updating-certain-bits-with-presets.patch
    upstream-venus/0016-media-platform-venus-Add-optional-LLCC-path.patch
    upstream-venus/0017-media-venus-core-Add-SM8350-resource-struct.patch
    upstream-venus/0018-media-venus-core-Add-SC8280XP-resource-struct.patch
    upstream-venus/0019-arm64-dts-qcom-sc8280xp-Add-Venus.patch
    upstream-venus/0020-arm64-dts-qcom-sc8280xp-huawei-gaokun3-Enable-Venus.patch
)

# ⚠️ 只列内核补丁。其余的归属别处：0003 AOSP glslang、0004/0005/0006 mesa、
#    0008 tinyalsa、0010 AOSP audio HAL —— 别往内核树上打。
KPATCHES=(
    0001-efi-pstore-register-backend-when-efivars-ops-arrive-.patch
    0002-arm64-dts-gaokun3-drive-ts-mode-gpio174-low.patch
    0007-bpf-inode-label-bpffs-lazily-for-android-genfscon.patch
    0009-arm64-dts-sc8280xp-add-cpu-cooling-maps.patch
    0011-arm64-dts-gaokun3-enable-venus.patch
    0012-arm64-dts-gaokun3-usb0-otg-for-usb-adb.patch
    0013-drm-crtc-drop-racy-BUG_ON-in-fence_to_crtc.patch
    0014-remoteproc-qcom-ratelimit-repeat-handover-error.patch
    0015-asoc-sc8280xp-raise-wsa-digital-volume-ceiling.patch
)

# ★★ 指纹判据：补丁是否【已在树里】。
#
# ⚠️ 为什么不能只靠 `git apply -R --check`：**用 fuzz 打进去的补丁，反向检查
#    一定失败**（上下文已经和补丁里的不一致了）。于是脚本会认为"没打过"，
#    再用 fuzz 打一遍 —— 结果就是【同一段代码出现两份】。
#    本仓已经因此中招两次：venus 的 sc8280xp_freq_table / sc8280xp_res 被打成
#    两份，of_match 里 sc8280xp-venus 出现三条。**构建不一定报错**，
#    重复的 static 结构体只是浪费空间，重复的 of_match 条目只是第一条生效，
#    所以症状极其隐蔽。
#
# 判据：从补丁里挑一条足够独特的新增行（长度 > 25、不是纯符号），
#       到它要改的文件里 grep。找到 ⇒ 已应用。
already_applied() {
    local f="$1" probe files ff
    probe=$(grep -E '^\+[^+]' "$f" | sed 's/^+//'             | grep -vE '^[[:space:]]*$'             | awk 'length($0) > 25' | head -1)
    [ -n "$probe" ] || return 1
    files=$(grep -E '^\+\+\+ b/' "$f" | sed 's|^+++ b/||')
    for ff in $files; do
        [ -f "$TREE/$ff" ] || continue
        grep -qF "$probe" "$TREE/$ff" && return 0
    done
    return 1
}

cd "$TREE"
echo "内核树: $TREE"
echo "版本:   $(make kernelversion 2>/dev/null || echo 未知)"
echo

applied=0; skipped=0; failed=0; fuzzed=0
for p in "${UPATCHES[@]}" "${KPATCHES[@]}"; do
    f="$REPO/patches/$p"
    [ -f "$f" ] || { echo "✗ 缺文件 $p"; failed=$((failed + 1)); continue; }

    # 反向能打通 ⇒ 已经在树里了（精确应用过的走这条）
    if git apply --check -R "$f" 2>/dev/null; then
        echo "· 已应用，跳过  $p"
        skipped=$((skipped + 1)); continue
    fi
    # ★ 指纹判据 —— 专治"用 fuzz 打进去过"的情况，反向检查对它们无效。
    #   少了这一步会把同一个补丁重复打进去（详见 already_applied 的注释）。
    if already_applied "$f"; then
        echo "· 已应用（指纹命中，当初多半是 fuzz 打的），跳过  $p"
        skipped=$((skipped + 1)); continue
    fi
    if ! git apply --check "$f" 2>/dev/null; then
        # ★ 回落到模糊匹配。0019 就需要这个（上下文里的 #include 列表与
        #   v7.2-rc2 差一行 qcom,scm.h）。⚠️ 用了 fuzz 必须【明说】，
        #   静默的模糊匹配是灾难的开始。
        if patch -p1 -d "$TREE" --dry-run --fuzz=3 < "$f" >/dev/null 2>&1; then
            if [ "$MODE" = "--check" ]; then
                echo "→ 可应用【需 fuzz=3】（--check 模式，未改动）  $p"
            else
                patch -p1 -d "$TREE" --fuzz=3 < "$f" | sed 's/^/    /'
                echo "✓ 已应用 ⚠️【用了 fuzz=3，不是精确匹配】  $p"
            fi
            applied=$((applied + 1)); fuzzed=$((fuzzed + 1)); continue
        fi
        echo "✗ 打不上（既不是已应用、也不干净、fuzz 也救不了）  $p"
        git apply --check "$f" 2>&1 | sed 's/^/    /'
        failed=$((failed + 1)); continue
    fi
    if [ "$MODE" = "--check" ]; then
        echo "→ 可应用（--check 模式，未改动）  $p"
    else
        git apply "$f" && echo "✓ 已应用  $p"
    fi
    applied=$((applied + 1))
done

echo
echo "可应用/已应用 $applied · 跳过 $skipped · 失败 $failed · 其中用了 fuzz $fuzzed"
[ "$failed" -eq 0 ] || exit 1
