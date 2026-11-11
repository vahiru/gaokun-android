#!/usr/bin/env bash
# 用【真后端】录图形安装器的 fixture（界面在 Mac 上开发、golden 出图时回放它们）。
#
#   bash scripts/live/test-in-container.sh scripts/live/gen-fixtures.sh
#   GK3_TEST_BOOTIMG=/repo/out/…/boot.img …   （用真发版 boot.img，进度里的大小更像真的）
#
# 产物：installer/live/testdata/<场景>/{index.txt, *.txt}，格式见 record-fixture.py。
#
# ★ 为什么录而不是手写：手写的 fixture 是"界面对着一份想象中的协议开发"。
#   本仓协议里有好几处不显然（百分号编码、PLANERR 走 stdout、PROGRESS 走 stderr、
#   gk3__run 往 stdout 回显 "+ 命令"），手写一定会写成作者以为的样子。
#   手写的只有容器里做不到的几项（common/ 里：预检、连 WiFi、下载清单），
#   index.txt 里逐条标了"手写"和理由。
#
# 场景：
#   factory       出厂布局（见 docs/dev/hardware.md），整盘都是 Windows，没有空闲区
#   windows-free  factory 上【真跑一次 gk3_shrink】把 Data 缩掉 80 GiB 之后 → 走双系统
#   blank         一块空盘 → 走整盘
#   android       blank 上真装一遍之后（"已经装过"：双系统应被 partlabel-conflict 拒绝；重新安装可行）
#   windows-live  免 U 盘装双系统：出厂盘缩出空闲区 + 一个放 live 的 FAT32 分区，
#                 安装器就从这块盘上跑（介质与目标同盘）—— 整盘清空要被拦、双系统要放行、介质分区不可缩
#   windows-setup 2026-09-27 起 Windows 脚本的默认：只缩出 512 MiB 放 GK3LIVE、没有空闲 → 在安装器里缩 Data
#   windows-setup-shrunk  上面那块盘在安装器里真缩了一次 Data 之后 → 走双系统
set -u
cd "$(dirname "$0")/../.."
[ "$(id -u)" = 0 ] || { echo "要 root（用 scripts/live/test-in-container.sh 跑）"; exit 2; }
. scripts/live/installer-lib.sh
export GK3_ALLOW_LOOP=1 GK3_MACHINE_ID=0123456789abcdef0123456789abcdef
OUT=installer/live/testdata
W=$(mktemp -d /tmp/gk3-fix.XXXX)
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
# 一个场景录完就把它的盘整个放掉：每个场景真装一遍会往 488 GB 的稀疏盘里实写约 12 GiB 的 super，
# 攒到最后 docker 的盘（98G，2026-09-27 实测剩 49G）就满了 —— 加了 windows-setup 那一次就是这样死在最后一节
drop_disk() {
    local l=$1 f m
    f=$(losetup -n -O BACK-FILE "$l" 2>/dev/null)
    for m in $(findmnt -rno TARGET,SOURCE | awk -v l="$l" 'index($2, l) == 1 {print $1}' | sort -r); do umount "$m" 2>/dev/null; done
    losetup -d "$l" 2>/dev/null; [ -n "$f" ] && rm -f "$f"
}

# ── 安装介质：一个假 U 盘挂在 /media/gk3，好让 gk3_probe 真的报出 medium=yes ────
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
# 发版的校验清单里还有 OTA zip 那一行（scripts/release.sh:136），gk3_release_info 从它取版本名
echo "65905f68074595215edac60616e2f633fb0c6db7fcab6ffb06f38e155b511e92  crDroidAndroid-16.0-20260916-gaokun3-v12.11.zip" >> "$REL/install-artifacts.sha256"
for n in rescue.squashfs initramfs.img; do head -c 65536 /dev/urandom > "/media/gk3/gaokun3/$n"; done
head -c 65536 /dev/urandom > "$W/sdboot.efi"; export GK3_SDBOOT=$W/sdboot.efi

# ── 录制 ─────────────────────────────────────────────────────────────────
# rec <场景> <盘> <文件名> <界面会发出的调用（用 nvme0n1 写）> [实际执行的命令]
rec() {
    local sc=$1 d=$2 name=$3 call=$4 real=${5:-}
    local dir=$OUT/$sc; mkdir -p "$dir"
    [ -n "$real" ] || real=$(printf '%s' "$call" | sed "s#/dev/nvme0n1#$d#g")
    python3 scripts/live/record-fixture.py "$dir/$name" \
        --sub "${STICK}p=/dev/sda" --sub "$STICK=/dev/sda" --sub "$d=/dev/nvme0n1" \
        --sub "-$(basename "$d")-=-nvme0n1-" \
        --sub "$REL=/media/gk3/gaokun3/payload" --sub "$W=/tmp" \
        -- bash -c ". scripts/live/installer-lib.sh && $real"
    printf '%-78s %s\n' "$call" "$name" >> "$dir/index.txt"
}
# 探测只留这块盘和 U 盘（容器里还看得见 colima 虚拟机自己的 vda 与别的 loop），
# 并把 loop 报不出来的两个字段按真机填上：内置盘 tran=nvme，U 盘 tran=usb removable=1
probe_of() {   # $2=nostick：这个场景里没有 U 盘（免 U 盘安装）
    local u=$STICK; [ "${2:-}" = nostick ] && u=/nonexistent
    printf '%s' "gk3_probe | awk -v d='$1' -v u='$u' '
      (\$1==\"DISK\" && \$2==\"path=\"d) { sub(/tran=[^ ]*/, \"tran=nvme\"); print; next }
      (\$1==\"DISK\" && \$2==\"path=\"u) { sub(/tran=[^ ]*/, \"tran=usb\"); sub(/removable=0/, \"removable=1\"); print; next }
      (\$1==\"PART\" && (index(\$2, \"path=\"d\"p\")==1 || index(\$2, \"path=\"u\"p\")==1)) { print; next }
      (\$1==\"FREE\" && (\$2==\"disk=\"d || \$2==\"disk=\"u)) { print }'"
}
header() {   # $1=场景 $2=说明
    mkdir -p "$OUT/$1"; rm -f "$OUT/$1"/*.txt
    { echo "# 场景：$1 —— $2"
      echo "# 录制：scripts/live/gen-fixtures.sh，$(date +%F)，Debian $(cat /etc/debian_version) 容器，loop 设备"
      echo "# 替换：loop 设备 → /dev/nvme0n1（内置盘）与 /dev/sda（安装 U 盘，挂在 /media/gk3）；"
      echo "#       DISK 行的 tran= 按真机填（loop 报不出来）；U 盘的 removable 填 1。其余一律原样。"
      echo "# 格式：<界面发出的调用>  <文件>。找不到精确匹配时用 '<函数> *' 那一行。"; } > "$OUT/$1/index.txt"
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
rec factory "$DF" probe.txt           "gk3_probe" "$(probe_of "$DF")"
rec factory "$DF" esp_info.txt        "gk3_esp_info /dev/nvme0n1p1"
rec factory "$DF" shrink_scan.txt     "gk3_shrink_scan /dev/nvme0n1"
rec factory "$DF" plan-wipe-rescue.txt   "gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue yes"
rec factory "$DF" plan-wipe-norescue.txt "gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue no"
# 没有空闲区时界面用一个空区间问"双系统至少要多少"（lib/session.dart 的 _assessAlong）
for r in yes no; do
  rec factory "$DF" "plan-along-empty-$r.txt" \
      "gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue $r --region-start 0 --region-end 0 --esp /dev/nvme0n1p1"
done
# Data 336.6 GiB → 缩掉 80 GiB
DATA_MIB=$(( $(blockdev --getsize64 "${DF}p4") / 1048576 )); TARGET=$(( DATA_MIB - 81920 ))
rec factory "$DF" shrink.txt          "gk3_shrink /dev/nvme0n1p4 $TARGET"
echo "gk3_shrink *                                                                   shrink.txt" >> "$OUT/factory/index.txt"
# 场景切换：缩成功之后，接着按"缩完之后"那份（下面 windows-free 就是在同一块盘上缩完录的）回放
echo "@next gk3_shrink windows-free" >> "$OUT/factory/index.txt"

echo "═══ windows-free ═══"
header windows-free "factory 上真跑了一次 gk3_shrink（Data 缩掉 80 GiB）之后 —— 走双系统"
rec windows-free "$DF" probe.txt       "gk3_probe" "$(probe_of "$DF")"
rec windows-free "$DF" esp_info.txt    "gk3_esp_info /dev/nvme0n1p1"
rec windows-free "$DF" shrink_scan.txt "gk3_shrink_scan /dev/nvme0n1"
FREE=$(bash -c ". scripts/live/installer-lib.sh && $(probe_of "$DF")" | grep '^FREE ' | sort -t= -k5 -n | tail -1)
RS=$(gk3__f "$FREE" start); RE=$(gk3__f "$FREE" end)
for r in yes no; do
  rec windows-free "$DF" "plan-along-rescue-$r.txt" \
      "gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue $r --region-start $RS --region-end $RE --esp /dev/nvme0n1p1"
done
rec windows-free "$DF" plan-wipe-rescue.txt "gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue yes"
rec windows-free "$DF" apply-along.txt \
    "gk3_apply --disk /dev/nvme0n1 --mode alongside --rescue yes --release /media/gk3/gaokun3/payload --region-start $RS --region-end $RE --esp /dev/nvme0n1p1" \
    "gk3_apply --disk $DF --mode alongside --rescue yes --release $REL --region-start $RS --region-end $RE --esp ${DF}p1"
echo "gk3_apply *                                                                    apply-along.txt" >> "$OUT/windows-free/index.txt"
drop_disk "$DF"

echo "═══ windows-live ═══"
# 免 U 盘装双系统的目标流程：Windows 里先"压缩卷"缩出空闲区、再在空闲区开头建一个 FAT32 小分区放 live，
# 下次开机从它起安装器。这里用 gk3_shrink 代替 Windows 的压缩卷（容器里没有 Windows），其余照实造。
# ⚠️ Windows 那一侧的引导程序还没做（要用户定方案），这个分区的大小与卷标是按设想写的：4 GiB、GK3LIVE。
DL=$(new_disk winlive 488386M); make_factory "$DL"
DATA_MIB=$(( $(blockdev --getsize64 "${DL}p4") / 1048576 ))
gk3_shrink "${DL}p4" $(( DATA_MIB - 81920 )) >/dev/null 2>&1 || { echo "windows-live：缩 Data 失败"; exit 1; }
# ⚠️ ntfsresize 缩完会【故意】置上 dirty 位，让 Windows 下次开机跑一遍 chkdsk（ntfs-3g ntfsresize.c:2987）；
#   Windows 自己的"压缩卷"不会。这里是在【代替 Windows】，所以把它清掉 —— 不清的话下一次缩会被（正确地）拒绝
ntfsfix -d "${DL}p4" >/dev/null 2>&1
settle "$DL"
LF=$(bash -c ". scripts/live/installer-lib.sh && $(probe_of "$DL" nostick)" | grep '^FREE ' | sort -t= -k5 -n | tail -1)
LS=$(gk3__f "$LF" start)
sgdisk -n 8:"$LS":+4G -t 8:0700 -c 8:"Basic data partition" "$DL" >/dev/null 2>&1; settle "$DL"
mkfs.vfat -F 32 -n GK3LIVE "${DL}p8" >/dev/null
umount /media/gk3 && mount "${DL}p8" /media/gk3 && mkdir -p /media/gk3/gaokun3
for n in rescue.squashfs initramfs.img live.squashfs; do head -c 65536 /dev/urandom > "/media/gk3/gaokun3/$n"; done
header windows-live "免 U 盘装双系统：Windows 缩出 80 GiB 空闲 + 4 GiB 的 GK3LIVE（FAT32）放 live，安装器就从这块盘上跑"
rec windows-live "$DL" probe.txt       "gk3_probe" "$(probe_of "$DL" nostick)"
rec windows-live "$DL" esp_info.txt    "gk3_esp_info /dev/nvme0n1p1"
rec windows-live "$DL" shrink_scan.txt "gk3_shrink_scan /dev/nvme0n1"
LF=$(bash -c ". scripts/live/installer-lib.sh && $(probe_of "$DL" nostick)" | grep '^FREE ' | sort -t= -k5 -n | tail -1)
RS=$(gk3__f "$LF" start); RE=$(gk3__f "$LF" end)
for r in yes no; do
  rec windows-live "$DL" "plan-along-rescue-$r.txt" \
      "gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue $r --region-start $RS --region-end $RE --esp /dev/nvme0n1p1"
done
rec windows-live "$DL" plan-wipe-rescue.txt "gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue yes"
rec windows-live "$DL" apply-along.txt \
    "gk3_apply --disk /dev/nvme0n1 --mode alongside --rescue yes --release /media/gk3/gaokun3/payload --region-start $RS --region-end $RE --esp /dev/nvme0n1p1" \
    "gk3_apply --disk $DL --mode alongside --rescue yes --release $REL --region-start $RS --region-end $RE --esp ${DL}p1"
echo "gk3_apply *                                                                    apply-along.txt" >> "$OUT/windows-live/index.txt"
umount /media/gk3 && mount "${STICK}p1" /media/gk3
drop_disk "$DL"

echo "═══ windows-setup ═══"
# ★ 2026-09-27 起 Windows 脚本的默认（用户："安装安装器应该仅划分自己需要的空间"）：Windows 只从 Data 缩出 GK3LIVE 要的
#   那一点（gaokun3-setup.ps1 的 Get-LiveMiB，不带载荷时 512 MiB），GK3LIVE 紧挨在 Data 后面、【没有空闲】——
#   给 Android 的空间在安装器里缩 Data。这里同样用 gk3_shrink 代替 Windows 的压缩卷。
DS=$(new_disk winsetup 488386M); make_factory "$DS"
DATA_MIB=$(( $(blockdev --getsize64 "${DS}p4") / 1048576 ))
gk3_shrink "${DS}p4" $(( DATA_MIB - 512 )) >/dev/null 2>&1 || { echo "windows-setup：缩 Data 失败"; exit 1; }
# ⚠️ ntfsresize 缩完会【故意】置上 dirty 位，让 Windows 下次开机跑一遍 chkdsk（ntfs-3g ntfsresize.c:2987）；
#   Windows 自己的"压缩卷"不会。这里是在【代替 Windows】，所以把它清掉 —— 不清的话下一次缩会被（正确地）拒绝
ntfsfix -d "${DS}p4" >/dev/null 2>&1
settle "$DS"
LF=$(bash -c ". scripts/live/installer-lib.sh && $(probe_of "$DS" nostick)" | grep '^FREE ' | sort -t= -k5 -n | tail -1)
sgdisk -n 8:"$(gk3__f "$LF" start)":"$(gk3__f "$LF" end)" -t 8:0700 -c 8:"Basic data partition" "$DS" >/dev/null 2>&1; settle "$DS"
mkfs.vfat -F 32 -n GK3LIVE "${DS}p8" >/dev/null
umount /media/gk3 && mount "${DS}p8" /media/gk3 && mkdir -p /media/gk3/gaokun3
for n in rescue.squashfs initramfs.img live.squashfs; do head -c 65536 /dev/urandom > "/media/gk3/gaokun3/$n"; done
header windows-setup "Windows 脚本的默认：只缩出 512 MiB 放 GK3LIVE（紧挨在 Data 后面），没有空闲 —— 在安装器里缩 Data 腾地方"
rec windows-setup "$DS" probe.txt       "gk3_probe" "$(probe_of "$DS" nostick)"
rec windows-setup "$DS" esp_info.txt    "gk3_esp_info /dev/nvme0n1p1"
rec windows-setup "$DS" shrink_scan.txt "gk3_shrink_scan /dev/nvme0n1"
for r in yes no; do
  rec windows-setup "$DS" "plan-along-empty-$r.txt" \
      "gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue $r --region-start 0 --region-end 0 --esp /dev/nvme0n1p1"
done
rec windows-setup "$DS" plan-wipe-rescue.txt "gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue yes"
DATA_MIB=$(( $(blockdev --getsize64 "${DS}p4") / 1048576 ))
rec windows-setup "$DS" shrink.txt "gk3_shrink /dev/nvme0n1p4 $(( DATA_MIB - 81920 ))"
echo "gk3_shrink *                                                                   shrink.txt" >> "$OUT/windows-setup/index.txt"
echo "@next gk3_shrink windows-setup-shrunk" >> "$OUT/windows-setup/index.txt"

echo "═══ windows-setup-shrunk ═══"
header windows-setup-shrunk "windows-setup 上在安装器里真缩了一次 Data（80 GiB）之后：空闲在 Data 与 GK3LIVE 之间 —— 走双系统"
rec windows-setup-shrunk "$DS" probe.txt       "gk3_probe" "$(probe_of "$DS" nostick)"
rec windows-setup-shrunk "$DS" esp_info.txt    "gk3_esp_info /dev/nvme0n1p1"
rec windows-setup-shrunk "$DS" shrink_scan.txt "gk3_shrink_scan /dev/nvme0n1"
LF=$(bash -c ". scripts/live/installer-lib.sh && $(probe_of "$DS" nostick)" | grep '^FREE ' | sort -t= -k5 -n | tail -1)
RS=$(gk3__f "$LF" start); RE=$(gk3__f "$LF" end)
for r in yes no; do
  rec windows-setup-shrunk "$DS" "plan-along-rescue-$r.txt" \
      "gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue $r --region-start $RS --region-end $RE --esp /dev/nvme0n1p1"
done
rec windows-setup-shrunk "$DS" plan-wipe-rescue.txt "gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue yes"
rec windows-setup-shrunk "$DS" apply-along.txt \
    "gk3_apply --disk /dev/nvme0n1 --mode alongside --rescue yes --release /media/gk3/gaokun3/payload --region-start $RS --region-end $RE --esp /dev/nvme0n1p1" \
    "gk3_apply --disk $DS --mode alongside --rescue yes --release $REL --region-start $RS --region-end $RE --esp ${DS}p1"
echo "gk3_apply *                                                                    apply-along.txt" >> "$OUT/windows-setup-shrunk/index.txt"
umount /media/gk3 && mount "${STICK}p1" /media/gk3
drop_disk "$DS"

echo "═══ blank ═══"
DB=$(new_disk blank 488386M); sgdisk -o "$DB" >/dev/null 2>&1; settle "$DB"
header blank "一块空盘（只有一张空 GPT）—— 走整盘"
rec blank "$DB" probe.txt                "gk3_probe" "$(probe_of "$DB")"
rec blank "$DB" plan-wipe-rescue.txt     "gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue yes"
rec blank "$DB" plan-wipe-norescue.txt   "gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue no"
rec blank "$DB" plan-wipe-small.txt      "gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue yes --userdata-mib 100"
rec blank "$DB" apply-wipe.txt \
    "gk3_apply --disk /dev/nvme0n1 --mode wipe --rescue yes --release /media/gk3/gaokun3/payload" \
    "gk3_apply --disk $DB --mode wipe --rescue yes --release $REL"
echo "gk3_apply *                                                                    apply-wipe.txt" >> "$OUT/blank/index.txt"

echo "═══ android ═══"
header android "blank 上真装了一遍之后（已经装过）—— 双系统应被 partlabel-conflict 拒绝"
rec android "$DB" probe.txt       "gk3_probe" "$(probe_of "$DB")"
rec android "$DB" esp_info.txt    "gk3_esp_info /dev/nvme0n1p1"
for r in yes no; do
  rec android "$DB" "plan-along-$r.txt" "gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue $r --region-start 0 --region-end 0 --esp /dev/nvme0n1p1"
done
rec android "$DB" plan-wipe-rescue.txt "gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue yes"
# 重新安装（用户 2026-09-25）：界面会发的四种组合（救援装不装 × 数据留不留），最后真装一遍
for r in yes no; do for k in no yes; do
  rec android "$DB" "plan-reinstall-rescue-$r-keep-$k.txt" \
      "gk3_plan --disk /dev/nvme0n1 --mode reinstall --rescue $r --esp /dev/nvme0n1p1 --keep-data $k"
done; done
rec android "$DB" apply-reinstall.txt \
    "gk3_apply --release /media/gk3/gaokun3/payload --disk /dev/nvme0n1 --mode reinstall --rescue yes --esp /dev/nvme0n1p1 --keep-data no" \
    "gk3_apply --release $REL --disk $DB --mode reinstall --rescue yes --esp ${DB}p1 --keep-data no"
echo "gk3_apply *                                                                    apply-reinstall.txt" >> "$OUT/android/index.txt"

echo "═══ common（与盘无关）═══"
C=$OUT/common; mkdir -p "$C"; rm -f "$C"/*.txt
{ echo "# 与盘无关的调用。所有场景找不到时都回落到这里。"
  echo "# 标了「手写」的几条是容器里做不到的：不是这台机器（没有 DMI）、没有 wlan0、下载清单还不存在。"
  echo "# 它们照着真实输出的格式写：预检的值取自实测 dmesg（见 docs/dev/hardware.md）。"; } > "$C/index.txt"
# 手写：预检（容器不是 gaokun3）
cat > "$C/preflight-ok.txt" <<'EOF'
O CHECK id=root ok=yes
O CHECK id=uefi ok=yes
O CHECK id=model ok=yes value=GK-W7X
O CHECK id=bios ok=yes value=2.16
O CHECK id=secureboot ok=yes value=disabled
O CHECK id=tools ok=yes
O CHECK id=power ok=yes value=76 ac=no min=15
X 0
EOF
# 电量 9%、没接电源 ⇒ 拦（v1.0 计划 GUI-8；界面测试用 overrides 选它）
cat > "$C/preflight-lowbatt.txt" <<'EOF'
O CHECK id=root ok=yes
O CHECK id=uefi ok=yes
O CHECK id=model ok=yes value=GK-W7X
O CHECK id=bios ok=yes value=2.16
O CHECK id=secureboot ok=yes value=disabled
O CHECK id=tools ok=yes
O CHECK id=power ok=no value=9 ac=no min=15
X 0
EOF
# 缺工具（INST-16：pkgs= 是 Debian 包名；在别的 arm64 live U 盘上跑命令行版 / 图形版时会遇到）
cat > "$C/preflight-tools.txt" <<'EOF'
O CHECK id=root ok=yes
O CHECK id=uefi ok=yes
O CHECK id=model ok=yes value=GK-W7X
O CHECK id=bios ok=yes value=2.16
O CHECK id=secureboot ok=yes value=disabled
O CHECK id=tools ok=no missing=sgdisk,partprobe pkgs=gdisk,parted
O CHECK id=power ok=yes value=76 ac=no min=15
X 0
EOF
# BIOS 2.17 照样 ok=yes（不再限制 BIOS 版本，2026-09-25）；拦住它的是安全启动
cat > "$C/preflight-secureboot.txt" <<'EOF'
O CHECK id=root ok=yes
O CHECK id=uefi ok=yes
O CHECK id=model ok=yes value=GK-W7X
O CHECK id=bios ok=yes value=2.17
O CHECK id=secureboot ok=no value=enabled
O CHECK id=tools ok=yes
O CHECK id=power ok=yes value=40 ac=yes min=15
X 0
EOF
printf '%-78s %s\n' "gk3_preflight" "preflight-ok.txt   # 手写（容器不是 gaokun3）" >> "$C/index.txt"
# 录：WiFi 扫描 —— 跑的是真 gk3-wpa-scan.py，输入是按 printf_encode 编码的合成 scan_results
python3 - > "$W/scan_results" <<'PYEOF'
def enc(b):
    m = {0x22: '\\"', 0x5C: '\\\\', 0x09: '\\t'}
    return "".join(m[x] if x in m else chr(x) if 32 <= x <= 126 else "\\x%02x" % x for x in b)
print("bssid / frequency / signal level / flags / ssid")
for i, (f, s, fl, n) in enumerate([
    (5180, -42, "[WPA2-PSK-CCMP][ESS]", "宿舍网-5G".encode()), (2412, -63, "[WPA2-PSK-CCMP][ESS]", "宿舍网".encode()),
    (5745, -55, "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]", b"TP-LINK_8A3F"), (2437, -70, "[ESS]", b"CMCC-WEB"),
    (5200, -58, "[WPA2-EAP-CCMP][ESS]", b"eduroam"), (2462, -77, "[WPA2-PSK-CCMP][ESS]", "隔壁 的 网".encode()),
    (5240, -84, "[WPA2-PSK-CCMP][ESS]", b"ChinaNet-xk9q"),
    # v1.0 计划 GUI-9：纯 WPA3（能连，要 key_mgmt SAE）、WEP 与 OWE（连不了，界面标灰）
    (5260, -60, "[WPA2-SAE-CCMP][ESS]", b"WPA3-Home"), (2422, -72, "[WEP][ESS]", b"OldRouter-WEP"),
    (5280, -74, "[WPA2-OWE-CCMP][ESS]", b"Cafe-OWE")]):
    print("aa:bb:cc:00:00:%02x\t%d\t%d\t%s\t%s" % (i, f, s, fl, enc(n)))
PYEOF
python3 scripts/live/record-fixture.py "$C/wifi_scan.txt" -- python3 scripts/live/gk3-wpa-scan.py < "$W/scan_results" 2>/dev/null \
  || python3 scripts/live/record-fixture.py "$C/wifi_scan.txt" -- bash -c "python3 scripts/live/gk3-wpa-scan.py < $W/scan_results"
printf '%-78s %s\n' "gk3_wifi_scan" "wifi_scan.txt   # 录（真 gk3-wpa-scan.py，合成的 scan_results）" >> "$C/index.txt"
# 手写：连 WiFi（容器里没有 wlan0）—— 进度与结尾的 NET 记录照 gk3_wifi_connect 的输出写
cat > "$C/wifi_connect-ok.txt" <<'EOF'
E PROGRESS 20 wifi-assoc
D 1200
E PROGRESS 60 wifi-dhcp
D 900
E PROGRESS 100 wifi-ok
O NET if=wlan0 ip=192.168.10.239 ssid=宿舍网-5G online=yes
X 0
EOF
cat > "$C/wifi_connect-fail.txt" <<'EOF'
E PROGRESS 20 wifi-assoc
D 1500
E ERR code=wifi-assoc
E !! 连不上 所选网络（密码错？信号弱？）
X 1
EOF
printf '%-78s %s\n' "gk3_wifi_connect *" "wifi_connect-ok.txt   # 手写（容器里没有 wlan0）" >> "$C/index.txt"
cat > "$C/net_status-off.txt" <<'EOF'
O NET if=wlan0 ip=none ssid=none online=no
X 0
EOF
printf '%-78s %s\n' "gk3_net_status" "net_status-off.txt   # 手写" >> "$C/index.txt"
# 半手写（S15 双系统，只给测试按文件名用 overrides）：从 windows-free 录到的那一行 ESP 记录改几个字段 —— 这些状态要一整块
# 带 BitLocker 卷 / 休眠的 Windows 盘才造得出来，后端的判据在 scripts/live/test-apply.sh 的 K 组里真测过；这里只给界面用
WESP=$(grep '^O ESP ' "$OUT/windows-free/esp_info.txt")
esp_var() { printf '%s\nX 0\n' "$(printf '%s' "$WESP" | sed "$@")"; }
esp_var -e 's/ bootaa64=[a-z]*/ bootaa64=other/' -e 's/ bitlocker=no/ bitlocker=yes/' -e 's/ hibernated=[a-z]*/ hibernated=unknown/' > "$C/esp_info-bitlocker.txt"
esp_var -e 's/ hibernated=[a-z]*/ hibernated=yes/' > "$C/esp_info-hibernated.txt"
esp_var -e 's/ size_mib=[0-9]*/ size_mib=100/' -e 's/ free_mib=[0-9]*/ free_mib=70/' -e 's/ small=no/ small=yes/' > "$C/esp_info-small.txt"
{ grep -v '^X ' "$OUT/windows-free/apply-along.txt"; echo 'O NOTE code=loadervar-stuck name=LoaderEntryDefault value=auto-windows'; echo 'X 0'; } > "$C/apply-note.txt"
{ echo "#  esp_info-bitlocker.txt：Windows 卷是 BitLocker、BOOTAA64 还是 Windows 的（确认页要勾恢复密钥，U16）"
  echo "#  esp_info-hibernated.txt：Windows 在休眠（双系统 / 重新安装禁用，U18）"
  echo "#  esp_info-small.txt：100 MiB 的 ESP（双系统明确不装，U17）"
  echo "#  apply-note.txt：windows-free 录的那次 apply + 一条 NOTE code=loadervar-stuck（EFI 变量删不掉，完成页要说）"
  echo "#  ↑ 这四份是从录到的输出改字段得来的（半手写，gen-fixtures.sh 里写着怎么改），后端判据见 test-apply.sh 的 K 组"; } >> "$C/index.txt"
# GUI-11：写盘任务。没有在跑的（默认）；"界面崩过又起来、apply 还在跑"那种给测试按文件名用。
# 手写：容器里起不了 systemd 单元；格式照 gk3_job_status / gk3_job_start 的输出写。跟读的内容就是 blank 场景录的那次 apply
printf 'X 0\n' > "$C/job_status-none.txt"
printf '%-78s %s\n' "gk3_job_status" "job_status-none.txt   # 手写（没有在跑的写盘任务）" >> "$C/index.txt"
cat > "$C/job_status-running.txt" <<'EOF'
O JOB id=20261005-101500-4242-31337 fn=gk3_apply state=running rc=- mode=systemd-run unit=gk3-job-20261005-101500-4242-31337
X 0
EOF
cp "$OUT/blank/apply-wipe.txt" "$C/job_follow-apply.txt"
echo "#  job_status-running.txt / job_follow-apply.txt：界面重新起来时 apply 还在跑（测试用 overrides 选它们）" >> "$C/index.txt"
# 录：GUI-12 的日志另存 —— 另插一个 FAT 的 U 盘（Basic data 类型，像普通 U 盘那样）+ 安装介质本身（ESP 类型，esp=yes）。
# 只留这两块的行：容器里别的场景留下的 loop 盘（GK3_ALLOW_LOOP=1 下也算"可移动"）不是真机上会有的东西
U2=$(new_disk usb2 1G); sgdisk -o -n 1:2048:0 -t 1:0700 -c 1:"Basic data partition" "$U2" >/dev/null 2>&1; settle "$U2"
mkfs.vfat -F 32 -n KINGSTON "${U2}p1" >/dev/null
python3 scripts/live/record-fixture.py "$C/log_targets.txt" --sub "${STICK}p=/dev/sda" --sub "${U2}p=/dev/sdb" \
    -- bash -c ". scripts/live/installer-lib.sh && gk3_log_targets | grep -e 'part=${STICK}p' -e 'part=${U2}p'"
drop_disk "$U2"
printf '%-78s %s\n' "gk3_log_targets" "log_targets.txt   # 录（假 U 盘挂在 /media/gk3）" >> "$C/index.txt"
python3 scripts/live/record-fixture.py "$C/save_logs.txt" --sub "${STICK}p=/dev/sda" \
    -- bash -c ". scripts/live/installer-lib.sh && gk3_save_logs ${STICK}p1"
printf '%-78s %s\n' "gk3_save_logs *" "save_logs.txt   # 录（存到假 U 盘上）" >> "$C/index.txt"
# 录：发布目录 / 安装 U 盘里带了什么
python3 scripts/live/record-fixture.py "$C/release_info.txt" --sub "$REL=/media/gk3/gaokun3/payload" \
    -- bash -c ". scripts/live/installer-lib.sh && gk3_release_info $REL"
printf '%-78s %s\n' "gk3_release_info" "release_info.txt   # 录" >> "$C/index.txt"
python3 scripts/live/record-fixture.py "$C/release_info-none.txt" -- bash -c ". scripts/live/installer-lib.sh && gk3_release_info /media/gk3/gaokun3/payload"
echo "#  release_info-none.txt：U 盘里没有镜像（界面测试用 overrides 选它）" >> "$C/index.txt"
# 录：网络安装的下载（本地起一个支持 Range 的服务器，内容就是上面那份发布目录）
cat > "$W/srv.py" <<'SRVEOF'
import http.server, os, sys
root = sys.argv[1]
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(s, *a, **k): super().__init__(*a, directory=root, **k)
    def log_message(s, *a): pass
http.server.ThreadingHTTPServer(("127.0.0.1", 18090), H).serve_forever()
SRVEOF
python3 "$W/srv.py" "$W" & SRVPID=$!; sleep 1
python3 scripts/live/record-fixture.py "$C/net_release.txt" --sub "http://127.0.0.1:18090/rel=https://ota.072172.xyz/install/crDroidAndroid-16.0-20260916-gaokun3-v12.11" --sub "$W/dl=/run/gaokun3/payload" \
    -- bash -c ". scripts/live/installer-lib.sh && gk3_net_release http://127.0.0.1:18090/rel/ $W/dl"
kill $SRVPID 2>/dev/null
printf '%-78s %s\n' "gk3_net_release *" "net_release.txt   # 录（本地服务器，内容是上面那份发布目录）" >> "$C/index.txt"
# 手写：变体清单（ota.072172.xyz/installer/variants.txt 还不存在 —— 要等发版流程生成它）
# 格式见 installer-lib.sh 的 gk3_net_manifest：base= 指向 R2 上现成的 install/<VER>/ 目录
cat > "$C/net_manifest.txt" <<'EOF'
O VARIANT id=stock name=标准版 desc=不带%20Google%20服务，不带%20root。最接近%20AOSP。 name_en=Standard desc_en=No%20Google%20services,%20no%20root.%20Closest%20to%20AOSP. base=https://ota.072172.xyz/install/crDroidAndroid-16.0-20260916-gaokun3-v12.11/ size_mib=1245
O VARIANT id=gapps name=带%20Google%20服务 desc=预装%20GApps。首次开机要登录%20Google%20账号并按%20docs/INSTALL.md%20做一次认证。 name_en=With%20Google%20services desc_en=GApps%20preinstalled.%20Sign%20in%20to%20Google%20on%20first%20boot%20and%20certify%20the%20device%20as%20described%20in%20docs/INSTALL.md. base=https://ota.072172.xyz/install/example-gapps/ size_mib=1611
O VARIANT id=ksu name=带%20root（KernelSU） desc=内核内置%20KernelSU。适合要调试或改系统的人。 name_en=With%20root%20(KernelSU) desc_en=KernelSU%20built%20into%20the%20kernel.%20For%20debugging%20or%20changing%20the%20system. base=https://ota.072172.xyz/install/example-ksu/ size_mib=1252
X 0
EOF
printf '%-78s %s\n' "gk3_net_manifest" "net_manifest.txt   # 手写（清单 URL 还不存在）" >> "$C/index.txt"
# 手写：变体清单 404 时退回 OTA 清单推出来的那一项 —— 原样取自 2026-09-25 对真实 ota.072172.xyz 跑 gk3_net_manifest 的输出
cat > "$C/net_manifest-latest.txt" <<'EOF'
E 变体清单取不到（https://ota.072172.xyz/installer/variants.txt）—— 退回 OTA 清单 https://ota.072172.xyz/ota/gaokun3.json
O VARIANT id=latest name=crDroid%2012.11 desc= base=https://ota.072172.xyz/install/crDroidAndroid-16.0-20260916-gaokun3-v12.11/ size_mib=1247 latest=yes
X 0
EOF

# 录：手动调整磁盘的四个操作（用户 2026-09-25）—— 在另一块出厂布局的盘上真做一遍。放 common/（任何场景都能用），
# 界面按 '<函数> *' 取；做完之后界面会重新探测，拿到的仍是各场景自己那份 probe（测试只核对发出去的调用）
# 录：C: 与 D: 加了密（BitLocker / Windows 的设备加密）的出厂盘 —— 安装器缩不了，要报 why=bitlocker（2026-09-27）。
# 容器里造不出真 BitLocker 卷，按 util-linux 2.41 libblkid 的判据造卷头（与 test-shrink.sh 第 7 节同一份），
# 让真 blkid 认、真后端报。只给流程测试按文件名用（overrides），不进 index 的调用映射 —— 否则别的场景会落到它身上
DK=$(new_disk bitlocker 488386M); make_factory "$DK"
for n in 3 4; do python3 - "${DK}p$n" <<'PYEOF'
import sys, struct
META = 0x10000
with open(sys.argv[1], 'r+b') as f:
    b = bytearray(512); b[0:11] = b'\xeb\x58\x90-FVE-FS-'
    struct.pack_into('<H', b, 11, 512); b[13] = 8; struct.pack_into('<Q', b, 176, META); b[510:512] = b'\x55\xaa'
    f.write(b)
    m = bytearray(64 + 48); m[0:8] = b'-FVE-FS-'; struct.pack_into('<H', m, 10, 2); struct.pack_into('<IIII', m, 64, 48, 1, 48, 48)
    f.seek(META); f.write(m)
PYEOF
done
python3 scripts/live/record-fixture.py "$C/shrink_scan-bitlocker.txt" --sub "${DK}p=/dev/nvme0n1p" --sub "$DK=/dev/nvme0n1" \
    -- bash -c ". scripts/live/installer-lib.sh && gk3_shrink_scan $DK"
echo "# shrink_scan-bitlocker.txt   gk3_shrink_scan：出厂盘、C: 与 D: 是 BitLocker 卷头（录；只给测试按文件名用）" >> "$C/index.txt"
drop_disk "$DK"

DE=$(new_disk edit 488386M); make_factory "$DE"
recc() {   # 和 rec 一样，但录进 common/；$1=文件名 $2=界面发出的调用（nvme0n1）$3=实际执行的命令
    python3 scripts/live/record-fixture.py "$C/$1" --sub "${DE}p=/dev/nvme0n1p" --sub "$DE=/dev/nvme0n1" --sub "-$(basename "$DE")-=-nvme0n1-" --sub "$W=/tmp" \
        -- bash -c ". scripts/live/installer-lib.sh && $3"
    printf '%-78s %s\n' "$2" "$1   # 录（出厂布局的另一块盘上真做）" >> "$C/index.txt"
}
recc part_delete.txt   "gk3_part_delete *"  "gk3_part_delete ${DE}p6"
DMIB=$(( $(blockdev --getsize64 "${DE}p4") / 1048576 ))
recc part_resize.txt   "gk3_part_resize *"  "gk3_part_resize ${DE}p4 $(( DMIB - 10240 ))"
EF=$(gk3__probe_parts "$DE" "$(blockdev --getsz "$DE")" | grep '^FREE ' | sort -t= -k5 -n | tail -1)
recc part_create.txt   "gk3_part_create *"  "gk3_part_create --disk $DE --start $(gk3__f "$EF" start) --size-mib 4096 --fs ext4"
NEWP=$(sed -n 's/^O RESULT op=create part=\([^ ]*\).*/\1/p' "$C/part_create.txt" | sed "s#/dev/nvme0n1p#${DE}p#")
recc part_format.txt   "gk3_part_format *"  "gk3_part_format $NEWP vfat"
# 反例：删 ESP（界面不给按钮，但后端要拒绝 —— 测试用 overrides 换进来）
recc part_delete-esp.txt "gk3_part_delete-esp" "gk3_part_delete ${DE}p1"

echo; echo "fixture 写到 $OUT/："
find "$OUT" -name '*.txt' | sort | sed 's/^/  /'
