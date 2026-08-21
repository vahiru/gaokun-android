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
)

cd "$TREE"
echo "内核树: $TREE"
echo "版本:   $(make kernelversion 2>/dev/null || echo 未知)"
echo

applied=0; skipped=0; failed=0
for p in "${KPATCHES[@]}"; do
    f="$REPO/patches/$p"
    [ -f "$f" ] || { echo "✗ 缺文件 $p"; failed=$((failed + 1)); continue; }

    # 反向能打通 ⇒ 已经在树里了
    if git apply --check -R "$f" 2>/dev/null; then
        echo "· 已应用，跳过  $p"
        skipped=$((skipped + 1)); continue
    fi
    if ! git apply --check "$f" 2>/dev/null; then
        echo "✗ 打不上（既不是已应用、也不干净）  $p"
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
echo "可应用/已应用 $applied · 跳过 $skipped · 失败 $failed"
[ "$failed" -eq 0 ] || exit 1
