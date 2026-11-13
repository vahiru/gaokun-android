#!/usr/bin/env bash
# 造 LiveCD 的 U 盘镜像：在 arm64 容器里依次跑 build-rootfs.sh → build-initramfs.sh → 拆 boot.img → build-usb.sh。
# 要在 arm64 的主机上跑（Apple Silicon 的 Mac、GitHub 的 ubuntu-24.04-arm 运行器），容器里是原生的，不需要 qemu。
#
#   bash scripts/live/build-flutter.sh                 # 先有图形安装器
#   bash scripts/live/build-live.sh --boot-img <boot.img> --firmware <GPU 固件目录>
#   bash scripts/live/build-live.sh --release <发布目录> --firmware <目录> --payload    # 带上系统文件，装机不用另放
#
#   --boot-img  内核 / dtb / 内核参数的来源：LiveCD 与 Android 共用同一个内核（见 scripts/live/README.md）。
#               给 --release 时默认用里面的 boot.img
#   --firmware  GPU 固件的来源，形如 /vendor/firmware 的目录（adb pull /vendor/firmware out/vendor-firmware）。
#               cage 要 GPU；zap shader 是华为专有的，Debian 的固件包里没有（见 build-rootfs.sh 的 GPU_FW）
#   --payload   把 --release 目录整个放进 U 盘（gaokun3/payload/），安装器直接从它装
#   换源：GK3_DEBIAN_MIRROR=http://mirrors.ustc.edu.cn/debian；可复现：GK3_SNAPSHOT=20260925T000000Z（透传给 build-rootfs.sh）
#
# 产物 → out/live/：gaokun3-live.squashfs · initramfs.img · gaokun3-live.img · packages-live.lock · release.txt
# packages-live.lock 另拷一份到 scripts/live/ —— 入库，两次构建之间 diff 它就知道变了什么。
#
# ⚠️ --privileged：mmdebstrap 的 root 模式要在 chroot 里挂 proc/sys/dev。只动容器自己的文件系统。
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
die() { echo "✗ $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }

REL=; BOOTIMG=; PAYLOAD=; FW=
while [ $# -gt 0 ]; do
    case "$1" in
        --release)   REL=$2; shift 2 ;;
        --boot-img)  BOOTIMG=$2; shift 2 ;;
        --payload)   PAYLOAD=1; shift ;;
        --firmware)  FW=$2; shift 2 ;;
        *) die "不认识的参数：$1" ;;
    esac
done
[ -n "$BOOTIMG" ] || { [ -n "$REL" ] && BOOTIMG=$REL/boot.img; }
[ -n "$BOOTIMG" ] && [ -f "$BOOTIMG" ] || die "要 --boot-img <boot.img>（或 --release <含 boot.img 的发布目录>）"
[ -z "$PAYLOAD" ] || [ -n "$REL" ] || die "--payload 要配 --release <发布目录>"
[ -n "$FW" ] && [ -d "$FW" ] || die "要 --firmware <目录>（GPU 固件；adb pull /vendor/firmware out/vendor-firmware）"
[ -x "$REPO/out/installer-flutter-linux-arm64/gk3_installer" ] || die "先跑 scripts/live/build-flutter.sh（图形安装器）"
docker info >/dev/null 2>&1 || die "docker 没在跑"
[ "$(docker info --format '{{.Architecture}}')" = aarch64 ] || die "docker 不是 arm64"

# 容器里看得见的路径：仓库只读挂在 /repo，输入文件另外挂
abs() { (cd "$(dirname "$1")" && echo "$(pwd)/$(basename "$1")"); }
MOUNTS=(-v "$REPO:/repo:ro" -v "$REPO/out/live:/outlive" -v "$(abs "$BOOTIMG"):/in/boot.img:ro" -v "$(cd "$FW" && pwd):/in/firmware:ro")
[ -n "$PAYLOAD" ] && MOUNTS+=(-v "$(cd "$REL" && pwd):/in/payload:ro")
mkdir -p "$REPO/out/live"

H=$(shasum -a 256 "$REPO/scripts/live/live-build.Dockerfile" 2>/dev/null || sha256sum "$REPO/scripts/live/live-build.Dockerfile")
TAG="gk3-live-build:${H:0:12}"
docker image inspect "$TAG" >/dev/null 2>&1 || { say "构建 live 构建环境 ${TAG}"; docker build -q -t "$TAG" -f "$REPO/scripts/live/live-build.Dockerfile" "$REPO/scripts/live" >/dev/null; }

# 版本信息：安装器版本只从 pubspec.yaml 读（界面上的常量由 test/version_test.dart 核对与它一致）；
# 另记 git 提交（工作区有改动就带 -dirty）、内核来源 boot.img 的 sha256 与 gk3l 的 sha256 ——
# 进镜像的 /etc/gaokun3-release 与 U 盘上的 gaokun3/release.txt，bug 报告与发布说明都按它对版本
GK3_VERSION=$(sed -n 's/^version:[[:space:]]*\([^+[:space:]]*\).*/\1/p' "$REPO/installer/live/pubspec.yaml")
[ -n "$GK3_VERSION" ] || die "读不到 installer/live/pubspec.yaml 的 version"
GK3_GIT=$(git -C "$REPO" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
[ -z "$(git -C "$REPO" status --porcelain 2>/dev/null)" ] || GK3_GIT="$GK3_GIT-dirty"
GK3_BOOTSHA=$(shasum -a 256 "$BOOTIMG" 2>/dev/null | cut -d' ' -f1 || sha256sum "$BOOTIMG" | cut -d' ' -f1)
GK3_BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
echo "   安装器 $GK3_VERSION · git $GK3_GIT · 内核来自 boot.img ${GK3_BOOTSHA:0:16}…"

say "在容器里构建"
docker run --rm --privileged "${MOUNTS[@]}" -e PAYLOAD="$PAYLOAD" \
    -e GK3_VERSION="$GK3_VERSION" -e GK3_GIT="$GK3_GIT" -e GK3_BOOTSHA="$GK3_BOOTSHA" -e GK3_BUILT="$GK3_BUILT" \
    ${GK3_DEBIAN_MIRROR:+-e GK3_DEBIAN_MIRROR="$GK3_DEBIAN_MIRROR"} ${GK3_SNAPSHOT:+-e GK3_SNAPSHOT="$GK3_SNAPSHOT"} \
    "$TAG" bash -euo pipefail -c '
    O=/build/out; mkdir -p $O /build/boot
    # 仓库只读挂进来；构建脚本按自己所在目录找 overlay 与清单，所以拷一份可写的
    cp -a /repo/scripts /build/scripts
    # gk3l 的源码（build-rootfs.sh 用 installer/backend/linux/build.sh 现编），连同它用的 libgk3core（tools/gk3boot/core）
    mkdir -p /build/installer/backend /build/tools/gk3boot
    cp -a /repo/installer/backend/src /repo/installer/backend/linux /repo/installer/backend/third_party /build/installer/backend/
    cp -a /repo/tools/gk3boot/core /build/tools/gk3boot/
    mkdir -p /build/out-installer && cp -a /repo/out/installer-flutter-linux-arm64/. /build/out-installer/
    bash /build/scripts/live/build-rootfs.sh --out $O --installer /build/out-installer --firmware /in/firmware
    bash /build/scripts/live/build-initramfs.sh --busybox $O/busybox.static --out $O
    python3 /build/scripts/live/gk3-bootimg.py /in/boot.img /build/boot
    printf "GK3_INSTALLER_VERSION=%s\nGK3_GIT=%s\nGK3_BUILT=%s\nGK3_BOOTIMG_SHA256=%s\nGK3_GK3L_SHA256=%s\n" \
        "$GK3_VERSION" "$GK3_GIT" "$GK3_BUILT" "$GK3_BOOTSHA" "$(cat $O/gk3l.sha256)" > $O/release.txt
    bash /build/scripts/live/build-usb.sh --squashfs $O/gaokun3-live.squashfs --initramfs $O/initramfs.img \
        --kernel /build/boot/Image --dtb /build/boot/gaokun3.dtb --sdboot $O/systemd-bootaa64.efi \
        --cmdline /build/boot/cmdline.txt ${PAYLOAD:+--payload /in/payload} --release-info $O/release.txt --out $O/gaokun3-live.img
    cp $O/gaokun3-live.squashfs $O/initramfs.img $O/gaokun3-live.img $O/packages-live.lock $O/release.txt /outlive/
'
cp "$REPO/out/live/packages-live.lock" "$REPO/scripts/live/packages-live.lock"
say "产物（out/live/）"
ls -lh "$REPO/out/live/"
echo
echo "写 U 盘（⚠️ 先确认 /dev/sdX 是 U 盘）："
echo "  sudo dd if=out/live/gaokun3-live.img of=/dev/sdX bs=4M conv=fsync status=progress"
