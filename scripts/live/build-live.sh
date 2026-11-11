#!/usr/bin/env bash
# 在 Mac 上造 gaokun3 的 live U 盘镜像（或救援镜像）：arm64 容器里依次跑
# build-rootfs.sh → build-initramfs.sh → 拆 boot.img → build-usb.sh。
#
#   bash scripts/live/build-flutter.sh                 # 先有图形安装器（live 要）
#   bash scripts/live/build-live.sh --boot-img out/test-bootimg/boot.img --firmware out/vendor-firmware [--m0]
#   bash scripts/live/build-live.sh --release <发布目录> --payload      # 带上安装载荷
#   bash scripts/live/build-live.sh --profile rescue --boot-img … --ssh-key ~/.ssh/id_ed25519.pub
#
#   --boot-img  内核 / dtb / 内核参数的来源：live 与 Android 共用同一个内核
#               （见 scripts/live/README.md）。给 --release 时默认用里面的 boot.img
#   --firmware  GPU 固件的来源，形如 /vendor/firmware 的目录：adb pull /vendor/firmware out/vendor-firmware
#               live 必需（cage 要 GPU；zap shader 是华为专有的，Debian 的固件包里没有 —— 见 build-rootfs.sh 的 GPU_FW）
#               带着它的镜像可以公开发布（随镜像发，与 ROM 同待遇 —— 已发布的 ROM 的 vendor 里本来就带着它）
#   换源：GK3_DEBIAN_MIRROR=http://mirrors.ustc.edu.cn/debian bash scripts/live/build-live.sh …
#               （2026-09-25：换网之后 colima 里连 deb.debian.org 卡死，中科大镜像 0.3 秒；透传进容器给 build-rootfs.sh）
#   --payload   把 --release 目录整个放进 U 盘（/gaokun3/payload/），装机就不用联网
#   --m0        多放几个启动项给真机 M0 用（见下面 M0_ENTRIES）
#   --with-rescue  live U 盘上另带一份 rescue profile（约 104 MiB），装机时装进救援分区的是它 ——
#               不带的话，装进去的"救援系统"是 live 镜像本身（带图形安装器）
#
# 产物 → out/live/：gaokun3-<profile>.squashfs · initramfs.img · gaokun3-live.img · packages-<profile>.lock
#                  live 另有 gaokun3-windows/ 与 gaokun3-windows.zip（免 U 盘安装包，scripts/windows/build-bundle.sh）
# packages-<profile>.lock 另拷一份到 scripts/live/ —— 入库，两次构建之间 diff 它就知道变了什么。
#
# ⚠️ --privileged：mmdebstrap 的 root 模式要在 chroot 里挂 proc/sys/dev。只动容器自己的文件系统。
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
die() { echo "✗ $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }

PROFILE=live; REL=; BOOTIMG=; PAYLOAD=; M0=; SSH_KEY=; WIFI=; WITH_RESCUE=; FW=
while [ $# -gt 0 ]; do
    case "$1" in
        --profile)   PROFILE=$2; shift 2 ;;
        --release)   REL=$2; shift 2 ;;
        --boot-img)  BOOTIMG=$2; shift 2 ;;
        --payload)   PAYLOAD=1; shift ;;
        --m0)        M0=1; shift ;;
        --with-rescue) WITH_RESCUE=1; shift ;;
        --ssh-key)   SSH_KEY=$2; shift 2 ;;
        --wifi-conf) WIFI=$2; shift 2 ;;
        --firmware)  FW=$2; shift 2 ;;
        *) die "不认识的参数：$1" ;;
    esac
done
[ -n "$BOOTIMG" ] || { [ -n "$REL" ] && BOOTIMG=$REL/boot.img; }
[ -n "$BOOTIMG" ] && [ -f "$BOOTIMG" ] || die "要 --boot-img <boot.img>（或 --release <含 boot.img 的发布目录>）"
[ -z "$PAYLOAD" ] || [ -n "$REL" ] || die "--payload 要配 --release <发布目录>"
case "$PROFILE" in live|rescue) ;; *) die "--profile 只能是 live 或 rescue" ;; esac
[ "$PROFILE" = rescue ] || [ -x "$REPO/out/installer-flutter-linux-arm64/gk3_installer" ] \
    || die "先跑 scripts/live/build-flutter.sh（live 要图形安装器）"
docker info >/dev/null 2>&1 || die "docker 没在跑（colima start）"
[ "$(docker info --format '{{.Architecture}}')" = aarch64 ] || die "docker 不是 arm64"

# 容器里看得见的路径：仓库只读挂在 /repo，输入文件另外挂
abs() { (cd "$(dirname "$1")" && echo "$(pwd)/$(basename "$1")"); }
MOUNTS=(-v "$REPO:/repo:ro" -v "$REPO/out/live:/outlive" -v "$(abs "$BOOTIMG"):/in/boot.img:ro")
[ -n "$PAYLOAD" ] && MOUNTS+=(-v "$(cd "$REL" && pwd):/in/payload:ro")
[ -n "$SSH_KEY" ] && MOUNTS+=(-v "$(abs "$SSH_KEY"):/in/ssh.pub:ro")
[ -n "$WIFI" ] && MOUNTS+=(-v "$(abs "$WIFI"):/in/wpa.conf:ro")
if [ "$PROFILE" = live ]; then
    [ -n "$FW" ] && [ -d "$FW" ] || die "live 要 --firmware <目录>（GPU 固件；adb pull /vendor/firmware out/vendor-firmware）"
fi
[ -n "$FW" ] && MOUNTS+=(-v "$(cd "$FW" && pwd):/in/firmware:ro")
mkdir -p "$REPO/out/live"

H=$(shasum -a 256 "$REPO/scripts/live/live-build.Dockerfile" 2>/dev/null || sha256sum "$REPO/scripts/live/live-build.Dockerfile")
TAG="gk3-live-build:${H:0:12}"
docker image inspect "$TAG" >/dev/null 2>&1 || { say "构建 live 构建环境 ${TAG}"; docker build -q -t "$TAG" -f "$REPO/scripts/live/live-build.Dockerfile" "$REPO/scripts/live" >/dev/null; }

# M0 的变体启动项（见 installer/live/README.md）：
#   渲染后端两条都要测（Impeller 是 3.47.2 的默认，#192915 说它在弱 GPU 上闪）；浸泡要持续出帧、
#   RSS 每 5 秒记到 U 盘的 gaokun3/diag/soak-*.log；旋转方向是推理出来的，反方向也放一个
#   ⚠️ 标题用 ASCII：开机菜单由 UEFI 固件的字体画，一般不含中文（见 build-usb.sh）
M0_ENTRIES=(
    "M0: Skia (Impeller off)|gk3.renderer=skia"
    "M0: soak test, Impeller|gk3.soak=1"
    "M0: soak test, Skia|gk3.soak=1 gk3.renderer=skia"
    "M0: rotate the other way (transform 90)|gk3.rotate=90"
)
# 条目逐行写进文件、挂进容器，在里面读成数组。
# ⚠️ 不用 printf %q + eval：第一版这么传，Mac 的 bash 3.2 与容器里的 bash 5 对 UTF-8 字节的
#    转义不一致，标题成了乱码。跨 shell 版本的转义不可信，文件不会出这种事。
ENTRIES_FILE=$REPO/out/live/.entries
: > "$ENTRIES_FILE"
[ -n "$M0" ] && printf '%s\n' "${M0_ENTRIES[@]}" > "$ENTRIES_FILE"

# 版本信息（2026-09-27）：安装器版本只从 pubspec.yaml 读（界面上的常量由 test/version_test.dart 核对与它一致）；
# 另记 git 提交（工作区有改动就带 -dirty）与内核来源 boot.img 的 sha256 —— 进镜像的 /etc/gaokun3-release、
# U 盘上的 gaokun3/release.txt 与 Windows 安装包，bug 报告与发布说明都按它对版本
GK3_VERSION=$(sed -n 's/^version:[[:space:]]*\([^+[:space:]]*\).*/\1/p' "$REPO/installer/live/pubspec.yaml")
[ -n "$GK3_VERSION" ] || die "读不到 installer/live/pubspec.yaml 的 version"
GK3_GIT=$(git -C "$REPO" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
[ -z "$(git -C "$REPO" status --porcelain 2>/dev/null)" ] || GK3_GIT="$GK3_GIT-dirty"
GK3_BOOTSHA=$(shasum -a 256 "$BOOTIMG" 2>/dev/null | cut -d' ' -f1 || sha256sum "$BOOTIMG" | cut -d' ' -f1)
GK3_BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
echo "   安装器 $GK3_VERSION · git $GK3_GIT · 内核来自 boot.img ${GK3_BOOTSHA:0:16}…"

say "在容器里构建（${PROFILE}）"
docker run --rm --privileged "${MOUNTS[@]}" \
    -e PROFILE="$PROFILE" -e PAYLOAD="$PAYLOAD" -e HAVE_KEY="$SSH_KEY" -e HAVE_WIFI="$WIFI" -e WITH_RESCUE="$WITH_RESCUE" -e HAVE_FW="$FW" \
    -e GK3_VERSION="$GK3_VERSION" -e GK3_GIT="$GK3_GIT" -e GK3_BOOTSHA="$GK3_BOOTSHA" -e GK3_BUILT="$GK3_BUILT" \
    ${GK3_DEBIAN_MIRROR:+-e GK3_DEBIAN_MIRROR="$GK3_DEBIAN_MIRROR"} ${GK3_SNAPSHOT:+-e GK3_SNAPSHOT="$GK3_SNAPSHOT"} \
    "$TAG" bash -euo pipefail -c '
    O=/build/out; mkdir -p $O /build/boot
    # 仓库只读挂进来；构建脚本按自己所在目录找 overlay 与清单，所以拷一份可写的
    cp -a /repo/scripts /build/scripts
    # gk3-misc 的源码（build-rootfs.sh 静态编进镜像，S10）；Mac 上编出来的 build/ 不要
    mkdir -p /build/tools && cp -a /repo/tools/gk3boot /build/tools/gk3boot && rm -rf /build/tools/gk3boot/build
    mkdir -p /build/out-installer && [ "$PROFILE" = rescue ] || cp -a /repo/out/installer-flutter-linux-arm64/. /build/out-installer/
    bash /build/scripts/live/build-rootfs.sh --profile "$PROFILE" --out $O --installer /build/out-installer \
        ${HAVE_KEY:+--ssh-key /in/ssh.pub} ${HAVE_WIFI:+--wifi-conf /in/wpa.conf} ${HAVE_FW:+--firmware /in/firmware}
    RSQ=
    if [ -n "$WITH_RESCUE" ] && [ "$PROFILE" = live ]; then
        GK3_WORK=/build/work-rescue bash /build/scripts/live/build-rootfs.sh --profile rescue --out /build/out-rescue \
            ${HAVE_KEY:+--ssh-key /in/ssh.pub} ${HAVE_WIFI:+--wifi-conf /in/wpa.conf}
        RSQ=/build/out-rescue/gaokun3-rescue.squashfs
    fi
    bash /build/scripts/live/build-initramfs.sh --busybox $O/busybox.static --firmware $O/fw --out $O
    python3 /build/scripts/live/gk3-bootimg.py /in/boot.img /build/boot
    printf "GK3_INSTALLER_VERSION=%s\nGK3_GIT=%s\nGK3_BUILT=%s\nGK3_BOOTIMG_SHA256=%s\nGK3_PROFILE=%s\nGK3_MISC_SHA256=%s\n" \
        "$GK3_VERSION" "$GK3_GIT" "$GK3_BUILT" "$GK3_BOOTSHA" "$PROFILE" "$(cat $O/gk3-misc.sha256)" > $O/release.txt
    EA=(); while IFS= read -r e; do [ -n "$e" ] && EA+=(--entry "$e"); done < /outlive/.entries
    bash /build/scripts/live/build-usb.sh --squashfs $O/gaokun3-$PROFILE.squashfs --initramfs $O/initramfs.img \
        --kernel /build/boot/Image --dtb /build/boot/gaokun3.dtb --sdboot $O/systemd-bootaa64.efi \
        --cmdline /build/boot/cmdline.txt ${PAYLOAD:+--payload /in/payload} ${RSQ:+--rescue-squashfs $RSQ} \
        ${EA[@]+"${EA[@]}"} --windows-tools /build/scripts/windows --release-info $O/release.txt --out $O/gaokun3-live.img
    cp $O/gaokun3-$PROFILE.squashfs $O/initramfs.img $O/gaokun3-live.img $O/packages-$PROFILE.lock $O/release.txt /outlive/
    # 免 U 盘安装（用户 2026-09-25）：给 Windows 用户的安装包 —— 同一套内核 / initramfs / squashfs
    if [ "$PROFILE" = live ]; then
        bash /build/scripts/windows/build-bundle.sh --squashfs $O/gaokun3-live.squashfs --initramfs $O/initramfs.img \
            --kernel /build/boot/Image --dtb /build/boot/gaokun3.dtb --sdboot $O/systemd-bootaa64.efi \
            --cmdline /build/boot/cmdline.txt ${RSQ:+--rescue-squashfs $RSQ} --release-info $O/release.txt --out $O/gaokun3-windows
        rm -rf /outlive/gaokun3-windows /outlive/gaokun3-windows.zip
        cp -a $O/gaokun3-windows $O/gaokun3-windows.zip /outlive/
    fi
    [ -z "$RSQ" ] || cp "$RSQ" /build/out-rescue/packages-rescue.lock /outlive/
'
cp "$REPO/out/live/packages-$PROFILE.lock" "$REPO/scripts/live/packages-$PROFILE.lock"
[ -z "$WITH_RESCUE" ] || cp "$REPO/out/live/packages-rescue.lock" "$REPO/scripts/live/packages-rescue.lock"
say "产物（out/live/）"
ls -lh "$REPO/out/live/"
echo
echo "写 U 盘（⚠️ 先确认 /dev/diskN 是 U 盘；macOS：diskutil list）："
echo "  sudo dd if=out/live/gaokun3-live.img of=/dev/rdiskN bs=4m && sync"
