#!/usr/bin/env bash
# 用【真后端】（gk3l，installer/backend/src/linux/）录图形安装器的 fixture（界面开发、出图时回放它们）。
#
#   bash scripts/live/test-in-container.sh scripts/live/gen-fixtures.sh
#   GK3_TEST_BOOTIMG=/repo/out/…/boot.img …   （用真发版 boot.img，进度里的大小更像真的）
#
# 产物：installer/live/testdata/<场景>/{index.txt, *.txt}，格式见 record-fixture.py。
#
# ★ 为什么录而不是手写：手写的 fixture 是"界面对着一份想象中的协议开发"。
#   本仓协议里有好几处不显然（百分号编码、PLANERR 走 stdout、PROGRESS 与 ERR 走 stderr），
#   手写一定会写成作者以为的样子。
#   手写的只有容器里做不到的几项（common/ 里：预检），
#   index.txt 里逐条标了"手写"和理由。
#
# 场景：
#   factory       出厂布局（见 docs/dev/hardware.md），整盘都是 Windows，没有空闲区
#   windows-free  factory 上把 Data 缩掉 80 GiB 之后（代替 Windows 的"压缩卷"，见 win_shrink）→ 走双系统
#   blank         一块空盘 → 走整盘
#   android       blank 上真装一遍之后（"已经装过"：双系统应被拒绝（分区重名）；重新安装可行）
#   windows-live  免 U 盘装双系统：出厂盘缩出空闲区 + 一个放 live 的 FAT32 分区，
#                 安装器就从这块盘上跑（介质与目标同盘）—— 整盘清空要被拦、双系统要放行、介质分区不可删
#   windows-setup Windows 脚本的默认：只缩出 512 MiB 放 GK3LIVE、没有空闲 → 要先回 Windows 压缩卷
#   windows-setup-shrunk  上面那块盘在 Windows 里再缩出 80 GiB 之后 → 走双系统
set -u
cd "$(dirname "$0")/../.."
[ "$(id -u)" = 0 ] || { echo "要 root（用 scripts/live/test-in-container.sh 跑）"; exit 2; }
export GK3_ALLOW_LOOP=1 GK3_MACHINE_ID=0123456789abcdef0123456789abcdef GK3_SKIP_MODEL_CHECK=1
# 测试容器的内核没有 ntfs3：休眠检查改用 ntfs-3g 只读挂载（gk3l 的 GK3_NTFS_MOUNT）
grep -qw ntfs3 /proc/filesystems || export GK3_NTFS_MOUNT="ntfs-3g -o ro"
OUT=installer/live/testdata
W=$(mktemp -d /tmp/gk3-fix.XXXX)
export GK3_RUNDIR=$W/run
# 电源装作接着适配器（容器看到的是宿主的电源，录出来的结果会跟着宿主变）
mkdir -p "$W/ps/ac"; echo Mains > "$W/ps/ac/type"; echo 1 > "$W/ps/ac/online"; export GK3_POWER_SUPPLY_DIR=$W/ps
G=$W/gk3l
sh installer/backend/linux/build.sh "$G" gcc || { echo "gk3l 编不过"; exit 1; }
field() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }
LOOPS=()
cleanup() {
    local m l
    for m in $(findmnt -rno TARGET | grep -e "^$W" -e '^/media/gk3' | sort -r); do umount "$m" 2>/dev/null; done
    # ⚠️★ 按背后的镜像文件找 loop，【不】靠 LOOPS 数组：new_disk 是在 $(…) 里调的，
    #   它往 LOOPS 里加的东西留在子 shell 里 —— 2026-09-24 就这样漏了 58 个 loop，
    #   每个攥着一个已删除的稀疏文件，把 colima 的 98G 盘写满。
    for img in "$W"/*.img; do
        for l in $(losetup -j "$img" 2>/dev/null | cut -d: -f1); do losetup -d "$l" 2>/dev/null; done
    done
    rm -rf "$W"
}
trap cleanup EXIT
new_disk() { truncate -s "$2" "$W/$1.img"; local l; l=$(losetup -fP --show "$W/$1.img"); LOOPS+=("$l"); echo "$l"; }
settle() { partprobe "$1" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1; }
# 代替 Windows 的"压缩卷"（容器里没有 Windows；安装器本身不缩分区）：先缩 NTFS，再在原起点按原 PARTUUID、
# 名字、类型与属性位重建分区项
win_shrink() {   # $1=盘 $2=分区号 $3=目标 MiB
    local disk=$1 n=$2 mib=$3 p st pu pt pl pa
    p=${disk}p$n
    st=$(sgdisk -i "$n" "$disk" | awk '/^First sector:/{print $3}')
    pu=$(sgdisk -i "$n" "$disk" | awk '/^Partition unique GUID:/{print $4}')
    pt=$(sgdisk -i "$n" "$disk" | awk '/^Partition GUID code:/{print $4}')
    pl=$(sgdisk -i "$n" "$disk" | sed -n "s/^Partition name: '\(.*\)'$/\1/p")
    pa=$(sgdisk -i "$n" "$disk" | awk '/^Attribute flags:/{print $3}')
    ntfsresize --force --size "${mib}M" "$p" >/dev/null 2>&1 </dev/null || return 1
    # ntfsresize 缩完会置上 dirty 位，让 Windows 下次开机跑 chkdsk；Windows 自己的压缩卷不会，这里清掉
    ntfsfix -d "$p" >/dev/null 2>&1
    sgdisk -d "$n" "$disk" >/dev/null 2>&1 || return 1
    sgdisk -n "$n:$st:+${mib}M" -t "$n:$pt" -u "$n:$pu" -c "$n:$pl" "$disk" >/dev/null 2>&1 || return 1
    [ "$pa" = 0000000000000000 ] || sgdisk -A "$n:=:$pa" "$disk" >/dev/null 2>&1 || return 1
    settle "$disk"
}
# 一个场景录完就把它的盘整个放掉：每个场景真装一遍会往 488 GB 的稀疏盘里实写约 12 GiB 的 super，
# 攒到最后 docker 的盘（98G，2026-09-27 实测剩 49G）就满了 —— 加了 windows-setup 那一次就是这样死在最后一节
drop_disk() {
    local l=$1 f m
    f=$(losetup -n -O BACK-FILE "$l" 2>/dev/null)
    for m in $(findmnt -rno TARGET,SOURCE | awk -v l="$l" 'index($2, l) == 1 {print $1}' | sort -r); do umount "$m" 2>/dev/null; done
    losetup -d "$l" 2>/dev/null; [ -n "$f" ] && rm -f "$f"
}

# ── 安装介质：一个假 U 盘挂在 /media/gk3，好让 gk3l probe 真的报出 medium=yes ────
STICK=$(new_disk stick 7680M)
sgdisk -o -n 1:2048:0 -t 1:ef00 -c 1:esp "$STICK" >/dev/null 2>&1; settle "$STICK"
mkfs.vfat -F 32 -n GK3LIVE "${STICK}p1" >/dev/null
mkdir -p /media/gk3 && mount "${STICK}p1" /media/gk3 && mkdir -p /media/gk3/gaokun3

# ── 发布目录（与 test-apply.sh 同一套造法；super 做成 12 GiB，进度数字才像真的）──
REL=$W/rel; mkdir -p "$REL"
if [ -n "${GK3_TEST_BOOTIMG:-}" ]; then cp "$GK3_TEST_BOOTIMG" "$REL/boot.img"
else python3 - "$REL" <<'PYEOF'
import os, struct, sys
rel = sys.argv[1]; P = 4096; al = lambda x: (x + P - 1) // P * P
kern, ramd, dtb = b"MZ" + os.urandom(300000), os.urandom(123457), b"\xd0\x0d\xfe\xed" + os.urandom(20001)
c = b"androidboot.hardware=gaokun3 androidboot.boot_devices=soc@0/1c20000.pcie init=/init console=tty0 clk_ignore_unused efi=noruntime fbcon=rotate:1 usbhid.quirks=0x12d1:0x10b8:0x20000000"
h = bytearray(P); h[0:8] = b"ANDROID!"
struct.pack_into("<IIIIIIIIII", h, 8, len(kern), 0x8000, len(ramd), 0x1000000, 0, 0, 0x100, P, 2, 0)
h[64:64 + len(c)] = c; struct.pack_into("<IQII", h, 1632, 0, 0, 1660, len(dtb))
with open(os.path.join(rel, "boot.img"), "wb") as f:
    f.write(h); [f.write(x.ljust(al(len(x)), b"\0")) for x in (kern, ramd, dtb)]
PYEOF
fi
truncate -s 12288M "$W/super.raw"
printf 'gDla' | dd of="$W/super.raw" bs=1 seek=4096 conv=notrunc status=none
for off in 1 900 4000 9000; do head -c 16777216 /dev/urandom | dd of="$W/super.raw" bs=1M seek=$off conv=notrunc status=none; done
img2simg "$W/super.raw" "$W/super.img" >/dev/null && rm "$W/super.raw"
zstd -q -19 --long -f "$W/super.img" -o "$REL/super.img.zst" && rm "$W/super.img"
( cd "$REL" && sha256sum boot.img super.img.zst > install-artifacts.sha256 )
# 发版的校验清单里还有 OTA zip 那一行（scripts/release.sh:136），gk3l release-info 从它取版本名
echo "65905f68074595215edac60616e2f633fb0c6db7fcab6ffb06f38e155b511e92  crDroidAndroid-16.0-20260916-gaokun3-v12.11.zip" >> "$REL/install-artifacts.sha256"
# 假的 systemd-boot：gk3l 要它是 PE 文件（MZ 开头）
{ printf 'MZ'; head -c 65534 /dev/urandom; } > "$W/sdboot.efi"; export GK3_SDBOOT=$W/sdboot.efi

# ── 录制 ─────────────────────────────────────────────────────────────────
# rec <场景> <盘> <文件名> <界面会发出的调用（gk3l 的命令与参数，用 nvme0n1 写）> [实际执行的命令（默认：gk3l 加上这个调用）]
rec() {
    local sc=$1 d=$2 name=$3 call=$4 real=${5:-}
    local dir=$OUT/$sc; mkdir -p "$dir"
    [ -n "$real" ] || real="$G $(printf '%s' "$call" | sed "s#/dev/nvme0n1#$d#g")"
    python3 scripts/live/record-fixture.py "$dir/$name" \
        --sub "${STICK}p=/dev/sda" --sub "$STICK=/dev/sda" --sub "$d=/dev/nvme0n1" \
        --sub "-$(basename "$d")-=-nvme0n1-" \
        --sub "$REL=/media/gk3/gaokun3/payload" --sub "$W=/tmp" \
        -- bash -c "$real"
    printf '%-78s %s\n' "$call" "$name" >> "$dir/index.txt"
}
# 探测只留这块盘和 U 盘（容器里还看得见宿主自己的盘与别的 loop），
# 并把 loop 报不出来的字段按真机填上：内置盘 bus=nvme，U 盘 bus=usb removable=1
probe_of() {   # $2=nostick：这个场景里没有 U 盘（免 U 盘安装）
    local u=$STICK; [ "${2:-}" = nostick ] && u=/nonexistent
    printf '%s' "$G probe | awk -v d='$1' -v u='$u' '
      (\$1==\"DISK\" && \$2==\"path=\"d) { sub(/bus=[^ ]*/, \"bus=nvme\"); print; next }
      (\$1==\"DISK\" && \$2==\"path=\"u) { sub(/bus=[^ ]*/, \"bus=usb\"); sub(/removable=0/, \"removable=1\"); print; next }
      (\$1==\"PART\" && \$3==\"disk=\"d) { print; next }
      (\$1==\"PART\" && \$3==\"disk=\"u) { print; next }
      (\$1==\"FREE\" && (\$2==\"disk=\"d || \$2==\"disk=\"u)) { print }'"
}
header() {   # $1=场景 $2=说明
    mkdir -p "$OUT/$1"; rm -f "$OUT/$1"/*.txt
    { echo "# 场景：$1 —— $2"
      echo "# 录制：scripts/live/gen-fixtures.sh，$(date +%F)，Debian $(cat /etc/debian_version) 容器，loop 设备"
      echo "# 替换：loop 设备 → /dev/nvme0n1（内置盘）与 /dev/sda（安装 U 盘，挂在 /media/gk3）；"
      echo "#       DISK 行的 bus= 按真机填（loop 报不出来）；U 盘的 removable 填 1。其余一律原样。"
      echo "# 格式：<界面发出的调用（gk3l 的命令与参数）>  <文件>。找不到精确匹配时用 '<命令> *' 那一行。"; } > "$OUT/$1/index.txt"
}

# 出厂布局（见 docs/dev/hardware.md）：factory 与 windows-live 两个场景各造一块
make_factory() {
    local DF=$1 TOT LAST E7 S7 E6 S6 E5 S5 n
    TOT=$(blockdev --getsz "$DF"); LAST=$(( TOT - 34 ))
    E7=$LAST;                       S7=$(( (E7 - 2097152 + 1) / 2048 * 2048 ))
    E6=$(( S7 - 1 ));               S6=$(( (E6 - 18 * 2097152 + 1) / 2048 * 2048 ))
    E5=$(( S6 - 1 ));               S5=$(( (E5 - 2097152 + 1) / 2048 * 2048 ))
    sgdisk -o \
      -n 1:2048:+300M -t 1:ef00 -c 1:"EFI system partition" \
      -n 2:0:+16M     -t 2:0c01 -c 2:"Microsoft reserved partition" \
      -n 3:0:+120G    -t 3:0700 -c 3:"Basic data partition" \
      -n 4:0:$(( S5 - 1 )) -t 4:0700 -c 4:"Basic data partition" \
      -n 5:$S5:$E5 -t 5:0700 -c 5:"Basic data partition" \
      -n 6:$S6:$E6 -t 6:0700 -c 6:"Basic data partition" \
      -n 7:$S7:$E7 -t 7:2700 -c 7:"Basic data partition" "$DF" >/dev/null 2>&1
    settle "$DF"
    mkfs.vfat -F 32 -n SYSTEM "${DF}p1" >/dev/null; mkfs.vfat -F 32 -n WINPE "${DF}p5" >/dev/null
    mkntfs -Q -F -L Windows "${DF}p3" >/dev/null 2>&1; mkntfs -Q -F -L Data   "${DF}p4" >/dev/null 2>&1
    mkntfs -Q -F -L Onekey  "${DF}p6" >/dev/null 2>&1; mkntfs -Q -F -L WinRE  "${DF}p7" >/dev/null 2>&1
    # ESP 里放 Windows 的引导件与一块"固件胶囊"，占到接近出厂的用量（docs/dev/hardware.md：188 MiB 空闲）
    mmd -i "${DF}p1" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot ::/EFI/Boot
    head -c 1572864  /dev/urandom > "$W/f1"; mcopy -i "${DF}p1" "$W/f1" ::/EFI/Microsoft/Boot/bootmgfw.efi
    head -c 29360128 /dev/urandom > "$W/f2"; mcopy -i "${DF}p1" "$W/f2" ::/EFI/Microsoft/Boot/BCD-and-fonts.bin
    head -c 1572864  /dev/urandom > "$W/f3"; mcopy -i "${DF}p1" "$W/f3" ::/EFI/Boot/bootaa64.efi
    head -c 73400320 /dev/urandom > "$W/f4"; mcopy -i "${DF}p1" "$W/f4" ::/Persisted_Capsules.bin
    # NTFS 里放一点真数据，好让"最小能缩到多少"不是 0（mount 走 ntfs-3g）
    for n in 3 4; do mkdir -p "$W/n$n"; ntfs-3g "${DF}p$n" "$W/n$n" 2>/dev/null \
        && dd if=/dev/zero of="$W/n$n/data.bin" bs=1M count=$(( n == 3 ? 1024 : 2048 )) status=none; umount "$W/n$n" 2>/dev/null; done
}

echo "═══ factory ═══"
DF=$(new_disk factory 488386M); make_factory "$DF"
header factory "出厂布局（见 docs/dev/hardware.md），整盘都是 Windows，没有空闲区"
rec factory "$DF" probe.txt     "probe" "$(probe_of "$DF")"
rec factory "$DF" esp_info.txt  "esp-info --disk /dev/nvme0n1"
rec factory "$DF" plan-wipe.txt "plan --mode wipe --disk /dev/nvme0n1"
# 没有空闲区时界面用一个空区间问"双系统至少要多少"（lib/session.dart 的 _assessAlong）
rec factory "$DF" plan-along-empty.txt "plan --sector 512 --region-start 0 --region-end 0 --disk /dev/nvme0n1"
# Data 336.6 GiB → 缩掉 80 GiB（下面 windows-free 在同一块盘上接着录）
DATA_MIB=$(( $(blockdev --getsize64 "${DF}p4") / 1048576 ))
win_shrink "$DF" 4 $(( DATA_MIB - 81920 )) || { echo "windows-free：缩 Data 失败"; exit 1; }

# 双系统的一组：探测、ESP、方案（选中的空闲区）、整盘方案、真装一遍。$1=场景 $2=盘 $3=nostick（没有 U 盘）
along_set() {
    local sc=$1 d=$2 fr rs re
    rec "$sc" "$d" probe.txt    "probe" "$(probe_of "$d" "${3:-}")"
    rec "$sc" "$d" esp_info.txt "esp-info --disk /dev/nvme0n1"
    fr=$(bash -c "$(probe_of "$d" "${3:-}")" | grep '^FREE ' | sort -t= -k5 -n | tail -1)
    rs=$(field "$fr" start); re=$(field "$fr" end)
    rec "$sc" "$d" plan-along.txt "plan --sector 512 --region-start $rs --region-end $re --disk /dev/nvme0n1"
    rec "$sc" "$d" plan-wipe.txt  "plan --mode wipe --disk /dev/nvme0n1"
    rec "$sc" "$d" apply-along.txt \
        "apply --release /media/gk3/gaokun3/payload --disk /dev/nvme0n1 --mode alongside --region-start $rs --region-end $re" \
        "$G apply --release $REL --disk $d --mode alongside --region-start $rs --region-end $re"
    printf '%-78s %s\n' "apply *" apply-along.txt >> "$OUT/$sc/index.txt"
}

echo "═══ windows-free ═══"
header windows-free "factory 上把 Data 缩掉 80 GiB 之后（代替 Windows 的压缩卷）—— 走双系统"
along_set windows-free "$DF"
drop_disk "$DF"

echo "═══ windows-live ═══"
# 免 U 盘装双系统：Windows 里先"压缩卷"缩出空闲区、再在空闲区开头建一个 FAT32 小分区放 live，下次开机从它起安装器。
# 压缩卷用 win_shrink 代替，其余照实造。分区的大小与卷标：4 GiB、GK3LIVE
DL=$(new_disk winlive 488386M); make_factory "$DL"
DATA_MIB=$(( $(blockdev --getsize64 "${DL}p4") / 1048576 ))
win_shrink "$DL" 4 $(( DATA_MIB - 81920 )) || { echo "windows-live：缩 Data 失败"; exit 1; }
LF=$(bash -c "$(probe_of "$DL" nostick)" | grep '^FREE ' | sort -t= -k5 -n | tail -1)
sgdisk -n 8:"$(field "$LF" start)":+4G -t 8:0700 -c 8:"Basic data partition" "$DL" >/dev/null 2>&1; settle "$DL"
mkfs.vfat -F 32 -n GK3LIVE "${DL}p8" >/dev/null
umount /media/gk3 && mount "${DL}p8" /media/gk3 && mkdir -p /media/gk3/gaokun3
head -c 65536 /dev/urandom > /media/gk3/gaokun3/live.squashfs
header windows-live "免 U 盘装双系统：Windows 缩出 80 GiB 空闲 + 4 GiB 的 GK3LIVE（FAT32）放 live，安装器就从这块盘上跑"
along_set windows-live "$DL" nostick
umount /media/gk3 && mount "${STICK}p1" /media/gk3
drop_disk "$DL"

echo "═══ windows-setup ═══"
# 只从 Data 缩出 GK3LIVE 要的那一点（512 MiB），GK3LIVE 紧挨在 Data 后面、没有空闲 ——
# 给 Android 的空间要先回 Windows 压缩卷。压缩卷同样用 win_shrink 代替。
DS=$(new_disk winsetup 488386M); make_factory "$DS"
DATA_MIB=$(( $(blockdev --getsize64 "${DS}p4") / 1048576 ))
win_shrink "$DS" 4 $(( DATA_MIB - 512 )) || { echo "windows-setup：缩 Data 失败"; exit 1; }
LF=$(bash -c "$(probe_of "$DS" nostick)" | grep '^FREE ' | sort -t= -k5 -n | tail -1)
sgdisk -n 8:"$(field "$LF" start)":"$(field "$LF" end)" -t 8:0700 -c 8:"Basic data partition" "$DS" >/dev/null 2>&1; settle "$DS"
mkfs.vfat -F 32 -n GK3LIVE "${DS}p8" >/dev/null
umount /media/gk3 && mount "${DS}p8" /media/gk3 && mkdir -p /media/gk3/gaokun3
head -c 65536 /dev/urandom > /media/gk3/gaokun3/live.squashfs
header windows-setup "只缩出 512 MiB 放 GK3LIVE（紧挨在 Data 后面），没有空闲 —— 要先回 Windows 压缩卷"
rec windows-setup "$DS" probe.txt     "probe" "$(probe_of "$DS" nostick)"
rec windows-setup "$DS" esp_info.txt  "esp-info --disk /dev/nvme0n1"
rec windows-setup "$DS" plan-along-empty.txt "plan --sector 512 --region-start 0 --region-end 0 --disk /dev/nvme0n1"
rec windows-setup "$DS" plan-wipe.txt "plan --mode wipe --disk /dev/nvme0n1"
DATA_MIB=$(( $(blockdev --getsize64 "${DS}p4") / 1048576 ))
win_shrink "$DS" 4 $(( DATA_MIB - 81920 )) || { echo "windows-setup-shrunk：缩 Data 失败"; exit 1; }

echo "═══ windows-setup-shrunk ═══"
header windows-setup-shrunk "windows-setup 上在 Windows 里再缩出 80 GiB 之后：空闲在 Data 与 GK3LIVE 之间 —— 走双系统"
along_set windows-setup-shrunk "$DS" nostick
umount /media/gk3 && mount "${STICK}p1" /media/gk3
drop_disk "$DS"

echo "═══ blank ═══"
DB=$(new_disk blank 488386M); sgdisk -o "$DB" >/dev/null 2>&1; settle "$DB"
header blank "一块空盘（只有一张空 GPT）—— 走整盘"
rec blank "$DB" probe.txt           "probe" "$(probe_of "$DB")"
rec blank "$DB" plan-wipe.txt       "plan --mode wipe --disk /dev/nvme0n1"
rec blank "$DB" plan-wipe-small.txt "plan --mode wipe --disk /dev/nvme0n1 --userdata-mib 100"
rec blank "$DB" apply-wipe.txt \
    "apply --release /media/gk3/gaokun3/payload --disk /dev/nvme0n1 --mode wipe" \
    "$G apply --release $REL --disk $DB --mode wipe"
printf '%-78s %s\n' "apply *" apply-wipe.txt >> "$OUT/blank/index.txt"

echo "═══ android ═══"
header android "blank 上真装了一遍之后（已经装过）—— 双系统应被拒绝（分区重名），重新安装可行"
rec android "$DB" probe.txt     "probe" "$(probe_of "$DB")"
rec android "$DB" esp_info.txt  "esp-info --disk /dev/nvme0n1"
rec android "$DB" plan-wipe.txt "plan --mode wipe --disk /dev/nvme0n1"
for k in no yes; do
  rec android "$DB" "plan-reinstall-keep-$k.txt" "plan --mode reinstall --disk /dev/nvme0n1 --keep-data $k"
done
rec android "$DB" apply-reinstall.txt \
    "apply --release /media/gk3/gaokun3/payload --disk /dev/nvme0n1 --mode reinstall --keep-data no" \
    "$G apply --release $REL --disk $DB --mode reinstall --keep-data no"
printf '%-78s %s\n' "apply *" apply-reinstall.txt >> "$OUT/android/index.txt"

echo "═══ common（与盘无关）═══"
C=$OUT/common; mkdir -p "$C"; rm -f -- "${C:?}"/*.txt
{ echo "# 与盘无关的调用。所有场景找不到时都回落到这里。"
  echo "# 标了「手写」的几条是容器里做不到的：不是这台机器（没有 DMI）、起不了 systemd 单元。"
  echo "# 它们照着 gk3l 真实输出的格式写：预检的值取自实测（见 docs/dev/hardware.md）。"; } > "$C/index.txt"
# 手写：预检（容器不是 gaokun3）
cat > "$C/preflight-ok.txt" <<'EOF'
O PREFLIGHT gk3l=dev manufacturer=HUAWEI model=GK-W7X model_ok=yes ac=no battery=76 battery_min=15
X 0
EOF
cat > "$C/preflight-lowbatt.txt" <<'EOF'
O PREFLIGHT gk3l=dev manufacturer=HUAWEI model=GK-W7X model_ok=yes ac=no battery=9 battery_min=15
X 0
EOF
cat > "$C/preflight-model.txt" <<'EOF'
O PREFLIGHT gk3l=dev manufacturer=HUAWEI model=GK-W57 model_ok=no ac=yes battery=40 battery_min=15
X 0
EOF
printf '%-78s %s\n' "preflight" "preflight-ok.txt   # 手写（容器不是 gaokun3）" >> "$C/index.txt"
echo "#  preflight-lowbatt.txt：电量 9%、没接电源；preflight-model.txt：型号不是 GK-W7X（测试用 overrides 选它们）" >> "$C/index.txt"
# 半手写（双系统，只给测试按文件名用 overrides）：从 windows-free 录到的那一行 ESP 记录改几个字段 —— 这些状态要一整块
# 带 BitLocker 卷 / 休眠的 Windows 盘才造得出来，后端的判据在 installer/backend/test/linux/test-apply.sh 里真测过；这里只给界面用
WESP=$(grep '^O ESP ' "$OUT/windows-free/esp_info.txt")
esp_var() { printf '%s\nX 0\n' "$(printf '%s' "$WESP" | sed "$@")"; }
esp_var -e 's/ bootaa64=[a-z]*/ bootaa64=other/' -e 's/ bitlocker=no/ bitlocker=yes/' -e 's/ hibernated=[a-z]*/ hibernated=unknown/' > "$C/esp_info-bitlocker.txt"
esp_var -e 's/ hibernated=[a-z]*/ hibernated=yes/' > "$C/esp_info-hibernated.txt"
esp_var -e 's/ size_mib=[0-9]*/ size_mib=100/' -e 's/ free_mib=[0-9]*/ free_mib=70/' -e 's/ small=no/ small=yes/' > "$C/esp_info-small.txt"
{ grep -v '^X ' "$OUT/windows-free/apply-along.txt"; echo 'O NOTE code=loadervar-stuck name=LoaderEntryDefault value=auto-windows'; echo 'X 0'; } > "$C/apply-note.txt"
{ echo "#  esp_info-bitlocker.txt：Windows 卷是 BitLocker、BOOTAA64 还是 Windows 的（确认页要勾恢复密钥）"
  echo "#  esp_info-hibernated.txt：Windows 在休眠（双系统 / 重新安装禁用）"
  echo "#  esp_info-small.txt：100 MiB 的 ESP（双系统明确不装）"
  echo "#  apply-note.txt：windows-free 录的那次 apply + 一条 NOTE code=loadervar-stuck（EFI 变量删不掉，完成页要说）"
  echo "#  ↑ 这四份是从录到的输出改字段得来的（半手写，gen-fixtures.sh 里写着怎么改）"; } >> "$C/index.txt"
# 写盘任务。没有在跑的（默认）；"界面崩过又起来、apply 还在跑"那种给测试按文件名用。
# 手写：容器里起不了 systemd 单元；格式照 gk3l job status 的输出写。跟读的内容就是 blank 场景录的那次 apply
printf 'X 0\n' > "$C/job_status-none.txt"
printf '%-78s %s\n' "job status" "job_status-none.txt   # 手写（没有在跑的写盘任务）" >> "$C/index.txt"
cat > "$C/job_status-running.txt" <<'EOF'
O JOB id=20261113-101500-4242-7a3f cmd=apply%20--release%20/media/gk3/gaokun3/payload%20--disk%20/dev/nvme0n1%20--mode%20wipe state=running rc=- mode=systemd-run unit=gk3-job-20261113-101500-4242-7a3f
X 0
EOF
cp "$OUT/blank/apply-wipe.txt" "$C/job_follow-apply.txt"
echo "#  job_status-running.txt / job_follow-apply.txt：界面重新起来时 apply 还在跑（测试用 overrides 选它们）" >> "$C/index.txt"
# 录：日志另存 —— 另插一个 FAT 的 U 盘（Basic data 类型，像普通 U 盘那样）+ 安装介质本身（ESP 类型，esp=yes）。
# 只留这两块的行：容器里别的场景留下的 loop 盘（GK3_ALLOW_LOOP=1 下也算"可移动"）不是真机上会有的东西
U2=$(new_disk usb2 1G); sgdisk -o -n 1:2048:0 -t 1:0700 -c 1:"Basic data partition" "$U2" >/dev/null 2>&1; settle "$U2"
mkfs.vfat -F 32 -n KINGSTON "${U2}p1" >/dev/null
python3 scripts/live/record-fixture.py "$C/log_targets.txt" --sub "${STICK}p=/dev/sda" --sub "${U2}p=/dev/sdb" \
    -- bash -c "$G log-targets | grep -e 'part=${STICK}p' -e 'part=${U2}p'"
drop_disk "$U2"
printf '%-78s %s\n' "log-targets" "log_targets.txt   # 录（假 U 盘挂在 /media/gk3）" >> "$C/index.txt"
python3 scripts/live/record-fixture.py "$C/save_logs.txt" --sub "${STICK}p=/dev/sda" \
    -- bash -c "$G save-logs --to ${STICK}p1"
printf '%-78s %s\n' "save-logs *" "save_logs.txt   # 录（存到假 U 盘上）" >> "$C/index.txt"
# 录：安装介质里带了什么
python3 scripts/live/record-fixture.py "$C/release_info.txt" --sub "$REL=/media/gk3/gaokun3/payload" \
    -- bash -c "$G release-info --dir $REL"
printf '%-78s %s\n' "release-info" "release_info.txt   # 录" >> "$C/index.txt"
python3 scripts/live/record-fixture.py "$C/release_info-none.txt" -- bash -c "$G release-info"
echo "#  release_info-none.txt：介质里没有系统文件（界面测试用 overrides 选它）" >> "$C/index.txt"
# 录：删除分区 —— 在另一块出厂布局的盘上真做一遍。放 common/（任何场景都能用），界面按 '<命令> *' 取；
# 做完之后界面会重新探测，拿到的仍是各场景自己那份 probe（测试只核对发出去的调用）
DE=$(new_disk edit 488386M); make_factory "$DE"
recc() {   # 和 rec 一样，但录进 common/；$1=文件名 $2=界面发出的调用（nvme0n1）$3=实际执行的命令
    python3 scripts/live/record-fixture.py "$C/$1" --sub "${DE}p=/dev/nvme0n1p" --sub "$DE=/dev/nvme0n1" --sub "-$(basename "$DE")-=-nvme0n1-" --sub "$W=/tmp" \
        --sub "${STICK}p=/dev/sda" -- bash -c "$3"
    printf '%-78s %s\n' "$2" "$1   # 录（出厂布局的另一块盘上真做）" >> "$C/index.txt"
}
recc part_delete.txt   "delete *"  "$G delete --part ${DE}p6"
# 反例：删 ESP（界面不给按钮，但后端要拒绝 —— 测试用 overrides 换进来）
recc part_delete-esp.txt "delete-esp" "$G delete --part ${DE}p1"

# 录：一块 MBR 盘 + 一块读不出分区表的盘（loop 叠在 dm-error 上，GPT 那几个扇区读不出），与 U 盘一起探测。
# 替换：MBR 盘 → /dev/nvme0n1，读不出的盘 → /dev/sdb；bus= 按真机填；只留这三块盘的行
DM=$(new_disk mbr 30720M); printf 'label: dos\n,300M,ef\n,4G,7\n' | sfdisk -q "$DM"; settle "$DM"
mkfs.vfat -F 32 "${DM}p1" >/dev/null
DEB=$(new_disk eiobase 20480M); sgdisk -o "$DEB" >/dev/null 2>&1
NS=$(( 20480 * 2048 ))
printf '0 1 linear %s 0\n1 33 error\n34 %s linear %s 34\n' "$DEB" $((NS - 34)) "$DEB" | dmsetup create --noudevsync gk3-fix-eio
EIO=$(losetup -f --show "/dev/$(dmsetup info -c --noheadings -o blkdevname gk3-fix-eio | tr -d ' ')")
python3 scripts/live/record-fixture.py "$W/probe-raw.txt" --sub "${STICK}p=/dev/sda" --sub "$STICK=/dev/sda" \
    --sub "${DM}p=/dev/nvme0n1p" --sub "$DM=/dev/nvme0n1" --sub "$EIO=/dev/sdb" -- "$G" probe
python3 - "$W/probe-raw.txt" "$C/probe-mbr-unreadable.txt" <<'PY'
import re, sys
keep = {"/dev/sda": "usb", "/dev/nvme0n1": "nvme", "/dev/sdb": "usb"}
rows, skip_bang = [], True
for l in open(sys.argv[1]).read().splitlines():
    if l.startswith("D "):
        continue
    if l.startswith("X "):
        rows.append(l)
        continue
    m = re.search(r" (?:path|disk)=(/dev/[a-z0-9]+)", l)
    if l.startswith("O DISK "):
        d = m.group(1) if m else ""
        if d in keep:
            l = re.sub(r" bus=\S+", " bus=" + keep[d], l)
            if d == "/dev/sda":
                l = l.replace(" removable=0", " removable=1")
            rows.append(l)
        continue
    if l.startswith("O PART ") or l.startswith("O FREE "):
        d = re.search(r" disk=(\S+)", l)
        if d and d.group(1) in keep:
            rows.append(l)
        continue
    if l.startswith("E ERR "):
        skip_bang = not (m and m.group(1) in keep)
        if not skip_bang:
            rows.append(l)
        continue
    if l.startswith("E !! ") and not skip_bang:
        rows.append(l)
        skip_bang = True
open(sys.argv[2], "w").write("\n".join(rows) + "\n")
PY
echo "#  probe-mbr-unreadable.txt：一块 MBR 盘（nvme0n1）+ 一块读不出分区表的盘（sdb，dm-error）（界面测试用 overrides 选它）" >> "$C/index.txt"
losetup -d "$EIO"; for i in 1 2 3 4 5 6 7 8 9 10; do dmsetup remove --noudevsync gk3-fix-eio 2>/dev/null && break; sleep 0.5; done
drop_disk "$DM"; drop_disk "$DEB"

echo; echo "fixture 写到 $OUT/："
find "$OUT" -name '*.txt' | sort | sed 's/^/  /'
