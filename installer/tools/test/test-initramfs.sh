#!/usr/bin/env bash
# initramfs-init 的容器测试：只测找到介质之后的那一段（读进内存、介质记录、挂进新根）。
#   - 介质在不可移动的盘上（loop 盘的 removable 是 0，算内置盘）→ gaokun3/ 读进内存、介质分区卸下，
#     新根里 /media/gk3 是只读的 tmpfs、内容与介质相同，/run 已挂好，/run/gaokun3/medium 记 ram=yes
#   - gk3.toram=0 → 照旧直接挂着，ram=no
#   - 内存不够（MemAvailable 换成很小的数）→ 不读，ram=no
# 跑的是改过的一份副本：cmdline 与 meminfo 从环境变量来；switch_root 换成打印状态后退出、reboot 换成退出（容器里
# 不能真的切根或重启），sleep 去掉。在新的挂载命名空间里跑（unshare -m），它挂的东西随它一起消失。
# "squashfs"用 ext2 镜像代替（测试镜像里没有 mksquashfs；init 只用 mount -o loop，认得出就行）。
#
#   bash installer/tools/test/test-in-container.sh installer/tools/test/test-initramfs.sh
set -u
cd "$(dirname "$0")/../../.."
[ "$(id -u)" = 0 ] || { echo "要 root（用 installer/tools/test/test-in-container.sh 跑）"; exit 2; }
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ✓ $*"; }
bad() { FAIL=$((FAIL+1)); echo "  ✗ $*"; }
check() { local m=$1; shift; if "$@"; then ok "$m"; else bad "$m"; fi; }
val() { printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -1; }

W=$(mktemp -d /tmp/gk3-initrd.XXXX)
cleanup() {
    local l
    for l in $(losetup -j "$W/disk.img" 2>/dev/null | cut -d: -f1); do losetup -d "$l" 2>/dev/null; done
    rm -rf "$W"
}
trap cleanup EXIT

# 新根（代替 live.squashfs）
R=$W/root; mkdir -p "$R"/sbin "$R"/proc "$R"/sys "$R"/dev "$R"/run "$R"/media/gk3
printf '#!/bin/sh\n' > "$R/sbin/init"; chmod +x "$R/sbin/init"
mke2fs -q -t ext2 -d "$R" "$W/live.img" 16M || { echo "造不出新根"; exit 1; }

# 介质：一块盘、一个 FAT32 分区（类型 XBOOTLDR，卷标 GK3LIVE），gaokun3/ 下放新根、载荷、旧日志
truncate -s 256M "$W/disk.img"
L=$(losetup -fP --show "$W/disk.img")
sgdisk -o -n 1:2048:0 -t 1:EA00 -c 1:GK3LIVE "$L" >/dev/null; partprobe "$L"; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 -n GK3LIVE "${L}p1" >/dev/null
M=$W/m; mkdir -p "$M"; mount "${L}p1" "$M"
mkdir -p "$M/gaokun3/payload" "$M/gaokun3/diag" "$M/loader/entries"
cp "$W/live.img" "$M/gaokun3/live.squashfs"
head -c 20971520 /dev/urandom > "$M/gaokun3/payload/super.img.zst"
echo "an earlier boot report" > "$M/gaokun3/diag/boot-20261101-000000.log"
echo "title LiveCD" > "$M/loader/entries/gaokun3-live.conf"
want=$(cd "$M/gaokun3" && find . -type f | LC_ALL=C sort | xargs sha256sum)
umount "$M"

# 改过的副本：switch_root 之前打印状态
cat > "$W/report.sh" <<'EOF'
report() {
    mount -t proc proc /proc
    echo "MEDIA_FS=$(findmnt -no FSTYPE /newroot/media/gk3)"
    echo "MEDIA_SRC=$(findmnt -no SOURCE /newroot/media/gk3)"
    echo "MEDIA_RW=$(findmnt -no OPTIONS /newroot/media/gk3 | cut -d, -f1)"
    echo "RUN_FS=$(findmnt -no FSTYPE /newroot/run)"
    echo "MNT=$(findmnt -no SOURCE /mnt)"
    echo "ROOT_FS=$(findmnt -no FSTYPE /newroot)"
    sed 's/^/REC_/' /newroot/run/gaokun3/medium
    ( cd /newroot/media/gk3/gaokun3 && find . -type f | LC_ALL=C sort | xargs sha256sum ) > "$GK3_TEST_FILES"
}
EOF
sed -e '/^export PATH$/r '"$W/report.sh" \
    -e 's|for arg in \$(cat /proc/cmdline); do|for arg in $GK3_TEST_CMDLINE; do|' \
    -e 's|/proc/meminfo|${GK3_TEST_MEMINFO:-/proc/meminfo}|' \
    -e 's|^exec switch_root /newroot /sbin/init$|report; exit 0|' \
    -e 's|reboot -f|exit 99|' -e 's|sleep 60|:|g' \
    installer/tools/build/initramfs-init > "$W/init"
grep -q 'report; exit 0' "$W/init" && grep -q 'GK3_TEST_CMDLINE' "$W/init" || { echo "改副本失败（initramfs-init 的写法变了？）"; exit 1; }

run_init() {   # run_init <额外 cmdline> [meminfo 文件]
    GK3_TEST_CMDLINE="gk3.squash=/gaokun3/live.squashfs gk3.dev=${L}p1 $1" GK3_TEST_MEMINFO=${2:-} GK3_TEST_FILES=$W/files \
        unshare -m sh -c "mount --make-rprivate / && sh $W/init" 2>&1
}

echo "═══ 1. 内置盘：读进内存 ═══"
out=$(run_init "")
printf '%s\n' "$out" | grep -E '^\[gk3\]' | sed 's/^/    /'
check "介质是 tmpfs（内存里那份）、只读" test "$(val "$out" MEDIA_FS)/$(val "$out" MEDIA_RW)" = tmpfs/ro
check "新根是 overlay、/run 是 tmpfs" test "$(val "$out" ROOT_FS)/$(val "$out" RUN_FS)" = overlay/tmpfs
check "介质分区卸下了" test -z "$(val "$out" MNT)"
check "内容与介质上的 gaokun3/ 逐字节相同" test "$(cat "$W/files")" = "$want"
n=$(basename "${L}p1")
check "记录：part / disk / start / size 对、ram=yes" \
    test "$(val "$out" REC_part)/$(val "$out" REC_disk)/$(val "$out" REC_start)/$(val "$out" REC_size)/$(val "$out" REC_ram)" = \
         "$n/$(basename "$L")/$(cat /sys/class/block/$n/start)/$(cat /sys/class/block/$n/size)/yes"

echo "═══ 2. gk3.toram=0：直接挂着 ═══"
out=$(run_init "gk3.toram=0")
check "介质是 vfat、来源是介质分区" test "$(val "$out" MEDIA_FS)/$(val "$out" MEDIA_SRC)" = "vfat/${L}p1"
check "记录 ram=no" test "$(val "$out" REC_ram)" = no
check "内容照样看得到" test "$(cat "$W/files")" = "$want"

echo "═══ 3. 内存不够：不读 ═══"
printf 'MemTotal: 2000000 kB\nMemAvailable: 1000000 kB\n' > "$W/meminfo"
out=$(run_init "" "$W/meminfo")
check "说明不够、照旧直接挂着" sh -c 'printf "%s\n" "$1" | grep -q "not enough memory" && test "$2" = vfat' _ "$out" "$(val "$out" MEDIA_FS)"
check "记录 ram=no" test "$(val "$out" REC_ram)" = no

echo
echo "合计：通过 $PASS，失败 $FAIL"
[ "$FAIL" = 0 ]
