#!/usr/bin/env bash
# gk3l apply（双系统）的容器测试：同一张盘的两份拷贝，一份用 shell 版 gk3_apply --mode alongside 装、一份用 gk3l apply 装，
# 逐项比对结果（分区表、misc / boot / super 的内容、ESP 上的每个文件、ext4 的参数、EFI 变量）；再测 gk3l 自己的拒绝路径
# （盘一个字节不变、ERR 带 touched=no）、写到一半出错（touched=yes）、4Kn 盘。
#
#   bash scripts/live/test-in-container.sh installer/backend/test/linux/test-apply.sh
#
# ⚠️ 只动容器所在虚拟机里的 loop 设备（背后是 /tmp 下的稀疏文件）。
set -u
cd "$(dirname "$0")/../../../.."
[ "$(id -u)" = 0 ] || { echo "要 root（用 scripts/live/test-in-container.sh 跑）"; exit 2; }
LIB=scripts/live/installer-lib.sh
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ✓ $*"; }
bad() { FAIL=$((FAIL+1)); echo "  ✗ $*"; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
field() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }

W=$(mktemp -d /tmp/gk3l-apply.XXXX)
cleanup() {
    local m img l
    for m in $(findmnt -rno TARGET | grep -e "^$W" | sort -r); do umount "$m" 2>/dev/null; done
    for img in "$W"/*.img; do
        for l in $(losetup -j "$img" 2>/dev/null | cut -d: -f1); do losetup -d "$l" 2>/dev/null; done
    done
    [ -n "${EL:-}" ] && losetup -d "$EL" 2>/dev/null
    dmsetup remove --noudevsync gk3l-apply-eio 2>/dev/null
    rm -rf "$W"
}
trap cleanup EXIT

MID=0123456789abcdef0123456789abcdef
LOADER_GUID=4a67b082-0a4c-41cf-b6c7-440b29bb8c4f
export GK3_ALLOW_LOOP=1 GK3_MACHINE_ID=$MID GK3_RUNDIR=$W/run GK3_SKIP_MODEL_CHECK=1
# 测试容器的内核没有 ntfs3：休眠检查改用 ntfs-3g 只读挂载（两边都认 GK3_NTFS_MOUNT）
grep -qw ntfs3 /proc/filesystems || export GK3_NTFS_MOUNT="ntfs-3g -o ro"
# 电源：一律装作接着电源（低电量另测）
PSD=$W/ps; mkdir -p "$PSD/ac"; echo Mains > "$PSD/ac/type"; echo 1 > "$PSD/ac/online"
export GK3_POWER_SUPPLY_DIR=$PSD

echo "═══ 编 gk3l ═══"
GK3L_CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    sh installer/backend/linux/build.sh "$W/gk3l" gcc || { echo "编不过"; exit 1; }
G=$W/gk3l
ok "ASan 版编出来了"

echo "═══ 0. 发布目录 ═══"
REL=$W/rel; mkdir -p "$REL" "$W/expect"
python3 - "$REL" "$W/expect" <<'PYEOF'
import os, struct, sys
rel, exp = sys.argv[1], sys.argv[2]
P = 4096
al = lambda x: (x + P - 1) // P * P
kern = b"MZ" + os.urandom(300000)
ramd = os.urandom(123457)
dtb = b"\xd0\x0d\xfe\xed" + os.urandom(20001)
cmd = ("androidboot.hardware=gaokun3 androidboot.boot_devices=soc@0/1c20000.pcie init=/init console=tty0 "
       "clk_ignore_unused efi=noruntime " + " ".join("gk3.pad%02d=%s" % (i, "x" * 12) for i in range(25)))
c = cmd.encode()
h = bytearray(P)
h[0:8] = b"ANDROID!"
struct.pack_into("<IIIIIIIIII", h, 8, len(kern), 0x8000, len(ramd), 0x1000000, 0, 0, 0x100, P, 2, 0)
h[64:64 + 511] = c[:511]
h[608:608 + len(c) - 511] = c[511:]
struct.pack_into("<IQII", h, 1632, 0, 0, 1660, len(dtb))
with open(os.path.join(rel, "boot.img"), "wb") as f:
    f.write(h); f.write(kern.ljust(al(len(kern)), b"\0"))
    f.write(ramd.ljust(al(len(ramd)), b"\0")); f.write(dtb.ljust(al(len(dtb)), b"\0"))
raw = bytearray(256 << 20)
raw[4096:4100] = b"gDla"
for off in (1 << 20, 37 << 20, 200 << 20):
    raw[off:off + (2 << 20)] = os.urandom(2 << 20)
open(os.path.join(exp, "super.raw"), "wb").write(raw)
open(os.path.join(exp, "fake-sdboot.efi"), "wb").write(b"MZ" + os.urandom(65536))
open(os.path.join(exp, "win-bootmgr.efi"), "wb").write(b"MZ" + os.urandom(40000))
open(os.path.join(exp, "other-loader.efi"), "wb").write(b"MZ" + os.urandom(30000))
PYEOF
img2simg "$W/expect/super.raw" "$W/super.img" >/dev/null
zstd -q -19 --long -f "$W/super.img" -o "$REL/super.img.zst"
( cd "$REL" && sha256sum boot.img super.img.zst > install-artifacts.sha256 )
export GK3_SDBOOT=$W/expect/fake-sdboot.efi
ok "发布目录：boot.img + super.img.zst + 校验清单"

# ── 一块"出厂 Windows"的盘：ESP（bootmgfw.efi、回落路径上是 Windows 的拷贝）、MSR、C 盘、压缩出来的空闲区、WinRE ──
#   mkwin <镜像文件> <大小> [扇区大小]
mkwin() {
    local img=$1 size=$2 ss=${3:-512} l m
    truncate -s "$size" "$img"
    l=$(losetup -fP --show ${3:+--sector-size "$ss"} "$img") || return 1
    if [ "$ss" = 4096 ]; then
        sgdisk -o -n 1:256:+300M -t 1:EF00 -c 1:"EFI system partition" -n 2:0:+16M -t 2:0C01 -c 2:"Microsoft reserved partition" \
               -n 3:0:+20G -t 3:0700 -c 3:"Basic data partition" "$l" >/dev/null
        S=$(( $(sgdisk -i 3 "$l" | awk '/^Last sector:/{print $3}') + 1 + 30 * 256 * 1024 ))
    else
        sgdisk -o -n 1:2048:+300M -t 1:EF00 -c 1:"EFI system partition" -n 2:0:+16M -t 2:0C01 -c 2:"Microsoft reserved partition" \
               -n 3:0:+20G -t 3:0700 -c 3:"Basic data partition" "$l" >/dev/null
        S=$(( $(sgdisk -i 3 "$l" | awk '/^Last sector:/{print $3}') + 1 + 30 * 2048 * 1024 ))
    fi
    sgdisk -n 4:$S:+1G -t 4:2700 -c 4:"Basic data partition" -A 4:set:0 -A 4:set:63 "$l" >/dev/null
    partprobe "$l"; udevadm settle 2>/dev/null; sleep 1
    mkfs.vfat -F 32 ${3:+-S $ss} -n SYSTEM "${l}p1" >/dev/null 2>&1 || return 1
    mkntfs -Q -F -L Windows "${l}p3" >/dev/null 2>&1
    mkntfs -Q -F -L "Windows RE tools" "${l}p4" >/dev/null 2>&1
    m=$W/seed; mkdir -p "$m"; mount -t vfat "${l}p1" "$m" || return 1
    mkdir -p "$m/EFI/Microsoft/Boot" "$m/EFI/BOOT" "$m/EFI/gk3test"
    cp "$W/expect/win-bootmgr.efi" "$m/EFI/Microsoft/Boot/bootmgfw.efi"
    cp "$W/expect/win-bootmgr.efi" "$m/EFI/BOOT/BOOTAA64.EFI"
    echo hello > "$m/EFI/gk3test/hello.txt"
    umount "$m"
    losetup -d "$l"
}
attach() { local l; l=$(losetup -fP --show ${2:+--sector-size "$2"} "$1") && partprobe "$l" && udevadm settle 2>/dev/null; sleep 1; echo "$l"; }
# 与 gk3w dump 同一格式的摘要（misc / boot / super 带整个分区的 sha256）
dump_disk() {
    local n info name first last type attrs line
    for n in $(sgdisk -p "$1" 2>/dev/null | awk '/^ *[0-9]+ /{print $1}'); do
        info=$(sgdisk -i "$n" "$1")
        name=$(printf '%s\n' "$info" | sed -n "s/^Partition name: '\(.*\)'$/\1/p")
        first=$(printf '%s\n' "$info" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
        last=$(printf '%s\n' "$info" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
        type=$(printf '%s\n' "$info" | sed -n 's/^Partition GUID code: \([0-9A-F-]*\).*/\1/p')
        attrs=$(printf '%s\n' "$info" | sed -n 's/^Attribute flags: \([0-9a-fA-F]*\).*/\1/p')
        line="DUMPPART num=$n name=$name start=$first end=$last type=$type attrs=$attrs"
        case "$name" in misc|boot_a|boot_b|super) line="$line sha256=$(sha "$1p$n")" ;; esac
        echo "$line"
    done
}
dump_esp() {   # $1=ESP 分区
    local m=$W/dump; mkdir -p "$m"; mount -o ro -t vfat "$1" "$m" || return 1
    ( cd "$m" && find . -type f | sed 's#^\./##' | LC_ALL=C sort | while read -r f; do
        echo "ESPFILE $f size=$(stat -c %s "$f") sha256=$(sha256sum "$f" | cut -d' ' -f1)"
      done )
    umount "$m"
}
ext4_sum() {   # $1=分区：与 UUID、时间无关的参数
    dumpe2fs -h "$1" 2>/dev/null | grep -E '^(Filesystem volume name|Block count|Reserved block count|Block size|Inode count|Filesystem features|Inode size|Flex block group size):'
}
efw() {   # efw <目录> <名字> <值>：假 efivarfs 里放一个 systemd-boot 变量
    local i v=$3
    { printf '\007\000\000\000'; for ((i = 0; i < ${#v}; i++)); do printf '%s\000' "${v:i:1}"; done; printf '\000\000'; } > "$1/$2-$LOADER_GUID"
}
fp() { sgdisk -p "$1" 2>/dev/null | grep -v '^Disk identifier'; head -c 1048576 "$1" | sha256sum; sha256sum "$1p1"; }
prog_codes() { grep -aoE '^PROGRESS [0-9]+ [a-z-]+' "$1" | awk '{print $3}' | uniq | tr '\n' ' '; }

echo "═══ 1. 对拍：同一张盘的两份拷贝，shell 版与 gk3l 各装一遍 ═══"
mkwin "$W/base.img" 64G || { bad "造盘失败"; exit 1; }
cp --sparse=always "$W/base.img" "$W/a.img"; cp --sparse=always "$W/base.img" "$W/b.img"
A=$(attach "$W/a.img"); B=$(attach "$W/b.img")
"$G" probe >"$W/probe0.out" 2>/dev/null
FR=$(grep "^FREE disk=$B " "$W/probe0.out" | sort -t= -k5 -n | tail -1)
RS=$(field "$FR" start); RE=$(field "$FR" end)
[ -n "$RS" ] && ok "空闲区 $RS-$RE（$(field "$FR" size_mib) MiB）" || { bad "找不到空闲区"; exit 1; }
mkdir -p "$W/efa" "$W/efb"; efw "$W/efa" LoaderEntryDefault auto-windows; efw "$W/efb" LoaderEntryDefault auto-windows
GK3_EFIVARS=$W/efa bash -c '. "$0" && gk3_apply "$@"' "$LIB" --disk "$A" --mode alongside --rescue no --release "$REL" \
    --region-start "$RS" --region-end "$RE" --esp "${A}p1" >"$W/a.out" 2>"$W/a.err"; RA=$?
GK3_EFIVARS=$W/efb "$G" apply --disk "$B" --region-start "$RS" --region-end "$RE" --release "$REL" >"$W/b.out" 2>"$W/b.err"; RB=$?
if grep -q 'ERROR: \(Address\|Undefined\)Sanitizer\|runtime error' "$W/b.err"; then bad "sanitizer 报错"; sed -n 1,30p "$W/b.err"; fi
[ "$RA" = 0 ] && [ "$RB" = 0 ] && ok "两边都装完（shell rc=$RA，gk3l rc=$RB）" || { bad "装失败：shell rc=$RA gk3l rc=$RB"; tail -5 "$W/a.err"; tail -15 "$W/b.err"; }
[ "$(prog_codes "$W/b.err")" = "check verify-sha test-zst plan write-gpt format write-super write-boot bootloader done " ] \
    && ok "进度代码：$(prog_codes "$W/b.err")" || bad "进度代码：$(prog_codes "$W/b.err")"
partprobe "$A" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1
dump_disk "$A" | sed "s#$A#DISK#g" >"$W/a.dump"; dump_disk "$B" | sed "s#$B#DISK#g" >"$W/b.dump"
if cmp -s "$W/a.dump" "$W/b.dump"; then ok "分区表（序号、名字、起止、类型、属性）与 misc / boot_a / boot_b / super 的内容逐字节相同（$(grep -c . "$W/b.dump") 个分区）"
else bad "分区表或分区内容不同"; diff "$W/a.dump" "$W/b.dump" | head -20; fi
dump_esp "${A}p1" >"$W/a.esp"; dump_esp "${B}p1" >"$W/b.esp"
if cmp -s "$W/a.esp" "$W/b.esp"; then ok "ESP 上的每个文件（$(grep -c . "$W/b.esp") 个）路径、大小、内容都相同"
else bad "ESP 不同"; diff "$W/a.esp" "$W/b.esp" | head -20; fi
NA=$(dump_disk "$A" | awk '/name=metadata/{print $2}' | cut -d= -f2); NU=$(dump_disk "$A" | awk '/name=userdata/{print $2}' | cut -d= -f2)
if [ "$(ext4_sum "${A}p$NA")" = "$(ext4_sum "${B}p$NA")" ] && [ "$(ext4_sum "${A}p$NU")" = "$(ext4_sum "${B}p$NU")" ] \
   && ext4_sum "${B}p$NU" | grep -q 'Reserved block count: *0$'; then ok "metadata / userdata 的 ext4 参数相同（卷标、块数、块大小、特性、userdata 无保留块）"
else bad "ext4 参数不同"; diff <(ext4_sum "${A}p$NU") <(ext4_sum "${B}p$NU"); diff <(ext4_sum "${A}p$NA") <(ext4_sum "${B}p$NA"); fi
[ ! -e "$W/efa/LoaderEntryDefault-$LOADER_GUID" ] && [ ! -e "$W/efb/LoaderEntryDefault-$LOADER_GUID" ] \
    && ok "两边都删了 LoaderEntryDefault" || bad "LoaderEntryDefault 没删"
V=$(sgdisk -v "$B" 2>&1)
printf '%s\n' "$V" | grep -q 'No problems found' && ok "sgdisk -v：主表与备份表都没问题" || { bad "sgdisk -v 有问题"; printf '%s\n' "$V" | head; }
"$G" probe >"$W/probe1.out" 2>/dev/null
L=$(grep "^DISK path=$B " "$W/probe1.out"); PU=$(grep "^PART path=${B}p$NU " "$W/probe1.out")
[ "$(field "$L" kernel)" = ok ] && [ "$(field "$PU" fs)" = ext4 ] && [ "$(field "$PU" fslabel)" = userdata ] \
    && ok "装完 gk3l probe：kernel=ok，userdata 认得出 ext4 / 卷标" || bad "装完的探测：$L / $PU"
grep -q "^PLAN op=mkpart disk=$B name=super " "$W/b.out" && ok "stdout 有方案（PLAN / PLANSUM）" || bad "stdout 没有方案：$(head -3 "$W/b.out")"
ls "$W/run"/gpt-before-apply-* >/dev/null 2>&1 && ok "分区表备份：$(basename "$(ls "$W/run"/gpt-before-apply-* | head -1)")" || bad "没有分区表备份"
H=$(ls "$W/run"/gpt-before-apply-* | head -1)
[ "$(head -c $((34 * 512)) "$H" | sha256sum)" = "$(head -c $((34 * 512)) "$W/base.img" | sha256sum)" ] \
    && ok "备份的主表区与装之前的盘逐字节相同" || bad "备份的主表区不对"
losetup -d "$A"

echo "═══ 2. 拒绝：盘一个字节不变、ERR 带 touched=no ═══"
# refuse <说明> <期望的 ERR 代码> <盘> -- <gk3l apply 的其余参数…>
refuse() {
    local what=$1 code=$2 disk=$3 before out rc; shift 4
    before=$(fp "$disk")
    out=$("$G" apply --disk "$disk" "$@" 2>&1 >/dev/null); rc=$?
    if [ "$rc" != 0 ] && printf '%s\n' "$out" | grep -q "^ERR code=$code .*touched=no\|^ERR code=$code touched=no" && [ "$(fp "$disk")" = "$before" ]; then ok "$what ⇒ $code"
    else bad "$what：rc=$rc，盘$([ "$(fp "$disk")" = "$before" ] && echo 没 || echo )变"; printf '%s\n' "$out" | grep -E '^(ERR|!!)' | head -3 | sed 's/^/      /'; fi
}
cp --sparse=always "$W/base.img" "$W/r.img"; R=$(attach "$W/r.img")
refuse "区间压着 C 盘" bad-region "$R" -- --region-start 2048 --region-end $((RE)) --release "$REL"
refuse "区间太小" plan "$R" -- --region-start "$RS" --region-end $((RS + 2048 * 1024)) --release "$REL"
refuse "/data 小于 8 GiB" plan "$R" -- --region-start "$RS" --region-end "$RE" --userdata-mib 8191 --release "$REL"
mkdir -p "$W/badrel"; cp "$REL/boot.img" "$REL/super.img.zst" "$REL/install-artifacts.sha256" "$W/badrel/"
printf 'X' | dd of="$W/badrel/boot.img" bs=1 seek=100000 conv=notrunc status=none
refuse "boot.img 与校验清单不符" sha-mismatch "$R" -- --region-start "$RS" --region-end "$RE" --release "$W/badrel"
rm "$W/badrel/install-artifacts.sha256"; head -c 300000 "$REL/super.img.zst" > "$W/badrel/super.img.zst"; cp "$REL/boot.img" "$W/badrel/"
refuse "super.img.zst 下载不完整（没有清单）" zst-corrupt "$R" -- --region-start "$RS" --region-end "$RE" --release "$W/badrel"
head -c 8192 /dev/urandom > "$W/badrel/boot.img"; cp "$REL/super.img.zst" "$W/badrel/"
refuse "boot.img 是坏的" bootimg-unpack "$R" -- --region-start "$RS" --region-end "$RE" --release "$W/badrel"
refuse "Windows 设为默认、ESP 上却没有 Windows" default-os-no-windows "$(
    m=$W/seed; mount -t vfat "${R}p1" "$m"; rm -rf "$m/EFI/Microsoft"; umount "$m"; echo "$R")" -- --region-start "$RS" --region-end "$RE" --release "$REL" --default-os windows
losetup -d "$R"
# 低电量：上面那次用的是"接着电源"，这里单独换一个电源目录再跑一次
cp --sparse=always "$W/base.img" "$W/r.img"; R=$(attach "$W/r.img")
mkdir -p "$W/psl/bat"; echo Battery > "$W/psl/bat/type"; echo 9 > "$W/psl/bat/capacity"; echo Discharging > "$W/psl/bat/status"
before=$(fp "$R"); out=$(GK3_POWER_SUPPLY_DIR=$W/psl "$G" apply --disk "$R" --region-start "$RS" --region-end "$RE" --release "$REL" 2>&1 >/dev/null); rc=$?
[ "$rc" != 0 ] && printf '%s\n' "$out" | grep -q '^ERR code=low-battery touched=no' && [ "$(fp "$R")" = "$before" ] \
    && ok "电量 9%、没接电源 ⇒ low-battery" || bad "低电量没拦住：$out"
# Windows 在休眠：C 盘根上的 hiberfil.sys 以 hibr 开头
m=$W/ntfs; mkdir -p "$m"; ntfs-3g "${R}p3" "$m" && { printf 'hibr'; head -c 8192 /dev/urandom; } > "$m/hiberfil.sys"; umount "$m"
refuse "Windows 在休眠" esp-windows-hibernated "$R" -- --region-start "$RS" --region-end "$RE" --release "$REL"
ntfs-3g -o remove_hiberfile "${R}p3" "$m" && umount "$m"
# BitLocker：WinRE 那个分区换成一个假的加密卷（blkid 认 TYPE=BitLocker）
python3 - "${R}p4" <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    s = bytearray(512); s[0:3] = b"\xeb\x58\x90"; s[3:11] = b"-FVE-FS-"
    struct.pack_into("<Q", s, 176, 1 << 20); s[510:512] = b"\x55\xaa"
    f.write(s); f.seek(1 << 20); f.write(b"-FVE-FS-" + struct.pack("<HH", 64, 2) + bytes(52))
PY
[ "$(blkid -p -o value -s TYPE "${R}p4")" = BitLocker ] || bad "假 BitLocker 卷没被 blkid 认出来"
refuse "BitLocker 卷 + 要换 BOOTAA64、没确认恢复密钥" bitlocker-key-unconfirmed "$R" -- --region-start "$RS" --region-end "$RE" --release "$REL"
# 分区名撞了：盘上已有一个叫 super 的
sgdisk -n 5:$((RE - 2048 * 100)):$RE -c 5:super "$R" >/dev/null; partprobe "$R"; udevadm settle 2>/dev/null; sleep 1
refuse "盘上已有叫 super 的分区" partlabel-conflict "$R" -- --region-start "$RS" --region-end $((RE - 2048 * 200)) --release "$REL" --bitlocker-confirmed
# 内核的分区视图过期：绕过 loop 设备直接改镜像文件
sgdisk -d 5 "$W/r.img" >/dev/null; blockdev --flushbufs "$R"
refuse "内核的分区视图与盘上不一致" kernel-stale "$R" -- --region-start "$RS" --region-end "$RE" --release "$REL" --bitlocker-confirmed
blockdev --rereadpt "$R"; udevadm settle 2>/dev/null; sleep 1
# ESP 回落路径：另一个系统的引导程序，.before-gaokun3 里又是另一个（都不是 Windows）
m=$W/seed; mount -t vfat "${R}p1" "$m"; cp "$W/expect/other-loader.efi" "$m/EFI/BOOT/BOOTAA64.EFI"
cp "$W/expect/fake-sdboot.efi" "$W/x.efi"; printf 'Y' >> "$W/x.efi"; cp "$W/x.efi" "$m/EFI/BOOT/BOOTAA64.EFI.before-gaokun3"; umount "$m"
refuse "回落路径与备份是两个不同的别的引导程序" fallback-conflict "$R" -- --region-start "$RS" --region-end "$RE" --release "$REL" --bitlocker-confirmed
losetup -d "$R"
# 没有 ESP、MBR 盘、ESP 太小
D=$(truncate -s 16G "$W/noesp.img" && losetup -fP --show "$W/noesp.img"); sgdisk -o -n 1:2048:+1G -t 1:0700 "$D" >/dev/null; partprobe "$D"; sleep 1
refuse "盘上没有 ESP" esp-missing "$D" -- --region-start 4196352 --region-end 33554398 --release "$REL"
losetup -d "$D"
D=$(truncate -s 16G "$W/mbr.img" && losetup -fP --show "$W/mbr.img"); printf 'label: dos\n,1G,7\n' | sfdisk -q "$D"
refuse "MBR 盘" mbr-disk "$D" -- --region-start 4196352 --region-end 33554398 --release "$REL"
losetup -d "$D"
D=$(truncate -s 32G "$W/small.img" && losetup -fP --show "$W/small.img")
sgdisk -o -n 1:2048:+100M -t 1:EF00 -c 1:"EFI system partition" "$D" >/dev/null; partprobe "$D"; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 "${D}p1" >/dev/null 2>&1; mount -t vfat "${D}p1" "$m"; mkdir -p "$m/EFI/Microsoft/Boot"; cp "$W/expect/win-bootmgr.efi" "$m/EFI/Microsoft/Boot/bootmgfw.efi"; umount "$m"
refuse "100 MiB 的 ESP" esp-too-small "$D" -- --region-start 411648 --region-end 67108830 --release "$REL"
losetup -d "$D"
O=$("$G" apply --disk "$B" 2>&1); [ $? != 0 ] && printf '%s\n' "$O" | grep -q '^ERR code=usage' && ok "缺参数 ⇒ usage" || bad "缺参数：$O"

echo "═══ 3. BitLocker 已确认、Windows 为默认 ═══"
cp --sparse=always "$W/base.img" "$W/c.img"; C=$(attach "$W/c.img")
python3 - "${C}p4" <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    s = bytearray(512); s[0:3] = b"\xeb\x58\x90"; s[3:11] = b"-FVE-FS-"
    struct.pack_into("<Q", s, 176, 1 << 20); s[510:512] = b"\x55\xaa"
    f.write(s); f.seek(1 << 20); f.write(b"-FVE-FS-" + struct.pack("<HH", 64, 2) + bytes(52))
PY
H3=$(sha "${C}p3"); H4=$(sha "${C}p4")
mkdir -p "$W/elsewhere"; mount -t vfat "${C}p1" "$W/elsewhere"
refuse "ESP 另有一处挂着" esp-busy "$C" -- --region-start "$RS" --region-end "$RE" --release "$REL" --bitlocker-confirmed
umount "$W/elsewhere"
"$G" apply --disk "$C" --region-start "$RS" --region-end "$RE" --release "$REL" --bitlocker-confirmed --default-os windows >"$W/c.out" 2>"$W/c.err"; rc=$?
[ "$rc" = 0 ] && ok "装完" || { bad "装失败 rc=$rc"; tail -8 "$W/c.err"; }
NM=$(dump_disk "$C" | awk '/name=misc/{print $2}' | cut -d= -f2)
misc_want() {
    python3 - "$1" "$2" <<'PY'
import hashlib, struct, sys, zlib
setdef = {"none": 0, "windows": 1, "android": 2}[sys.argv[1]]
m = bytearray(65536)
bc = bytearray(32); bc[0:2] = b"_a"; struct.pack_into("<I", bc, 4, 0x42414342); bc[8] = 1
struct.pack_into("<H", bc, 9, 2); struct.pack_into("<H", bc, 12, 15 | (6 << 4))
struct.pack_into("<I", bc, 28, zlib.crc32(bytes(bc[0:28])) & 0xffffffff); m[2048:2080] = bc
r = bytearray(2048); struct.pack_into("<IHHIII", r, 0, 0x52334B47, 1, 2048, 1, 1, 1); r[26] = 1
r[48:68] = hashlib.sha1(bytes(2048)).digest(); r[360] = setdef
struct.pack_into("<IHBBII", r, 1024, 1, 8, 0xff, 0, 1, 0)
struct.pack_into("<I", r, 2044, zlib.crc32(bytes(r[0:2044])) & 0xffffffff); m[8192:10240] = r
open(sys.argv[2], "wb").write(m)
PY
}
misc_want windows "$W/misc.want"
cmp -s <(head -c 65536 "${C}p$NM") "$W/misc.want" && [ "$(tail -c +65537 "${C}p$NM" | tr -d '\0' | wc -c)" = 0 ] \
    && ok "misc：初始状态带"默认 Windows"的请求，其余全零" || bad "misc 内容不对"
[ "$(sha "${C}p3")" = "$H3" ] && [ "$(sha "${C}p4")" = "$H4" ] && ok "Windows 卷（NTFS、BitLocker）逐字节没动" || bad "Windows 卷被动了"
m=$W/seed; mount -o ro -t vfat "${C}p1" "$m"
cmp -s "$m/EFI/BOOT/BOOTAA64.EFI.before-gaokun3" "$W/expect/win-bootmgr.efi" && cmp -s "$m/EFI/BOOT/BOOTAA64.EFI" "$W/expect/fake-sdboot.efi" \
    && [ -f "$m/EFI/gk3test/hello.txt" ] && grep -q '^default \*-android-a.conf' "$m/loader/loader.conf" \
    && ok "回落路径换成 systemd-boot、原件留成 .before-gaokun3；别人的文件还在" || bad "ESP 上的回落路径 / 备份不对"
umount "$m"
losetup -d "$C"

echo "═══ 4. 写到一半出错：super 那一段读写都出错（loop 叠在 dm 上）⇒ ERR 带 touched=yes ═══"
cp --sparse=always "$W/base.img" "$W/e.img"; EB=$(losetup -f --show "$W/e.img")
SUP=$("$G" plan --sector 512 --region-start "$RS" --region-end "$RE" | grep 'name=super ')
SS=$(field "$SUP" start); N=$(( 64 * 1024 * 2048 ))
printf '0 %s linear %s 0\n%s 2048 error\n%s %s linear %s %s\n' $((SS + 4096)) "$EB" $((SS + 4096)) $((SS + 6144)) $((N - SS - 6144)) "$EB" $((SS + 6144)) \
    | dmsetup create --noudevsync gk3l-apply-eio
EL=$(losetup -fP --show "/dev/$(dmsetup info -c --noheadings -o blkdevname gk3l-apply-eio | tr -d ' ')"); partprobe "$EL"; udevadm settle 2>/dev/null; sleep 1
out=$("$G" apply --disk "$EL" --region-start "$RS" --region-end "$RE" --release "$REL" 2>&1 >/dev/null); rc=$?
[ "$rc" != 0 ] && printf '%s\n' "$out" | grep -q '^ERR code=super-write .*touched=yes' \
    && ok "super 写不进去 ⇒ ERR super-write touched=yes" || { bad "写到一半出错：rc=$rc"; printf '%s\n' "$out" | grep -E '^(ERR|!!)' | head -3; }
losetup -d "$EL"; EL=""
for i in 1 2 3 4 5 6 7 8 9 10; do dmsetup remove --noudevsync gk3l-apply-eio 2>/dev/null && break; sleep 0.5; done
losetup -d "$EB"

echo "═══ 5. 4Kn 盘 ═══"
mkwin "$W/k4.img" 64G 4096 || bad "造 4Kn 盘失败"
K=$(attach "$W/k4.img" 4096)
"$G" probe >"$W/probe4.out" 2>/dev/null
FR=$(grep "^FREE disk=$K " "$W/probe4.out" | sort -t= -k5 -n | tail -1)
"$G" apply --disk "$K" --region-start "$(field "$FR" start)" --region-end "$(field "$FR" end)" --release "$REL" >"$W/k.out" 2>"$W/k.err"; rc=$?
[ "$rc" = 0 ] && ok "4Kn 盘装完" || { bad "4Kn 装失败 rc=$rc"; tail -8 "$W/k.err"; }
V=$(sgdisk -v "$K" 2>&1); printf '%s\n' "$V" | grep -q 'No problems found' && ok "4Kn：sgdisk -v 没问题" || { bad "4Kn：sgdisk -v"; printf '%s\n' "$V" | head -5; }
DK=$(dump_disk "$K")
NS=$(printf '%s\n' "$DK" | awk '/name=super/{print $2}' | cut -d= -f2)
SL=$(printf '%s\n' "$DK" | grep 'name=super'); SZ=$(( ($(field "$SL" end) - $(field "$SL" start) + 1) * 4096 ))
[ "$SZ" = $((12288 << 20)) ] && cmp -s <(head -c "$(stat -c%s "$W/expect/super.raw")" "${K}p$NS") "$W/expect/super.raw" \
    && ok "4Kn：super 12 GiB，内容与 super.raw 相同" || bad "4Kn：super 大小 $SZ 或内容不对"
NU=$(printf '%s\n' "$DK" | awk '/name=userdata/{print $2}' | cut -d= -f2)
dumpe2fs -h "${K}p$NU" 2>/dev/null | grep -q '^Block size: *4096' && ok "4Kn：userdata 是 4 KiB 块的 ext4" || bad "4Kn：userdata 的 ext4 不对"
losetup -d "$K"
losetup -d "$B"

echo
echo "合计：通过 $PASS，失败 $FAIL"
[ "$FAIL" = 0 ]
