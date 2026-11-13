#!/usr/bin/env bash
# gk3l apply / delete / restore-table 的容器测试：在 loop 盘上真装（双系统、整盘清空、重新安装），装完的盘由
# check_install.py 对照测试的输入与独立的工具（sgdisk、dumpe2fs）逐项检查（分区、misc / boot / super 的内容、ext4、
# ESP 上的每个文件）；再测拒绝路径（盘一个字节不变、ERR 带 touched=no）、写到一半出错（touched=yes）、4Kn 盘。
#
#   bash scripts/live/test-in-container.sh installer/backend/test/linux/test-apply.sh
#
# ⚠️ 只动容器所在虚拟机里的 loop 设备（背后是 /tmp 下的稀疏文件）。
set -u
cd "$(dirname "$0")/../../../.."
[ "$(id -u)" = 0 ] || { echo "要 root（用 scripts/live/test-in-container.sh 跑）"; exit 2; }
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
# 期望的 ESP 内容：boot.img 拆开的三样与内核参数（check_install.py 比对）
for n, b in (("Image", kern), ("ramdisk.img", ramd), ("gaokun3.dtb", dtb), ("cmdline.txt", c)):
    open(os.path.join(exp, n), "wb").write(b)
open(os.path.join(exp, "fake-sdboot.efi"), "wb").write(b"MZ" + os.urandom(65536))
open(os.path.join(exp, "win-bootmgr.efi"), "wb").write(b"MZ" + os.urandom(40000))
open(os.path.join(exp, "other-loader.efi"), "wb").write(b"MZ" + os.urandom(30000))
PYEOF
img2simg "$W/expect/super.raw" "$W/super.img" >/dev/null
zstd -q -19 --long -f "$W/super.img" -o "$REL/super.img.zst"
( cd "$REL" && sha256sum boot.img super.img.zst > install-artifacts.sha256 )
cp "$REL/boot.img" "$W/expect/boot.img"
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
# check <说明> <盘> <gk3l apply 的 stdout> <check_install.py 的其余参数…>：ESP 只读挂上再查
check() {
    local what=$1 disk=$2 out=$3 m=$W/chk r; shift 3
    mkdir -p "$m"; mount -o ro -t vfat "${disk}p$(sgdisk -p "$disk" | awk '$6 == "EF00" {print $1; exit}')" "$m" || { bad "$what：ESP 挂不上"; return; }
    if r=$(python3 -I installer/backend/test/linux/check_install.py --disk "$disk" --esp "$m" --expect "$W/expect" --out "$out" "$@"); then ok "$what"
    else bad "$what"; printf '%s\n' "$r"; fi
    umount "$m"
}
prog_codes() { grep -aoE '^PROGRESS [0-9]+ [a-z-]+' "$1" | awk '{print $3}' | uniq | tr '\n' ' '; }

echo "═══ 1. 双系统：装在 Windows 盘的空闲区里 ═══"
mkwin "$W/base.img" 64G || { bad "造盘失败"; exit 1; }
cp --sparse=always "$W/base.img" "$W/b.img"
B=$(attach "$W/b.img")
"$G" probe >"$W/probe0.out" 2>/dev/null
FR=$(grep "^FREE disk=$B " "$W/probe0.out" | sort -t= -k5 -n | tail -1)
RS=$(field "$FR" start); RE=$(field "$FR" end)
[ -n "$RS" ] && ok "空闲区 $RS-$RE（$(field "$FR" size_mib) MiB）" || { bad "找不到空闲区"; exit 1; }
WIN0=$(for n in 1 2 3 4; do sgdisk -i "$n" "$B"; done)
H3=$(sha "${B}p3"); H4=$(sha "${B}p4")
mkdir -p "$W/efb"; efw "$W/efb" LoaderEntryDefault auto-windows
GK3_EFIVARS=$W/efb "$G" apply --disk "$B" --region-start "$RS" --region-end "$RE" --release "$REL" >"$W/b.out" 2>"$W/b.err"; RB=$?
if grep -q 'ERROR: \(Address\|Undefined\)Sanitizer\|runtime error' "$W/b.err"; then bad "sanitizer 报错"; sed -n 1,30p "$W/b.err"; fi
[ "$RB" = 0 ] && ok "装完" || { bad "装失败：rc=$RB"; tail -15 "$W/b.err"; }
[ "$(prog_codes "$W/b.err")" = "check verify-sha test-zst plan write-gpt format write-super write-boot bootloader done " ] \
    && ok "进度代码：$(prog_codes "$W/b.err")" || bad "进度代码：$(prog_codes "$W/b.err")"
check "分区、misc / boot / super、ext4、ESP 都对（默认进 Android）" "$B" "$W/b.out" --mode alongside --default android
# 检查器本身管用：把回落路径原件的备份换成 systemd-boot、改 boot_a 的一个字节，它都得报出来（改完复原）
chk_quiet() { local m=$W/chk; mkdir -p "$m"; mount -o ro -t vfat "${B}p1" "$m"; python3 -I installer/backend/test/linux/check_install.py --disk "$B" --esp "$m" --expect "$W/expect" --out "$W/b.out" --mode alongside --default android >"$W/chk.txt"; local r=$?; umount "$m"; return $r; }
mount -t vfat "${B}p1" "$W/seed"; cp "$W/seed/EFI/BOOT/BOOTAA64.EFI.before-gaokun3" "$W/bk.efi"; cp "$W/expect/fake-sdboot.efi" "$W/seed/EFI/BOOT/BOOTAA64.EFI.before-gaokun3"; umount "$W/seed"
! chk_quiet && grep -q 'before-gaokun3' "$W/chk.txt" && ok "检查器认得出：原件的备份被换成了 systemd-boot" || bad "检查器没认出被换掉的备份"
mount -t vfat "${B}p1" "$W/seed"; cp "$W/bk.efi" "$W/seed/EFI/BOOT/BOOTAA64.EFI.before-gaokun3"; umount "$W/seed"
NBA=$(sgdisk -p "$B" | awk '$7 == "boot_a" {print $1}')
printf 'X' | dd of="${B}p$NBA" bs=1 seek=5000 conv=notrunc status=none 2>/dev/null
! chk_quiet && grep -q 'boot_a' "$W/chk.txt" && ok "检查器认得出：boot_a 改了一个字节" || bad "检查器没认出 boot_a 被改"
dd if="$W/expect/boot.img" of="${B}p$NBA" bs=1 skip=5000 seek=5000 count=1 conv=notrunc status=none 2>/dev/null
chk_quiet && ok "复原之后检查又通过" || { bad "复原之后检查不通过"; cat "$W/chk.txt"; }
[ "$(for n in 1 2 3 4; do sgdisk -i "$n" "$B"; done)" = "$WIN0" ] && [ "$(sha "${B}p3")" = "$H3" ] && [ "$(sha "${B}p4")" = "$H4" ] \
    && ok "原来的 4 个分区：表项（GUID、起止、类型、属性、名字）与 Windows 卷的内容都没变" || bad "原来的分区被动了"
NU=$(sgdisk -p "$B" | awk '$7 == "userdata" {print $1}')
[ ! -e "$W/efb/LoaderEntryDefault-$LOADER_GUID" ] && ok "删了 LoaderEntryDefault" || bad "LoaderEntryDefault 没删"
V=$(sgdisk -v "$B" 2>&1)
printf '%s\n' "$V" | grep -q 'No problems found' && ok "sgdisk -v：主表与备份表都没问题" || { bad "sgdisk -v 有问题"; printf '%s\n' "$V" | head; }
"$G" probe >"$W/probe1.out" 2>/dev/null
L=$(grep "^DISK path=$B " "$W/probe1.out"); PU=$(grep "^PART path=${B}p$NU " "$W/probe1.out")
[ "$(field "$L" kernel)" = ok ] && [ "$(field "$PU" fs)" = ext4 ] && [ "$(field "$PU" fslabel)" = userdata ] \
    && ok "装完 gk3l probe：kernel=ok，userdata 认得出 ext4 / 卷标" || bad "装完的探测：$L / $PU"
grep -q "^PLAN op=mkpart disk=$B name=super " "$W/b.out" && ok "stdout 有方案（PLAN / PLANSUM）" || bad "stdout 没有方案：$(head -3 "$W/b.out")"
grep -q "^BACKUP path=$W/run/gpt-before-apply-.* disk=$B sector=512 head=34 tail=33 ram=yes$" "$W/b.out" \
    && ok "stdout 有 BACKUP 记录（没有安装介质 ⇒ ram=yes）" || bad "BACKUP 记录不对：$(grep ^BACKUP "$W/b.out")"
ls "$W/run"/gpt-before-apply-* >/dev/null 2>&1 && ok "分区表备份：$(basename "$(ls "$W/run"/gpt-before-apply-* | head -1)")" || bad "没有分区表备份"
H=$(ls "$W/run"/gpt-before-apply-* | head -1)
[ "$(head -c $((34 * 512)) "$H" | sha256sum)" = "$(head -c $((34 * 512)) "$W/base.img" | sha256sum)" ] \
    && ok "备份的主表区与装之前的盘逐字节相同" || bad "备份的主表区不对"

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
# 安装介质写保护（只读的 loop）：分区表备份退回内存，照样装
truncate -s 64M "$W/medium.img"; mkfs.vfat "$W/medium.img" >/dev/null; mmd -i "$W/medium.img" ::/gaokun3
MD=$(losetup -fr --show "$W/medium.img"); mkdir -p /media/gk3; mount -o ro -t vfat "$MD" /media/gk3
"$G" apply --disk "$C" --region-start "$RS" --region-end "$RE" --release "$REL" --bitlocker-confirmed --default-os windows >"$W/c.out" 2>"$W/c.err"; rc=$?
umount /media/gk3; losetup -d "$MD"
grep -q '^BACKUP .* ram=yes$' "$W/c.out" && grep -q 'only in RAM' "$W/c.err" && ok "介质写保护：分区表备份留在内存里，照样装" || bad "写保护的介质：$(grep ^BACKUP "$W/c.out")"
[ "$rc" = 0 ] && ok "装完" || { bad "装失败 rc=$rc"; tail -8 "$W/c.err"; }
check "分区、内容、ESP 都对，misc 带“默认 Windows”的请求" "$C" "$W/c.out" --mode alongside --default windows
[ "$(sha "${C}p3")" = "$H3" ] && [ "$(sha "${C}p4")" = "$H4" ] && ok "Windows 卷（NTFS、BitLocker）逐字节没动" || bad "Windows 卷被动了"
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

echo "═══ 5b. restore-table：用双系统安装前的备份把分区表写回去 ═══"
cp --sparse=always "$W/base.img" "$W/rt.img"; RT=$(attach "$W/rt.img")
"$G" apply --disk "$RT" --region-start "$RS" --region-end "$RE" --release "$REL" --backup-dir "$W/rtbk" >"$W/rt.out" 2>/dev/null
BK=$(field "$(grep '^BACKUP ' "$W/rt.out")" path)
O=$("$G" restore-table --file "$BK" --disk "$RT" 2>"$W/rt.err"); rc=$?
N=$(( 64 * 1024 * 2048 ))
[ "$rc" = 0 ] && [ "$(field "$O" op)" = restore-table ] && cmp -s <(head -c $((34 * 512)) "$RT") <(head -c $((34 * 512)) "$W/base.img") \
    && cmp -s <(tail -c $((33 * 512)) "$W/rt.img") <(tail -c $((33 * 512)) "$W/base.img") && [ "$(field "$O" reread)" = yes ] \
    && [ "$(sgdisk -p "$RT" | awk '/^ *[0-9]+ /' | wc -l)" = 4 ] && [ ! -e "${RT}p5" ] \
    && ok "备份写回：盘头盘尾与装之前逐字节相同，内核重读后只剩 4 个分区" || { bad "restore-table：rc=$rc $O"; cat "$W/rt.err" | tail -3; }
truncate -s 1M "$W/gpt-before-apply-loop0-20260101-000000-head34-tail33.bin"
O=$("$G" restore-table --file "$W/gpt-before-apply-loop0-20260101-000000-head34-tail33.bin" --disk "$RT" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$O" | grep -q '^ERR code=restore-mismatch' && ok "长度不对的备份 ⇒ restore-mismatch" || bad "长度不对：$O"
losetup -d "$RT"

echo "═══ 6. 重新安装：在第 1 组装好的盘上 ═══"
# reinst <keep yes|no> <说明>：userdata 里先放一个文件，装完看它在不在；分区表不动，Windows 原件的备份不能被换掉
reinst() {
    local keep=$1 what=$2 m=$W/ud before
    mkdir -p "$m"
    mount "${B}p$NU" "$m" && echo marker > "$m/marker" && umount "$m"
    before=$(sgdisk -p "$B" | grep -v '^Disk identifier')
    "$G" apply --disk "$B" --mode reinstall --keep-data "$keep" --release "$REL" >"$W/rb.out" 2>"$W/rb.err"; RB=$?
    if grep -q 'ERROR: \(Address\|Undefined\)Sanitizer\|runtime error' "$W/rb.err"; then bad "sanitizer 报错"; sed -n 1,30p "$W/rb.err"; fi
    [ "$RB" = 0 ] && ok "$what：装完" || { bad "$what：rc=$RB"; tail -10 "$W/rb.err"; return; }
    [ "$(prog_codes "$W/rb.err")" = "check verify-sha test-zst plan format write-super write-boot bootloader done " ] \
        || bad "$what：进度代码 $(prog_codes "$W/rb.err")"
    [ "$(sgdisk -p "$B" | grep -v '^Disk identifier')" = "$before" ] && ok "$what：分区表没变" || bad "$what：分区表变了"
    check "$what：内容、ESP 都对，回落路径原件的备份还是 Windows 的" "$B" "$W/rb.out" --mode reinstall --default android $([ "$keep" = yes ] && echo --keep-data)
    mount -o ro "${B}p$NU" "$m"
    if [ "$keep" = yes ]; then [ -f "$m/marker" ] && ok "$what：用户数据留下了" || bad "$what：用户数据没留下"
    else [ ! -e "$m/marker" ] && ok "$what：userdata 格式化了" || bad "$what：userdata 没格式化"; fi
    umount "$m"
    grep -q "^PLAN op=reuse name=super path=${B}p" "$W/rb.out" && grep -q "^PLANSUM mode=reinstall keep_data=$keep " "$W/rb.out" \
        && ok "$what：stdout 有方案（PLAN op=reuse / PLANSUM）" || bad "$what：方案不对：$(head -2 "$W/rb.out")"
}
reinst no "重新安装（格式化数据）"
reinst yes "重新安装（保留数据）"
mount "${B}p$NU" "$W/ud"
refuse "要重写的 userdata 正挂着" reinstall-busy "$B" -- --mode reinstall --release "$REL"
umount "$W/ud"

echo "═══ 7. 方案与查询：gk3l plan / esp-info / release-info ═══"
# 整盘清空的方案：先 PLAN op=wipe，ESP 在 2048 起 300 MiB（ef00），之后 6 个分区依次相连，userdata 到 usable 区末尾（差不到 1 MiB）
"$G" plan --mode wipe --disk "$B" >"$W/pb.out" 2>/dev/null
LU=$(sgdisk -p "$B" | sed -n 's/.*last usable sector is \([0-9]*\).*/\1/p')
python3 - "$W/pb.out" "$LU" <<'PY' && ok "整盘清空的方案：ESP 在前（2048 起 300 MiB、ef00），其余依次相连，userdata 到盘尾" || bad "整盘清空的方案不对：$(cat "$W/pb.out")"
import sys
recs = [dict(x.split("=", 1) for x in l.split()[1:]) for l in open(sys.argv[1]) if l.startswith("PLAN ")]
assert recs[0].get("op") == "wipe"
mk = [r for r in recs if r.get("op") == "mkpart"]
assert [r["name"] for r in mk] == ["esp", "misc", "metadata", "boot_a", "boot_b", "super", "userdata"]
assert mk[0]["start"] == "2048" and mk[0]["size_mib"] == "300" and mk[0]["type"] == "ef00"
for a, b in zip(mk, mk[1:]):
    assert int(b["start"]) == int(a["end"]) + 1
assert [int(r["size_mib"]) for r in mk[1:6]] == [4, 32, 64, 64, 12288]
assert 0 <= int(sys.argv[2]) - int(mk[-1]["end"]) < 2048
PY
# 重新安装的方案：每个分区的路径与起止就是盘上同名的那一个；动作按保不保留数据定
for kd in no yes; do
    "$G" plan --mode reinstall --disk "$B" --keep-data "$kd" >"$W/pb.out" 2>/dev/null
    python3 - "$W/pb.out" "$B" "$kd" <<'PY' && ok "重新安装的方案（keep=$kd）：位置与盘上相同，动作对" || bad "重新安装的方案（keep=$kd）不对：$(cat "$W/pb.out")"
import re, subprocess, sys
out, disk, keep = sys.argv[1:4]
recs = [dict(x.split("=", 1) for x in l.split()[1:]) for l in open(out) if l.startswith("PLAN op=reuse")]
want = {"misc": "write", "metadata": "keep" if keep == "yes" else "format", "boot_a": "write", "boot_b": "write",
        "super": "write", "userdata": "keep" if keep == "yes" else "format"}
assert {r["name"]: r["action"] for r in recs} == want
for r in recs:
    info = subprocess.run(["sgdisk", "-i", r["num"], disk], capture_output=True, text=True).stdout
    assert re.search(r"^Partition name: '%s'$" % r["name"], info, re.M)
    assert re.search(r"^First sector: %s " % r["start"], info, re.M) and re.search(r"^Last sector: %s " % r["end"], info, re.M)
    assert r["path"] == "%sp%s" % (disk, r["num"])
PY
done
"$G" plan --mode reinstall --disk "${B}p1" >/dev/null 2>&1; [ $? != 0 ] && ok "plan --mode reinstall 给的是分区 ⇒ 失败" || bad "plan 没拒绝分区"
# esp-info 对照事实：ESP 的大小（sgdisk）、空闲（df，差 1 MiB 以内）、Windows 与我们的启动项在、回落路径是 systemd-boot、EFI 变量
mkdir -p "$W/ef1"; efw "$W/ef1" LoaderEntryDefault auto-windows
EB=$(GK3_EFIVARS=$W/ef1 "$G" esp-info --disk "$B" 2>/dev/null)
mount -o ro -t vfat "${B}p1" "$W/seed"; FD=$(df -BM --output=avail "$W/seed" | tail -1 | tr -dc 0-9); umount "$W/seed"
SZ=$(( ($(sgdisk -i 1 "$B" | awk '/^Last sector:/{print $3}') - $(sgdisk -i 1 "$B" | awk '/^First sector:/{print $3}') + 1) / 2048 ))
FB=$(field "$EB" free_mib)
[ "$(field "$EB" part)" = "${B}p1" ] && [ "$(field "$EB" size_mib)" = "$SZ" ] && [ $((FD - FB)) -ge 0 ] && [ $((FD - FB)) -le 1 ] \
    && [ "$(field "$EB" windows)/$(field "$EB" gaokun3)/$(field "$EB" mountable)/$(field "$EB" small)" = yes/yes/yes/no ] \
    && [ "$(field "$EB" bootaa64)/$(field "$EB" bitlocker)/$(field "$EB" hibernated)/$(field "$EB" loader_default)" = sdboot/no/no/auto-windows ] \
    && ok "esp-info：$EB" || { bad "esp-info 不对：$EB（df 报 $FD MiB 空闲、分区 $SZ MiB）"; }
# release-info 对照 sha256sum 与文件本身
RLB=$("$G" release-info --dir "$REL" 2>/dev/null)
SUPMIB=$(( $(stat -c%s "$REL/super.img.zst") >> 20 ))
[ "$(field "$RLB" dir)/$(field "$RLB" boot)/$(field "$RLB" super)/$(field "$RLB" sha256)/$(field "$RLB" super_mib)" = "$REL/yes/zst/yes/$SUPMIB" ] \
    && ok "release-info：$RLB" || bad "release-info 不对：$RLB（super.img.zst 是 $SUPMIB MiB）"
printf '%s  %s\n' "$(printf 'a%.0s' $(seq 64))" "crDroid-12.6-20261101-gaokun3.zip" >> "$REL/install-artifacts.sha256"
RLB=$("$G" release-info --dir "$REL" 2>/dev/null)
[ "$(field "$RLB" version)" = crDroid-12.6-20261101-gaokun3 ] && ok "版本取自校验清单里的 .zip" || bad "版本：$RLB"
sed -i '$d' "$REL/install-artifacts.sha256"
losetup -d "$B"

echo "═══ 8. 整盘清空：一块 Windows 盘 ═══"
cp --sparse=always "$W/base.img" "$W/b.img"
B=$(attach "$W/b.img")
mkdir -p "$W/efb"; efw "$W/efb" LoaderEntryDefault auto-windows; efw "$W/efb" LoaderEntryOneShot auto-windows
GK3_EFIVARS=$W/efb "$G" apply --disk "$B" --mode wipe --release "$REL" >"$W/b.out" 2>"$W/b.err"; RB=$?
if grep -q 'ERROR: \(Address\|Undefined\)Sanitizer\|runtime error' "$W/b.err"; then bad "sanitizer 报错"; sed -n 1,30p "$W/b.err"; fi
[ "$RB" = 0 ] && ok "装完" || { bad "整盘清空：rc=$RB"; tail -10 "$W/b.err"; }
check "分区（ESP 在前）、内容、ext4、ESP（只有我们的文件）都对" "$B" "$W/b.out" --mode wipe --default none
[ "$(sgdisk -p "$B" | grep -cE '^ +[0-9]+ ')" = 7 ] && ok "盘上正好 7 个分区（ESP + Android 的 6 个）" || bad "分区数：$(sgdisk -p "$B" | grep -cE '^ +[0-9]+ ')"
[ "$(blkid -p -o value -s TYPE "${B}p1")/$(blkid -p -o value -s LABEL "${B}p1")" = vfat/ESP ] \
    && ok "ESP：FAT32，卷标 ESP" || bad "ESP 的文件系统：$(blkid -p "${B}p1")"
[ ! -e "$W/efb/LoaderEntryDefault-$LOADER_GUID" ] && [ ! -e "$W/efb/LoaderEntryOneShot-$LOADER_GUID" ] && ok "LoaderEntryDefault 与 LoaderEntryOneShot 都删了" || bad "EFI 变量没删干净"
V=$(sgdisk -v "$B" 2>&1); printf '%s\n' "$V" | grep -q 'No problems found' && ok "sgdisk -v 没问题" || { bad "sgdisk -v"; printf '%s\n' "$V" | head -5; }
"$G" probe >"$W/probe8.out" 2>/dev/null
[ "$(field "$(grep "^DISK path=$B " "$W/probe8.out")" kernel)" = ok ] && ! grep -q "^PART path=${B}p[0-9]* .*name=Basic" "$W/probe8.out" \
    && ok "内核只剩新分区（kernel=ok，旧的 Windows 分区不见了）" || bad "内核的分区视图不对：$(grep "^DISK path=$B " "$W/probe8.out")"
[ "$(head -c 512 "$B" | tail -c 66 | head -c 5 | od -An -tx1 | tr -d ' \n')" = 00000200ee ] && ok "保护性 MBR" || bad "LBA 0 不是保护性 MBR"
ls "$W/run"/gpt-before-wipe-* >/dev/null 2>&1 && ok "整盘清空之前备份了盘头盘尾" || bad "整盘清空没有备份"
losetup -d "$B"
# 拒绝：安装介质在这块盘上；有分区挂着
cp --sparse=always "$W/base.img" "$W/r.img"; R=$(attach "$W/r.img")
mkdir -p /media/gk3; mount -o ro -t vfat "${R}p1" /media/gk3
refuse "安装介质就在这块盘上" wipe-medium "$R" -- --mode wipe --release "$REL"
umount /media/gk3
mount -t vfat "${R}p1" "$W/seed"
refuse "有分区挂着" wipe-mounted "$R" -- --mode wipe --release "$REL"
umount "$W/seed"
ntfs-3g "${R}p3" "$W/ntfs" && { refuse "有分区经 FUSE 挂着" wipe-mounted "$R" -- --mode wipe --release "$REL"; umount "$W/ntfs"; }
refuse "整盘清空却要 Windows 为默认" default-os-no-windows "$R" -- --mode wipe --release "$REL" --default-os windows
losetup -d "$R"
# MBR 盘也能整盘清空
D=$(truncate -s 32G "$W/mbr2.img" && losetup -fP --show "$W/mbr2.img"); printf 'label: dos\n,1G,7\n,1G,83\n' | sfdisk -q "$D"; partprobe "$D"; sleep 1
"$G" apply --disk "$D" --mode wipe --release "$REL" >/dev/null 2>"$W/m.err"; rc=$?
[ "$rc" = 0 ] && [ "$(blkid -p -o value -s PTTYPE "$D")" = gpt ] && sgdisk -v "$D" 2>&1 | grep -q 'No problems found' \
    && ok "MBR 盘整盘清空成 GPT" || { bad "MBR 盘整盘清空失败 rc=$rc"; tail -5 "$W/m.err"; }
losetup -d "$D"

echo "═══ 9. 删除分区 ═══"
cp --sparse=always "$W/base.img" "$W/b.img"
B=$(attach "$W/b.img")
rows() { sgdisk -p "$1" | sed -n '/^Number/,$p'; }
WANT=$(rows "$B" | grep -vE '^ +4 ')
"$G" delete --part "${B}p4" >"$W/b.out" 2>"$W/b.err"; RB=$?
[ "$RB" = 0 ] && grep -q "^RESULT op=delete part=${B}p4$" "$W/b.out" && ok "删了 p4（RESULT op=delete）" || { bad "删除：rc=$RB"; tail -5 "$W/b.err"; }
[ "$(rows "$B")" = "$WANT" ] && ok "分区表正好少了 p4，其余 $(rows "$B" | grep -cE '^ +[0-9]+ ') 个不变" || { bad "删除后的分区表不对"; diff <(echo "$WANT") <(rows "$B"); }
# refusedel <说明> <ERR 代码> <分区>
refusedel() {
    local before out rc
    before=$(fp "$B"); out=$("$G" delete --part "$3" 2>&1 >/dev/null); rc=$?
    [ "$rc" != 0 ] && printf '%s\n' "$out" | grep -q "^ERR code=$2 " && [ "$(fp "$B")" = "$before" ] && ok "$1 ⇒ $2" || { bad "$1：rc=$rc"; printf '%s\n' "$out" | head -3; }
}
refusedel "删 ESP" edit-esp "${B}p1"
# FUSE 挂着的 NTFS：mountinfo 里是匿名设备号，靠独占打开与内核的 BLKPG 认出来
ntfs-3g "${B}p3" "$W/ntfs" && { refusedel "删 FUSE 挂着的 NTFS 分区" edit-mounted "${B}p3"; umount "$W/ntfs"; }
# 内核文件系统挂着的
mkfs.vfat "${B}p3" >/dev/null 2>&1; mount -t vfat "${B}p3" "$W/seed"
refusedel "删挂着的分区" edit-mounted "${B}p3"
umount "$W/seed"
refusedel "给的是整块盘" part-unknown "$B"
refusedel "不是块设备" not-block "$W/b.img"
losetup -d "$B"


echo
echo "合计：通过 $PASS，失败 $FAIL"
[ "$FAIL" = 0 ]
