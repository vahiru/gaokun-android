#!/usr/bin/env bash
# 把LiveCD组装成混合ISO。同一个文件有两种用法：
#   * 在Windows资源管理器中打开ISO，把EFI、loader、gaokun3三个目录拖进格式化为FAT32的U盘；
#   * 用dd或Rufus的DD模式把ISO整个写入U盘。
#
#   bash installer/tools/build/build-iso.sh \
#        --squashfs  <gaokun3-live.squashfs> --initramfs <initramfs.img> \
#        --kernel    <Image（EFI stub）>      --dtb       <gaokun3.dtb> \
#        --sdboot    <systemd-bootaa64.efi>  --cmdline   <boot.img的cmdline.txt> \
#        --kconfig   <内核的.config> [--allow-no-iso9660] \
#        [--payload <发布目录>] [--release-info <release.txt>] \
#        --out <gaokun3-live.iso>
#
#   --cmdline  启动项的内核参数从boot.img的cmdline派生（gk3-bootimg.py拆出来的那份）：LiveCD与Android用同一个内核，
#              参数也跟着同一份走，去掉只给Android的（见下面live_cmdline）。
#   --kconfig  内核编译时的配置（scripts/extract-kconfig.py从boot.img中提取）。dd写入后initramfs要从ISO9660分区读取
#              squashfs，内核没有开启CONFIG_ISO9660_FS时这条路走不通，默认拒绝构建；--allow-no-iso9660只供测试构建使用
#              （拖放方式不受影响）。
#   --payload  把发布目录（boot.img、super.img.zst、校验清单）放进gaokun3/payload/：安装器从这里安装，不联网。
#              CI构建的ISO不带，由用户放入，或由Windows安装器制作安装介质时写入。
#
# ISO的布局：
#   ISO9660文件树（Rock Ridge给Linux长文件名，Joliet给Windows长文件名）是U盘的完整内容：
#     EFI/BOOT/BOOTAA64.EFI           systemd-boot
#     loader/loader.conf、loader/entries/gaokun3-live.conf
#     gaokun3/Image、gaokun3.dtb、initramfs.img、live.squashfs、release.txt
#     gaokun3/payload/                （可选）
#   GPT分区1（ISO9660）：-partition_offset 16让它从分区起点就能按ISO9660挂载，initramfs只扫描分区、不挂整块盘。
#   GPT分区2（EFI系统分区，FAT，附加在ISO之后）：上面除live.squashfs与payload以外的文件。dd写入后固件从这里启动
#     systemd-boot，内核与initramfs也从这里读取。El Torito的UEFI启动项指向同一块数据，只是为了让按El Torito判断ISO
#     能否启动的工具认出它；LiveCD本身只能从U盘运行（内核没有光驱驱动，initramfs只扫描分区）。
# -no-pad：xorriso默认在ISO末尾补的300 KiB会变成一个没有文件系统的GPT分区，dd写入后Windows会提示格式化它。
# ⚠️ 不能去掉-J：没有Joliet时资源管理器只显示8.3文件名，拖进U盘的live.squashfs会变成LIVE.SQU，initramfs找不到它。
#
# 写盘：sudo dd if=gaokun3-live.iso of=/dev/sdX bs=4M conv=fsync status=progress
set -euo pipefail

die() { echo "!! $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }
ok()  { echo "   ✓ $*"; }

# 启动项的内核参数：boot.img的cmdline去掉只给Android的（androidboot.*、init=、firmware_class.path=），
# 加上LiveCD自己的。统一启动入口的执行端用的是同一条规则（tools/gk3boot/core/src/cmdline.c）
live_cmdline() {
    local keep
    keep=$(printf '%s\n' "$1" | tr ' ' '\n' \
           | grep -v -e '^androidboot\.' -e '^init=' -e '^firmware_class\.path=' -e '^$' \
           | tr '\n' ' ')
    echo "${keep}loglevel=3 panic=10 gk3.squash=/gaokun3/live.squashfs"
}

SQUASH=; INITRAMFS=; KERNEL=; DTB=; SDBOOT=; PAYLOAD=; OUT=; CMDLINE=; RELINFO=; KCONFIG=; ALLOW_NO_ISO=
while [ $# -gt 0 ]; do
    case "$1" in
        --squashfs)  SQUASH=$2; shift 2 ;;
        --initramfs) INITRAMFS=$2; shift 2 ;;
        --kernel)    KERNEL=$2; shift 2 ;;
        --dtb)       DTB=$2; shift 2 ;;
        --sdboot)    SDBOOT=$2; shift 2 ;;
        --payload)   PAYLOAD=$2; shift 2 ;;
        --cmdline)   CMDLINE=$2; shift 2 ;;
        --kconfig)   KCONFIG=$2; shift 2 ;;
        --allow-no-iso9660) ALLOW_NO_ISO=1; shift ;;
        --release-info) RELINFO=$2; shift 2 ;;
        --out)       OUT=$2; shift 2 ;;
        *) die "不认识的参数：$1" ;;
    esac
done
for v in SQUASH INITRAMFS KERNEL DTB SDBOOT CMDLINE KCONFIG OUT; do
    eval "x=\${$v}"
    [ -n "$x" ] || die "缺参数 --$(echo "$v" | tr 'A-Z' 'a-z')"
done
for f in "$SQUASH" "$INITRAMFS" "$KERNEL" "$DTB" "$SDBOOT" "$CMDLINE" "$KCONFIG"; do
    [ -f "$f" ] || die "文件不在：$f"
done
[ -z "$RELINFO" ] || [ -f "$RELINFO" ] || die "文件不在：$RELINFO"
[ -z "$PAYLOAD" ] || [ -d "$PAYLOAD" ] || die "--payload 不是目录：$PAYLOAD"
head -c 2 "$SDBOOT" | grep -q MZ || die "$SDBOOT 不是 PE 文件"
head -c 2 "$KERNEL" | grep -q MZ || die "$KERNEL 不是 PE 文件（要 EFI stub 内核 / vmlinuz.efi）"
[ "$(od -An -tx1 -N4 "$DTB" | tr -d ' ')" = "d00dfeed" ] || die "$DTB 不是 FDT（magic 不对）"
ok "输入体检通过（PE / PE / FDT）"
if grep -qx 'CONFIG_ISO9660_FS=y' "$KCONFIG"; then
    ok "内核开启了 CONFIG_ISO9660_FS（dd 写入后能从 ISO9660 分区读取 squashfs）"
elif [ -n "$ALLOW_NO_ISO" ]; then
    echo "   ⚠️ 内核没有开启 CONFIG_ISO9660_FS：dd 写入的 ISO 启动后找不到 squashfs，只有拖放方式可用（--allow-no-iso9660，测试构建）"
else
    die "内核没有开启 CONFIG_ISO9660_FS：dd 写入的 ISO 启动后找不到 squashfs。要用开启了它的 ROM 内核（scripts/kernel-config-android.sh），测试构建可加 --allow-no-iso9660"
fi

for t in xorriso mkfs.vfat mmd mcopy mdir; do
    command -v "$t" >/dev/null || die "缺工具：${t}（apt install xorriso dosfstools mtools）"
done

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
T=$W/tree
mkdir -p "$T/EFI/BOOT" "$T/loader/entries" "$T/gaokun3"

# —— 文件树 ——
cp "$SDBOOT"    "$T/EFI/BOOT/BOOTAA64.EFI"
cp "$KERNEL"    "$T/gaokun3/Image"
cp "$DTB"       "$T/gaokun3/gaokun3.dtb"
cp "$INITRAMFS" "$T/gaokun3/initramfs.img"
cp "$SQUASH"    "$T/gaokun3/live.squashfs"
[ -z "$RELINFO" ] || cp "$RELINFO" "$T/gaokun3/release.txt"
OPTS=$(live_cmdline "$(tr -d '\r\n' < "$CMDLINE")")
# ⚠️【不要】设 timeout 0：万一起不来，用户连菜单都进不去
printf 'timeout 5\nconsole-mode keep\neditor no\n' > "$T/loader/loader.conf"
# ⚠️ 标题用 ASCII：开机菜单是 UEFI 固件用它自己的字体画的，一般不含中文
printf 'title      gaokun3 installer\nversion    live\nsort-key   live0\nlinux      /gaokun3/Image\ndevicetree /gaokun3/gaokun3.dtb\ninitrd     /gaokun3/initramfs.img\noptions    %s\n' \
    "$OPTS" > "$T/loader/entries/gaokun3-live.conf"
ok "引导链 + 内核 + initramfs + squashfs，内核参数从 $(basename "$CMDLINE") 派生"
[ -z "$RELINFO" ] || ok "版本信息 gaokun3/release.txt（$(sed -n 's/^GK3_INSTALLER_VERSION=//p' "$RELINFO")）"

# —— EFI 系统分区：文件树里除 squashfs 以外的东西（payload 只进文件树）——
BOOT_FILES="EFI/BOOT/BOOTAA64.EFI loader/loader.conf loader/entries/gaokun3-live.conf gaokun3/Image gaokun3/gaokun3.dtb gaokun3/initramfs.img"
[ -z "$RELINFO" ] || BOOT_FILES="$BOOT_FILES gaokun3/release.txt"
need=0
for f in $BOOT_FILES; do need=$(( need + $(stat -c %s "$T/$f") )); done
# 10% 余量 + 2 MiB（FAT 表与目录）
EFI_KIB=$(( need / 1024 * 110 / 100 + 2048 ))
EFI=$W/efi.img
mkfs.vfat -n GK3BOOT -C "$EFI" "$EFI_KIB" >/dev/null
mmd -i "$EFI" ::/EFI ::/EFI/BOOT ::/loader ::/loader/entries ::/gaokun3
for f in $BOOT_FILES; do mcopy -i "$EFI" "$T/$f" "::/$f"; done
ok "EFI 系统分区 $(( EFI_KIB / 1024 )) MiB"

if [ -n "$PAYLOAD" ]; then
    mkdir -p "$T/gaokun3/payload"
    n=0
    for f in "$PAYLOAD"/*; do
        [ -f "$f" ] || continue
        cp "$f" "$T/gaokun3/payload/"; n=$(( n + 1 ))
    done
    ok "带上了安装载荷（$n 个文件）"
fi

# —— 混合 ISO ——
rm -f "$OUT"
if ! xorriso -as mkisofs -o "$OUT" -V GK3LIVE -R -J -joliet-long \
        -partition_offset 16 -no-pad \
        -append_partition 2 C12A7328-F81F-11D2-BA4B-00A0C93EC93B "$EFI" -appended_part_as_gpt \
        -iso_mbr_part_type EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 \
        -c boot.catalog --boot-catalog-hide \
        -e --interval:appended_partition_2::: -no-emul-boot \
        "$T" > "$W/xorriso.log" 2>&1; then
    cat "$W/xorriso.log" >&2
    die "xorriso 失败"
fi
ok "混合 ISO（ISO9660 + Rock Ridge + Joliet，GPT：ISO9660 分区 + EFI 系统分区）"

say "体检"
SA=$(xorriso -indev "$OUT" -report_system_area plain 2>/dev/null) || die "xorriso 无法读取 ISO 的分区表"
ET=$(xorriso -indev "$OUT" -report_el_torito plain 2>/dev/null) || die "xorriso 无法读取 ISO 的 El Torito 信息"
# GPT 的类型 GUID 按磁盘上的字节序打印：28732ac1… 是 EFI 系统分区，a2a0d0eb… 是 Microsoft 基本数据
printf '%s\n' "$SA" | grep -Eq '^GPT type GUID *: *2 +28732ac11ff8d211ba4b00a0c93ec93b$' && ok "GPT 分区 2 是 EFI 系统分区" || die "GPT 分区 2 不是 EFI 系统分区"
printf '%s\n' "$SA" | grep -Eq '^GPT start and size *: *1 +64 ' && ok "GPT 分区 1 从 64 扇区（ISO 的第 16 块）开始" || die "GPT 分区 1 的起点不对，initramfs 无法从分区挂载 ISO9660"
printf '%s\n' "$ET" | grep -Eq '^El Torito boot img *: *1 +UEFI ' && ok "El Torito UEFI 启动项" || die "没有 El Torito UEFI 启动项"
# 两棵目录树各查一遍：Rock Ridge（-read_fs any，Linux 挂载时用）与 Joliet（-read_fs norock，Windows 打开时用；
# 没有 Joliet 时读到的是 8.3 文件名，下面的路径都对不上）
for rf in any norock; do
    L=$(xorriso -read_fs "$rf" -indev "$OUT" -find / -type f 2>/dev/null) || die "xorriso 无法读取 ISO（-read_fs $rf）"
    for want in /EFI/BOOT/BOOTAA64.EFI /loader/loader.conf /loader/entries/gaokun3-live.conf \
                /gaokun3/Image /gaokun3/gaokun3.dtb /gaokun3/initramfs.img /gaokun3/live.squashfs; do
        printf '%s\n' "$L" | grep -qx "'$want'" || die "ISO（-read_fs $rf）里没有 $want"
    done
    if printf '%s\n' "$L" | grep -q boot.catalog; then die "ISO（-read_fs $rf）里看得到 boot.catalog"; fi
    ok "目录树（-read_fs $rf）文件齐全，boot.catalog 已隐藏"
done
EL=$(mdir -i "$EFI" -b -/ :: 2>/dev/null)
for want in $BOOT_FILES; do
    printf '%s\n' "$EL" | grep -qx "::/$want" || die "EFI 系统分区里没有 $want"
done
ok "EFI 系统分区文件齐全"
echo
echo "$OUT  $(du -h "$OUT" | cut -f1)"
echo "写盘： sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
