#!/usr/bin/env bash
# 造 LiveCD 的混合 ISO：在 arm64 容器里依次跑 build-rootfs.sh → build-initramfs.sh → 拆 boot.img → build-iso.sh。
# 要在 arm64 的主机上跑（Apple Silicon 的 Mac、GitHub 的 ubuntu-24.04-arm 运行器），容器里是原生的，不需要 qemu。
#
#   bash installer/tools/build/build-flutter.sh                 # 先有图形安装器
#   bash installer/tools/build/build-live.sh --boot-img <boot.img> --firmware <GPU 固件目录>
#   bash installer/tools/build/build-live.sh --release <发布目录> --firmware <目录> --payload    # 另出一个带系统文件的 ISO
#
#   --boot-img  内核 / dtb / 内核参数的来源：LiveCD 与 Android 共用同一个内核（见 installer/tools/README.md）。
#               给 --release 时默认用里面的 boot.img
#   --firmware  GPU 固件的来源，形如 /vendor/firmware 的目录（adb pull /vendor/firmware out/vendor-firmware）。
#               cage 要 GPU；zap shader 是华为专有的，Debian 的固件包里没有（见 build-rootfs.sh 的 GPU_FW）
#   --payload   另出一个 ISO，把 --release 目录整个放进它的 gaokun3/payload/：安装器直接从它装，dd 写入后也能安装
#   --allow-no-iso9660  boot.img 的内核没有开启 CONFIG_ISO9660_FS 时照样构建（测试用；dd 写入的 ISO 找不到 squashfs，
#               只有拖放方式可用，见 build-iso.sh）
#   换源：GK3_DEBIAN_MIRROR=http://mirrors.ustc.edu.cn/debian；可复现：GK3_SNAPSHOT=20260925T000000Z（透传给 build-rootfs.sh）
#
# 产物 → out/live/：gaokun3-live.squashfs · initramfs.img · gaokun3-live.iso · [gaokun3-live-payload.iso] · packages-live.lock · release.txt
# packages-live.lock 另拷一份到 installer/tools/build/ —— 入库，两次构建之间 diff 它就知道变了什么。
#
# ⚠️ --privileged：mmdebstrap 的 root 模式要在 chroot 里挂 proc/sys/dev。只动容器自己的文件系统。
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
die() { echo "✗ $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }

REL=; BOOTIMG=; PAYLOAD=; FW=; ALLOW_NO_ISO=
while [ $# -gt 0 ]; do
    case "$1" in
        --release)   REL=$2; shift 2 ;;
        --boot-img)  BOOTIMG=$2; shift 2 ;;
        --payload)   PAYLOAD=1; shift ;;
        --firmware)  FW=$2; shift 2 ;;
        --allow-no-iso9660) ALLOW_NO_ISO=1; shift ;;
        *) die "不认识的参数：$1" ;;
    esac
done
[ -n "$BOOTIMG" ] || { [ -n "$REL" ] && BOOTIMG=$REL/boot.img; }
[ -n "$BOOTIMG" ] && [ -f "$BOOTIMG" ] || die "要 --boot-img <boot.img>（或 --release <含 boot.img 的发布目录>）"
[ -z "$PAYLOAD" ] || [ -n "$REL" ] || die "--payload 要配 --release <发布目录>"
[ -n "$FW" ] && [ -d "$FW" ] || die "要 --firmware <目录>（GPU 固件；adb pull /vendor/firmware out/vendor-firmware）"
[ -x "$REPO/out/installer-flutter-linux-arm64/gk3_installer" ] || die "先跑 installer/tools/build/build-flutter.sh（图形安装器）"
docker info >/dev/null 2>&1 || die "docker 没在跑"
[ "$(docker info --format '{{.Architecture}}')" = aarch64 ] || die "docker 不是 arm64"

# 容器里看得见的路径：仓库只读挂在 /repo，输入文件另外挂
abs() { (cd "$(dirname "$1")" && echo "$(pwd)/$(basename "$1")"); }
MOUNTS=(-v "$REPO:/repo:ro" -v "$REPO/out/live:/outlive" -v "$(abs "$BOOTIMG"):/in/boot.img:ro" -v "$(cd "$FW" && pwd):/in/firmware:ro")
[ -n "$PAYLOAD" ] && MOUNTS+=(-v "$(cd "$REL" && pwd):/in/payload:ro")
mkdir -p "$REPO/out/live"

H=$(shasum -a 256 "$REPO/installer/tools/build/live-build.Dockerfile" 2>/dev/null || sha256sum "$REPO/installer/tools/build/live-build.Dockerfile")
TAG="gk3-live-build:${H:0:12}"
docker image inspect "$TAG" >/dev/null 2>&1 || { say "构建 live 构建环境 ${TAG}"; docker build -q -t "$TAG" -f "$REPO/installer/tools/build/live-build.Dockerfile" "$REPO/installer/tools/build" >/dev/null; }

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
docker run --rm --privileged "${MOUNTS[@]}" -e PAYLOAD="$PAYLOAD" -e ALLOW_NO_ISO="$ALLOW_NO_ISO" \
    -e GK3_VERSION="$GK3_VERSION" -e GK3_GIT="$GK3_GIT" -e GK3_BOOTSHA="$GK3_BOOTSHA" -e GK3_BUILT="$GK3_BUILT" \
    ${GK3_DEBIAN_MIRROR:+-e GK3_DEBIAN_MIRROR="$GK3_DEBIAN_MIRROR"} ${GK3_SNAPSHOT:+-e GK3_SNAPSHOT="$GK3_SNAPSHOT"} \
    "$TAG" bash -euo pipefail -c '
    O=/build/out; mkdir -p $O /build/boot
    # 仓库只读挂进来；构建脚本按自己所在目录找 overlay 与清单，所以拷一份可写的
    mkdir -p /build/installer/tools && cp -a /repo/installer/tools/build /build/installer/tools/
    # gk3l 的源码（build-rootfs.sh 用 installer/backend/linux/build.sh 现编），连同它用的 libgk3core（tools/gk3boot/core）
    mkdir -p /build/installer/backend /build/tools/gk3boot
    cp -a /repo/installer/backend/src /repo/installer/backend/linux /repo/installer/backend/third_party /build/installer/backend/
    cp -a /repo/tools/gk3boot/core /build/tools/gk3boot/
    mkdir -p /build/out-installer && cp -a /repo/out/installer-flutter-linux-arm64/. /build/out-installer/
    bash /build/installer/tools/build/build-rootfs.sh --out $O --installer /build/out-installer --firmware /in/firmware
    bash /build/installer/tools/build/build-initramfs.sh --busybox $O/busybox.static --out $O
    python3 /build/installer/tools/build/gk3-bootimg.py /in/boot.img /build/boot
    # 测试构建（ALLOW_NO_ISO）不要求能提取内核配置：提取不了就当作没有开启 ISO9660，交给 build-iso.sh 提醒
    python3 /repo/scripts/extract-kconfig.py /in/boot.img > /build/boot/kernel-config.txt \
        || { [ -n "$ALLOW_NO_ISO" ] && : > /build/boot/kernel-config.txt; }
    printf "GK3_INSTALLER_VERSION=%s\nGK3_GIT=%s\nGK3_BUILT=%s\nGK3_BOOTIMG_SHA256=%s\nGK3_GK3L_SHA256=%s\n" \
        "$GK3_VERSION" "$GK3_GIT" "$GK3_BUILT" "$GK3_BOOTSHA" "$(cat $O/gk3l.sha256)" > $O/release.txt
    ISO=(bash /build/installer/tools/build/build-iso.sh --squashfs $O/gaokun3-live.squashfs --initramfs $O/initramfs.img
        --kernel /build/boot/Image --dtb /build/boot/gaokun3.dtb --sdboot $O/systemd-bootaa64.efi
        --cmdline /build/boot/cmdline.txt --kconfig /build/boot/kernel-config.txt ${ALLOW_NO_ISO:+--allow-no-iso9660}
        --release-info $O/release.txt)
    "${ISO[@]}" --out $O/gaokun3-live.iso
    cp $O/gaokun3-live.squashfs $O/initramfs.img $O/gaokun3-live.iso $O/packages-live.lock $O/release.txt /outlive/
    # 带系统文件的那个直接写到输出目录：1.5 GB 左右，不在容器里多存一份
    rm -f /outlive/gaokun3-live-payload.iso
    if [ -n "$PAYLOAD" ]; then "${ISO[@]}" --payload /in/payload --out /outlive/gaokun3-live-payload.iso; fi
'
cp "$REPO/out/live/packages-live.lock" "$REPO/installer/tools/build/packages-live.lock"
say "产物（out/live/）"
ls -lh "$REPO/out/live/"
echo
echo "写 U 盘（⚠️ 先确认 /dev/sdX 是 U 盘）："
echo "  sudo dd if=out/live/gaokun3-live.iso of=/dev/sdX bs=4M conv=fsync status=progress"
echo "或者在 Windows 中打开 ISO，把 EFI、loader、gaokun3 三个目录拖进格式化为 FAT32 的 U 盘"
