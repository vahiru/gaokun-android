#!/usr/bin/env bash
# gk3boot.efi 一键：构建 → 准备测试载荷 → 造夹具盘 → QEMU aarch64 + AAVMF + systemd-boot 257.13
# → 收串口 → 判 PASS/FAIL。说明见 tools/gk3boot/README.md §10。
#
#   bash scripts/gk3boot/test-boot.sh                       全部场景（real linux-a linux-b force-a badsha miscerr）
#   bash scripts/gk3boot/test-boot.sh linux-a badsha        只跑列出的场景
#   GK3_BOOTIMG=/path/boot.img bash scripts/gk3boot/test-boot.sh real
#       real 场景用的真 boot.img（默认找主 checkout 的 out/issues-1791053208/boot.img；没有就跳过 real）
#   GK3_TEST_KERNEL=/path/vmlinuz bash scripts/gk3boot/test-boot.sh
#       换一个通用 arm64 EFI-stub 内核（默认第一次从 Debian 取 linux-image-*-arm64-unsigned，缓存在 build/cache/）
#   GK3_DEBIAN_MIRROR=http://mirrors.ustc.edu.cn/debian bash scripts/gk3boot/test-boot.sh   换源
#
# 要 docker（本机是 colima：`colima start`；用完 `colima stop`）。全程离线于设备：不碰 adb、不碰构建机。
# 产物：tools/gk3boot/build/efi/gk3boot.efi（上机用的就是这一份）；串口与日志在 tools/gk3boot/build/qemu-boot/。
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMG=${GK3_DOCKER_PREFIX:-}gk3boot-build   # GK3_DOCKER_PREFIX：与别的会话共用 colima 时加前缀

docker info >/dev/null 2>&1 || { echo "✗ docker 不通 —— 本机先 colima start" >&2; exit 2; }

echo "▶ 构建环境镜像 ${IMG}（scripts/gk3boot/gk3boot-build.Dockerfile）"
docker build -q -t "$IMG" ${GK3_DEBIAN_MIRROR:+--build-arg GK3_DEBIAN_MIRROR=$GK3_DEBIAN_MIRROR} \
    -f "$ROOT/scripts/gk3boot/gk3boot-build.Dockerfile" "$ROOT/scripts/gk3boot" >/dev/null \
    || { echo "✗ 镜像构建失败（网络？试 GK3_DEBIAN_MIRROR=…）" >&2; exit 1; }

# 真 boot.img：worktree 里没有 out/，去主 checkout 找
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
    MNT+=(-v "$BOOTIMG:/bootimg/boot.img:ro" -e BOOTIMG=/bootimg/boot.img)
    echo "▶ real 场景用 $BOOTIMG"
else
    echo "▶ 没找到真 boot.img，跳过 real 场景"
fi
if [ -n "${GK3_TEST_KERNEL:-}" ]; then
    [ -f "$GK3_TEST_KERNEL" ] || { echo "✗ $GK3_TEST_KERNEL 不存在" >&2; exit 2; }
    MNT+=(-v "$GK3_TEST_KERNEL:/testkernel/vmlinuz:ro" -e GK3_TEST_KERNEL=/testkernel/vmlinuz)
fi
# S7c 执行端端到端（exec-* 场景）：fastboot.img（build-fastboot-img.sh）与 build/cache-fbi/（initramfs/test/run-tests.sh
# --prep）都在时自动加上；测试内核换成 cache-fbi 那一份 —— overlay 里的模块必须与内核同版本。
FB=$ROOT/tools/gk3boot/build
if [ -s "$FB/fastboot/fastboot.img" ] && [ -s "$FB/cache-fbi/vmlinuz" ] && [ -s "$FB/cache-fbi/modules/order" ]; then
    MNT+=(-e FBI_IMG=/src/tools/gk3boot/build/fastboot/fastboot.img -e FBI_MODULES=/src/tools/gk3boot/build/cache-fbi/modules)
    if [ -z "${GK3_TEST_KERNEL:-}" ]; then
        MNT+=(-e GK3_TEST_KERNEL=/src/tools/gk3boot/build/cache-fbi/vmlinuz)
    else
        echo "⚠️ 给了 GK3_TEST_KERNEL：exec-* 场景的模块来自 build/cache-fbi，版本对不上就会 insmod 失败"
    fi
    echo "▶ 执行端端到端：fastboot.img $(shasum -a 256 "$FB/fastboot/fastboot.img" | cut -c1-16)…，内核 $(cat "$FB/cache-fbi/vmlinuz.version")"
else
    echo "▶ 没有 build/fastboot/fastboot.img 或 build/cache-fbi/（exec-* 场景跳过；scripts/gk3boot/test-executor.sh 会先备齐）"
fi

# 版本串进 androidboot.bootloader=gk3boot-<串>（→ ro.bootloader）：只准 [A-Za-z0-9._+-]
rev=$(git -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
dirty=$(git -C "$ROOT" status --porcelain -- tools/gk3boot scripts/gk3boot 2>/dev/null | grep -q . && echo .dirty || true)
BOOT_VERSION="0.2.0-e5.g$rev$dirty"
echo "▶ gk3boot 版本串 $BOOT_VERSION"

docker run --rm -v "$ROOT:/src" "${MNT[@]}" -e BOOT_VERSION="$BOOT_VERSION" \
    ${GK3_DEBIAN_MIRROR:+-e GK3_DEBIAN_MIRROR=$GK3_DEBIAN_MIRROR} -w /src/tools/gk3boot "$IMG" \
    bash qemu/run-boot-tests.sh "$@"
rc=$?
f=$ROOT/tools/gk3boot/build/efi/gk3boot.efi
[ -f "$f" ] && echo "   宿主路径：$f  sha256 $(shasum -a 256 "$f" | cut -d' ' -f1)"
exit $rc
