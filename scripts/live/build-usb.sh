#!/usr/bin/env bash
# 把 LiveCD 组装成一个可启动的 U 盘镜像（GPT + 单个 ESP，卷标 GK3LIVE）。
#
#   bash scripts/live/build-usb.sh \
#        --squashfs  <gaokun3-live.squashfs> --initramfs <initramfs.img> \
#        --kernel    <Image（EFI stub）>      --dtb       <gaokun3.dtb> \
#        --sdboot    <systemd-bootaa64.efi>  --cmdline   <boot.img 的 cmdline.txt> \
#        [--payload <发布目录>] [--release-info <release.txt>] [--size MiB] \
#        --out <gaokun3-live.img>
#
#   --cmdline  启动项的内核参数从 boot.img 的 cmdline 派生（gk3-bootimg.py 拆出来的那份）：LiveCD 与 Android 用同一个内核，
#              参数也跟着同一份走，去掉只给 Android 的（见下面 live_cmdline）。
#   --payload  把发布目录（boot.img、super.img.zst、校验清单）放进 gaokun3/payload/：安装器从这里装，不联网。
#              CI 构建的镜像不带，由用户放入，或由 Windows 安装器制作安装介质时写入。
#
# U 盘上的布局：
#   EFI/BOOT/BOOTAA64.EFI           systemd-boot
#   loader/loader.conf、loader/entries/gaokun3-live.conf
#   gaokun3/Image、gaokun3.dtb、initramfs.img、live.squashfs、release.txt
#   gaokun3/payload/                （可选）
#
# ★ 用 mtools 往 FAT 里写文件，【不需要 root】，也不需要 loop 设备：不会因为一次手滑把宿主机的分区写了。
#
# 写盘： sudo dd if=gaokun3-live.img of=/dev/sdX bs=4M conv=fsync status=progress
set -euo pipefail

die() { echo "!! $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }
ok()  { echo "   ✓ $*"; }

# 启动项的内核参数：boot.img 的 cmdline 去掉只给 Android 的（androidboot.*、init=、firmware_class.path=），
# 加上 LiveCD 自己的。统一启动入口的执行端用的是同一条规则（tools/gk3boot/core/src/cmdline.c）
live_cmdline() {
    local keep
    keep=$(printf '%s\n' "$1" | tr ' ' '\n' \
           | grep -v -e '^androidboot\.' -e '^init=' -e '^firmware_class\.path=' -e '^$' \
           | tr '\n' ' ')
    echo "${keep}loglevel=3 panic=10 gk3.squash=/gaokun3/live.squashfs"
}

SQUASH=; INITRAMFS=; KERNEL=; DTB=; SDBOOT=; PAYLOAD=; OUT=; SIZE_MIB=; CMDLINE=; RELINFO=
while [ $# -gt 0 ]; do
    case "$1" in
        --squashfs)  SQUASH=$2; shift 2 ;;
        --initramfs) INITRAMFS=$2; shift 2 ;;
        --kernel)    KERNEL=$2; shift 2 ;;
        --dtb)       DTB=$2; shift 2 ;;
        --sdboot)    SDBOOT=$2; shift 2 ;;
        --payload)   PAYLOAD=$2; shift 2 ;;
        --cmdline)   CMDLINE=$2; shift 2 ;;
        --release-info) RELINFO=$2; shift 2 ;;
        --size)      SIZE_MIB=$2; shift 2 ;;
        --out)       OUT=$2; shift 2 ;;
        *) die "不认识的参数：$1" ;;
    esac
done
for v in SQUASH INITRAMFS KERNEL DTB SDBOOT CMDLINE OUT; do
    eval "x=\${$v}"
    [ -n "$x" ] || die "缺参数 --$(echo "$v" | tr 'A-Z' 'a-z')"
done
for f in "$SQUASH" "$INITRAMFS" "$KERNEL" "$DTB" "$SDBOOT" "$CMDLINE"; do
    [ -f "$f" ] || die "文件不在：$f"
done
head -c 2 "$SDBOOT" | grep -q MZ || die "$SDBOOT 不是 PE 文件"
head -c 2 "$KERNEL" | grep -q MZ || die "$KERNEL 不是 PE 文件（要 EFI stub 内核 / vmlinuz.efi）"
[ "$(od -An -tx1 -N4 "$DTB" | tr -d ' ')" = "d00dfeed" ] || die "$DTB 不是 FDT（magic 不对）"
ok "输入体检通过（PE / PE / FDT）"

for t in sgdisk mformat mmd mcopy; do
    command -v "$t" >/dev/null || die "缺工具：${t}（apt install gdisk mtools）"
done

# —— 算大小 ——
need=0
for f in "$SQUASH" "$INITRAMFS" "$KERNEL" "$DTB" "$SDBOOT"; do
    need=$(( need + $(stat -c %s "$f") ))
done
if [ -n "$PAYLOAD" ]; then
    [ -d "$PAYLOAD" ] || die "--payload 不是目录：$PAYLOAD"
    need=$(( need + $(du -sb "$PAYLOAD" | cut -f1) ))
fi
# 25% 余量 + 64 MiB 底
MIN=$(( need / 1048576 * 125 / 100 + 64 ))
SIZE_MIB=${SIZE_MIB:-$MIN}
[ "$SIZE_MIB" -ge "$MIN" ] || die "--size $SIZE_MIB MiB 不够，至少要 $MIN MiB"
say "镜像 ${SIZE_MIB} MiB（内容 $(( need / 1048576 )) MiB）"

PART_OFF=1048576   # 1 MiB 对齐
rm -f "$OUT"
truncate -s "${SIZE_MIB}M" "$OUT"

# —— GPT + ESP ——
sgdisk --zap-all "$OUT" >/dev/null
sgdisk -n 1:2048:0 -t 1:ef00 -c 1:"esp" "$OUT" >/dev/null
ok "GPT + ESP 分区"

mformat -i "$OUT@@$PART_OFF" -F -v GK3LIVE ::
ok "FAT32（卷标 GK3LIVE）"

M() { mcopy -i "$OUT@@$PART_OFF" -o "$@"; }
mmd -i "$OUT@@$PART_OFF" ::/EFI ::/EFI/BOOT ::/loader ::/loader/entries ::/gaokun3

M "$SDBOOT"    ::/EFI/BOOT/BOOTAA64.EFI
M "$KERNEL"    ::/gaokun3/Image
M "$DTB"       ::/gaokun3/gaokun3.dtb
M "$INITRAMFS" ::/gaokun3/initramfs.img
M "$SQUASH"    ::/gaokun3/live.squashfs
ok "引导链 + 内核 + initramfs + squashfs"
if [ -n "$RELINFO" ]; then M "$RELINFO" ::/gaokun3/release.txt; ok "版本信息 gaokun3/release.txt（$(sed -n 's/^GK3_INSTALLER_VERSION=//p' "$RELINFO")）"; fi

if [ -n "$PAYLOAD" ]; then
    mmd -i "$OUT@@$PART_OFF" ::/gaokun3/payload
    for f in "$PAYLOAD"/*; do
        [ -f "$f" ] || continue
        M "$f" "::/gaokun3/payload/$(basename "$f")"
    done
    ok "带上了安装载荷（$(ls -1 "$PAYLOAD" | wc -l) 个文件）"
fi

OPTS=$(live_cmdline "$(tr -d '\r\n' < "$CMDLINE")")
ok "内核参数从 $(basename "$CMDLINE") 派生"
TMP=$(mktemp)
# ⚠️ 【不要】设 timeout 0 —— 万一起不来，用户连菜单都进不去
printf 'timeout 5\nconsole-mode keep\neditor no\n' > "$TMP"
M "$TMP" ::/loader/loader.conf
# ⚠️ 标题用 ASCII：开机菜单是 UEFI 固件用它自己的字体画的，一般不含中文
printf 'title      gaokun3 installer\nversion    live\nsort-key   live0\nlinux      /gaokun3/Image\ndevicetree /gaokun3/gaokun3.dtb\ninitrd     /gaokun3/initramfs.img\noptions    %s\n' \
    "$OPTS" > "$TMP"
M "$TMP" ::/loader/entries/gaokun3-live.conf
rm -f "$TMP"
ok "启动项 gaokun3-live.conf"

say "体检"
LIST=$(mdir -i "$OUT@@$PART_OFF" -b ::/gaokun3 ::/EFI/BOOT ::/loader/entries 2>/dev/null)
for want in Image gaokun3.dtb initramfs.img live.squashfs BOOTAA64.EFI gaokun3-live.conf; do
    printf '%s' "$LIST" | grep -qi "$want" && ok "$want" || die "镜像里没有 $want"
done
echo
echo "$OUT  $(du -h "$OUT" | cut -f1)"
echo "写盘： sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
