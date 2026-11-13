#!/usr/bin/env bash
# gk3l 的只读命令（probe / plan / preflight）在 loop 盘上的测试：probe 的每个字段由 check_probe.py 对照 sgdisk、
# blkid 与 sysfs 检查，方案按 plan.h 的规矩检查；另测 table= 的细分、4Kn 盘、内核分区视图过期、读盘出错、介质、预检。
#
#   bash installer/tools/test/test-in-container.sh installer/backend/test/linux/test-probe.sh
#
# gk3l 在容器里现编（ASan + UBSan）；另编一份静态的发布形态，跑一遍 probe 确认静态链接没问题。
# ⚠️ 只动容器所在虚拟机里的 loop 设备（背后是 /tmp 下的稀疏文件）。
set -u
cd "$(dirname "$0")/../../../.."
[ "$(id -u)" = 0 ] || { echo "要 root（用 installer/tools/test/test-in-container.sh 跑）"; exit 2; }
export GK3_ALLOW_LOOP=1
CHK="python3 -I installer/backend/test/linux/check_probe.py"
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ✓ $*"; }
bad() { FAIL=$((FAIL+1)); echo "  ✗ $*"; }

W=$(mktemp -d /tmp/gk3l-probe.XXXX)
cleanup() {
    local img l
    findmnt -rn /media/gk3 >/dev/null 2>&1 && umount /media/gk3
    findmnt -rn "$W/mnt" >/dev/null 2>&1 && umount "$W/mnt"
    for img in "$W"/*.img; do
        for l in $(losetup -j "$img" 2>/dev/null | cut -d: -f1); do losetup -d "$l" 2>/dev/null; done
    done
    [ -n "${EL:-}" ] && losetup -d "$EL" 2>/dev/null
    dmsetup remove --noudevsync gk3l-eio 2>/dev/null
    rm -rf "$W"
}
trap cleanup EXIT

echo "═══ 编 gk3l ═══"
GK3L_CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    sh installer/backend/linux/build.sh "$W/gk3l" gcc || { echo "编不过"; exit 1; }
sh installer/backend/linux/build.sh "$W/gk3l-static" gcc || { echo "静态版编不过"; exit 1; }
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0
G=$W/gk3l
ok "ASan 版与静态版都编出来了"

new_disk() { truncate -s "$2" "$W/$1.img"; losetup -fP --show ${3:+--sector-size "$3"} "$W/$1.img"; }
settle() { partprobe "$1" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1; }

# 探测一次，检查指定的盘。参数：标签 盘=期望的table…
check() {
    local label=$1; shift
    "$G" probe >"$W/lx.out" 2>"$W/lx.err"; echo $? >"$W/lx.rc"
    if grep -q 'ERROR: \(Address\|Undefined\)Sanitizer\|runtime error' "$W/lx.err"; then
        bad "$label：sanitizer 报错"; sed -n 1,20p "$W/lx.err"; return
    fi
    if $CHK probe "$W/lx.out" "$@" >"$W/chk.txt"; then ok "$label"
    else bad "$label"; cat "$W/chk.txt"; fi
}

# 每段空闲区，外加一段太小的：gk3l plan 按方案的规矩检查（不给 /data 大小、刚好下限、低于下限、超过能给的）
check_plans() {
    local label=$1 disk=$2 sector=$3 line rs re n=0 ud
    while read -r line; do
        rs=$(printf '%s\n' "$line" | tr ' ' '\n' | sed -n 's/^start=//p')
        re=$(printf '%s\n' "$line" | tr ' ' '\n' | sed -n 's/^end=//p')
        for r in "$rs $re" "$rs $((rs + 20000 * 2048))"; do
            set -- $r
            for ud in "" 8192 8191 99999999; do
                "$G" plan --sector "$sector" --region-start "$1" --region-end "$2" --disk "$disk" ${ud:+--userdata-mib $ud} >"$W/pl.out" 2>/dev/null
                if $CHK plan "$W/pl.out" "$1" "$2" "$sector" "$ud" >"$W/chk.txt"; then n=$((n+1))
                else bad "$label：方案 $1-$2 ud=${ud:-全部}"; cat "$W/chk.txt"; fi
            done
        done
    done < <(grep "^FREE disk=$disk " "$W/lx.out")
    [ "$n" -gt 0 ] && ok "$label：$n 个方案都合乎规矩" || bad "$label：没有可查的空闲区"
}

field() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }
disk_line() { grep "^DISK path=$1 " "$W/lx.out"; }

echo "═══ 1. 没有分区表 / 坏的 GPT / MBR / 混合 MBR / 只剩保护性 MBR ═══"
D1=$(new_disk raw 3G)
D2=$(new_disk badgpt 3G)
sgdisk -o "$D2" >/dev/null 2>&1; head -c 4096 /dev/urandom | dd of="$D2" bs=512 seek=1 conv=notrunc status=none
# 表头还在、CRC 不对（备份表完好：内核与 sgdisk 会退而用它，我们不）
D2C=$(new_disk badcrc 3G)
sgdisk -o -n 1:2048:+100M "$D2C" >/dev/null 2>&1; printf '\377\377' | dd of="$D2C" bs=1 seek=$((512 + 16)) conv=notrunc status=none
D3=$(new_disk mbr 30G)
printf 'label: dos\n,100M,c\n,4G,7\n,200M,83\n' | sfdisk -q "$D3"; settle "$D3"
mkfs.vfat "${D3}p1" >/dev/null 2>&1; mkntfs -Q -F "${D3}p2" >/dev/null 2>&1
D3Z=$(new_disk mbrempty 3G); printf 'label: dos\n' | sfdisk -q "$D3Z"
D3H=$(new_disk hybrid 3G); sgdisk -o -n 1:2048:+100M -t 1:0700 -n 2:0:+100M "$D3H" >/dev/null; sgdisk -h 1 "$D3H" >/dev/null 2>&1; settle "$D3H"
D3P=$(new_disk pmbr 3G); sgdisk -o "$D3P" >/dev/null 2>&1
dd if=/dev/zero of="$D3P" bs=512 seek=1 count=33 conv=notrunc status=none
dd if=/dev/zero of="$D3P" bs=512 seek=$(( 3 * 1024 * 2048 - 33 )) count=33 conv=notrunc status=none
# 整盘格式化的 U 盘（没有分区表，LBA 0 就是 FAT 的引导扇区）
D3F=$(new_disk superfloppy 2G); mkfs.vfat -F 32 -I "$D3F" >/dev/null 2>&1
settle "$D1"
check "各种分区表" "$D1=raw" "$D2=damaged" "$D2C=damaged" "$D3=mbr" "$D3Z=mbr" "$D3H=gpt" "$D3P=damaged" "$D3F=raw"
[ "$(field "$(disk_line "$D2")" why)" = magic ] && [ "$(field "$(disk_line "$D2C")" why)" = crc ] \
    && [ "$(field "$(disk_line "$D3P")" why)" = magic ] && ok "坏的 GPT：why=magic / crc" \
    || bad "坏的 GPT 的 why：$(disk_line "$D2") / $(disk_line "$D2C") / $(disk_line "$D3P")"

echo "═══ 2. 怪名字、怪卷标、乱序、不对齐、小缝 ═══"
D4=$(new_disk names 30G)
sgdisk -o \
  -n 3:2048:+100M  -t 3:ef00 -c 3:"EFI system partition" \
  -n 1:0:+300M     -t 1:0700 -c 1:"50% off name" \
  -n 2:0:+64M      -t 2:8300 -c 2:"it's x" \
  -n 5:0:+64M      -t 5:8300 -c 5:"名字 中文" \
  -n 6:0:+8M       -t 6:8300 -c 6:"" \
  "$D4" >/dev/null 2>&1
S=$(( $(sgdisk -i 6 "$D4" | awk '/^Last sector:/{print $3}') + 1 + 20480 + 7 ))
sgdisk -a 1 -n 7:$S:+50M -t 7:8300 -c 7:"misc metadata" "$D4" >/dev/null 2>&1
S=$(( $(sgdisk -i 7 "$D4" | awk '/^Last sector:/{print $3}') + 1 + 40960 ))
sgdisk -n 8:$S:+12G -t 8:8300 -c 8:"super x" "$D4" >/dev/null 2>&1
settle "$D4"
mkfs.vfat -F 32 "${D4}p3" >/dev/null 2>&1; mkfs.ext4 -q -F -L "lab el%" "${D4}p1"
mkfs.ext4 -q -F -L "$(printf '\xcf\xb5\xcd\xb3 \xc5\xcc')" "${D4}p5"
check "怪名字" "$D4=gpt"

echo "═══ 3. 一套装好的 Android（真机那种 34 扇区起的 misc）、带 Windows 的盘 ═══"
D5=$(new_disk android 30G)
sgdisk -o -a 1 -n 4:34:2047 -c 4:misc -t 4:8300 "$D5" >/dev/null 2>&1
sgdisk -a 2048 -n 1:2048:+300M -t 1:ef00 -c 1:esp -n 2:0:+64M -c 2:boot_a -n 3:0:+64M -c 3:boot_b \
       -n 5:0:+32M -c 5:metadata -n 6:0:+12G -c 6:super -n 7:0:+9G -c 7:userdata -n 8:0:+500M -c 8:gk3rescue "$D5" >/dev/null 2>&1
settle "$D5"; mkfs.vfat -F 32 -n ESP "${D5}p1" >/dev/null; mkfs.ext4 -q -F -L userdata "${D5}p7"
D6=$(new_disk windows 64G)
sgdisk -o -n 1:2048:+300M -t 1:ef00 -c 1:"EFI system partition" -n 2:0:+16M -t 2:0c01 -c 2:"Microsoft reserved partition" \
       -n 3:0:+30G -t 3:0700 -c 3:"Basic data partition" -n 4:0:+1G -t 4:2700 -c 4:"Basic data partition" "$D6" >/dev/null 2>&1
settle "$D6"; mkfs.vfat -F 32 -n SYSTEM "${D6}p1" >/dev/null; mkntfs -Q -F -L Windows "${D6}p3" >/dev/null 2>&1
mkntfs -Q -F -L "Windows RE tools" "${D6}p4" >/dev/null 2>&1
check "Android 与 Windows" "$D5=gpt" "$D6=gpt"
check_plans "Windows 盘" "$D6" 512
L=$(disk_line "$D6")
[ "$(field "$L" kernel)" = ok ] && [ "$(field "$L" bus)" = loop ] && [ "$(field "$L" sector)" = 512 ] \
    && ok "DISK kernel=ok bus=loop sector=512" || bad "DISK 字段：$L"
P=$(grep "^PART path=${D6}p4 " "$W/lx.out")
[ "$(field "$P" os)" = winre ] && [ "$(field "$P" type)" = DE94BBA4-06D1-4D40-A16A-BFD50179D6AC ] \
    && [ -n "$(field "$P" uuid)" ] && [ "$(field "$P" attrs)" = 0x0000000000000000 ] \
    && ok "PART uuid / attrs / os=winre" || bad "PART 字段：$P"

echo "═══ 4. 4Kn 盘 ═══"
D7=$(new_disk k4 64G 4096)
sgdisk -o -n 1:256:+300M -t 1:ef00 -c 1:esp -n 2:0:+16G -t 2:0700 -c 2:win "$D7" >/dev/null 2>&1; settle "$D7"
mkfs.vfat -F 32 -S 4096 "${D7}p1" >/dev/null 2>&1
check "4Kn 盘" "$D7=gpt"
L=$(disk_line "$D7"); P=$(grep "^PART path=${D7}p2 " "$W/lx.out"); F=$(grep "^FREE disk=$D7 " "$W/lx.out")
LAST=$(sgdisk -p "$D7" | sed -n 's/.*last usable sector is \([0-9]*\).*/\1/p')
P2END=$(sgdisk -i 2 "$D7" | awk '/^Last sector:/{print $3}')
[ "$(field "$L" sector)" = 4096 ] && [ "$(field "$P" size_mib)" = 16384 ] \
    && [ "$(field "$F" start)" = $((P2END + 1)) ] && [ "$(field "$F" end)" = "$LAST" ] \
    && [ "$(field "$F" size_mib)" = $(( (LAST - P2END) * 4096 / 1048576 )) ] \
    && ok "4Kn：sector=4096，分区与空闲区按 4096 字节算" || bad "4Kn：$L / $P / $F"
"$G" plan --sector 4096 --region-start "$(field "$F" start)" --region-end "$(field "$F" end)" --disk "$D7" >"$W/pl.out" 2>&1
SUP=$(grep 'name=super ' "$W/pl.out")
[ $(( $(field "$SUP" end) - $(field "$SUP" start) + 1 )) = $(( 12288 * 256 )) ] && [ $(( $(field "$SUP" start) % 256 )) = 0 ] \
    && ok "4Kn 方案：super 12 GiB、1 MiB 对齐" || bad "4Kn 方案：$(cat "$W/pl.out")"

echo "═══ 5. 内核的分区视图过期（绕过 loop 设备直接改后面的镜像文件，内核不知道）═══"
sgdisk -n 5:0:+1G -t 5:8300 -c 5:extra "$W/windows.img" >/dev/null 2>&1; blockdev --flushbufs "$D6"
"$G" probe >"$W/lx.out" 2>"$W/lx.err"
L=$(disk_line "$D6"); P=$(grep "^PART path=${D6}p5 " "$W/lx.out")
[ "$(field "$L" kernel)" = differs ] && [ -n "$P" ] && [ "$(field "$P" fs)" = "" ] \
    && ok "kernel=differs，内核还不知道的分区照样列出（路径按命名规则拼，不探测文件系统）" || bad "过期视图：$L / $P"
blockdev --rereadpt "$D6"; settle "$D6"
"$G" probe >"$W/lx.out" 2>"$W/lx.err"
[ "$(field "$(disk_line "$D6")" kernel)" = ok ] && ok "重读之后 kernel=ok" || bad "重读之后：$(disk_line "$D6")"

echo "═══ 6. 安装介质在这块盘上（/media/gk3 挂着它的一个分区）═══"
mkdir -p /media/gk3 && mount "${D4}p3" /media/gk3
check "介质在怪名字盘上" "$D4=gpt" "$D6=gpt"
[ "$(field "$(disk_line "$D4")" medium)" = yes ] && [ "$(field "$(grep "^PART path=${D4}p3 " "$W/lx.out")" medium)" = yes ] \
    && [ "$(field "$(disk_line "$D6")" medium)" = no ] && ok "介质盘与介质分区标出来了" || bad "介质：$(disk_line "$D4")"
umount /media/gk3

echo "═══ 7. 读盘出错（loop 叠在 dm-error 上：GPT 那几个扇区读不出）═══"
if command -v dmsetup >/dev/null; then
    E=$(new_disk eiobase 2G); sgdisk -o -n 1:2048:+100M "$E" >/dev/null 2>&1
    N=$(( 2 * 1024 * 2048 ))
    printf '0 1 linear %s 0\n1 33 error\n34 %s linear %s 34\n' "$E" $((N - 34)) "$E" | dmsetup create --noudevsync gk3l-eio
    # 容器里没有 udev，/dev/mapper 下不会有节点；用 devtmpfs 自己建的 /dev/dm-N
    EL=$(losetup -f --show "/dev/$(dmsetup info -c --noheadings -o blkdevname gk3l-eio | tr -d ' ')")
    "$G" probe >"$W/lx.out" 2>"$W/lx.err"; rc=$?
    L=$(disk_line "$EL")
    [ "$(field "$L" table)" = unreadable ] && grep -q "^ERR code=disk-unreadable disk=$EL" "$W/lx.err" && [ "$rc" = 1 ] \
        && ! grep -q "disk=$EL " <(grep -E '^(PART|FREE) ' "$W/lx.out") \
        && [ "$(field "$(disk_line "$D6")" table)" = gpt ] \
        && ok "读不出：table=unreadable + ERR disk-unreadable、不列分区，别的盘照常，退出码 1" || { bad "读不出：rc=$rc $L"; cat "$W/lx.err"; }
    losetup -d "$EL"; EL=""
    for i in 1 2 3 4 5 6 7 8 9 10; do dmsetup remove --noudevsync gk3l-eio 2>/dev/null && break; sleep 0.5; done
else
    bad "容器里没有 dmsetup（test-env.Dockerfile）"
fi

echo "═══ 8. 预检 ═══"
PS=$W/ps; mkdir -p "$PS/gaokun-ec-battery" "$PS/gaokun-ec-adapter"
echo Battery > "$PS/gaokun-ec-battery/type"; echo 9 > "$PS/gaokun-ec-battery/capacity"; echo Discharging > "$PS/gaokun-ec-battery/status"
echo USB > "$PS/gaokun-ec-adapter/type"; echo 0 > "$PS/gaokun-ec-adapter/online"
R=$(GK3_POWER_SUPPLY_DIR=$PS "$G" preflight)
[ "$(field "$R" ac)" = no ] && [ "$(field "$R" battery)" = 9 ] && [ "$(field "$R" battery_min)" = 15 ] \
    && ok "电量 9%、没接电源" || bad "预检：$R"
echo 1 > "$PS/gaokun-ec-adapter/online"
R=$(GK3_POWER_SUPPLY_DIR=$PS "$G" preflight); [ "$(field "$R" ac)" = yes ] && ok "接着适配器 ⇒ ac=yes" || bad "预检：$R"
echo 0 > "$PS/gaokun-ec-adapter/online"; echo Charging > "$PS/gaokun-ec-battery/status"
R=$(GK3_POWER_SUPPLY_DIR=$PS "$G" preflight); [ "$(field "$R" ac)" = yes ] && ok "电池在充电 ⇒ ac=yes" || bad "预检：$R"
R=$(GK3_POWER_SUPPLY_DIR=$W/none "$G" preflight)
[ "$(field "$R" ac)" = no ] && [ -z "$(field "$R" battery)" ] && ok "没有电源信息 ⇒ 不报 battery" || bad "预检：$R"
R=$(GK3_SKIP_MODEL_CHECK=1 "$G" preflight)
[ "$(field "$R" model_ok)" = unknown ] && [ "$(field "$R" skipped)" = yes ] && ok "GK3_SKIP_MODEL_CHECK=1" || bad "预检：$R"
R=$("$G" preflight); case "$(field "$R" model_ok)" in yes|no|unknown) ok "本机预检：$R" ;; *) bad "预检：$R" ;; esac

echo "═══ 9. 静态版 ═══"
"$W/gk3l-static" probe >"$W/st.out" 2>"$W/st.err"; "$G" probe >"$W/lx.out" 2>/dev/null
cmp -s "$W/st.out" "$W/lx.out" && ok "静态版的 probe 输出与 ASan 版逐字节相同" || { bad "静态版输出不同"; diff "$W/st.out" "$W/lx.out" | head; }

echo
echo "合计：通过 $PASS，失败 $FAIL"
[ "$FAIL" = 0 ]
