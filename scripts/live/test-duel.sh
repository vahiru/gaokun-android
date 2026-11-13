#!/usr/bin/env bash
# shell 版与 Rust 版（tools/gk3-installer）的对拍 —— 只读入口（gk3_probe / gk3_plan）。
# 设计：docs/dev/installer-rust-design.md。
#
#   bash tools/gk3-installer/build.sh musl
#   GK3_TEST_DUEL=/repo/tools/gk3-installer/target/aarch64-unknown-linux-musl/release/gk3-installer \
#       bash scripts/live/test-in-container.sh scripts/live/test-duel.sh
#
# 两部分：
#   1. 这里自己造的边角盘（test-apply.sh 里没有的）：没有分区表的盘、坏的 GPT、MBR 盘（含 0xEF 分区 / 空 dos 表；混合 MBR 与
#      只剩保护性 MBR 不算 MBR）、读不出分区表的盘（假 sgdisk，四种说法，S1）、怪名字（空格 / % / 引号 /
#      中文 / 名字里带"super x"）、GBK 卷标、乱序与不对齐的分区、16 MiB 以下的缝、重名的 super、正好 1 GiB 的盘……
#   2. 与盘无关的方案计算（duel_pure）
# test-apply.sh 的全部场景另外由它自己在 GK3_TEST_DUEL 设了时顺带对拍（同一批盘、同一个时刻）。
#
# ⚠️ 只动 colima 虚拟机里的 loop 设备（背后是 /tmp 下的稀疏文件），碰不到 Mac 的盘，更碰不到平板。
set -u
cd "$(dirname "$0")/../.."
[ "$(id -u)" = 0 ] || { echo "要 root（用 scripts/live/test-in-container.sh 跑）"; exit 2; }
[ -n "${GK3_TEST_DUEL:-}" ] || GK3_TEST_DUEL=$PWD/tools/gk3-installer/target/aarch64-unknown-linux-musl/release/gk3-installer
export GK3_TEST_DUEL GK3_ALLOW_LOOP=1
# 测试容器的内核没有 ntfs3：休眠检查改用 ntfs-3g 只读挂载（gk3__ntfs_hibernated 的 GK3_NTFS_MOUNT）
grep -qw ntfs3 /proc/filesystems || export GK3_NTFS_MOUNT="ntfs-3g -o ro"
. scripts/live/duel-lib.sh

W=$(mktemp -d /tmp/gk3-duel-disks.XXXX)
cleanup() {
    local img l
    findmnt -rn /media/gk3 >/dev/null 2>&1 && umount /media/gk3
    for img in "$W"/*.img; do
        for l in $(losetup -j "$img" 2>/dev/null | cut -d: -f1); do losetup -d "$l" 2>/dev/null; done
    done
    rm -rf "$W"
}
trap cleanup EXIT
new_disk() { truncate -s "$2" "$W/$1.img"; losetup -fP --show "$W/$1.img"; }
settle() { partprobe "$1" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1; }

echo "═══ 0. 与盘无关的方案计算 ═══"
duel_pure

echo "═══ 1. 没有分区表的盘 / 坏的 GPT ═══"
D1=$(new_disk raw 3G)
duel_scene "没有分区表" "$D1"
D2=$(new_disk badgpt 3G)
sgdisk -o "$D2" >/dev/null 2>&1; head -c 4096 /dev/urandom | dd of="$D2" bs=512 seek=1 conv=notrunc status=none
duel_scene "坏的 GPT 头" "$D2"

echo "═══ 2. MBR 盘（sgdisk 读它时会在内存里转成 GPT；写时不带 -g 就拒绝）═══"
D3=$(new_disk mbr 30G)
printf 'label: dos\n,100M,c\n,4G,7\n,200M,83\n' | sfdisk -q "$D3"; settle "$D3"
mkfs.vfat "${D3}p1" >/dev/null 2>&1; mkntfs -Q -F "${D3}p2" >/dev/null 2>&1
duel_scene "MBR 盘" "$D3"
# 带 0xEF 分区（MBR 上的 ESP）、空的 dos 表（S2：两边都要报 table=mbr / mbr-disk）
D3E=$(new_disk mbresp 30G)
printf 'label: dos\n,300M,ef\n,4G,7\n' | sfdisk -q "$D3E"; settle "$D3E"; mkfs.vfat -F 32 "${D3E}p1" >/dev/null 2>&1
duel_scene "MBR 盘（带 0xEF 分区）" "$D3E"
D3Z=$(new_disk mbrempty 3G); printf 'label: dos\n' | sfdisk -q "$D3Z"
duel_scene "空的 dos 表" "$D3Z"
# 混合 MBR（sgdisk -h）、只剩保护性 MBR：都不算 MBR
D3H=$(new_disk hybrid 3G); sgdisk -o -n 1:2048:+100M -t 1:0700 -n 2:0:+100M "$D3H" >/dev/null; sgdisk -h 1 "$D3H" >/dev/null 2>&1; settle "$D3H"
duel_scene "混合 MBR" "$D3H"
D3P=$(new_disk pmbr 3G); sgdisk -o "$D3P" >/dev/null 2>&1
dd if=/dev/zero of="$D3P" bs=512 seek=1 count=33 conv=notrunc status=none
dd if=/dev/zero of="$D3P" bs=512 seek=$(( 3 * 1024 * 2048 - 33 )) count=33 conv=notrunc status=none
duel_scene "只剩保护性 MBR" "$D3P"

echo "═══ 3. 怪名字、怪卷标、乱序、不对齐、小缝 ═══"
D4=$(new_disk names 30G)
sgdisk -o \
  -n 3:2048:+100M  -t 3:ef00 -c 3:"EFI system partition" \
  -n 1:0:+300M     -t 1:0700 -c 1:"50% off name" \
  -n 2:0:+64M      -t 2:8300 -c 2:"it's x" \
  -n 5:0:+64M      -t 5:8300 -c 5:"名字 中文" \
  -n 6:0:+8M       -t 6:8300 -c 6:"" \
  "$D4" >/dev/null 2>&1
# 不对齐的分区、它前面留一个 10 MiB 的缝（< 16 MiB 不报）、后面留一个 20 MiB 的缝（要报）
S=$(( $(sgdisk -i 6 "$D4" | awk '/^Last sector:/{print $3}') + 1 + 20480 + 7 ))
sgdisk -a 1 -n 7:$S:+50M -t 7:8300 -c 7:"misc metadata" "$D4" >/dev/null 2>&1
S=$(( $(sgdisk -i 7 "$D4" | awk '/^Last sector:/{print $3}') + 1 + 40960 ))
sgdisk -n 8:$S:+12G -t 8:8300 -c 8:"super x" "$D4" >/dev/null 2>&1
settle "$D4"
mkfs.vfat -F 32 "${D4}p3" >/dev/null 2>&1; mkfs.ext4 -q -F -L "lab el%" "${D4}p1"
# 非 UTF-8 的卷标：GBK 的"系统 盘"（Windows 中文版的卷标就是本地代码页的字节）。ext4 的卷标是原样的 16 字节，
# libblkid 原样给出 —— shell 版原样透过，Rust 版必须按字节处理才能一致（protocol.rs 的 enc 按字节做的理由）
mkfs.ext4 -q -F -L "$(printf '\xcf\xb5\xcd\xb3 \xc5\xcc')" "${D4}p5"
duel_scene "怪名字" "$D4"

echo "═══ 3b. 读不出分区表的盘（S1）：假 sgdisk 照 dm-error 上实录的输出回答 ═══"
# gk3_probe 枚举 /sys/block 时跳过 dm-*，真的 dm-error 设备进不了探测 —— 两边都经 PATH 找 sgdisk，于是用一个假的
FB=$W/fakebin; mkdir -p "$FB"
cat > "$FB/sgdisk" <<EOF
#!/bin/bash
if [ "\$1" = -p ] && [ "\$2" = "\${GK3_TEST_EIO_DISK:-}" ]; then
    case "\${GK3_TEST_EIO_MODE:-eio}" in
        eio)  echo "Warning! Read error 5; strange behavior now likely!" >&2
              echo "Creating new GPT entries in memory."; echo "Disk \$2: 62914560 sectors, 30.0 GiB"
              echo "First usable sector is 34, last usable sector is 62914526"; echo
              echo "Number  Start (sector)    End (sector)  Size       Code  Name"; exit 0 ;;
        open) echo "Problem opening \$2 for reading! Error is 2." >&2; echo "The specified file does not exist!" >&2; exit 2 ;;
        crc)  echo "Warning! Error 5 reading partition table for CRC check!" >&2 ;;
        none) echo "nothing useful"; exit 0 ;;
    esac
fi
exec $(command -v sgdisk) "\$@"
EOF
chmod +x "$FB/sgdisk"
for m in eio open crc none; do
    duel_call "读不出（${m}）" "PATH=$FB:$PATH" "GK3_TEST_EIO_DISK=$D4" "GK3_TEST_EIO_MODE=$m" gk3_probe
    duel_call "读不出（${m}）" "PATH=$FB:$PATH" "GK3_TEST_EIO_DISK=$D4" "GK3_TEST_EIO_MODE=$m" \
        gk3_plan --disk "$D4" --mode alongside --rescue no --region-start 2048 --region-end 62914526 --esp "${D4}p3"
done

echo "═══ 4. 重名的 super、真机那种 34 扇区起的 misc、缺 userdata ═══"
D5=$(new_disk dup 30G)
sgdisk -o -a 1 -n 4:34:2047 -c 4:misc -t 4:8300 "$D5" >/dev/null 2>&1
sgdisk -a 2048 -n 1:2048:+300M -t 1:ef00 -c 1:esp -n 2:0:+64M -c 2:boot_a -n 3:0:+64M -c 3:boot_b \
       -n 5:0:+32M -c 5:metadata -n 6:0:+12G -c 6:super -n 7:0:+1G -c 7:super "$D5" >/dev/null 2>&1
settle "$D5"; mkfs.vfat -F 32 -n ESP "${D5}p1" >/dev/null
duel_scene "重名 super + 34 扇区的 misc" "$D5"
sgdisk -d 7 "$D5" >/dev/null 2>&1; settle "$D5"
duel_scene "缺 userdata" "$D5"
sgdisk -n 7:0:+9G -c 7:userdata "$D5" >/dev/null 2>&1; settle "$D5"; mkfs.ext4 -q -F -L userdata "${D5}p7"
duel_scene "一套完整的（小 misc）" "$D5"
sgdisk -n 8:0:+500M -c 8:gk3rescue "$D5" >/dev/null 2>&1; settle "$D5"
duel_scene "救援分区太小" "$D5"

echo "═══ 5. 正好 1 GiB 的盘、差一个扇区不到 1 GiB 的盘 ═══"
D6=$(new_disk one 1G); sgdisk -o "$D6" >/dev/null 2>&1
D7=$(new_disk lessone $(( (1 << 30) - 512 ))); sgdisk -o "$D7" >/dev/null 2>&1
duel_scene "正好 1 GiB" "$D6"
duel_call "不到 1 GiB" gk3_probe

echo "═══ 6. 安装介质在这块盘上（/media/gk3 挂着它的一个分区）═══"
mkdir -p /media/gk3 && mount "${D4}p3" /media/gk3
duel_scene "介质在怪名字盘上" "$D4"
umount /media/gk3

# 预检不再对拍：shell 版只剩设备型号与电量两项，Rust 版（tools/gk3-installer）没有跟着改
echo
duel_summary
