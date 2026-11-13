#!/usr/bin/env bash
# 打执行端 initramfs fastboot.img（容器内跑；宿主上用 scripts/gk3boot/build-fastboot-img.sh）。
#
#   bash tools/gk3boot/initramfs/build.sh --out <目录> [--fastbootd <静态 aarch64 gk3-fastbootd> | --no-fastbootd]
#
# 内容（见 tools/gk3boot/README.md）：静态 busybox + 我们的 /init + gk3-fbi（按键 / 只读状态，
# 链接 libgk3core）+ Terminus 32x16 控制台字体 + gk3-fastbootd（协议守护进程）。
# gk3-fastbootd 缺省就在这里从 tools/gk3boot/fastbootd/ 用 musl 静态编（与 gk3-fbi 同一套工具链，比 glibc 静态小一半，
# 版本串取环境变量 FBD_VER）；--fastbootd 换成外面给的二进制；--no-fastbootd 打一份只有界面的（EXECUTOR MISSING）。
# 不带模块（本机内核全内建）、不带固件、不带 python。
# 尺寸预算：2–4 MiB（见 tools/gk3boot/README.md）—— 超过 4 MiB 直接失败。
# 可复现：mtime 固定为 SOURCE_DATE_EPOCH（缺省 0）、属主 0:0、按名字排序、gzip -n。同一份输入 → 同一个 sha256。
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GK3=$(cd "$HERE/.." && pwd)
OUT=; FBD=; NO_FBD=0
BUDGET=$((4 * 1024 * 1024))
BB=/usr/bin/busybox
FONT_SRC=/usr/share/consolefonts/Uni2-TerminusBold32x16.psf.gz
EPOCH=${SOURCE_DATE_EPOCH:-0}

die() { echo "✗ $*" >&2; exit 1; }
ok()  { echo "   ✓ $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT=$2; shift 2 ;;
        --fastbootd) FBD=$2; shift 2 ;;
        --no-fastbootd) NO_FBD=1; shift ;;
        *) die "不认识的参数：$1" ;;
    esac
done
[ -n "$OUT" ] || die "要 --out <目录>"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# 断言"静态 aarch64"：判失败条件（动态 / 带解释器），不枚举成功条件 —— 理由见 installer/tools/build/build-initramfs.sh
assert_static() {
    local f=$1 info
    info=$(file -b "$f")
    case "$info" in *"ARM aarch64"*) ;; *) die "$f 不是 aarch64：$info" ;; esac
    case "$info" in
        *"dynamically linked"*|*interpreter*) die "$f 是动态链接的：${info}（initramfs 里没有动态链接器）" ;;
        *statically*|*"static-pie"*) ;;
        *) die "认不出 $f 的链接方式：$info" ;;
    esac
}

# —— gk3-fbi（musl 静态）——
B=$OUT/obj
rm -rf "$B"; mkdir -p "$B"
# -Wno-format-truncation：sysfs / dev 的名字远短于缓冲区（nvme0n1p4、event3），截断只会让"找不到"，不会"找错"
CFLAGS="-std=c11 -Os -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format-truncation -ffunction-sections -fdata-sections"
musl-gcc $CFLAGS -static -isystem /opt/kh -I"$GK3/core/include" -Wl,--gc-sections -s \
    -o "$B/gk3-fbi" "$HERE/gk3-fbi.c" "$GK3"/core/src/*.c
assert_static "$B/gk3-fbi"
ok "gk3-fbi $(stat -c %s "$B/gk3-fbi") 字节（musl 静态）"

# —— 根目录 ——
# 根目录摆在容器自己的文件系统里：宿主挂进来的目录（colima 的 virtiofs）不让 mknod
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
D=$STAGE/root
mkdir -p "$D"/{bin,sbin,proc,sys,dev,run,tmp,etc,usr/share/gk3}
# /dev/console：内核在跑 /init 之前就要打开它当 0/1/2。内核自带的默认 initramfs 一般已有，
# 但别靠它（CONFIG_INITRAMFS_SOURCE 一改就没了）。容器里是 root，mknod 能用。
mknod -m 600 "$D/dev/console" c 5 1 || die "mknod /dev/console 失败（要在容器里以 root 跑）"
install -m755 "$BB" "$D/bin/busybox"
assert_static "$D/bin/busybox"
# /init 用到的 applet（busybox 按 argv[0] 分派）。多给几个排障常用的（dd / hexdump / cmp）。
for a in sh mount umount mkdir cat echo sleep reboot poweroff dmesg mdev grep ls stty mkfifo \
         kill tail head stat sync printf ln rm test [ tr cut sed wc dd hexdump cmp uptime; do
    ln -s busybox "$D/bin/$a"
done
install -m755 "$HERE/init" "$D/init"
install -m755 "$B/gk3-fbi" "$D/bin/gk3-fbi"
gzip -dc "$FONT_SRC" > "$D/usr/share/gk3/font.psf"
[ "$(head -c 4 "$D/usr/share/gk3/font.psf" | od -An -tx1 | tr -d ' ')" = 72b54a86 ] || die "字体不是 PSF2"
if [ -z "$FBD" ] && [ "$NO_FBD" = 0 ]; then
    # 与 gk3-fbi 同一套 musl 工具链；-Wno-missing-field-initializers 同 tools/gk3boot/Makefile 的 FBD_FLAGS 外加
    FCF="-std=gnu11 -Os -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers -ffunction-sections -fdata-sections"
    musl-gcc $FCF -static -isystem /opt/kh -I"$GK3/core/include" -I"$GK3/fastbootd" \
        -DGK3FB_VERSION="\"${FBD_VER:-unknown}\"" -Wl,--gc-sections -s \
        -o "$B/gk3-fastbootd" "$GK3"/fastbootd/*.c "$GK3"/core/src/*.c -lpthread
    FBD=$B/gk3-fastbootd
    ok "gk3-fastbootd ${FBD_VER:-unknown}：$(stat -c %s "$FBD") 字节（musl 静态，从 tools/gk3boot/fastbootd 编）"
fi
if [ -n "$FBD" ]; then
    [ -f "$FBD" ] || die "--fastbootd $FBD 不存在"
    assert_static "$FBD"
    install -m755 "$FBD" "$D/bin/gk3-fastbootd"
    ok "带上 gk3-fastbootd（$(stat -c %s "$FBD") 字节）"
else
    echo "   ⚠️ 没给 gk3-fastbootd —— 这一份执行端只有界面，屏幕上会显示 \"EXECUTOR MISSING\""
fi
sh -n "$D/init" || die "/init 语法错"
# busybox ash 自己再查一遍（Debian 的 /bin/sh 是 dash，两者的语法检查不完全一样）
"$BB" sh -n "$D/init" || die "/init 在 busybox ash 下语法错"

# —— cpio（newc）+ gzip，可复现 ——
find "$D" -exec touch -h -d "@$EPOCH" {} +
IMG=$OUT/fastboot.img
( cd "$D" && find . -mindepth 1 | LC_ALL=C sort | cpio -o -H newc -R 0:0 --reproducible --quiet ) | gzip -9 -n > "$IMG.new"
sz=$(stat -c %s "$IMG.new")
[ "$sz" -le "$BUDGET" ] || die "fastboot.img $sz 字节，超过 4 MiB 预算"
mv "$IMG.new" "$IMG"
( cd "$OUT" && sha256sum fastboot.img > fastboot.img.sha256 )
( cd "$D" && find . -mindepth 1 | LC_ALL=C sort | while read -r f; do
      if [ -L "$f" ]; then echo "$f -> $(readlink "$f")"; elif [ -c "$f" ]; then echo "$f char $(stat -c '%t,%T' "$f")"; elif [ -f "$f" ]; then echo "$f $(stat -c %s "$f")"; else echo "$f/"; fi
  done ) > "$OUT/fastboot.img.manifest"
ok "fastboot.img $sz 字节（$(awk "BEGIN{printf \"%.2f\", $sz/1048576}") MiB，预算 4 MiB） sha256 $(cut -c1-16 "$OUT/fastboot.img.sha256")…"

# —— 回解体检 ——
T=$(mktemp -d)
( cd "$T" && gzip -dc "$IMG" | cpio -idm --quiet )
[ -x "$T/init" ] || die "回解：/init 不可执行"
[ -x "$T/bin/busybox" ] && [ -x "$T/bin/gk3-fbi" ] || die "回解：缺 busybox / gk3-fbi"
[ -L "$T/bin/sh" ] || die "回解：缺 /bin/sh"
[ -c "$T/dev/console" ] || die "回解：缺 /dev/console"
cmp -s "$T/init" "$HERE/init" || die "回解：/init 与源文件不同"
rm -rf "$T"
ok "回解体检通过"
