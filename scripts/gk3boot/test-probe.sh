#!/usr/bin/env bash
# gk3probe.efi 一键：构建 → 造夹具盘 → QEMU aarch64 + AAVMF + systemd-boot 257.13 无头启动 → 收串口 → 判 PASS/FAIL。
# 说明见 tools/gk3boot/README.md §9。
#
#   bash scripts/gk3boot/test-probe.sh                      五个场景（first second strictnx broken espfull）
#   bash scripts/gk3boot/test-probe.sh first broken         只跑列出的场景
#   GK3_BOOTIMG=/path/boot.img bash scripts/gk3boot/test-probe.sh
#       boot_a 放这份完整 boot.img（默认找主 checkout 的 out/issues-1791053208/boot.img；没有就用合成镜像）
#   GK3_DEBIAN_MIRROR=http://mirrors.ustc.edu.cn/debian bash scripts/gk3boot/test-probe.sh   换源构建镜像
#
# 要 docker（本机是 colima：`colima start`；用完 `colima stop`）。全程离线于设备：不碰 adb、不碰构建机。
# 产物：tools/gk3boot/build/efi/gk3probe.efi（上机用的就是这一份）；串口与日志在 tools/gk3boot/build/qemu/。
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMG=gk3boot-build

docker info >/dev/null 2>&1 || { echo "✗ docker 不通 —— 本机先 colima start" >&2; exit 2; }

echo "▶ 构建环境镜像 ${IMG}（scripts/gk3boot/gk3boot-build.Dockerfile）"
docker build -q -t "$IMG" ${GK3_DEBIAN_MIRROR:+--build-arg GK3_DEBIAN_MIRROR=$GK3_DEBIAN_MIRROR} \
    -f "$ROOT/scripts/gk3boot/gk3boot-build.Dockerfile" "$ROOT/scripts/gk3boot" >/dev/null \
    || { echo "✗ 镜像构建失败（网络？试 GK3_DEBIAN_MIRROR=…）" >&2; exit 1; }

# 完整 boot.img：worktree 里没有 out/，去主 checkout 找
BOOTIMG=${GK3_BOOTIMG:-}
if [ -z "$BOOTIMG" ]; then
    common=$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)
    for c in "$ROOT/out/issues-1791053208/boot.img" "${common%/.git}/out/issues-1791053208/boot.img"; do
        [ -f "$c" ] && { BOOTIMG=$c; break; }
    done
fi
MNT=()
if [ -n "$BOOTIMG" ]; then
    [ -f "$BOOTIMG" ] || { echo "✗ $BOOTIMG 不存在" >&2; exit 2; }
    MNT=(-v "$BOOTIMG:/bootimg/boot.img:ro" -e BOOTIMG=/bootimg/boot.img)
    echo "▶ boot_a 用 $BOOTIMG"
else
    echo "▶ 没找到完整 boot.img，boot_a 用合成镜像"
fi

VERSION="$(git -C "$ROOT" describe --always --dirty --abbrev=12 2>/dev/null || echo unknown)-$(date -u +%Y%m%d)"
echo "▶ 探针版本串 $VERSION"

docker run --rm -v "$ROOT:/src" "${MNT[@]}" -e VERSION="$VERSION" -w /src/tools/gk3boot "$IMG" \
    bash qemu/run-tests.sh "$@"
rc=$?
f=$ROOT/tools/gk3boot/build/efi/gk3probe.efi
[ -f "$f" ] && echo "   宿主路径：$f  sha256 $(shasum -a 256 "$f" | cut -d' ' -f1)"
exit $rc
