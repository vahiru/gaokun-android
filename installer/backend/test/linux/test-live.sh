#!/usr/bin/env bash
# 安装器分区（读进了内存的内置 GK3LIVE，见 src/linux/live.h）的容器测试：
#   - probe 认出它（medium=yes ram=yes）；分区表备份与任务日志只在内存（GK3_RUNDIR）里，它本身与 ESP 一个字节都不写；
#   - 双系统 + --remove-installer：装完删掉它，userdata 紧挨着它时并进去（GPT、内核、ext4 一起扩）；
#   - 整盘清空：它的表项原样留在新表里、内容不动，新 ESP 上先有 systemd-boot；之后 remove-installer 扩到盘尾；
#   - 不相邻（GK3LIVE 在空闲区开头）：删掉但不扩；
#   - 拒绝：没有读进内存、挂着、apply --remove-installer 而盘上没有它 —— 盘一个字节不变。
#
#   bash installer/tools/test/test-in-container.sh installer/backend/test/linux/test-live.sh
#
# ⚠️ 只动容器所在虚拟机里的 loop 设备（背后是 /tmp 下的稀疏文件）；/media/gk3 在容器里挂一个 tmpfs 当内存副本。
set -u
cd "$(dirname "$0")/../../../.."
[ "$(id -u)" = 0 ] || { echo "要 root（用 installer/tools/test/test-in-container.sh 跑）"; exit 2; }
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ✓ $*"; }
bad() { FAIL=$((FAIL+1)); echo "  ✗ $*"; }
check() { local m=$1; shift; if "$@"; then ok "$m"; else bad "$m"; fi; }
field() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }

W=$(mktemp -d /tmp/gk3l-live.XXXX)
cleanup() {
    local m img l
    for m in $(findmnt -rno TARGET | grep -e "^$W" -e '^/media/gk3$' | sort -r); do umount "$m" 2>/dev/null; done
    for img in "$W"/*.img; do
        for l in $(losetup -j "$img" 2>/dev/null | cut -d: -f1); do losetup -d "$l" 2>/dev/null; done
    done
    rm -rf "$W"
}
trap cleanup EXIT

MID=0123456789abcdef0123456789abcdef
XBOOTLDR=BC13C2FF-59E6-4262-A352-B275FD6F7172
export GK3_ALLOW_LOOP=1 GK3_MACHINE_ID=$MID GK3_RUNDIR=$W/run GK3_SKIP_MODEL_CHECK=1 GK3_MEDIUM_FILE=$W/medium
export GK3_EFIVARS=$W/efivars GK3_JOB_POLL=0.05; mkdir -p "$GK3_EFIVARS"
grep -qw ntfs3 /proc/filesystems || export GK3_NTFS_MOUNT="ntfs-3g -o ro"
PSD=$W/ps; mkdir -p "$PSD/ac"; echo Mains > "$PSD/ac/type"; echo 1 > "$PSD/ac/online"
export GK3_POWER_SUPPLY_DIR=$PSD

echo "═══ 编 gk3l ═══"
GK3L_CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    sh installer/backend/linux/build.sh "$W/gk3l" gcc || { echo "编不过"; exit 1; }
G=$W/gk3l
ok "ASan 版编出来了"

echo "═══ 0. 发布目录 ═══"
REL=$W/rel; mkdir -p "$REL"
python3 - "$REL" "$W" <<'PYEOF'
import os, struct, sys
rel, w = sys.argv[1], sys.argv[2]
P = 4096
al = lambda x: (x + P - 1) // P * P
kern, ramd, dtb = b"MZ" + os.urandom(300000), os.urandom(123457), b"\xd0\x0d\xfe\xed" + os.urandom(20001)
c = b"androidboot.hardware=gaokun3 init=/init console=tty0"
h = bytearray(P)
h[0:8] = b"ANDROID!"
struct.pack_into("<IIIIIIIIII", h, 8, len(kern), 0x8000, len(ramd), 0x1000000, 0, 0, 0x100, P, 2, 0)
h[64:64 + len(c)] = c
struct.pack_into("<IQII", h, 1632, 0, 0, 1660, len(dtb))
with open(os.path.join(rel, "boot.img"), "wb") as f:
    f.write(h); f.write(kern.ljust(al(len(kern)), b"\0"))
    f.write(ramd.ljust(al(len(ramd)), b"\0")); f.write(dtb.ljust(al(len(dtb)), b"\0"))
raw = bytearray(64 << 20)
raw[4096:4100] = b"gDla"
raw[1 << 20:(1 << 20) + 4096] = os.urandom(4096)
open(os.path.join(w, "super.raw"), "wb").write(raw)
open(os.path.join(w, "fake-sdboot.efi"), "wb").write(b"MZ" + os.urandom(65536))
open(os.path.join(w, "win-bootmgr.efi"), "wb").write(b"MZ" + os.urandom(40000))
PYEOF
img2simg "$W/super.raw" "$W/super.img" >/dev/null
zstd -q -3 -f "$W/super.img" -o "$REL/super.img.zst"
export GK3_SDBOOT=$W/fake-sdboot.efi
ok "发布目录"

# ── 盘：ESP | MSR | C: | 空闲 | GK3LIVE（XBOOTLDR，FAT32）| WinRE
#   mkdisk <镜像> end|start   end：GK3LIVE 在空闲区末尾（Windows 安装器的做法）；start：在开头（旧脚本的做法）
mkdisk() {
    local img=$1 where=$2 l m S
    truncate -s 56G "$img"
    l=$(losetup -fP --show "$img") || return 1
    sgdisk -o -n 1:2048:+300M -t 1:EF00 -c 1:"EFI system partition" -n 2:0:+16M -t 2:0C01 -c 2:"Microsoft reserved partition" \
           -n 3:0:+20G -t 3:0700 -c 3:"Basic data partition" "$l" >/dev/null
    S=$(( $(sgdisk -i 3 "$l" | awk '/^Last sector:/{print $3}') + 1 ))
    if [ "$where" = end ]; then
        sgdisk -n 5:$((S + 30 * 2048 * 1024)):+1G -t 5:EA00 -c 5:GK3LIVE -A 5:set:63 \
               -n 4:$((S + 31 * 2048 * 1024)):+1G -t 4:2700 -c 4:"Basic data partition" "$l" >/dev/null
    else
        sgdisk -n 5:$S:+1G -t 5:EA00 -c 5:GK3LIVE -A 5:set:63 \
               -n 4:$((S + 31 * 2048 * 1024)):+1G -t 4:2700 -c 4:"Basic data partition" "$l" >/dev/null
    fi
    partprobe "$l"; udevadm settle 2>/dev/null; sleep 1
    mkfs.vfat -F 32 -n SYSTEM "${l}p1" >/dev/null 2>&1 || return 1
    mkntfs -Q -F -L Windows "${l}p3" >/dev/null 2>&1
    mkntfs -Q -F -L "Windows RE tools" "${l}p4" >/dev/null 2>&1
    mkfs.vfat -F 32 -n GK3LIVE "${l}p5" >/dev/null 2>&1 || return 1
    m=$W/seed; mkdir -p "$m"
    mount -t vfat "${l}p1" "$m" || return 1
    mkdir -p "$m/EFI/Microsoft/Boot" "$m/EFI/BOOT" "$m/EFI/gaokun3" "$m/loader/entries"
    cp "$W/win-bootmgr.efi" "$m/EFI/Microsoft/Boot/bootmgfw.efi"
    cp "$W/win-bootmgr.efi" "$m/EFI/BOOT/BOOTAA64.EFI"
    # 旧 Windows 脚本放在 ESP 上的 LiveCD 内核与启动项（删安装器分区时一起删）；别人的启动项不动
    head -c 4096 /dev/urandom > "$m/EFI/gaokun3/Image"
    printf 'title LiveCD\nlinux /EFI/gaokun3/Image\n' > "$m/loader/entries/gaokun3-live.conf"
    printf 'title other\n' > "$m/loader/entries/other.conf"
    umount "$m"
    mount -t vfat "${l}p5" "$m" || return 1
    mkdir -p "$m/gaokun3/diag" "$m/loader/entries" "$m/gaokun3-live"
    head -c 1048576 /dev/urandom > "$m/gaokun3/live.squashfs"
    echo "boot report from an earlier session" > "$m/gaokun3/diag/boot-20261101-000000.log"
    printf 'title LiveCD\nlinux /gaokun3-live/Image\n' > "$m/loader/entries/gaokun3-live.conf"
    head -c 4096 /dev/urandom > "$m/gaokun3-live/Image"
    umount "$m"
    echo "$l"
}
# 模拟 initramfs 的 toram：gaokun3/ 抄进 /media/gk3（tmpfs，抄完改成只读），介质分区卸下，记录写进 GK3_MEDIUM_FILE
toram() {
    local l=$1 p=$1p5 n m=$W/seed
    n=$(basename "$p")
    findmnt -rno TARGET /media/gk3 >/dev/null 2>&1 && umount /media/gk3
    mkdir -p /media/gk3 && mount -t tmpfs -o size=64m tmpfs /media/gk3
    mount -o ro -t vfat "$p" "$m" && cp -a "$m/gaokun3" /media/gk3/ && umount "$m" && mount -o remount,ro /media/gk3
    printf 'part=%s\ndisk=%s\nstart=%s\nsize=%s\nram=yes\n' "$n" "$(basename "$l")" \
        "$(cat /sys/class/block/$n/start)" "$(cat /sys/class/block/$n/size)" > "$GK3_MEDIUM_FILE"
}
gpt_sum() { dd if="$1" bs=512 count=34 2>/dev/null | sha256sum | cut -d' ' -f1; }
on_part() {   # on_part <分区> <命令…>：只读挂上，在挂载点里跑命令
    local p=$1 m=$W/look r; shift
    mkdir -p "$m"; mount -o ro "$p" "$m" || return 1
    ( cd "$m" && "$@" ); r=$?
    umount "$m"; return $r
}
files_sum() { on_part "$1" sh -c 'find . -type f | LC_ALL=C sort | xargs sha256sum'; }
part_line() { sgdisk -i "$2" "$1" | grep -E "^(Partition GUID code|Partition unique GUID|First sector|Last sector|Attribute flags|Partition name)"; }
ext4_bytes() { dumpe2fs -h "$1" 2>/dev/null | awk -F: '/^Block count/{c=$2} /^Block size/{s=$2} END{print c*s}'; }
part_bytes() { echo $(( $(cat /sys/class/block/$(basename "$1")/size) * 512 )); }
first_of() { sgdisk -i "$2" "$1" | awk '/^First sector:/{print $3}'; }
last_of() { sgdisk -i "$2" "$1" | awk '/^Last sector:/{print $3}'; }
num_of() { sgdisk -p "$1" | awk -v n="$2" '$NF == n {print $1}' | head -1; }

echo "═══ 1. 认出安装器分区；分区表备份与任务日志只在内存里 ═══"
L=$(mkdisk "$W/a.img" start) || { bad "造盘"; exit 1; }
toram "$L"
out=$("$G" probe 2>/dev/null)
pl=$(printf '%s\n' "$out" | grep "^PART path=${L}p5 ")
check "probe：GK3LIVE medium=yes ram=yes" test "$(field "$pl" medium)/$(field "$pl" ram)" = yes/yes
check "probe：别的分区没有 ram=" test "$(printf '%s\n' "$out" | grep -c ' ram=')" = 1
check "probe：盘 medium=yes" test "$(field "$(printf '%s\n' "$out" | grep "^DISK path=$L ")" medium)" = yes
LS=$(( $(last_of "$L" 5) + 1 ))
RE=$(( $(first_of "$L" 4) - 1 ))
out=$("$G" plan --sector 512 --region-start $LS --region-end $RE --disk "$L" --remove-installer 2>/dev/null)
rm_line=$(printf '%s\n' "$out" | grep '^PLAN op=rmlive')
check "plan --remove-installer：不相邻 → merge=no" test "$(field "$rm_line" merge)/$(field "$rm_line" part)" = "no/${L}p5"
lsum=$(files_sum "${L}p5")
out=$("$G" apply --disk "$L" --release "$REL" --region-start $LS --region-end $RE 2>"$W/err1"); rc=$?
check "双系统（不删安装器分区）成功" test $rc = 0
bk=$(printf '%s\n' "$out" | grep '^BACKUP')
bp=$(field "$bk" path); bn=$(basename "$bp")
check "BACKUP：在内存里（GK3_RUNDIR）、ram=yes" test "${bp%/*}/$(field "$bk" ram)" = "$GK3_RUNDIR/yes"
check "这份备份在" test -s "$bp"
check "介质分区一个字节都没写" test "$(files_sum "${L}p5")" = "$lsum"
check "ESP 上没有日志与备份（gaokun3/ 不存在）" on_part "${L}p1" test ! -e gaokun3
check "介质分区没有被挂上" test -z "$(findmnt -rno TARGET -S "${L}p5")"
"$G" job run plan --sector 512 --region-start 2048 --region-end 1000000 >/dev/null 2>"$W/errj"
jid=$(sed -n 's/^JOB .*id=\([^ ]*\).*/\1/p' "$W/errj" | head -1)
check "任务的输出在内存里" test -s "$GK3_RUNDIR/jobs/$jid/out"
check "任务结束后介质分区仍然没写" test "$(files_sum "${L}p5")" = "$lsum"

echo "═══ 2. remove-installer：不相邻 → 删掉、不扩 ═══"
G1=$(gpt_sum "$L")
out=$(GK3_MEDIUM_FILE=$W/none "$G" remove-installer --disk "$L" 2>"$W/err2"); rc=$?
check "没有读进内存（没有记录）：拒绝 live-missing、盘没动" test "$rc/$(grep -o 'code=[a-z-]*' "$W/err2" | head -1)/$(gpt_sum "$L")" = "1/code=live-missing/$G1"
mount -o ro "${L}p5" "$W/seed"
out=$("$G" remove-installer --disk "$L" 2>"$W/err3"); rc=$?
umount "$W/seed"
check "挂着：拒绝 live-busy、盘没动" test "$rc/$(grep -o 'code=[a-z-]*' "$W/err3" | head -1)/$(gpt_sum "$L")" = "1/code=live-busy/$G1"
UD=$(num_of "$L" userdata); UDL=$(last_of "$L" "$UD")
out=$("$G" remove-installer --disk "$L" 2>"$W/err4"); rc=$?
rmv=$(printf '%s\n' "$out" | grep '^REMOVED')
check "删掉了：REMOVED grow=no" test "$rc/$(field "$rmv" grow)" = 0/no
check "GPT 里没有 GK3LIVE 了" test -z "$(num_of "$L" GK3LIVE)"
check "内核里也没有了" test ! -e "/sys/class/block/$(basename "$L")p5"
check "userdata 没动" test "$(last_of "$L" "$UD")" = "$UDL"
ESP=${L}p1
bk=$(printf '%s\n' "$out" | grep '^BACKUP')
check "这一步的备份在内存里（ram=yes）" test "$(dirname "$(field "$bk" path)")/$(field "$bk" ram)" = "$GK3_RUNDIR/yes"
check "ESP 上没有日志与备份（gaokun3/ 不存在）" on_part "$ESP" test ! -e gaokun3
check "ESP：旧脚本的 LiveCD 启动项与内核删掉了，别的启动项还在" on_part "$ESP" sh -c 'test ! -e loader/entries/gaokun3-live.conf && test ! -e EFI/gaokun3 && test -s loader/entries/other.conf'
check "probe：介质分区没有了 → 盘不再标 medium" test "$(field "$("$G" probe 2>/dev/null | grep "^DISK path=$L ")" medium)" = no

echo "═══ 3. 双系统 + --remove-installer：相邻 → 并进 userdata ═══"
losetup -d "$L"
L=$(mkdisk "$W/b.img" end) || { bad "造盘"; exit 1; }
toram "$L"
CE=$(( $(last_of "$L" 3) + 1 )); LF=$(first_of "$L" 5); LL=$(last_of "$L" 5); WR=$(first_of "$L" 4)
out=$("$G" plan --sector 512 --region-start $CE --region-end $((LF - 1)) --disk "$L" --remove-installer 2>/dev/null)
rm_line=$(printf '%s\n' "$out" | grep '^PLAN op=rmlive')
pud=$(printf '%s\n' "$out" | grep '^PLANSUM'); pud=$(field "$pud" userdata_mib)
check "plan：merge=yes，并进去之后多 1 GiB" test "$(field "$rm_line" merge)/$(field "$rm_line" userdata_mib)" = "yes/$((pud + 1024))"
out=$("$G" apply --disk "$L" --release "$REL" --region-start $CE --region-end $((LF - 1)) --remove-installer 2>"$W/err5"); rc=$?
check "安装 + 删安装器分区成功" test $rc = 0 || tail -20 "$W/err5"
rmv=$(printf '%s\n' "$out" | grep '^REMOVED')
check "REMOVED grow=yes、userdata_mib 对" test "$(field "$rmv" grow)/$(field "$rmv" userdata_mib)" = "yes/$((pud + 1024))"
check "进度里有 remove-installer" grep -q '^PROGRESS 92 remove-installer' "$W/err5"
check "没有 NOTE installer-kept" test -z "$(printf '%s\n' "$out" | grep 'installer-kept')"
UD=$(num_of "$L" userdata)
want=$(( $(first_of "$L" "$UD") + ($WR - $(first_of "$L" "$UD")) / 2048 * 2048 - 1 ))
check "userdata 扩到 WinRE 之前（按 MiB 取整）" test "$(last_of "$L" "$UD")" = "$want"
check "GK3LIVE 没有了，WinRE 还在原处" test "$(num_of "$L" GK3LIVE)/$(first_of "$L" 4)" = "/$WR"
check "内核里的 userdata 是新的大小" test "$(part_bytes "${L}p$UD")" = "$(( (want - $(first_of "$L" "$UD") + 1) * 512 ))"
check "ext4 扩满了分区" test "$(ext4_bytes "${L}p$UD")" = "$(part_bytes "${L}p$UD")"
check "e2fsck 之后文件系统干净" e2fsck -f -n "${L}p$UD" >/dev/null 2>&1
check "ESP 上有 Android 的启动项、没有日志与备份" on_part "${L}p1" sh -c "test -s loader/entries/$MID-android-a.conf && test ! -e gaokun3"

echo "═══ 4. 整盘清空：保留安装器分区 ═══"
losetup -d "$L"
L=$(mkdisk "$W/c.img" end) || { bad "造盘"; exit 1; }
toram "$L"
before=$(part_line "$L" 5); LF=$(first_of "$L" 5); LL=$(last_of "$L" 5)
lsum=$(files_sum "${L}p5")
out=$("$G" plan --mode wipe --disk "$L" 2>/dev/null)
kp=$(printf '%s\n' "$out" | grep '^PLAN op=keep')
check "plan --mode wipe：PLAN op=keep name=GK3LIVE 原位置" test "$(field "$kp" name)/$(field "$kp" start)/$(field "$kp" end)" = "GK3LIVE/$LF/$LL"
ud_end=$(field "$(printf '%s\n' "$out" | grep 'name=userdata')" end)
check "Android 排在它前面、userdata 紧挨着它" test "$ud_end" -lt "$LF" -a $((LF - ud_end)) -le 2048
out=$("$G" apply --disk "$L" --release "$REL" --mode wipe 2>"$W/err6"); rc=$?
check "整盘清空成功" test $rc = 0 || tail -20 "$W/err6"
n=$(num_of "$L" GK3LIVE)
check "新表里 GK3LIVE 的类型、GUID、起止、属性、名字原样" test "$(part_line "$L" "$n")" = "$before"
check "GK3LIVE 上的文件一个字节都没变" test "$(files_sum "${L}p$n")" = "$lsum"
bk=$(printf '%s\n' "$out" | grep '^BACKUP')
check "这次的分区表备份在内存里（ram=yes）" test "$(dirname "$(field "$bk" path)")/$(field "$bk" ram)" = "$GK3_RUNDIR/yes"
check "WinRE、C: 都没了（8 个分区：ESP + 6 个 Android + GK3LIVE）" test "$(sgdisk -p "$L" | grep -cE '^ +[0-9]+ ')" = 8
check "日志写明先写了 systemd-boot" grep -q 'systemd-boot written first' "$W/err6"
check "ESP：回落路径是 systemd-boot" on_part "${L}p1" cmp -s EFI/BOOT/BOOTAA64.EFI "$GK3_SDBOOT"
check "记录里的介质照样认得出（分区号变了也行）" test "$(field "$("$G" probe 2>/dev/null | grep "GK3LIVE")" ram)" = yes
UD=$(num_of "$L" userdata)
out=$("$G" remove-installer --disk "$L" 2>"$W/err7"); rc=$?
rmv=$(printf '%s\n' "$out" | grep '^REMOVED')
check "remove-installer：grow=yes" test "$rc/$(field "$rmv" grow)" = 0/yes
lu=$(sgdisk -p "$L" | awk '/last usable sector is/{print $NF}')
check "userdata 扩到盘尾（原 WinRE 的空间也并进来）" test $(( lu - $(last_of "$L" "$UD") )) -lt 2048
check "ext4 扩满了分区" test "$(ext4_bytes "${L}p$UD")" = "$(part_bytes "${L}p$UD")"

echo "═══ 5. apply --remove-installer 而盘上没有它：动盘之前拒绝 ═══"
G1=$(gpt_sum "$L")
out=$("$G" apply --disk "$L" --release "$REL" --mode reinstall --remove-installer 2>"$W/err8"); rc=$?
check "ERR live-missing touched=no、盘没动" test "$rc/$(grep -o 'code=[a-z-]*' "$W/err8" | head -1)/$(grep -o 'touched=[a-z]*' "$W/err8" | head -1)/$(gpt_sum "$L")" = "1/code=live-missing/touched=no/$G1"

echo "═══ 6. 读进内存的介质不像安装器分区（这里是 ESP）：不认、不删 ═══"
n=$(basename "${L}p1")
printf 'part=%s\ndisk=%s\nstart=%s\nsize=%s\nram=yes\n' "$n" "$(basename "$L")" "$(cat /sys/class/block/$n/start)" "$(cat /sys/class/block/$n/size)" > "$GK3_MEDIUM_FILE"
pl=$("$G" probe 2>/dev/null | grep "^PART path=${L}p1 ")
check "probe：ESP 是介质（medium=yes）但没有 ram=" test "$(field "$pl" medium)/$(field "$pl" ram)" = yes/
G1=$(gpt_sum "$L")
out=$("$G" remove-installer --disk "$L" 2>"$W/err9"); rc=$?
check "remove-installer：拒绝 live-missing、盘没动" test "$rc/$(grep -o 'code=[a-z-]*' "$W/err9" | head -1)/$(gpt_sum "$L")" = "1/code=live-missing/$G1"
out=$("$G" apply --disk "$L" --release "$REL" --mode wipe 2>"$W/err10"); rc=$?
check "整盘清空：介质在这块盘上又不是安装器分区 → 拒绝 wipe-medium、盘没动" test "$rc/$(grep -o 'code=[a-z-]*' "$W/err10" | head -1)/$(gpt_sum "$L")" = "1/code=wipe-medium/$G1"

echo
echo "通过 $PASS，失败 $FAIL"
[ "$FAIL" = 0 ]
