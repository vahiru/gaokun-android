#!/usr/bin/env bash
# 编 Windows 版 mke2fs.exe（Windows 安装器格式化 metadata / userdata 用，见 ../src/win/apply.c）。
#
#   bash installer/backend/mke2fs/build.sh x64|arm64|native
#     → installer/backend/build/mke2fs-<目标>/{mke2fs(.exe), mke2fs.conf}
#
# 源码：Debian 的 e2fsprogs 1.47.2-3（与 LiveCD 的 Debian 13 同一版本：installer/tools/build/packages-live.lock），
#   上游 1.47.2 原始源码 + Debian 的补丁 + 本仓 patches/e2fsprogs/。下载的两个包按 sha256 钉死。
# mke2fs.conf 取自源码的 misc/mke2fs.conf.in：与 Debian 13 的 /etc/mke2fs.conf 一字不差，
#   所以格式化出来的特性、块大小、inode 大小与 LiveCD 的 mkfs.ext4 相同。
# -E android_sparse 要 AOSP 的 libsparse，那份是 Apache-2.0、与 GPLv2 的 mke2fs 不兼容：换成 sparse_shim/（自写，MIT）。
# native：同一份源码编成 Linux 本机版（ASan/UBSan），对拍 Debian 的 mkfs.ext4 用（Windows 版在 mingw 下读不到
#   E2FSPROGS_FAKE_TIME：include/mingw/unistd.h 里 geteuid() 是 1，ext2fs_safe_getenv 一律返回空）。
#
# 工具链：llvm-mingw（LLVM_MINGW，默认同 ../Makefile）。需要 curl、tar、patch、本机 cc。
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
TARGET=${1:-}
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261104-ucrt-ubuntu-22.04-x86_64}
die() { echo "FAIL: $*" >&2; exit 1; }

VER=1.47.2
DEBREV=3
BASE=https://deb.debian.org/debian/pool/main/e/e2fsprogs
ORIG=e2fsprogs_${VER}.orig.tar.gz
ORIG_SHA=6dcd67ff9d8b13274ba3f088e4318be4f5b71412cd863524423fc49d39a6371f
DEB=e2fsprogs_${VER}-${DEBREV}.debian.tar.xz
DEB_SHA=5517aae5ce5196e49fa314492f0639ad7a1692c9521d703b6c81acff77a1564e

case "$TARGET" in
    x64) HOST=x86_64-w64-mingw32 ;;
    arm64) HOST=aarch64-w64-mingw32 ;;
    native) HOST= ;;
    *) die "usage: $0 x64|arm64|native" ;;
esac

B=$HERE/../build
DL=$B/e2fsprogs-dl
W=$B/e2fsprogs-$TARGET
OUT=$B/mke2fs-$TARGET
mkdir -p "$DL"

fetch() {   # $1=文件 $2=sha256
    if [ ! -f "$DL/$1" ] || ! echo "$2  $DL/$1" | sha256sum -c --quiet - 2>/dev/null; then
        curl -fsSL --max-time 600 -o "$DL/$1.part" "$BASE/$1" || die "download failed: $1"
        echo "$2  $DL/$1.part" | sha256sum -c --quiet - || die "sha256 mismatch: $1"
        mv "$DL/$1.part" "$DL/$1"
    fi
}
fetch "$ORIG" "$ORIG_SHA"
fetch "$DEB" "$DEB_SHA"

# 每次从干净的源码开始
rm -rf "${W:?}"
mkdir -p "$W/obj"
tar xzf "$DL/$ORIG" -C "$W"
SRC=$W/e2fsprogs-$VER
tar xJf "$DL/$DEB" -C "$SRC"
while read -r p; do
    case "$p" in ''|'#'*) continue ;; esac
    patch -d "$SRC" -p1 -s < "$SRC/debian/patches/$p" || die "Debian patch does not apply: $p"
done < "$SRC/debian/patches/series"
for p in "$REPO"/patches/e2fsprogs/*.patch; do
    patch -d "$SRC" -p1 -s < "$p" || die "patch does not apply: ${p##*/}"
done

SHIM=$HERE/sparse_shim
if [ -n "$HOST" ]; then
    export PATH=$LLVM_MINGW/bin:$PATH
    CC=$HOST-clang
    F="-O2 -DENABLE_LIBSPARSE -I$SHIM"
    LD="-static"
    CONF_HOST=(--host="$HOST")
else
    CC=${CC:-clang}
    F="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DENABLE_LIBSPARSE -I$SHIM"
    LD="-fsanitize=address,undefined"
    CONF_HOST=()
fi
cd "$W/obj"
"$SRC/configure" ${CONF_HOST[@]+"${CONF_HOST[@]}"} CC="$CC" --disable-nls --disable-fuse2fs --disable-uuidd \
    --disable-defrag --disable-debugfs --disable-imager --disable-resizer --disable-e2initrd-helper --disable-tls \
    --disable-backtrace --without-pthread --disable-fsck --disable-elf-shlibs > configure.log 2>&1 \
    || { tail -20 configure.log; die "configure failed"; }
$CC $F -c -o sparse_shim.o "$SHIM/sparse_shim.c"
# 构建时在本机跑的小工具（util/subst 等）用本机的 cc，且不能带 configure 给目标加的 -I include/mingw
MK=(BUILD_CC=cc "BUILD_CFLAGS=-g -O2 -I." "CFLAGS=$F" "CFLAGS_STLIB=$F")
make subs "${MK[@]}" > make.log 2>&1 || { tail -20 make.log; die "make subs failed"; }
# 只编 mke2fs 用到的库（lib/ss 是 debugfs 用的，在 mingw 下编不过）
for d in lib/et lib/uuid lib/blkid lib/e2p lib/support lib/ext2fs; do
    [ -d "$d" ] || continue
    make -C "$d" -j"$(nproc)" "${MK[@]}" >> make.log 2>&1 || { tail -20 make.log; die "building $d failed"; }
done
make -C misc mke2fs "${MK[@]}" "SYSLIBS=$W/obj/sparse_shim.o" "ALL_LDFLAGS=$LD" >> make.log 2>&1 \
    || { tail -20 make.log; die "building mke2fs failed"; }

rm -rf "${OUT:?}"
mkdir -p "$OUT"
if [ -n "$HOST" ]; then cp misc/mke2fs.exe "$OUT/"; else cp misc/mke2fs "$OUT/"; fi
cp "$SRC/misc/mke2fs.conf.in" "$OUT/mke2fs.conf"
( cd "$OUT" && sha256sum * )
echo "OK: $OUT"
