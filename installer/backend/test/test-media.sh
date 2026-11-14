#!/usr/bin/env bash
# 制作安装介质用的 ISO 读取（src/iso9660.c）与 FAT32 写入（src/fatw.c）的容器测试。对照的都是独立的实现：
#   xorriso 造 ISO；fsck.fat 查文件系统；mtools 与内核的 vfat 取回文件，与源文件逐字节比对。
#
#   bash installer/tools/test/test-in-container.sh installer/backend/test/test-media.sh
#
# ⚠️ 只动容器里的文件与 loop 设备。
set -u
cd "$(dirname "$0")/../../.."
[ "$(id -u)" = 0 ] || { echo "要 root（用 installer/tools/test/test-in-container.sh 跑）"; exit 2; }
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ✓ $*"; }
bad() { FAIL=$((FAIL+1)); echo "  ✗ $*"; }
check() { local m=$1; shift; if "$@"; then ok "$m"; else bad "$m"; fi; }

W=$(mktemp -d /tmp/gk3-media.XXXX)
cleanup() {
    umount "$W/mnt" 2>/dev/null
    for img in "$W"/*.img; do
        for l in $(losetup -j "$img" 2>/dev/null | cut -d: -f1); do losetup -d "$l" 2>/dev/null; done
    done
    rm -rf "$W"
}
trap cleanup EXIT
export MTOOLS_SKIP_CHECK=1
# mtools 按 locale 转换长文件名：不是 UTF-8 时中文名会变成下划线
export LANG=C.UTF-8 LC_ALL=C.UTF-8

echo "═══ 编 media_host ═══"
B=installer/backend
gcc -O2 -w -I$B/third_party/zstd/lib -c -o "$W/zstd.o" $B/third_party/zstd/zstd_dec.c || { echo "zstd 编不过"; exit 1; }
gcc -std=gnu11 -O1 -g -Wall -Wextra -Wshadow -Wno-unused-parameter -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Itools/gk3boot/core/include -I$B/third_party/zstd/lib -o "$W/mh" $B/test/media_host.c $B/src/*.c tools/gk3boot/core/src/*.c "$W/zstd.o" \
    || { echo "media_host 编不过"; exit 1; }
M=$W/mh
ok "ASan 版编出来了"

# 两棵树逐个文件比：路径集合相同、内容相同
same_tree() {
    local a=$1 b=$2
    [ "$(cd "$a" && find . -type f | LC_ALL=C sort)" = "$(cd "$b" && find . -type f | LC_ALL=C sort)" ] || return 1
    [ "$(cd "$a" && find . -type d | LC_ALL=C sort)" = "$(cd "$b" && find . -type d | LC_ALL=C sort)" ] || return 1
    diff -r "$a" "$b" >/dev/null
}

echo "═══ 1. ISO：Joliet 名字、偏移与大小 ═══"
S=$W/src
mkdir -p "$S/EFI/BOOT" "$S/loader/entries" "$S/gaokun3" "$S/深一层/a/b/c/d/e" "$S/many"
head -c 114688 /dev/urandom > "$S/EFI/BOOT/BOOTAA64.EFI"
printf 'timeout 5\n' > "$S/loader/loader.conf"
printf 'title gaokun3 installer\n' > "$S/loader/entries/gaokun3-live.conf"
head -c 5000000 /dev/urandom > "$S/gaokun3/Image"
head -c 20000000 /dev/urandom > "$S/gaokun3/live.squashfs"
: > "$S/gaokun3/empty.txt"
head -c 2048 /dev/urandom > "$S/gaokun3/exactly-2048.bin"
head -c 1000 /dev/urandom > "$S/中文 名字.txt"
# Joliet 的长文件名最多 103 个字符（xorriso -joliet-long），超过的会被截断：用 100 个字符
LONG=$(printf 'L%.0s' $(seq 1 96)).bin
head -c 10 /dev/urandom > "$S/$LONG"
head -c 10 /dev/urandom > "$S/longfilename1.txt"
head -c 10 /dev/urandom > "$S/longfilename2.txt"
for i in $(seq 1 300); do echo "$i" > "$S/many/file-with-a-long-name-$i.txt"; done
echo deep > "$S/深一层/a/b/c/d/e/deep.txt"
# -input-charset：容器里不是 UTF-8 的 locale，不给的话 xorriso 把中文名换成下划线
xorriso -as mkisofs -input-charset utf-8 -o "$W/t.iso" -V GK3LIVE -R -J -joliet-long "$S" >/dev/null 2>&1 || { bad "xorriso 造不出 ISO"; exit 1; }
"$M" iso-ls "$W/t.iso" > "$W/ls.txt"; rc=$?
check "iso-ls 成功" test $rc = 0
check "文件路径与源目录一致（Joliet 长名字、中文、空格、100 字符、深层目录）" \
    test "$(sed -n 's/^F \(.*\) [0-9]* [0-9]*$/\1/p' "$W/ls.txt" | LC_ALL=C sort)" = "$(cd "$S" && find . -type f | sed 's#^\./##' | LC_ALL=C sort)"
check "目录与源目录一致" \
    test "$(sed -n 's/^D //p' "$W/ls.txt" | LC_ALL=C sort)" = "$(cd "$S" && find . -mindepth 1 -type d | sed 's#^\./##' | LC_ALL=C sort)"
check "每个目录都排在它里面的条目之前" python3 -I - "$W/ls.txt" <<'PY'
import sys
seen = set()
for line in open(sys.argv[1], encoding='utf-8'):
    kind, rest = line.rstrip('\n').split(' ', 1)
    path = rest if kind == 'D' else rest.rsplit(' ', 2)[0]
    parent = path.rsplit('/', 1)[0] if '/' in path else ''
    if parent and parent not in seen:
        sys.exit(1)
    if kind == 'D':
        seen.add(path)
PY
check "按读到的偏移与大小取出的内容与源文件逐字节相同" python3 -I - "$W/ls.txt" "$W/t.iso" "$S" <<'PY'
import os, sys
iso = open(sys.argv[2], 'rb')
for line in open(sys.argv[1], encoding='utf-8'):
    if not line.startswith('F '):
        continue
    path, size, off = line.rstrip('\n')[2:].rsplit(' ', 2)
    iso.seek(int(off))
    if iso.read(int(size)) != open(os.path.join(sys.argv[3], path), 'rb').read():
        print('differs:', path)
        sys.exit(1)
PY
xorriso -as mkisofs -o "$W/nojoliet.iso" -V X -R "$S/loader" >/dev/null 2>&1
check "没有 Joliet 的 ISO：拒绝" sh -c '! "$1" iso-ls "$2" 2>&1 | grep -q . || "$1" iso-ls "$2" 2>&1 | grep -q "no Joliet"' _ "$M" "$W/nojoliet.iso"
check "不是 ISO：拒绝" sh -c '"$1" iso-ls "$2" 2>&1 | grep -q "not an ISO"' _ "$M" "$S/gaokun3/Image"

# 期望的 FAT 内容：源目录 + 安装载荷
E=$W/expect; cp -a "$S" "$E"; mkdir -p "$E/gaokun3/payload"
head -c 3000000 /dev/urandom > "$E/gaokun3/payload/super.img.zst"
printf 'x  boot.img\n' > "$E/gaokun3/payload/install-artifacts.sha256"
PAY=(gaokun3/payload/super.img.zst="$E/gaokun3/payload/super.img.zst" gaokun3/payload/install-artifacts.sha256="$E/gaokun3/payload/install-artifacts.sha256")

fsck_clean() { fsck.fat -n -V "$1" >"$W/fsck.txt" 2>&1 && ! grep -qiE 'dirty|error|corrupt|invalid|differ|duplicate|lost' "$W/fsck.txt"; }
mount_same() {   # mount_same <镜像> [扇区]：内核 vfat 只读挂上，与期望的树比对
    local img=$1 sec=${2:-512} l r
    mkdir -p "$W/mnt"
    l=$(losetup -f --show -b "$sec" -r "$img") || return 1
    mount -o ro,utf8 -t vfat "$l" "$W/mnt" || { losetup -d "$l"; return 1; }
    same_tree "$W/mnt" "$E"; r=$?
    umount "$W/mnt"; losetup -d "$l"; return $r
}

echo "═══ 2. ISO → FAT32（512 字节扇区，96 MiB）═══"
"$M" iso2fat "$W/t.iso" "$W/fat512.img" $((96 << 20)) 512 GK3LIVE "${PAY[@]}"; rc=$?
check "写成功" test $rc = 0
check "fsck.fat -n：没有任何问题" fsck_clean "$W/fat512.img"
minfo -i "$W/fat512.img" :: > "$W/minfo.txt" 2>&1
check "BPB：512 字节扇区、隐藏扇区 2048（分区起点 1 MiB）" sh -c 'grep -q "sector size: 512" "$1" && grep -q "hidden sectors: 2048" "$1"' _ "$W/minfo.txt"
check "卷标 GK3LIVE（引导扇区与根目录卷标项）" sh -c 'mlabel -s -i "$1" :: | grep -q "GK3LIVE" && grep -q "disk label=\"GK3LIVE" "$2"' _ "$W/fat512.img" "$W/minfo.txt"
mkdir -p "$W/m"; mcopy -s -n -i "$W/fat512.img" '::/*' "$W/m/" 2>/dev/null
check "mtools 取回的整棵树与期望逐字节相同（长文件名都在）" same_tree "$W/m" "$E"
check "内核 vfat 挂上看到的整棵树与期望逐字节相同" mount_same "$W/fat512.img"
mdir -i "$W/fat512.img" ::/ > "$W/mdir.txt" 2>&1
check "8.3 短名不重复：LONGFI~1 与 LONGFI~2" sh -c 'grep -q "LONGFI~1 TXT" "$1" && grep -q "LONGFI~2 TXT" "$1"' _ "$W/mdir.txt"

echo "═══ 3. 4096 字节扇区 ═══"
"$M" iso2fat "$W/t.iso" "$W/fat4k.img" $((300 << 20)) 4096 GK3LIVE "${PAY[@]}"; rc=$?
check "写成功" test $rc = 0
check "fsck.fat -n：没有任何问题" fsck_clean "$W/fat4k.img"
check "内核 vfat（4096 字节扇区的 loop）挂上看到的树与期望相同" mount_same "$W/fat4k.img" 4096

echo "═══ 4. 簇大小随分区变 ═══"
"$M" fat-mk "$W/big.img" $((9 << 30)) 512 BIG a.bin="$S/gaokun3/Image" >/dev/null; rc=$?
check "9 GiB：写成功、fsck.fat 干净" sh -c 'test "$1" = 0 && fsck.fat -n "$2" >/dev/null 2>&1' _ $rc "$W/big.img"
check "9 GiB：簇 8 KiB（Windows 的默认）" sh -c 'minfo -i "$1" :: 2>/dev/null | grep -q "cluster size: 16 sectors"' _ "$W/big.img"
"$M" fat-mk "$W/small.img" $((40 << 20)) 512 SMALL a.bin="$S/gaokun3/Image" >/dev/null; rc=$?
check "40 MiB：簇不够 4 KiB 时减小簇，仍是 FAT32、fsck.fat 干净" \
    sh -c 'test "$1" = 0 && fsck.fat -n "$2" >/dev/null 2>&1 && minfo -i "$2" :: 2>/dev/null | grep -q "cluster size: 1 sectors"' _ $rc "$W/small.img"

echo "═══ 5. 拒绝 ═══"
err_of() { "$M" "$@" 2>/dev/null | sed -n 's/^FATERR //p'; }
check "重名（不分大小写）→ ARG" test "$(err_of fat-mk "$W/x.img" $((64 << 20)) 512 X a.txt="$S/loader/loader.conf" A.TXT="$S/loader/loader.conf")" = 1
check "名字里有 ':' → ARG" test "$(err_of fat-mk "$W/x.img" $((64 << 20)) 512 X 'a:b'="$S/loader/loader.conf")" = 1
check "路径里有 '.' 段 → ARG" test "$(err_of fat-mk "$W/x.img" $((64 << 20)) 512 X 'a/./b'="$S/loader/loader.conf")" = 1
check "名字以点结尾 → ARG" test "$(err_of fat-mk "$W/x.img" $((64 << 20)) 512 X 'a.'="$S/loader/loader.conf")" = 1
check "文件与目录同名 → ARG" test "$(err_of fat-mk "$W/x.img" $((64 << 20)) 512 X a="$S/loader/loader.conf" a/b="$S/loader/loader.conf")" = 1
check "卷标里有小写 → ARG" test "$(err_of fat-mk "$W/x.img" $((64 << 20)) 512 lower a="$S/loader/loader.conf")" = 1
check "1 MiB 的分区 → TOO_SMALL" test "$(err_of fat-mk "$W/x.img" $((1 << 20)) 512 X a="$S/loader/loader.conf")" = 2
check "放不下 → NO_SPACE" test "$(err_of fat-mk "$W/x.img" $((40 << 20)) 512 X a="$S/gaokun3/live.squashfs" b="$S/gaokun3/live.squashfs" c="$S/gaokun3/live.squashfs")" = 3

echo "═══ 6. fat-size 给的大小确实放得下 ═══"
ALL=(); while IFS= read -r f; do ALL+=("${f#./}=$E/${f#./}"); done < <(cd "$E" && find . -type f)
SZ=$("$M" fat-size 512 "${ALL[@]}")
check "fat-size 给了一个按 1 MiB 取整的大小（$SZ 字节）" test -n "$SZ" -a "$((SZ % (1 << 20)))" = 0
"$M" fat-mk "$W/sized.img" "$SZ" 512 SIZED "${ALL[@]}" >/dev/null; rc=$?
check "按这个大小写成功、fsck.fat 干净、内容与期望相同" sh -c 'test "$1" = 0' _ $rc
check "  …fsck.fat 干净" fsck_clean "$W/sized.img"
check "  …内核挂上的树与期望相同" mount_same "$W/sized.img"

echo
echo "合计：通过 $PASS，失败 $FAIL"
[ "$FAIL" = 0 ]
