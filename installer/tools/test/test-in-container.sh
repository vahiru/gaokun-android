#!/usr/bin/env bash
# 在 Linux 容器里跑要 root、loop 设备与分区工具的测试（安装器后端、initramfs、界面的 fixture 录制）。
#
#   bash installer/tools/test/test-in-container.sh installer/backend/test/linux/test-apply.sh
#   bash installer/tools/test/test-in-container.sh installer/tools/test/test-initramfs.sh
#
# ⚠️ 用 --privileged 并把宿主（docker 所在的虚拟机或机器）的 /dev 挂进来：loop 设备的
#    分区节点（loop0p1…）是【之后】才由内核建出来的，不挂 /dev 的话容器里永远看不见它们，
#    测出来的是容器的问题而不是代码的问题。
# ⚠️ 测试只动 loop 设备（背后是容器里的稀疏文件），不碰宿主的盘。
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
IMG=gk3-test-env
die() { echo "✗ $*" >&2; exit 1; }

[ $# -ge 1 ] || die "用法：$0 <测试脚本> [参数…]"
command -v docker >/dev/null || die "没有 docker（macOS 上：colima start）"
docker info >/dev/null 2>&1 || die "docker 没在跑（colima start）"

# Dockerfile 一变就重建：标签里带上它的哈希
H=$(shasum -a 256 "$REPO/installer/tools/test/test-env.Dockerfile" 2>/dev/null || sha256sum "$REPO/installer/tools/test/test-env.Dockerfile")
TAG="$IMG:${H:0:12}"
if ! docker image inspect "$TAG" >/dev/null 2>&1; then
    echo "══ 构建测试环境 $TAG"
    docker build -q -t "$TAG" -f "$REPO/installer/tools/test/test-env.Dockerfile" "$REPO/installer/tools/test" >/dev/null
fi
# GK3_TEST_* 原样转发（例如 GK3_TEST_BOOTIMG=/repo/out/…/boot.img 用真发版的镜像测）
ENVS=()
while IFS='=' read -r k _; do ENVS+=(-e "$k"); done < <(env | grep '^GK3_TEST_' || true)
exec docker run --rm --privileged -v /dev:/dev -v "$REPO:/repo" -w /repo \
    ${ENVS[@]+"${ENVS[@]}"} "$TAG" bash "$@"
