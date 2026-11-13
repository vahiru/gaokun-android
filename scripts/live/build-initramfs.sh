#!/usr/bin/env bash
# 造 gaokun3 救援/LiveCD 的 initramfs。
#
#   bash scripts/live/build-initramfs.sh --busybox <busybox.static> --out <目录>
#
# 里面只有：静态 busybox + 我们的 /init。没有模块 —— 本机内核该有的都是 =y
# （NVMe / USB_STORAGE / SQUASHFS / OVERLAY_FS / loop / vfat / ext4）。
# 不需要 root。
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
LIVE=$REPO/scripts/live
BB=; OUT=; FW=

die() { echo "!! $*" >&2; exit 1; }
ok()  { echo "   ✓ $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --busybox)  BB=$2; shift 2 ;;
        --firmware) FW=$2; shift 2 ;;
        --out)     OUT=$2; shift 2 ;;
        *) die "不认识的参数：$1" ;;
    esac
done
[ -n "$BB" ] && [ -f "$BB" ] || die "要 --busybox <静态 busybox 二进制>（build-rootfs.sh 会在 --out 里留一个 busybox.static）"
[ -n "$OUT" ] || die "要 --out <目录>"
command -v cpio >/dev/null || die "缺 cpio"

# ★ 断言它真是静态的 aarch64 —— 动态链接的 busybox 在 initramfs 里会直接
#   "No such file or directory"（找不到 ld-musl），而那个报错完全指不到根因。
if command -v file >/dev/null; then
    finfo=$(file -b "$BB")
    case "$finfo" in
        *"ARM aarch64"*) ;;
        *) die "$BB 不是 aarch64：$finfo" ;;
    esac
    # ⚠️★ 判"是不是静态"不要只认 "statically linked"。Alpine 的 busybox-static
    #    是 **static-pie**，file 报的是 "static-pie linked" —— 第一版就因此把一个
    #    完全正确的二进制判成了失败。把【失败条件】写清楚，比枚举成功条件可靠。
    case "$finfo" in
        *"dynamically linked"*|*interpreter*)
            die "$BB 是动态链接的：$finfo
    initramfs 里没有 ld-musl，动态 busybox 会以 'No such file or directory' 失败，
    而那个报错完全指不到根因。" ;;
        *statically*|*"static-pie"*)
            ok "静态 aarch64 busybox（$(echo "$finfo" | grep -o 'stat[a-z-]*[a-z]* linked')）" ;;
        *)
            die "认不出链接方式，不敢用：$finfo" ;;
    esac
fi

D=$OUT/initramfs
rm -rf "$D"
mkdir -p "$D"/{bin,sbin,proc,sys,dev,mnt,lower,upper,newroot}

install -m755 "$BB" "$D/bin/busybox"
# /init 需要的 applet。busybox 靠 argv[0] 分派，所以要有对应的符号链接。
for a in sh ash mount umount mkdir rmdir ls cat echo sleep basename dirname \
         readlink switch_root reboot mdev losetup tr cut grep sed find \
         mknod chmod ln rm dmesg cp du; do
    ln -sf busybox "$D/bin/$a"
done
ln -sf ../bin/busybox "$D/sbin/switch_root"

# ★ WiFi 固件必须进 initramfs —— 理由见 build-rootfs.sh 里那段注释
#   （内建 ath11k 在 initramfs 阶段就要固件，squashfs 那时还没挂）。
if [ -n "$FW" ]; then
    [ -d "$FW/lib/firmware" ] || die "--firmware 目录里没有 lib/firmware：$FW"
    mkdir -p "$D/lib"
    cp -a "$FW/lib/firmware" "$D/lib/"
    n=$(find "$D/lib/firmware" -type f | wc -l)
    [ "$n" -gt 0 ] || die "固件目录是空的"
    ok "带上 $n 个固件文件（$(du -sh "$D/lib/firmware" | cut -f1)）"
else
    echo "   ⚠️ 没给 --firmware —— 内建 ath11k 会在 initramfs 阶段拿不到固件，"
    echo "      救援系统会【起来但没网】。除非你知道自己在干什么，否则别这样。"
fi

install -m755 "$LIVE/initramfs-init" "$D/init"
ok "init + $(ls "$D/bin" | wc -l) 个 applet"

IMG=$OUT/initramfs.img
( cd "$D" && find . -print0 | cpio --null -o -H newc --quiet ) | gzip -9 > "$IMG"
ok "$IMG  $(du -h "$IMG" | cut -f1)"

# 体检：解回来看看 /init 在不在、是不是可执行
T=$(mktemp -d)
( cd "$T" && gzip -dc "$IMG" | cpio -idm --quiet )
[ -x "$T/init" ] || die "打出来的 initramfs 里 /init 不可执行"
[ -x "$T/bin/busybox" ] || die "打出来的 initramfs 里没有 busybox"
if [ -n "$FW" ]; then
    ls "$T"/lib/firmware/ath11k/WCN6855/*/amss.bin* >/dev/null 2>&1         || die "打出来的 initramfs 里没有 ath11k 固件 —— 会造出没网的救援系统"
    ok "initramfs 里有 ath11k 固件"
fi
rm -rf "$T"
ok "回解体检通过"
