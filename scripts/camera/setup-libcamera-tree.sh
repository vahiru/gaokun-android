#!/usr/bin/env bash
# ============================================================================
# setup-libcamera-tree.sh — 把 crDroid 树里的 external/libcamera 换成本仓验证过的
#                           上游 libcamera + patches/libcamera/*，并产出生成文件。
#
#   bash scripts/camera/setup-libcamera-tree.sh ~/crdroid [上游 libcamera clone]
#
# 为什么要有这个脚本：
#   相机 HAL（device/huawei/gaokun3/camera/Android.bp）依赖 libcamera_gk3 / libcamera_base_gk3，
#   它们定义在 patches/libcamera/libcamera-Android.bp 里，而那份 bp 要放进【上游】libcamera
#   源码树（AOSP 的 external/libcamera 没有 qcom-ife）。A16 时这一步一直是手工做的、没入库：
#   2026-10-25 A17 迁移时 reset 了树，第一次冷编在 Soong 阶段报
#     "android.hardware.camera.provider-service.gaokun3" depends on undefined module "libcamera_gk3"
#   才发现。现在把它写死在这里。
#
# 做的事（幂等：每次都从干净的上游树重来）：
#   1. 核对上游 clone 的 commit = patches/libcamera/README.md 里验证过的那个
#   2. 清空 external/libcamera 的工作区（保留 .git，repo 仍认得这个项目），铺上游源码
#   3. 按序打 patches/libcamera/0*.patch
#   4. libcamera-Android.bp → Android.bp
#   5. scripts/camera/gen-libcamera-sources.sh --apply（meson 只跑生成类目标）
#   6. config.h → generated/include/config.h（bp 用 -includeconfig.h，经 local_include_dirs 找到）
#
# ⚠️ 这棵 external/libcamera 从此是"脏"的：repo sync 会碰它（--force-sync 会还原成 AOSP 那份），
#    sync 之后要重跑本脚本。
# ============================================================================
set -euo pipefail

TREE="${1:?用法: $0 <crdroid 树> [上游 libcamera clone]}"
UP="${2:-$HOME/libcamera}"
PIN=87c7285663aaad7608fdc18d5216ec6811c685c7      # patches/libcamera/README.md
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
PD="$REPO/patches/libcamera"
LC="$TREE/external/libcamera"

die() { echo "✗ $*" >&2; exit 1; }
[ -d "$LC/.git" ] || [ -f "$LC/.git" ] || die "$LC 不是 repo 管的 git 项目"
[ -d "$UP/.git" ] || die "上游 clone 不在：$UP（git clone https://gitlab.freedesktop.org/camera/libcamera.git）"
git -C "$UP" cat-file -e "$PIN^{commit}" 2>/dev/null || die "$UP 里没有 $PIN（git -C $UP fetch）"

echo "── 1. 上游 $PIN → $LC"
find "$LC" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
git -C "$UP" archive "$PIN" | tar -x -C "$LC"
[ -f "$LC/meson.build" ] || die "铺完没有 meson.build"

echo "── 2. 补丁"
for p in "$PD"/0*.patch; do
    (cd "$LC" && patch -p1 --dry-run -s < "$p" >/dev/null) || die "打不上：$(basename "$p")"
    (cd "$LC" && patch -p1 -s < "$p") || die "应用失败：$(basename "$p")"
    echo "  ✓ $(basename "$p")"
done
[ -d "$LC/src/ipa/qcom-ife" ] || die "打完补丁没有 src/ipa/qcom-ife"

echo "── 3. Android.bp"
cp "$PD/libcamera-Android.bp" "$LC/Android.bp"

echo "── 4. 生成文件"
bash "$REPO/scripts/camera/gen-libcamera-sources.sh" "$LC" --apply
rm -rf "$LC/build-gen" "$LC/build-gen.setup.log"

echo "── 5. config.h"
cp "$PD/config.h" "$LC/generated/include/config.h"

echo "✓ external/libcamera 就绪（上游 ${PIN:0:12} + $(ls "$PD"/0*.patch | wc -l) 个补丁 + 生成文件）"
