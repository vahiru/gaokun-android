#!/usr/bin/env bash
#
# 把构建好的 Android 镜像部署到 Ego —— MateBook E Go (gaokun3)
#
# 在 Ego 上（从 U 盘启动的 Linux）运行：
#   sudo bash deploy-android.sh /path/to/android-images/
#
# 需要的输入文件（从构建机 out/target/product/gaokun3/ 取）：
#   super.img            动态分区镜像（system/system_ext/product/vendor）
#   ramdisk.img          generic ramdisk（first stage init）
#   vendor_ramdisk.img   vendor ramdisk（设备相关；可能在 vendor_boot.img 里）
#
# 内核不从这里来 —— 用我们自编的 7.2.0-rc2-gaokun3+，
# 它已经装在 Ego 上并且带了 OTG DTB。
#
# 这个脚本会：
#   1. dd super.img 到 by-partlabel/super
#   2. 把 generic + vendor ramdisk 串接成一个 initrd
#   3. 把 kernel/initrd/dtb 放进 ESP
#   4. 建一个 Android 的 BLS entry（默认项不动，失败可选回 Linux）
#
# 不碰 Windows 分区，不改现有启动项。

set -uo pipefail

SRC="${1:-}"
KREL=7.2.0-rc2-gaokun3+
MID=8a29534fa802480d9fbb71aa18c01d7b
ESP=/boot/efi
ENTRIES=$ESP/loader/entries
DEST=$ESP/$MID/android

die() { echo "错误: $*" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || die "需要 root"
[ -n "$SRC" ] && [ -d "$SRC" ] || die "用法: $0 <镜像目录>"

# ------------------------------------------------------------------ 前置检查
echo "════════ 前置检查 ════════"
for f in super.img ramdisk.img; do
    [ -f "$SRC/$f" ] || die "缺少 $SRC/$f"
    printf "  ✅ %-20s %s\n" "$f" "$(du -h "$SRC/$f" | cut -f1)"
done
VRAMDISK=""
for cand in vendor_ramdisk.img vendor-ramdisk.img; do
    [ -f "$SRC/$cand" ] && VRAMDISK="$SRC/$cand" && break
done
if [ -n "$VRAMDISK" ]; then
    printf "  ✅ %-20s %s\n" "$(basename $VRAMDISK)" "$(du -h "$VRAMDISK" | cut -f1)"
else
    echo "  ⚠️  没有 vendor_ramdisk —— 只用 generic ramdisk 试试"
fi

for p in super userdata metadata; do
    [ -e "/dev/disk/by-partlabel/$p" ] || die "分区 $p 不存在，先跑 partition-android.sh"
done
echo "  ✅ super / userdata / metadata 分区就位"

[ -f "/boot/vmlinuz-$KREL" ] || die "内核 /boot/vmlinuz-$KREL 不存在"
DTB=$ESP/$MID/$KREL/dtb-otg.dtb
[ -f "$DTB" ] || die "OTG DTB 不存在: $DTB"
echo "  ✅ 内核与 OTG DTB 就位"

# super.img 必须装得下
SUPER_DEV=$(readlink -f /dev/disk/by-partlabel/super)
PART_BYTES=$(blockdev --getsize64 "$SUPER_DEV")
IMG_BYTES=$(stat -c%s "$SRC/super.img")
echo "  super 分区: $((PART_BYTES/1024/1024)) MiB"
echo "  super.img : $((IMG_BYTES/1024/1024)) MiB"
[ "$IMG_BYTES" -le "$PART_BYTES" ] || die "super.img 比分区大，装不下"

# 目标盘不能是当前根文件系统
case "$(findmnt -n -o SOURCE /)" in
    /dev/nvme0n1*) die "根文件系统在内置盘上，必须从 U 盘启动后运行" ;;
esac

echo
read -r -p "以上无误？输入 YES 继续: " ans
[ "$ans" = "YES" ] || die "已取消"

# ------------------------------------------------------------ 1. 写 super
echo
echo "════════ 1. 写 super.img ════════"
# ⚠⚠ super.img 默认是 Android sparse 格式（魔数 0xED26FF3A，小端 3a ff 26 ed）。
#    直接 dd 会把 sparse 容器原样写进去 —— init 读不到 LP 元数据，
#    挂载失败后主动复位，且不留任何日志（见 docs/dev/android-bringup.md）。
#    2026-08-19 审出：本脚本原先就是直接 dd，是个潜伏 bug；
#    实际在用的 deploy-from-ubuntu.sh 用的是 simg2img。
MAGIC=$(od -A n -t x1 -N 4 "$SRC/super.img" | tr -d ' ')
if [ "$MAGIC" = "3aff26ed" ]; then
    command -v simg2img >/dev/null || die "super.img 是 sparse 格式，但找不到 simg2img（apt install android-sdk-libsparse-utils）"
    echo "  检测到 sparse 镜像 → simg2img 展开写入"
    simg2img "$SRC/super.img" "$SUPER_DEV" || die "simg2img 失败"
else
    echo "  裸镜像 → dd"
    dd if="$SRC/super.img" of="$SUPER_DEV" bs=8M conv=fsync status=progress || die "dd 失败"
fi
sync
echo "  ✅ 完成"

# --------------------------------------------------------- 2. 拼接 ramdisk
# Linux 支持串接的 cpio 归档：内核会依次解包。
# Android 本来由 bootloader 分别喂 generic 和 vendor ramdisk，
# systemd-boot 只接受一个 initrd，所以在这里合并。
echo
echo "════════ 2. 拼接 ramdisk ════════"
mkdir -p "$DEST"
if [ -n "$VRAMDISK" ]; then
    cat "$SRC/ramdisk.img" "$VRAMDISK" > "$DEST/initrd-android.img"
    echo "  generic + vendor 串接 -> $(du -h "$DEST/initrd-android.img" | cut -f1)"
else
    cp "$SRC/ramdisk.img" "$DEST/initrd-android.img"
    echo "  仅 generic -> $(du -h "$DEST/initrd-android.img" | cut -f1)"
fi

# ------------------------------------------------------- 3. ESP 放置文件
echo
echo "════════ 3. 放置内核与 DTB ════════"
cp "/boot/vmlinuz-$KREL" "$DEST/linux"
cp "$DTB" "$DEST/sc8280xp-huawei-gaokun3.dtb"
ls -la "$DEST" | sed 's/^/  /'

# ----------------------------------------------------------- 4. BLS entry
echo
echo "════════ 4. 建 Android 启动项 ════════"
# cmdline 与 device/huawei/gaokun3/BoardConfig.mk 里的 BOARD_KERNEL_CMDLINE 保持一致。
# 两处必须同步改，否则调试时会对不上。
# 刻意不加 earlycon —— 强烈怀疑 earlycon=efifb 会挂死本机启动。
cat > "$ENTRIES/$MID-android.conf" <<CONF
title      >>> Android 16 (gaokun3) <<<
version    android-16-$KREL
machine-id $MID
sort-key   zzandroid
options    androidboot.hardware=gaokun3 androidboot.boot_devices=soc@0/1c20000.pcie androidboot.selinux=permissive androidboot.usbcontroller=a600000.usb firmware_class.path=/vendor/firmware/ init=/init printk.devkmsg=on deferred_probe_timeout=30 console=tty0 loglevel=7 clk_ignore_unused pd_ignore_unused arm64.nopauth efi=noruntime fbcon=rotate:1 usbhid.quirks=0x12d1:0x10b8:0x20000000
linux      /$MID/android/linux
devicetree /$MID/android/sc8280xp-huawei-gaokun3.dtb
initrd     /$MID/android/initrd-android.img
CONF
echo "  已建立 $ENTRIES/$MID-android.conf"

# 默认项保持不动 —— 上一次把所有可用项都停用，结果新内核起不来就没了退路
echo "  默认项保持不变: $(grep '^default' $ESP/loader/loader.conf | sed 's/default //')"
sync

echo
echo "════════ 完成 ════════"
bootctl list 2>/dev/null | grep -E '^\s+title:' | sed 's/^ *title: */  /'
echo
echo "重启后在菜单里选 '>>> Android 16 (gaokun3) <<<'"
echo "起不来就断电重启，等 20 秒自动回到 Linux；崩溃日志看 /var/lib/systemd/pstore/"
