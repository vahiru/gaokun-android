#!/usr/bin/env bash
# 安装器写盘路径的端到端测试：在 loop 设备上真装，逐项核对。
#
#   bash scripts/live/test-in-container.sh scripts/live/test-apply.sh
#   GK3_TEST_BOOTIMG=/repo/out/v0.6.2/boot.img bash scripts/live/test-in-container.sh scripts/live/test-apply.sh
#   GK3_TEST_FIXTURES=/repo/live/installer-flutter/testdata  …（顺带把探测输出存成界面的 fixture）
#
# 必须 root + loop 设备 + 能看见新出现的分区节点 —— 容器入口替你安排好了。
# ⚠️ 只动 colima 虚拟机里的 loop 设备（背后是容器里 /tmp 下的稀疏文件）。
#
# 覆盖：
#   A. 命令行版 install-gaokun3.sh：不输 ERASE 时一个字节都不写；输了就整盘装完
#   B. 双系统：盘上已有 ESP + MSR + NTFS「Basic data partition」+ 末尾 WinRE，
#      装进中间的空闲区 —— 原有分区的内容与 PARTUUID 一个都不许变、ESP 不许重新格式化
#   C. 反例，都必须【在动盘之前】拒绝：截断的 .zst、sha256 不符、在已装过的盘上再装一次；
#      以及 dry-run 一个字节都不写
#   D. 探测输出里 PARTLABEL 的空格按协议编码（"Basic data partition" 不能变成 "Basic"）
#   K. 双系统专项（见 tools/gk3boot/README.md）：Windows 休眠拒绝写 ESP、BitLocker + 换 BOOTAA64
#      要确认恢复密钥、默认系统写进 GK3 的 set_default、timeout 5、32 MiB 余量、
#      重新安装删掉统一启动入口的条目、BOOTAA64 字节相同就不重写、LoaderEntryDefault 被删掉
#   M. MBR 盘与读不出分区表的盘（见 docs/dev/installer-rust-design.md）：MBR 盘上方案报 mbr-disk、apply / 缩分区 / 手动调整在动盘前
#      ERR mbr-disk touched=no、整盘清空照常；读不出的盘（dm-error 真设备 + 照它实录输出回答的假 sgdisk）探测不报空闲、
#      写盘入口 ERR disk-unreadable
#   每一次装完都核对 misc 的前 64 KiB 与 Python 独立算的初始状态逐字节相同（gk3-misc init）
#   GK3_TEST_DUEL=<Rust 版路径>：在下面标了 duel_* 的场景点上顺带与 Rust 版（tools/gk3-installer）对拍只读入口
#      （scripts/live/duel-lib.sh，见 docs/dev/installer-rust-design.md）；不设时 duel_* 什么都不做
#
# ⚠️ 故意不开 pipefail：判据都显式取退出码（scripts/verify-root.sh:8-11）。
set -u
cd "$(dirname "$0")/../.."
REPO=$(pwd)
[ "$(id -u)" = 0 ] || { echo "要 root（用 scripts/live/test-in-container.sh 跑）"; exit 2; }
. scripts/live/installer-lib.sh
export GK3_ALLOW_LOOP=1
. scripts/live/duel-lib.sh

PASS=0; FAIL=0
ok()  { echo "  ✓ $*"; PASS=$((PASS+1)); }
bad() { echo "  ✗ $*"; FAIL=$((FAIL+1)); }
check() { local what=$1; shift; if "$@" >/dev/null 2>&1; then ok "$what"; else bad "$what"; fi; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
sha_head() { head -c "$2" "$1" | sha256sum | cut -d' ' -f1; }
# 装完之后 misc 应有的前 64 KiB（gk3-misc init --slot a --default <$1>，期望值独立算；同 tools/gk3boot/test/misc/run.sh）
misc_want() {   # $1=none|windows|android $2=输出文件
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
# efw <目录> <名字> <值>：在假 efivarfs 里放一个 systemd-boot 变量（属性 0x07 + UTF-16LE + 双字节 NUL，同 gk3-boot-android）
efw() {
    local i v=$3
    { printf '\007\000\000\000'; for ((i = 0; i < ${#v}; i++)); do printf '%s\000' "${v:i:1}"; done; printf '\000\000'; } > "$1/$2-$GK3_LOADER_GUID"
}
# misc_ok <misc 分区> <none|windows|android>：前 64 KiB = 初始状态，之后全零
misc_ok() {
    misc_want "$2" "$W/misc.want"
    cmp -s <(head -c 65536 "$1") "$W/misc.want" && [ "$(tail -c +65537 "$1" | tr -d '\0' | wc -c)" = 0 ]
}
fp() { sgdisk -p "$1" 2>/dev/null | grep -v '^Disk identifier'; head -c 1048576 "$1" | sha256sum; }

W=$(mktemp -d /tmp/gk3-test.XXXX)
LOOPS=()
cleanup() {
    local l m
    for m in $(findmnt -rno TARGET | grep -e "^$W" -e '^/media/gk3' | sort -r); do umount "$m" 2>/dev/null; done
    # ⚠️★ 按背后的镜像文件找 loop，【不】靠 LOOPS 数组：new_disk 是在 $(…) 里调的，
    #   它往 LOOPS 里加的东西留在子 shell 里 —— 2026-09-24 就这样漏了 58 个 loop，
    #   每个攥着一个已删除的稀疏文件，把 colima 的 98G 盘写满。
    for img in "$W"/*.img; do
        for l in $(losetup -j "$img" 2>/dev/null | cut -d: -f1); do losetup -d "$l" 2>/dev/null; done
    done
    # M 组的 dm-error 设备（中途失败时它还在 colima 虚拟机里）
    command -v dmsetup >/dev/null && dmsetup remove "gk3-test-eio-$$" 2>/dev/null
    rm -rf "$W"
}
trap cleanup EXIT
new_disk() {   # $1=名字 $2=大小 → 打印 loop 设备
    truncate -s "$2" "$W/$1.img"
    local l; l=$(losetup -fP --show "$W/$1.img") || { echo "losetup 失败" >&2; exit 2; }
    LOOPS+=("$l"); echo "$l"
}
MID=0123456789abcdef0123456789abcdef
export GK3_MACHINE_ID=$MID

# ── 造一份发布目录 ──────────────────────────────────────────────────────────
echo "═══ 0. 造发布目录 ═══"
REL=$W/rel; mkdir -p "$REL" "$W/expect"
python3 - "$REL" "$W/expect" "${GK3_TEST_BOOTIMG:-}" <<'PYEOF'
import os, shutil, struct, sys
rel, exp, real = sys.argv[1], sys.argv[2], sys.argv[3]
P = 4096
al = lambda x: (x + P - 1) // P * P
if real:
    shutil.copy(real, os.path.join(rel, "boot.img"))
else:
    # header v2，cmdline 故意超过 511 字节：mkbootimg 把前 511 放 cmdline[512]、
    # 其余放 extra_cmdline[1024] —— 拼回来必须是原样（将来 cmdline 变长时会走到这条路）
    kern = b"MZ" + os.urandom(300000)
    ramd = os.urandom(123457)
    dtb = b"\xd0\x0d\xfe\xed" + os.urandom(20001)
    cmd = ("androidboot.hardware=gaokun3 androidboot.boot_devices=soc@0/1c20000.pcie "
           "init=/init firmware_class.path=/vendor/firmware/ console=tty0 "
           "clk_ignore_unused pd_ignore_unused arm64.nopauth efi=noruntime fbcon=rotate:1 "
           "usbhid.quirks=0x12d1:0x10b8:0x20000000 " + " ".join("gk3.pad%02d=%s" % (i, "x" * 12) for i in range(25)))
    assert len(cmd) > 511
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
    for n, d in (("Image", kern), ("ramdisk.img", ramd), ("gaokun3.dtb", dtb)):
        open(os.path.join(exp, n), "wb").write(d)
    open(os.path.join(exp, "cmdline"), "w").write(cmd)
# super 的原始内容：256 MiB，偏移 4096 处是 LP geometry 魔数，几段随机数据，其余是零
raw = bytearray(256 << 20)
raw[4096:4100] = b"gDla"                      # 0x616c4467 小端
for off in (1 << 20, 37 << 20, 200 << 20):
    raw[off:off + (2 << 20)] = os.urandom(2 << 20)
open(os.path.join(exp, "super.raw"), "wb").write(raw)
# systemd-boot 的假件故意【不】叫 systemd-bootaa64.efi：GK3_SDBOOT 这个覆盖开关曾经
# 只在文件恰好叫这个名字时才生效（录 fixture 时才发现），这里把它钉住
for n in ("rescue.squashfs", "initramfs.img", "fake-sdboot.efi", "recovery-ramdisk.img"):
    open(os.path.join(rel if n != "fake-sdboot.efi" else exp, n), "wb").write(b"MZ" + os.urandom(65536))
open(os.path.join(rel, "wpa_supplicant.conf"), "w").write('network={\n\tssid="test"\n\tpsk="12345678"\n}\n')
open(os.path.join(rel, "authorized_keys"), "w").write("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIG0000000000000000000000000000000000000000000 test@gk3\n")
PYEOF
img2simg "$W/expect/super.raw" "$W/super.img" >/dev/null
zstd -q -19 --long -f "$W/super.img" -o "$REL/super.img.zst"
( cd "$REL" && sha256sum boot.img super.img.zst > install-artifacts.sha256 )
export GK3_SDBOOT=$W/expect/fake-sdboot.efi
if [ -z "${GK3_TEST_BOOTIMG:-}" ]; then
    python3 scripts/live/gk3-bootimg.py "$REL/boot.img" "$W/got" 2>/dev/null
    for n in Image ramdisk.img gaokun3.dtb; do
        [ "$(sha "$W/got/$n")" = "$(sha "$W/expect/$n")" ] || bad "合成 boot.img 解包：$n 不一致"
    done
    [ "$(tr -d '\n' < "$W/got/cmdline.txt")" = "$(cat "$W/expect/cmdline")" ] \
        && ok "合成 boot.img（cmdline $(wc -c < "$W/expect/cmdline") 字节，跨 cmdline/extra_cmdline 两段）解包逐字节一致" \
        || bad "跨段 cmdline 拼回来不一致"
else
    python3 scripts/live/gk3-bootimg.py "$REL/boot.img" "$W/expect" 2>/dev/null
    tr -d '\n' < "$W/expect/cmdline.txt" > "$W/expect/cmdline"
    ok "用真 boot.img：$GK3_TEST_BOOTIMG"
fi
ok "发布目录：boot.img + super.img.zst（$(du -h "$REL/super.img.zst" | cut -f1)，img2simg 编码）+ 校验清单 + 救援件"
CMDLINE=$(cat "$W/expect/cmdline")
RAWSZ=$(stat -c%s "$W/expect/super.raw")

# 装完之后的逐项核对。$1=盘 $2=救援 yes|no $3=ESP 节点（双系统时是别人的 ESP，不叫 esp）
#   $4 $5 = 救援分区里的 squashfs、ESP 上的救援 initramfs 应当来自哪个文件（默认发布目录里那两个）
verify_install() {
    local d=$1 resc=$2 esp=${3:-} rsq=${4:-$REL/rescue.squashfs} rini=${5:-$REL/initramfs.img} mdef=${6:-none} n p m
    [ -n "$esp" ] || esp=$(gk3__bylabel "$d" esp)
    [ -b "$esp" ] || { bad "找不到 ESP"; return; }
    for n in misc metadata boot_a boot_b super userdata; do
        p=$(gk3__bylabel "$d" "$n"); [ -b "$p" ] || { bad "缺分区 $n"; return; }
    done
    [ "$resc" = yes ] && { p=$(gk3__bylabel "$d" gk3rescue); [ -b "$p" ] || bad "缺分区 gk3rescue"; }
    ok "分区齐全（按 PARTLABEL 解析）"
    [ "$(blkid -o value -s TYPE "$(gk3__bylabel "$d" metadata)")" = ext4 ] \
      && [ "$(blkid -o value -s TYPE "$(gk3__bylabel "$d" userdata)")" = ext4 ] \
      && ok "metadata / userdata 是 ext4" || bad "文件系统类型不对"
    # v1.0 计划 STOR-5：userdata 不给 root 留 5%（开发机实测白占约 18.8 GiB）
    [ "$(tune2fs -l "$(gk3__bylabel "$d" userdata)" 2>/dev/null | awk -F: '/^Reserved block count/{gsub(/[ \t]/, "", $2); print $2}')" = 0 ] \
      && ok "userdata 没有 root 保留块（mkfs -m 0）" || bad "userdata 有 root 保留块（mkfs 没带 -m 0？）"
    p=$(gk3__bylabel "$d" super)
    [ "$(sha_head "$p" "$RAWSZ")" = "$(sha "$W/expect/super.raw")" ] \
        && ok "super 前 $((RAWSZ >> 20)) MiB 与原始镜像 sha256 一致（经 .zst → gk3-unsparse 流式写入）" \
        || bad "super 内容不对"
    for n in boot_a boot_b; do
        [ "$(sha_head "$(gk3__bylabel "$d" $n)" "$(stat -c%s "$REL/boot.img")")" = "$(sha "$REL/boot.img")" ] \
            || { bad "$n 内容不对"; return; }
    done; ok "boot_a / boot_b == boot.img"
    misc_ok "$(gk3__bylabel "$d" misc)" "$mdef" \
        && ok "misc：BCAB _a 15/6 未成功、_b 0/0，GK3 记录（已迁移、set_default=${mdef}）—— 与独立算的逐字节相同，其余全零" \
        || { bad "misc 不是期望的初始状态（set_default=${mdef}）"; "$(gk3__misc_tool)" dump "$(gk3__bylabel "$d" misc)" 2>/dev/null | sed 's/^/      /'; }

    m=$W/mnt-esp; mkdir -p "$m"; mount -o ro "$esp" "$m" || { bad "ESP 挂不上"; return; }
    [ "$(sha "$m/EFI/BOOT/BOOTAA64.EFI")" = "$(sha "$GK3_SDBOOT")" ] && ok "EFI/BOOT/BOOTAA64.EFI = systemd-boot" || bad "BOOTAA64.EFI 不对"
    grep -qx 'default \*-android-a.conf' "$m/loader/loader.conf" && ok "loader.conf default = *-android-a.conf" || bad "loader.conf default 不对"
    # 纯 Android 与双系统统一菜单 5 秒，都不写 menu-hidden
    local tmo=5
    grep -qx "timeout $tmo" "$m/loader/loader.conf" && ! grep -q 'menu-hidden' "$m/loader/loader.conf" \
        && ok "loader.conf timeout ${tmo}，没有 menu-hidden" || bad "loader.conf 的 timeout 不对（要 $tmo）：$(grep timeout "$m/loader/loader.conf")"
    for s in a b; do
        for n in Image gaokun3.dtb ramdisk.img; do
            [ "$(sha "$m/$MID/android/slot_$s/$n")" = "$(sha "$W/expect/$n")" ] || { bad "slot_$s/$n 不对"; umount "$m"; return; }
        done
        [ "$(sed -n 's/^options *//p' "$m/loader/entries/$MID-android-$s.conf")" = "$CMDLINE androidboot.slot_suffix=_$s" ] \
            || { bad "slot_$s 启动项 options 不是 boot.img 的 cmdline + slot_suffix"; umount "$m"; return; }
    done
    ok "两个槽：内核/dtb/ramdisk 与 boot.img 里的一致；options = boot.img 的 cmdline + slot_suffix"
    [ -f "$m/$MID/android/slot_a/recovery-ramdisk.img" ] && [ ! -e "$m/loader/entries/$MID-recovery-a.conf" ] \
        && ok "recovery ramdisk 铺了、但启动项默认不建" || bad "recovery 的处理不对"
    if [ "$resc" = yes ]; then
        local want; want=$(gk3__rescue_cmdline "$CMDLINE")
        [ "$(sed -n 's/^options *//p' "$m/loader/entries/$MID-rescue.conf")" = "$want" ] \
          && ! grep -q 'androidboot\.' "$m/loader/entries/$MID-rescue.conf" \
          && grep -q 'usbhid.quirks' "$m/loader/entries/$MID-rescue.conf" \
          && [ "$(sha "$m/$MID/rescue/initramfs.img")" = "$(sha "$rini")" ] \
            && ok "救援启动项：cmdline 从 boot.img 派生（去掉 androidboot.*、保留 usbhid.quirks），initramfs 正确" \
            || bad "救援启动项不对"
        # INST-17：第二条救援条目借 slot_b 的内核与 dtb，initramfs 与 options 与第一条相同（不另占 ESP 空间）
        local ra=$m/loader/entries/$MID-rescue.conf rb=$m/loader/entries/$MID-rescue-b.conf
        [ "$(sed -n 's/^linux *//p' "$ra")" = "/$MID/android/slot_a/Image" ] \
          && [ "$(sed -n 's/^devicetree *//p' "$ra")" = "/$MID/android/slot_a/gaokun3.dtb" ] \
          && [ "$(sed -n 's/^linux *//p' "$rb")" = "/$MID/android/slot_b/Image" ] \
          && [ "$(sed -n 's/^devicetree *//p' "$rb")" = "/$MID/android/slot_b/gaokun3.dtb" ] \
          && [ "$(sed -n 's/^options *//p' "$rb")" = "$want" ] \
          && [ "$(sed -n 's/^initrd *//p' "$rb")" = "/$MID/rescue/initramfs.img" ] \
          && [ "$(ls "$m/$MID/rescue/")" = initramfs.img ] \
            && ok "两条救援条目：rescue.conf 借 slot_a、rescue-b.conf 借 slot_b 的内核，同一个 initramfs、同样的 options" \
            || { bad "slot_b 的救援条目不对"; sed 's/^/      /' "$rb" 2>/dev/null; }
    fi
    umount "$m"
    if [ "$resc" = yes ]; then
        m=$W/mnt-resc; mkdir -p "$m"; mount -o ro "$(gk3__bylabel "$d" gk3rescue)" "$m"
        [ "$(sha "$m/gaokun3/rescue.squashfs")" = "$(sha "$rsq")" ] \
          && [ "$(stat -c%a "$m/gaokun3/wpa_supplicant.conf")" = 600 ] \
            && ok "救援分区：squashfs 正确、WiFi 配置权限 600" || bad "救援分区内容不对"
        # 公开的 live 镜像不带公钥 —— 救援系统要能远程进去，公钥得跟着装进来
        [ "$(sha "$m/gaokun3/authorized_keys")" = "$(sha "$REL/authorized_keys")" ] && [ "$(stat -c%a "$m/gaokun3/authorized_keys")" = 600 ] \
            && ok "救援分区：ssh 公钥带进来了（600）" || bad "救援分区里没有 ssh 公钥"
        umount "$m"
    fi
}

# ── P. 预检：缺工具时报出包名（v1.0 计划 INST-16）───────────────────────────
echo "═══ P. 预检：缺的工具对应哪个包 ═══"
PB=$W/pathbin; mkdir -p "$PB"
# 预检本身要用的命令照常给；故意不给 sgdisk、partprobe、blkid、lsblk（后两个同属 util-linux：包名只出一次）
for t in id cat od sed tr grep dmesg findmnt mkfs.vfat mkfs.ext4 dd zstd python3; do
    command -v "$t" >/dev/null && ln -sf "$(command -v "$t")" "$PB/$t"
done
TL=$(PATH=$PB gk3_preflight 2>/dev/null | grep '^CHECK id=tools ')
[ "$TL" = "CHECK id=tools ok=no missing=sgdisk,partprobe,blkid,lsblk pkgs=gdisk,parted,util-linux" ] \
    && ok "缺 sgdisk / partprobe / blkid / lsblk：$TL" || bad "预检的包名不对：$TL"
TL=$(gk3_preflight 2>/dev/null | grep '^CHECK id=tools ')
[ "$TL" = "CHECK id=tools ok=yes" ] && ok "工具齐全：${TL}（没有 pkgs= 字段）" || bad "工具齐全时预检不对：$TL"
duel_call P gk3_preflight; duel_call "P 缺工具" "PATH=$PB" gk3_preflight; duel_pure

# ── A. 命令行版，整盘 ───────────────────────────────────────────────────────
echo "═══ A. install-gaokun3.sh 整盘安装 ═══"
DA=$(new_disk a 40G)
sgdisk -o "$DA" >/dev/null                            # 一张空 GPT，"原来的盘"
BEFORE=$(fp "$DA")
echo nope | DISK=$DA GK3_SKIP_PREFLIGHT=1 bash scripts/install-gaokun3.sh "$REL" >"$W/a0.log" 2>&1; rc=$?
[ "$rc" != 0 ] && [ "$(fp "$DA")" = "$BEFORE" ] && grep -q 'gk3rescue' "$W/a0.log" \
    && ok "不输 ERASE：退出码 ${rc}、盘上一个字节没变（且事先打印了含 gk3rescue 的新布局）" \
    || { bad "不输 ERASE 时 rc=$rc 或盘被改了"; tail -5 "$W/a0.log"; }
echo ERASE | DISK=$DA GK3_SKIP_PREFLIGHT=1 bash scripts/install-gaokun3.sh "$REL" >"$W/a1.log" 2>&1; rc=$?
if [ "$rc" = 0 ]; then ok "输了 ERASE：装完（退出码 0）"; verify_install "$DA" yes
else bad "整盘安装失败 rc=$rc"; tail -20 "$W/a1.log" | sed 's/^/      /'; fi
grep -q '^PROGRESS 100 ' "$W/a1.log" && ok "进度走到 100" || bad "进度没走到 100"
[ "$(grep -c '^PROGRESS [3-6][0-9] write-super done_mib=[0-9]* total_mib=[1-9]' "$W/a1.log")" -ge 5 ] \
    && ok "写 super 期间有 $(grep -c '^PROGRESS [3-6][0-9] write-super ' "$W/a1.log") 行进度（simg2img 那里是几分钟的沉默）" \
    || bad "写 super 期间没有进度"
ls /tmp/gpt-before-apply-"$(basename "$DA")"-*.bin >/dev/null 2>&1 && grep -q '分区表只备份到了内存里' "$W/a1.log" \
    && ok "动盘前备份了分区表（介质不可写 → 落到 /tmp，并且警告了重启就没）" || bad "没有分区表备份，或者落到内存里却没警告"
# v1.0 计划 INST-10：给界面的进度只有代码，不夹中文（中文只在 `!!` 与日志里）
BADP=$(grep '^PROGRESS ' "$W/a1.log" | grep -v -E '^PROGRESS [0-9]+ [a-z][a-z0-9-]*( [a-z_]+=[^ ]*)*$' | head -3)
[ -z "$BADP" ] && ok "PROGRESS 行全是「百分比 代码 k=v…」（$(grep -c '^PROGRESS ' "$W/a1.log") 行）" || bad "有不合协议的进度行：$BADP"
# 命令行版失败时说清楚盘动没动过：发布目录里缺 super → 在第一次写盘之前就失败
mkdir -p "$W/badrel" && cp "$REL/boot.img" "$W/badrel/"
BEFORE=$(fp "$DA")
echo ERASE | DISK=$DA GK3_SKIP_PREFLIGHT=1 bash scripts/install-gaokun3.sh "$W/badrel" >"$W/a2.log" 2>&1; rc=$?
[ "$rc" != 0 ] && [ "$(fp "$DA")" = "$BEFORE" ] && grep -q 'before anything was written' "$W/a2.log" \
    && grep -q '^ERR code=release-no-super touched=no' "$W/a2.log" \
    && ok "命令行版失败在动盘之前：明说盘没动过（ERR code=release-no-super touched=no），盘确实没变" \
    || { bad "命令行版失败时没说清盘动没动过（rc=${rc}）"; tail -4 "$W/a2.log" | sed 's/^/      /'; }
duel_scene "A 整盘装完" "$DA"

# ── B. 双系统 ──────────────────────────────────────────────────────────────
echo "═══ B. 双系统：装进 Windows 盘中间的空闲区 ═══"
DB=$(new_disk b 40G)
TOT=$(blockdev --getsz "$DB"); LAST=$(( TOT - 34 )); WRS=$(( (LAST - 2097152 + 1) / 2048 * 2048 ))
sgdisk -o \
  -n 1:2048:+300M -t 1:ef00 -c 1:"EFI system partition" \
  -n 2:0:+16M     -t 2:0c01 -c 2:"Microsoft reserved partition" \
  -n 3:0:+4G      -t 3:0700 -c 3:"Basic data partition" \
  -n 4:$WRS:$LAST -t 4:2700 -c 4:"Basic data partition" "$DB" >/dev/null 2>&1
partprobe "$DB" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 -n SYSTEM "${DB}p1" >/dev/null
head -c 1048576 /dev/urandom > "$W/bootmgfw.efi"; head -c 700000 /dev/urandom > "$W/winfallback.efi"
mmd -i "${DB}p1" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot ::/EFI/Boot
mcopy -i "${DB}p1" "$W/bootmgfw.efi" ::/EFI/Microsoft/Boot/bootmgfw.efi
mcopy -i "${DB}p1" "$W/winfallback.efi" ::/EFI/Boot/bootaa64.efi
mkntfs -Q -F -L Windows "${DB}p3" >/dev/null 2>&1
head -c 1048576 /dev/urandom | dd of="${DB}p4" conv=notrunc status=none
declare -A H PU
for n in 2 3 4; do H[$n]=$(sha "${DB}p$n"); done
for n in 1 2 3 4; do PU[$n]=$(sgdisk -i "$n" "$DB" 2>/dev/null | awk '/unique GUID/{print $4}'); done
ESP_UUID=$(blkid -o value -s UUID "${DB}p1")

duel_scene "B Windows 盘（装前）" "$DB"
PROBE=$(gk3_probe 2>/dev/null | awk -v d="$DB" '$2=="path="d || index($0, "disk="d" ") || index($0, "path="d"p")')
printf '%s\n' "$PROBE" | sed 's/^/    /'
printf '%s\n' "$PROBE" | grep -q "name=Basic%20data%20partition " && printf '%s\n' "$PROBE" | grep -q "name=EFI%20system%20partition " \
    && ok "PARTLABEL 里的空格按协议编码（name=Basic%20data%20partition），不会被切成 \"Basic\"" || bad "PARTLABEL 编码不对"
printf '%s\n' "$PROBE" | grep -q "os=winre" && ok "认出 WinRE（os=winre）" || bad "没认出 WinRE"
FREE=$(printf '%s\n' "$PROBE" | grep '^FREE ' | sort -t= -k5 -n | tail -1)
RS=$(gk3__f "$FREE" start); RE=$(gk3__f "$FREE" end)
[ -n "$RS" ] && ok "空闲区 [$RS, $RE]（$(gk3__f "$FREE" size_mib) MiB）" || bad "没探到空闲区"
if [ -n "${GK3_TEST_FIXTURES:-}" ]; then
    mkdir -p "$GK3_TEST_FIXTURES"
    printf '%s\n' "$PROBE" | sed -e "s#$DB#/dev/nvme0n1#g" -e 's#/dev/nvme0n1p\([0-9]\)#/dev/nvme0n1p\1#g' \
        > "$GK3_TEST_FIXTURES/probe-windows.txt"
    ok "fixture → $GK3_TEST_FIXTURES/probe-windows.txt（loop 名换成了 nvme0n1）"
fi

gk3_apply --disk "$DB" --mode alongside --rescue no --release "$REL" \
          --region-start "$RS" --region-end "$RE" --esp "${DB}p1" >"$W/b.log" 2>&1; rc=$?
if [ "$rc" = 0 ]; then ok "双系统安装完成"
    verify_install "$DB" no "${DB}p1" "$REL/rescue.squashfs" "$REL/initramfs.img" android
else bad "双系统安装失败 rc=$rc"; tail -20 "$W/b.log" | sed 's/^/      /'; fi
same=1; for n in 2 3 4; do [ "$(sha "${DB}p$n")" = "${H[$n]}" ] || { same=0; bad "p$n 的内容变了"; }; done
[ "$same" = 1 ] && ok "MSR / NTFS / WinRE 三个分区逐字节未变"
same=1; for n in 1 2 3 4; do [ "$(sgdisk -i "$n" "$DB" 2>/dev/null | awk '/unique GUID/{print $4}')" = "${PU[$n]}" ] || same=0; done
[ "$same" = 1 ] && ok "四个原有分区的 PARTUUID 都没变（Windows 的 BCD 靠它）" || bad "有 PARTUUID 变了"
[ "$(blkid -o value -s UUID "${DB}p1")" = "$ESP_UUID" ] && ok "ESP 没被重新格式化（卷序列号 $ESP_UUID 未变）" || bad "ESP 被格式化了"
EI=$(gk3_esp_info "${DB}p1")
printf '%s' "$EI" | grep -q 'windows=yes gaokun3=yes' && ok "装完后 gk3_esp_info：$EI" || bad "gk3_esp_info 不对：$EI"
m=$W/mnt-b; mkdir -p "$m"; mount -o ro "${DB}p1" "$m"
[ "$(sha "$m/EFI/Microsoft/Boot/bootmgfw.efi")" = "$(sha "$W/bootmgfw.efi")" ] && ok "Windows 引导（EFI/Microsoft/Boot/bootmgfw.efi）还在、未变" || bad "Windows 引导被动了"
[ "$(sha "$m/EFI/BOOT/BOOTAA64.EFI.before-gaokun3")" = "$(sha "$W/winfallback.efi")" ] \
    && ok "原来的回落引导 bootaa64.efi 已留成 .before-gaokun3" || bad "原来的回落引导没备份"
umount "$m"
# 读盘上【实际】的起止扇区，不读方案 —— 方案对不等于落盘对
inside=0 outside=""
for n in $(sgdisk -p "$DB" | awk '/^ *[0-9]+ /{print $1}'); do
    case "$(sgdisk -i "$n" "$DB" | grep '^Partition name:' | cut -d"'" -f2)" in
        misc|metadata|boot_a|boot_b|super|userdata) ;; *) continue ;;
    esac
    s=$(sgdisk -i "$n" "$DB" | awk '/^First sector:/{print $3}'); e=$(sgdisk -i "$n" "$DB" | awk '/^Last sector:/{print $3}')
    if [ "$s" -ge "$RS" ] && [ "$e" -le "$RE" ]; then inside=$((inside+1)); else outside="$outside p$n[$s,$e]"; fi
done
[ "$inside" = 6 ] && [ -z "$outside" ] && ok "6 个新分区在盘上的起止扇区全部落在空闲区内" || bad "新分区：区内 $inside 个，越界：${outside:-无}"
BEFORE=$(fp "$DB")
OUT=$(gk3_apply --disk "$DB" --mode alongside --rescue no --release "$REL" \
          --region-start "$RS" --region-end "$RE" --esp "${DB}p1" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q 'partlabel-conflict' && [ "$(fp "$DB")" = "$BEFORE" ] \
    && ok "在已装过的盘上再装一次：partlabel-conflict 拒绝，盘没动" || bad "重复安装没被拦住（rc=${rc}）"
duel_scene "B 双系统装完" "$DB"

# ── C. 反例：都必须在动盘之前拒绝 ───────────────────────────────────────────
echo "═══ C. 反例 ═══"
DC=$(new_disk c 40G); sgdisk -o "$DC" >/dev/null; BEFORE=$(fp "$DC")
duel_scene "C 空 GPT" "$DC"
try() {   # $1=说明 $2=发布目录 $3=期望的报错片段
    local out rc; out=$(gk3_apply --disk "$DC" --mode wipe --rescue no --release "$2" 2>&1); rc=$?
    if [ "$rc" != 0 ] && printf '%s' "$out" | grep -q "$3" && [ "$(fp "$DC")" = "$BEFORE" ]; then
        ok "$1：拒绝且盘没动（$(printf '%s\n' "$out" | grep '^!!' | tail -1 | cut -c4-)）"
    else bad "$1：rc=${rc}，或报错不对，或盘被改了"; printf '%s\n' "$out" | tail -4 | sed 's/^/      /'; fi
}
R1=$W/rel-trunc; mkdir "$R1"; cp "$REL/boot.img" "$R1/"
head -c $(( $(stat -c%s "$REL/super.img.zst") * 2 / 3 )) "$REL/super.img.zst" > "$R1/super.img.zst"
try "截断的 super.img.zst（没有校验清单）" "$R1" "zstd -t"
R2=$W/rel-sha; mkdir "$R2"; cp "$REL"/boot.img "$REL"/super.img.zst "$REL"/install-artifacts.sha256 "$R2/"
printf 'X' | dd of="$R2/boot.img" bs=1 seek=100000 conv=notrunc status=none
try "boot.img 与校验清单不符" "$R2" "sha256"
R3=$W/rel-nosuper; mkdir "$R3"; cp "$REL/boot.img" "$R3/"
try "缺 super" "$R3" "super.img"
R4=$W/rel-badboot; mkdir "$R4"; cp "$REL/super.img.zst" "$R4/"; head -c 5000 "$REL/boot.img" > "$R4/boot.img"
try "boot.img 被截断" "$R4" "boot.img 拆不开"
# 双系统的 ESP 检查也必须在动盘之前：Windows 默认建的 ESP 只有 100 MiB，放不下我们要的
# GK3_ESP_NEED_MIB —— 这在真实世界里是最常见的一种"装不了"，不是边角情况
DE=$(new_disk e 40G)
sgdisk -o -n 1:2048:+100M -t 1:ef00 -c 1:"EFI system partition" \
          -n 2:0:+4G -t 2:0700 -c 2:"Basic data partition" "$DE" >/dev/null 2>&1
partprobe "$DE" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 "${DE}p1" >/dev/null 2>&1; mkntfs -Q -F "${DE}p2" >/dev/null 2>&1
FE=$(gk3_probe 2>/dev/null | grep "^FREE disk=$DE " | tail -1)
duel_scene "C 100 MiB 的 ESP" "$DE"
BEFORE_E=$(fp "$DE")
tryb() {  # $1=说明 $2=--esp $3=期望的报错片段
    local out rc; out=$(gk3_apply --disk "$DE" --mode alongside --rescue no --release "$REL" \
        --region-start "$(gk3__f "$FE" start)" --region-end "$(gk3__f "$FE" end)" --esp "$2" 2>&1); rc=$?
    if [ "$rc" != 0 ] && printf '%s' "$out" | grep -q "$3" && [ "$(fp "$DE")" = "$BEFORE_E" ]; then
        ok "$1：拒绝且盘没动（$(printf '%s\n' "$out" | grep '^!!' | tail -1 | cut -c4-)）"
    else bad "$1：rc=${rc}，或报错不对，或盘被改了"; printf '%s\n' "$out" | tail -3 | sed 's/^/      /'; fi
}
EI=$(gk3_esp_info "${DE}p1")
[ "$(gk3__f "$EI" free_mib)" -lt "$(gk3__f "$EI" need_mib)" ] && printf '%s' "$EI" | grep -q 'windows=no' && printf '%s' "$EI" | grep -q ' small=yes ' \
    && ok "gk3_esp_info 事先就报出来了（small=yes）：$EI" || bad "gk3_esp_info 不对：$EI"
# U17：100 MiB 的 ESP 明确拒绝（ERR esp-too-small），不去撞"空间不够、请清理"—— 那种 ESP 清理也清不出来
tryb "Windows 默认的 100 MiB ESP（U17 明确拒绝）" "${DE}p1" "^ERR code=esp-too-small size_mib=100 min_mib=200 touched=no"
tryb "--esp 指向 NTFS 分区" "${DE}p2" "不是 FAT"
tryb "--esp 指向不存在的节点" "${DE}p9" "要 --esp"
# U12：这个 ESP 上没有 Windows 的启动管理器 ⇒ 不能选 Windows 为默认（在空间检查之前就拒绝）
tryb_def() { local out rc; out=$(gk3_apply --disk "$DE" --mode alongside --rescue no --release "$REL" --default-os windows \
        --region-start "$(gk3__f "$FE" start)" --region-end "$(gk3__f "$FE" end)" --esp "${DE}p1" 2>&1); rc=$?
    [ "$rc" != 0 ] && printf '%s' "$out" | grep -q '^ERR code=default-os-no-windows ' && [ "$(fp "$DE")" = "$BEFORE_E" ] \
        && ok "没有 Windows 的 ESP 上选 Windows 为默认：default-os-no-windows，盘没动" || { bad "没有 Windows 也让选 Windows 为默认（rc=${rc}）"; printf '%s\n' "$out" | tail -3 | sed 's/^/      /'; }; }
tryb_def
OUT=$(gk3_apply --disk "$DC" --mode wipe --rescue no --release "$REL" --default-os windows 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q '^ERR code=default-os-no-windows touched=no' && [ "$(fp "$DC")" = "$BEFORE" ] \
    && ok "整盘清空 + 选 Windows 为默认：拒绝（清空之后没有 Windows），盘没动" || bad "整盘清空居然接受了 Windows 为默认（rc=${rc}）"
OUT=$(GK3_DRYRUN=1 gk3_apply --disk "$DC" --mode wipe --rescue yes --release "$REL" 2>&1); rc=$?
[ "$rc" = 0 ] && printf '%s' "$OUT" | grep -q '^DRY: sgdisk --zap-all' && [ "$(fp "$DC")" = "$BEFORE" ] \
  && printf '%s' "$OUT" | grep -q '^DRY: .*gk3-misc init <misc分区> --slot a --default none$' \
    && ok "dry-run：列出了 $(printf '%s\n' "$OUT" | grep -c '^DRY:') 条命令（含 gk3-misc init），盘一个字节没变" || bad "dry-run 不对（rc=${rc}）"

# ── D. 免 U 盘：安装器就跑在目标盘上 ─────────────────────────────────────────
# 用户 2026-09-25：LiveCD 的初衷之一是【免 U 盘安装】，并且要能装双系统。于是"介质与目标同盘"
# 是正经流程，不是边角：Windows 里缩出空闲区、建一个放 live 的小分区（这里是 p4 GK3LIVE），
# 从它起安装器。规则：双系统放行（只往空闲区建分区）、整盘清空拒绝、介质分区不可缩。
echo "═══ D. 免 U 盘：安装器就跑在目标盘上（介质与目标同盘）═══"
DD=$(new_disk d 40G)
TOT=$(blockdev --getsz "$DD"); LAST=$(( TOT - 34 )); WRS=$(( (LAST - 2097152 + 1) / 2048 * 2048 ))
sgdisk -o \
  -n 1:2048:+300M -t 1:ef00 -c 1:"EFI system partition" \
  -n 2:0:+16M     -t 2:0c01 -c 2:"Microsoft reserved partition" \
  -n 3:0:+4G      -t 3:0700 -c 3:"Basic data partition" \
  -n 4:0:+1G      -t 4:0700 -c 4:"Basic data partition" \
  -n 5:$WRS:$LAST -t 5:2700 -c 5:"Basic data partition" "$DD" >/dev/null 2>&1
partprobe "$DD" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 -n SYSTEM "${DD}p1" >/dev/null; mkntfs -Q -F -L Windows "${DD}p3" >/dev/null 2>&1
mkfs.vfat -F 32 -n GK3LIVE "${DD}p4" >/dev/null
mkdir -p /media/gk3 && mount "${DD}p4" /media/gk3 && mkdir -p /media/gk3/gaokun3
# GK3LIVE 的布局照 Windows 安装包（scripts/windows/build-bundle.sh）：live.squashfs + initramfs.img，没有 rescue.squashfs
head -c 1048576 /dev/urandom > /media/gk3/gaokun3/live.squashfs; LIVE_SHA=$(sha /media/gk3/gaokun3/live.squashfs)
head -c 70000 /dev/urandom > /media/gk3/gaokun3/initramfs.img; sync
PU4=$(sgdisk -i 4 "$DD" 2>/dev/null | awk '/unique GUID/{print $4}')
PROBE=$(gk3_probe 2>/dev/null | awk -v d="$DD" '$2=="path="d || index($0, "disk="d" ") || index($0, "path="d"p")')
duel_scene "D 介质与目标同盘" "$DD"
printf '%s\n' "$PROBE" | grep -q "^DISK path=$DD .*medium=yes" && printf '%s\n' "$PROBE" | grep -q "^PART path=${DD}p4 .*medium=yes" \
    && ! printf '%s\n' "$PROBE" | grep -q "^PART path=${DD}p3 .*medium=yes" \
    && ok "gk3_probe：整块盘 medium=yes，且只有 p4（安装器所在）标 medium=yes" || bad "medium 标得不对"
SI=$(gk3_shrink_info "${DD}p4" 2>&1)
printf '%s' "$SI" | grep -q 'can=no why=mounted' && ok "介质分区不可缩：$SI" || bad "介质分区居然可缩：$SI"
BEFORE_D=$(fp "$DD")
OUT=$(gk3_shrink "${DD}p4" 600 2>&1); rc=$?
[ "$rc" != 0 ] && [ "$(fp "$DD")" = "$BEFORE_D" ] && ok "gk3_shrink 自己也拒绝缩介质分区，盘没动" || bad "gk3_shrink 缩了介质分区（rc=${rc}）"
OUT=$(gk3_apply --disk "$DD" --mode wipe --rescue no --release "$REL" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q '锯掉' && [ "$(fp "$DD")" = "$BEFORE_D" ] \
    && printf '%s' "$OUT" | grep -q "^ERR code=wipe-medium disk=$DD touched=no" \
    && ok "整盘清空介质所在的盘：拒绝且盘没动" || bad "整盘清空没被拦住（rc=${rc}）"
FREE=$(printf '%s\n' "$PROBE" | grep '^FREE ' | sort -t= -k5 -n | tail -1)
RS=$(gk3__f "$FREE" start); RE=$(gk3__f "$FREE" end)
# ★ v1.0 计划 INST-6：从 Windows 开始的安装，发布目录（网络下载的 / payload/）里没有救援件，介质上也没有叫
#   rescue.squashfs 的文件 —— 原先因此永远装不上救援系统。现在用正在跑的 live.squashfs
RELW=$W/rel-win; mkdir -p "$RELW"; cp "$REL"/boot.img "$REL"/super.img.zst "$REL"/install-artifacts.sha256 "$REL"/wpa_supplicant.conf "$REL"/authorized_keys "$REL"/recovery-ramdisk.img "$RELW/"
RI=$(gk3_release_info "$RELW")
printf '%s' "$RI" | grep -q ' rescue=yes ' && [ "$(gk3__find_rescue_squashfs "$RELW")" = /media/gk3/gaokun3/live.squashfs ] \
    && ok "Windows 安装包的介质（只有 live.squashfs）：gk3_release_info 报 rescue=yes，用的是 live.squashfs" || bad "Windows 安装包的介质认不出救援镜像：$RI"
gk3_apply --disk "$DD" --mode alongside --rescue yes --release "$RELW" \
          --region-start "$RS" --region-end "$RE" --esp "${DD}p1" >"$W/d.log" 2>&1; rc=$?
if [ "$rc" = 0 ]; then ok "双系统装进同一块盘的空闲区（带救援系统）：完成（介质分区一直挂着）"
    # 这块盘的 ESP 上没放 Windows 的启动管理器 ⇒ 不算双系统：set_default 不放、菜单 15 秒
    verify_install "$DD" yes "${DD}p1" /media/gk3/gaokun3/live.squashfs /media/gk3/gaokun3/initramfs.img none
else bad "双系统安装失败 rc=$rc"; tail -20 "$W/d.log" | sed 's/^/      /'; fi
findmnt -rn -S "${DD}p4" -T /media/gk3 >/dev/null && [ "$(sha /media/gk3/gaokun3/live.squashfs)" = "$LIVE_SHA" ] \
    && [ "$(sgdisk -i 4 "$DD" 2>/dev/null | awk '/unique GUID/{print $4}')" = "$PU4" ] \
    && ok "介质分区：还挂着、live.squashfs 内容未变、PARTUUID 未变" || bad "介质分区被动了"
duel_scene "D 装完（介质还挂着）" "$DD"
umount /media/gk3

# ── F. 重新安装 ────────────────────────────────────────────────────────────
# 用户 2026-09-25：盘上已经有我们的 Android 时，整盘清空（免 U 盘时会锯掉安装器）和双系统（会建出
# 第二套同名分区）都走不通。重新安装 = 不改分区表、复用现有分区：写新系统，默认格式化 /data。
echo "═══ F. 重新安装：复用 A 节装好的那块盘 ═══"
ESPA=$(gk3__bylabel "$DA" esp); UDA=$(gk3__bylabel "$DA" userdata)
mk=$W/mnt-f; mkdir -p "$mk"
mount "$UDA" "$mk" && echo "用户的数据" > "$mk/marker.txt" && umount "$mk"
# 故意写坏：重装之后必须又对了（证明真的重写了，而不是"本来就对"）
head -c 1048576 /dev/urandom | dd of="$(gk3__bylabel "$DA" boot_a)" conv=notrunc status=none
head -c 1048576 /dev/urandom | dd of="$(gk3__bylabel "$DA" super)" bs=1M seek=0 conv=notrunc status=none
# ESP 只留约 100 MiB 空闲：比新装要求的 150 少 —— 已经装过的机器上就是这种情况（我们的文件占着地方）
mount "$ESPA" "$mk"; FREEA=$(df -m "$mk" | awk 'NR==2{print $4}')
dd if=/dev/zero of="$mk/filler.bin" bs=1M count=$(( FREEA - 100 )) status=none; sync; umount "$mk"
BEFORE_F=$(sgdisk -p "$DA" | grep -v '^Disk identifier')
duel_scene "F 重新安装前（ESP 只剩约 100 MiB）" "$DA"
P=$(gk3_plan --disk "$DA" --mode reinstall --rescue yes --esp "$ESPA" --keep-data yes)
printf '%s\n' "$P" | grep -q '^PLAN op=reuse name=userdata .*action=keep' && printf '%s\n' "$P" | grep -q '^PLAN op=reuse name=super .*action=write' \
    && [ "$(printf '%s\n' "$P" | grep -c '^PLAN op=reuse')" = 7 ] && ! printf '%s\n' "$P" | grep -q '^PLAN op=mkpart' \
    && ok "gk3_plan reinstall：7 个分区全是 reuse（userdata=keep、super=write），没有 mkpart" || { bad "reinstall 方案不对"; printf '%s\n' "$P" | sed 's/^/      /'; }
gk3_apply --disk "$DA" --mode reinstall --rescue yes --release "$REL" --esp "$ESPA" --keep-data yes >"$W/f1.log" 2>&1; rc=$?
if [ "$rc" = 0 ]; then ok "保留数据的重新安装：完成（ESP 只剩约 100 MiB 也放行）"; verify_install "$DA" yes "$ESPA"
else bad "保留数据的重新安装失败 rc=$rc"; tail -15 "$W/f1.log" | sed 's/^/      /'; fi
[ "$(sgdisk -p "$DA" | grep -v '^Disk identifier')" = "$BEFORE_F" ] && ok "分区表逐字节没变（不改分区表）" || bad "分区表变了"
mount -o ro "$UDA" "$mk" && { [ "$(cat "$mk/marker.txt" 2>/dev/null)" = "用户的数据" ] && ok "保留数据：userdata 里的文件还在" || bad "保留数据却丢了文件"; umount "$mk"; }
gk3_apply --disk "$DA" --mode reinstall --rescue yes --release "$REL" --esp "$ESPA" >"$W/f2.log" 2>&1; rc=$?
[ "$rc" = 0 ] && ok "默认（清除数据）的重新安装：完成" || { bad "清除数据的重新安装失败 rc=$rc"; tail -10 "$W/f2.log" | sed 's/^/      /'; }
mount -o ro "$UDA" "$mk" && { [ ! -e "$mk/marker.txt" ] && ok "默认清除数据：userdata 被格式化了" || bad "说好清除数据，文件还在"; umount "$mk"; }
# 反例：目标分区挂着（安装器要是从它上面跑的，写它就是锯地板）→ 动盘之前拒绝
mount -o ro "$UDA" "$mk"; SB=$(sha_head "$(gk3__bylabel "$DA" boot_a)" 1048576)
OUT=$(gk3_apply --disk "$DA" --mode reinstall --rescue yes --release "$REL" --esp "$ESPA" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q '还挂着' && [ "$(sha_head "$(gk3__bylabel "$DA" boot_a)" 1048576)" = "$SB" ] \
    && ok "目标分区挂着：拒绝，boot_a 没被碰" || bad "挂着的分区没被拦住（rc=${rc}）"
umount "$mk"
OUT=$(gk3_plan --disk "$DC" --mode reinstall --rescue no --esp /dev/null 2>&1)
printf '%s' "$OUT" | grep -q '^PLANERR msg=reinstall-missing names=misc,metadata,boot_a,boot_b,super,userdata' \
    && ok "不是一套完整的安装：PLANERR 列出缺的分区" || bad "缺分区没被报出来：$OUT"
mount "$ESPA" "$mk" && rm -f "$mk/filler.bin" && umount "$mk"
# ★★ 2026-09-26 M4b 真机：live 的 machine-id 是每次开机现生成的，而这个测试一直导出同一个 GK3_MACHINE_ID ——
#   所以没抓到"重新安装在 ESP 上另开一个目录、又写一整套内核"（真机上把 ESP 写满了，slot_b 的 ramdisk
#   截断、启动项是空文件，安装却报告成功）。下面三条是那次的三个面。
OTHER=fedcba9876543210fedcba9876543210
GK3_MACHINE_ID=$OTHER gk3_apply --disk "$DA" --mode reinstall --rescue yes --release "$REL" --esp "$ESPA" >"$W/f3.log" 2>&1; rc=$?
mount -o ro "$ESPA" "$mk"
NA=$(ls "$mk"/loader/entries/*-android-a.conf 2>/dev/null | wc -l); NB=$(ls "$mk"/loader/entries/*-android-b.conf 2>/dev/null | wc -l)
[ "$rc" = 0 ] && [ ! -e "$mk/$OTHER" ] && [ "$NA" = 1 ] && [ "$NB" = 1 ] && [ -s "$mk/loader/entries/$MID-android-b.conf" ] \
    && ok "machine-id 换了（live 每次开机都换）：仍写进 ESP 上现有的目录，每个槽恰好一个启动项" \
    || { bad "machine-id 换了：rc=${rc}，新目录 $([ -e "$mk/$OTHER" ] && echo 有 || echo 无)，a=$NA b=$NB"; tail -5 "$W/f3.log" | sed 's/^/      /'; }
umount "$mk"
# 另一个目录下留着我们的启动项（M4b 那次留下的局面）：default 的通配会同时匹配 → 必须停用
mount "$ESPA" "$mk" && cp "$mk/loader/entries/$MID-android-b.conf" "$mk/loader/entries/$OTHER-android-b.conf" && umount "$mk"
gk3_apply --disk "$DA" --mode reinstall --rescue yes --release "$REL" --esp "$ESPA" --keep-data yes >"$W/f4.log" 2>&1; rc=$?
mount -o ro "$ESPA" "$mk"
[ "$rc" = 0 ] && [ ! -e "$mk/loader/entries/$OTHER-android-b.conf" ] && [ -e "$mk/loader/entries/$OTHER-android-b.conf.disabled" ] \
    && [ "$(ls "$mk"/loader/entries/*-android-b.conf | wc -l)" = 1 ] \
    && ok "别的目录下的 *-android-b.conf：改名停用（.disabled），default 的通配只剩一个匹配" || bad "重复的启动项没被停用（rc=${rc}）"
umount "$mk"
# ESP 上没有我们的目录（要写一整套新文件）、空闲又少：旧的检查（重新安装只要 16 MiB）会放行，然后写满 ESP。
# 两道新闸各验一次：① 放不下要写的量 ② 放得下，但装完之后 OTA postinstall 的门槛（空闲 + 槽里旧文件 > 56 MiB）过不了
# （这里的测试内核只有 1 MiB 左右，所以①要把 ESP 塞到几乎满）
esp_left() {   # $1=留多少 MiB 空闲
    mount "$ESPA" "$mk" && rm -f "$mk/filler.bin" && rm -rf "$mk/$MID" "$mk"/loader/entries/*-android-* \
        && dd if=/dev/zero of="$mk/filler.bin" bs=1M count=$(( $(df -m "$mk" | awk 'NR==2{print $4}') - $1 )) status=none; sync; umount "$mk"
}
SB=$(sha_head "$(gk3__bylabel "$DA" super)" 1048576)
esp_left 0
OUT=$(GK3_MACHINE_ID=$OTHER gk3_apply --disk "$DA" --mode reinstall --rescue yes --release "$REL" --esp "$ESPA" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q 'ESP 空间不够：要写' && [ "$(sha_head "$(gk3__bylabel "$DA" super)" 1048576)" = "$SB" ] \
    && printf '%s' "$OUT" | grep -q '^ERR code=esp-full need_mib=[0-9]* free_mib=[0-9]* touched=no' \
    && ok "ESP 放不下一整套新文件：按真要写的量算，动盘之前拒绝（super 没被碰）" \
    || { bad "ESP 不够却没在动盘前拦住（rc=${rc}）"; printf '%s\n' "$OUT" | tail -3 | sed 's/^/      /'; }
esp_left 30
OUT=$(GK3_MACHINE_ID=$OTHER gk3_apply --disk "$DA" --mode reinstall --rescue yes --release "$REL" --esp "$ESPA" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q '以后的系统更新（OTA）会因为 ESP 空间不够失败' && [ "$(sha_head "$(gk3__bylabel "$DA" super)" 1048576)" = "$SB" ] \
    && ok "ESP 放得下但装完只剩约 27 MiB：OTA 的门槛过不了，同样动盘之前拒绝" \
    || { bad "OTA 余量没拦住（rc=${rc}）"; printf '%s\n' "$OUT" | tail -3 | sed 's/^/      /'; }
mount "$ESPA" "$mk" && rm -f "$mk/filler.bin" && umount "$mk"
# ★ 真机的布局（2026-09-25 从设备的 sysfs 读的，按比例缩小）：misc 只有 1007 KiB、从第 34 扇区起
#   （GPT 表之后那段空隙）；没有 gk3rescue；安装器从 p3（ubunturescue）上跑。第一版的方案按 MiB 比，
#   把 misc 判成 0 MiB"太小"，重新安装在真机上整个走不通 —— 离线的测试盘都是我们自己的 4 MiB 布局，没抓到。
DR=$(new_disk r 28G)
sgdisk -o -a 1 -n 4:34:2047 -c 4:misc -t 4:8300 "$DR" >/dev/null 2>&1
sgdisk -a 2048 -n 1:2048:+300M -t 1:ef00 -c 1:esp -n 2:0:+9G -c 2:userdata -n 3:0:+1G -c 3:ubunturescue \
       -n 5:0:+64M -c 5:boot_a -n 6:0:+64M -c 6:boot_b -n 8:0:+12G -c 8:super -n 10:0:+32M -c 10:metadata "$DR" >/dev/null 2>&1
partprobe "$DR" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 -n ESP "${DR}p1" >/dev/null; mkfs.ext4 -q -F -L userdata "${DR}p2"; mkfs.ext4 -q -F -L ubunturescue "${DR}p3"
head -c 1031168 /dev/urandom | dd of="${DR}p4" conv=notrunc status=none     # misc 里先放点垃圾：必须被清零
mkdir -p /media/gk3 && mount "${DR}p3" /media/gk3 && mkdir -p /media/gk3/gaokun3
MK=$(( $(blockdev --getsize64 "${DR}p4") / 1024 ))
duel_scene "F 真机布局（1007 KiB 的 misc，介质挂着）" "$DR"
P=$(gk3_plan --disk "$DR" --mode reinstall --rescue no --esp "${DR}p1")
printf '%s\n' "$P" | grep -q "^PLAN op=reuse name=misc .*size_kib=$MK " && printf '%s\n' "$P" | grep -q '^PLANSUM mode=reinstall' \
    && ok "真机布局（misc ${MK} KiB，从第 34 扇区起）：重新安装的方案成立" || { bad "真机布局的方案不成立"; printf '%s\n' "$P" | sed 's/^/      /'; }
BEFORE_R=$(sgdisk -p "$DR" | grep -v '^Disk identifier')
gk3_apply --disk "$DR" --mode reinstall --rescue no --release "$REL" --esp "${DR}p1" >"$W/r.log" 2>&1; rc=$?
[ "$rc" = 0 ] && ok "真机布局：重新安装完成（安装器所在的 p3 一直挂着）" || { bad "真机布局的重新安装失败 rc=$rc"; tail -8 "$W/r.log" | sed 's/^/      /'; }
misc_ok "${DR}p4" none && ok "1007 KiB 的 misc：垃圾全清掉、前 64 KiB 是初始状态、之后全零（不再按 4 MiB 写爆）" || bad "misc 没清干净 / 初始状态不对"
[ "$(sgdisk -p "$DR" | grep -v '^Disk identifier')" = "$BEFORE_R" ] && findmnt -rn -S "${DR}p3" -T /media/gk3 >/dev/null \
    && ok "分区表没变，p3 还挂着" || bad "真机布局：分区表变了或 p3 被动了"
umount /media/gk3

# ── K. 双系统专项（S10 / S15）────────────────────────────────────────────────
echo "═══ K. 双系统专项：休眠 / BitLocker / 默认系统 / 菜单 5 秒 / 32 MiB 余量 / 重新安装删入口条目 ═══"
DK=$(new_disk k 40G)
TOT=$(blockdev --getsz "$DK"); LAST=$(( TOT - 34 )); BLS=$(( (LAST - 131072 + 1) / 2048 * 2048 ))
sgdisk -o \
  -n 1:2048:+300M -t 1:ef00 -c 1:"EFI system partition" \
  -n 2:0:+16M     -t 2:0c01 -c 2:"Microsoft reserved partition" \
  -n 3:0:+4G      -t 3:0700 -c 3:"Basic data partition" \
  -n 4:$BLS:$LAST -t 4:0700 -c 4:"Basic data partition" "$DK" >/dev/null 2>&1
partprobe "$DK" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 -n SYSTEM "${DK}p1" >/dev/null
mmd -i "${DK}p1" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot ::/EFI/Boot
mcopy -i "${DK}p1" "$W/bootmgfw.efi" ::/EFI/Microsoft/Boot/bootmgfw.efi
mcopy -i "${DK}p1" "$W/winfallback.efi" ::/EFI/Boot/bootaa64.efi
mkntfs -Q -F -L Windows "${DK}p3" >/dev/null 2>&1
# p4：BitLocker 卷的样子（blkid 认 TYPE=BitLocker：Win7 头 eb 58 90 "-FVE-FS-"、偏移 176 的 FVE 元数据位置、那里再一个 "-FVE-FS-"）
python3 - "${DK}p4" <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    s = bytearray(512); s[0:3] = b"\xeb\x58\x90"; s[3:11] = b"-FVE-FS-"
    struct.pack_into("<H", s, 11, 512); s[13] = 8; struct.pack_into("<Q", s, 176, 1 << 20)
    f.write(s); f.seek(1 << 20); f.write(b"-FVE-FS-" + struct.pack("<HH", 64, 2) + bytes(52))
PY
[ "$(blkid -p -o value -s TYPE "${DK}p4")" = BitLocker ] && ok "p4 被 blkid 认成 BitLocker（假的加密卷）" || bad "假 BitLocker 卷没被认出来"
# Windows 在休眠（快速启动的"关机"）：C: 根上的 hiberfil.sys 以 hibr 开头
mk=$W/mnt-k; mkdir -p "$mk"
ntfs-3g "${DK}p3" "$mk" && { printf 'hibr'; head -c 8192 /dev/urandom; } > "$mk/hiberfil.sys" && umount "$mk"
# 假 efivarfs：一个指着 live 的 LoaderEntryDefault（§4.9.3 的"不合法值"）与一个 OneShot
EFV=$W/efivars; mkdir -p "$EFV"
efw "$EFV" LoaderEntryDefault gaokun3-live.conf; efw "$EFV" LoaderEntryOneShot "*-android-a.conf"
duel_scene "K Windows + BitLocker + 休眠" "$DK"
PROBE=$(gk3_probe 2>/dev/null | awk -v d="$DK" 'index($0, "disk="d" ")')
FREE=$(printf '%s\n' "$PROBE" | grep '^FREE ' | sort -t= -k5 -n | tail -1); RS=$(gk3__f "$FREE" start); RE=$(gk3__f "$FREE" end)
kapply() { gk3_apply --disk "$DK" --mode alongside --rescue no --release "$REL" --region-start "$RS" --region-end "$RE" --esp "${DK}p1" "$@"; }
EI=$(GK3_EFIVARS=$EFV gk3_esp_info "${DK}p1")
printf '%s' "$EI" | grep -q 'windows=yes gaokun3=no' && printf '%s' "$EI" | grep -q ' bootaa64=other bitlocker=yes hibernated=yes ' \
  && printf '%s' "$EI" | grep -q ' entry=none ' && printf '%s' "$EI" | grep -q ' loader_default=gaokun3-live.conf ' \
    && ok "gk3_esp_info 事先报出：Windows 在、BOOTAA64 不是我们的、BitLocker、在休眠、LoaderEntryDefault" || bad "gk3_esp_info 不对：$EI"
BEFORE_K=$(fp "$DK")
OUT=$(GK3_EFIVARS=$EFV kapply --bitlocker-key yes 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q "^ERR code=esp-windows-hibernated part=${DK}p3 touched=no" && [ "$(fp "$DK")" = "$BEFORE_K" ] \
    && ok "U18：Windows 在休眠 ⇒ 拒绝写 ESP（esp-windows-hibernated），盘没动" || { bad "休眠没拦住（rc=${rc}）"; printf '%s\n' "$OUT" | tail -3 | sed 's/^/      /'; }
ntfs-3g "${DK}p3" "$mk" && rm -f "$mk/hiberfil.sys" && umount "$mk"
EI=$(gk3_esp_info "${DK}p1")
printf '%s' "$EI" | grep -q ' bitlocker=yes hibernated=unknown ' \
    && ok "关了休眠、但有 BitLocker 卷（里面读不到）：hibernated=unknown（只警告，不拦）" || bad "hibernated 不对：$EI"
BEFORE_K=$(fp "$DK")
OUT=$(GK3_EFIVARS=$EFV kapply 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q '^ERR code=bitlocker-key-unconfirmed touched=no' && [ "$(fp "$DK")" = "$BEFORE_K" ] \
    && ok "U16：BitLocker + 这次要换 BOOTAA64、没给 --bitlocker-key ⇒ 拒绝，盘没动" || { bad "BitLocker 没拦住（rc=${rc}）"; printf '%s\n' "$OUT" | tail -3 | sed 's/^/      /'; }
OUT=$(GK3_ESP_WIN_RESERVE_KIB=1048576 GK3_EFIVARS=$EFV kapply --bitlocker-key yes 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q '^ERR code=esp-win-reserve left_mib=[0-9-]* need_mib=1024 touched=no' && [ "$(fp "$DK")" = "$BEFORE_K" ] \
    && ok "U17：扣掉一次 OTA 之后给 Windows 留的不够（把门槛临时调到 1 GiB）⇒ esp-win-reserve，盘没动" || { bad "U17 余量没拦住（rc=${rc}）"; printf '%s\n' "$OUT" | tail -3 | sed 's/^/      /'; }
H3=$(sha "${DK}p3"); H4=$(sha "${DK}p4")
GK3_EFIVARS=$EFV kapply --bitlocker-key yes --default-os windows >"$W/k.log" 2>&1; rc=$?
if [ "$rc" = 0 ]; then ok "双系统（BitLocker 已确认、Windows 为默认）：装完"
    verify_install "$DK" no "${DK}p1" "$REL/rescue.squashfs" "$REL/initramfs.img" windows
else bad "双系统安装失败 rc=$rc"; tail -15 "$W/k.log" | sed 's/^/      /'; fi
grep -q '^U17：装完、扣掉一次 OTA 的暂时空间后还剩 [0-9]* MiB 给 Windows 与固件' "$W/k.log" && ok "U17 的余量算了一遍、记进日志" || bad "日志里没有 U17 那一行"
[ ! -e "$EFV/LoaderEntryDefault-$GK3_LOADER_GUID" ] && [ -e "$EFV/LoaderEntryOneShot-$GK3_LOADER_GUID" ] \
  && grep -q "删掉了 EFI 变量 LoaderEntryDefault（原值 'gaokun3-live.conf'）" "$W/k.log" && ! grep -q '^NOTE ' "$W/k.log" \
    && ok "LoaderEntryDefault（指着 live）删掉了；OneShot 不是整盘清空、留着；Windows 为默认【不】直接写变量（走 GK3 的 set_default）" \
    || bad "EFI 变量的处理不对：$(ls "$EFV")"
[ "$(sha "${DK}p3")" = "$H3" ] && [ "$(sha "${DK}p4")" = "$H4" ] && ok "Windows 卷（NTFS / BitLocker）逐字节未变" || bad "Windows 卷被动了"
# 重新安装：ESP 上已有统一启动入口（上一版装的系统部署的）+ 一条手放的实验条目 + 另一个目录的直连条目
mount "${DK}p1" "$mk"
EK=$mk/loader/entries
for n in gk3boot-android-a.conf gk3boot-android-b+3.conf gk3prev-android-a.conf gk3boot-android-a.conf.staged; do
    printf 'title Android\nversion gk3boot-V1\nsort-key 0gk3\nefi /EFI/gk3boot/V1/gk3boot.efi\noptions gk3.observe=0 gk3.hint=a gk3.dispatch=1\n' > "$EK/$n"
done
printf 'title Android fastboot / boot menu\nversion gk3boot-V1\nefi /EFI/gk3boot/V1/gk3boot.efi\noptions gk3.action=fastboot\n' > "$EK/gk3boot-tools.conf"
printf 'title test\nefi /EFI/gk3boot/V2/gk3boot.efi\n' > "$EK/gk3test.conf"
mkdir -p "$mk/EFI/gk3boot/V1" "$mk/EFI/gk3boot/V2" "$mk/EFI/gk3boot/log"
echo x > "$mk/EFI/gk3boot/V1/gk3boot.efi"; echo y > "$mk/EFI/gk3boot/V2/gk3boot.efi"; echo z > "$mk/EFI/gk3boot/log/one.log"
cp "$EK/$MID-android-b.conf" "$EK/$OTHER-android-b.conf"
touch -d '2001-01-01 00:00:00' "$mk/EFI/BOOT/BOOTAA64.EFI" "$mk/EFI/systemd/systemd-bootaa64.efi"
umount "$mk"
EI=$(gk3_esp_info "${DK}p1")
printf '%s' "$EI" | grep -q ' bootaa64=sdboot ' && printf '%s' "$EI" | grep -q ' entry=V1 entry_mode=action entry_state=a,b+3 staged=yes prev=yes ' \
    && ok "gk3_esp_info 报出入口：版本 V1、动作模式、条目 a / b+3、有 .staged 与 gk3prev；BOOTAA64 已是我们的" || bad "入口状态报得不对：$EI"
# EFI 变量删不掉（只读）：装照样成功，但给界面一条 NOTE
EFV2=$W/efivars-ro; mkdir -p "$EFV2"; mount -t tmpfs tmpfs "$EFV2"; efw "$EFV2" LoaderEntryDefault auto-windows; mount -o remount,ro "$EFV2"
GK3_EFIVARS=$EFV2 gk3_apply --disk "$DK" --mode reinstall --rescue no --release "$REL" --esp "${DK}p1" --keep-data yes >"$W/k2.log" 2>&1; rc=$?
if [ "$rc" = 0 ]; then ok "重新安装（BOOTAA64 已是这一份 ⇒ 不要 --bitlocker-key；默认 Android）：完成"
    verify_install "$DK" no "${DK}p1" "$REL/rescue.squashfs" "$REL/initramfs.img" android
else bad "双系统上的重新安装失败 rc=$rc"; tail -15 "$W/k2.log" | sed 's/^/      /'; fi
mount -o ro "${DK}p1" "$mk"
[ -z "$(ls "$EK" | grep -E '^gk3(boot|prev)-')" ] && ! ls "$EK" | grep -q 'gk3boot.*disabled' && [ ! -e "$mk/EFI/gk3boot/V1" ] \
  && [ -f "$mk/EFI/gk3boot/V2/gk3boot.efi" ] && [ -f "$mk/EFI/gk3boot/log/one.log" ] && [ -f "$EK/gk3test.conf" ] \
    && ok "统一启动入口的条目（现役 / +3 / gk3prev / .staged / tools）删掉了、不是改名停用；没人引用的 V1/ 删掉，手放条目引用的 V2/ 与 log/ 留着" \
    || { bad "入口条目 / 目录的处理不对"; ls "$EK" "$mk/EFI/gk3boot" | sed 's/^/      /'; }
[ -e "$EK/$OTHER-android-b.conf.disabled" ] && [ ! -e "$EK/$OTHER-android-b.conf" ] \
    && ok "另一个目录的直连条目照旧改名停用（收紧后的匹配只认 <32 位十六进制>-android-<槽>.conf）" || bad "另一个目录的直连条目没被停用"
[ "$(stat -c %Y "$mk/EFI/BOOT/BOOTAA64.EFI")" -lt 1000000000 ] && [ "$(stat -c %Y "$mk/EFI/systemd/systemd-bootaa64.efi")" -lt 1000000000 ] \
    && ok "U16：BOOTAA64.EFI / systemd-bootaa64.efi 字节相同 ⇒ 没重写（mtime 还是 2001 年）" || bad "字节相同的 BOOTAA64 被重写了"
umount "$mk"
grep -q '^NOTE code=loadervar-stuck name=LoaderEntryDefault value=auto-windows$' "$W/k2.log" \
    && ok "LoaderEntryDefault 删不掉（只读）：安装照样成功，stdout 给一条 NOTE code=loadervar-stuck" || bad "删不掉变量时没有 NOTE"
duel_scene "K 双系统重新安装完" "$DK"
umount "$EFV2"

# ── G. 手动调整磁盘 ────────────────────────────────────────────────────────
# 用户 2026-09-25："能给的都给" —— 删除 / 新建 / 格式化 / 缩小 / 扩大。每个操作都要守住：
# ESP 不动、挂着的不动、越界的不做；删除不抹数据（分区表备份还原回去就在）；扩大保住 PARTUUID 与数据。
echo "═══ G. 手动调整磁盘 ═══"
DG=$(new_disk g 16G)
sgdisk -o -n 1:2048:+300M -t 1:ef00 -c 1:"EFI system partition" -n 2:0:+3G -t 2:0700 -c 2:"Basic data partition" \
       -n 3:0:+2G -t 3:8300 -c 3:linux "$DG" >/dev/null 2>&1
partprobe "$DG" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 "${DG}p1" >/dev/null; mkntfs -Q -F -L Data "${DG}p2" >/dev/null 2>&1; mkfs.ext4 -q -F "${DG}p3"
mg=$W/mnt-g; mkdir -p "$mg"
ntfs-3g "${DG}p2" "$mg" && head -c 20971520 /dev/urandom > "$mg/win.bin" && NT_SHA=$(sha "$mg/win.bin") && umount "$mg"
PU2=$(sgdisk -i 2 "$DG" | awk '/unique GUID/{print $4}')
gfail() {   # $1=说明 $2=期望的报错片段 $3…=命令：必须失败、报对原因、分区表一个字节不变
    local what=$1 want=$2; shift 2
    local before out rc; before=$(sgdisk -p "$DG" | grep -v '^Disk identifier')
    out=$("$@" 2>&1); rc=$?
    if [ "$rc" != 0 ] && printf '%s' "$out" | grep -q "$want" && [ "$(sgdisk -p "$DG" | grep -v '^Disk identifier')" = "$before" ]; then
        ok "${what}：拒绝且分区表没动"; else bad "${what}：rc=${rc}，或报错不对，或分区表变了"; printf '%s\n' "$out" | tail -2 | sed 's/^/      /'; fi
}
gfail "删 ESP" "EFI 系统分区" gk3_part_delete "${DG}p1"
gfail "格式化 ESP" "EFI 系统分区" gk3_part_format "${DG}p1" ext4
mount "${DG}p3" "$mg"; gfail "删挂着的分区" "正挂着" gk3_part_delete "${DG}p3"; umount "$mg"
gk3_part_delete "${DG}p3" >"$W/g1.log" 2>&1 && ! [ -b "${DG}p3" ] && [ "$(sgdisk -i 2 "$DG" | awk '/unique GUID/{print $4}')" = "$PU2" ] \
    && grep -q '^RESULT op=delete' "$W/g1.log" && ok "删 p3：分区没了，别的分区 PARTUUID 没变" || { bad "删 p3 不对"; tail -3 "$W/g1.log"; }
ls /tmp/gpt-before-delete-"$(basename "$DG")"-*.bin >/dev/null 2>&1 && ok "删之前备份了分区表" || bad "删之前没备份分区表"
FREEG=$(gk3__probe_parts "$DG" "$(blockdev --getsz "$DG")" | grep '^FREE ' | head -1); FS=$(gk3__f "$FREEG" start)
gfail "新建时压到别的分区" "不在任何一段空闲区里" gk3_part_create --disk "$DG" --start 616448 --size-mib 1024 --fs ext4
OUT=$(gk3_part_create --disk "$DG" --start "$FS" --size-mib 1024 --fs ext4 2>/dev/null); NP=$(printf '%s' "$OUT" | sed -n 's/^RESULT op=create part=\([^ ]*\).*/\1/p')
[ -b "$NP" ] && [ "$(blkid -o value -s TYPE "$NP")" = ext4 ] && [ "$(( $(blockdev --getsize64 "$NP") >> 20 ))" = 1024 ] \
    && [ $(( $(sgdisk -i "$(cat /sys/class/block/$(basename "$NP")/partition)" "$DG" | awk '/^First sector:/{print $3}') % 2048 )) = 0 ] \
    && ok "在空闲区新建 1 GiB ext4：${NP}（对齐 1 MiB）" || bad "新建分区不对：$OUT"
duel_scene "G 手动新建分区之后" "$DG"
gk3_part_format "$NP" vfat >/dev/null 2>&1 && [ "$(blkid -o value -s TYPE "$NP")" = vfat ] \
    && sgdisk -i "$(cat /sys/class/block/$(basename "$NP")/partition)" "$DG" | grep -q 'EBD0A0A2' \
    && ok "格式化成 FAT32：类型也跟着改成 Basic data" || bad "格式化不对"
gk3_part_format "$NP" ext4 >/dev/null 2>&1; mount "$NP" "$mg" && head -c 10485760 /dev/urandom > "$mg/f.bin" && X_SHA=$(sha "$mg/f.bin") && umount "$mg"
PUN=$(sgdisk -i "$(cat /sys/class/block/$(basename "$NP")/partition)" "$DG" | awk '/unique GUID/{print $4}')
gk3_part_resize "$NP" 2048 >"$W/g2.log" 2>&1; rc=$?
mount -o ro "$NP" "$mg"; SZ=$(df -m "$mg" | awk 'NR==2{print $2}'); GOT=$(sha "$mg/f.bin"); umount "$mg"
[ "$rc" = 0 ] && [ "$(( $(blockdev --getsize64 "$NP") >> 20 ))" = 2048 ] && [ "$SZ" -gt 1900 ] && [ "$GOT" = "$X_SHA" ] \
    && [ "$(sgdisk -i "$(cat /sys/class/block/$(basename "$NP")/partition)" "$DG" | awk '/unique GUID/{print $4}')" = "$PUN" ] \
    && ok "扩大 ext4 1 → 2 GiB：分区与文件系统都变大（df ${SZ} MiB），文件没变，PARTUUID 没变" || { bad "扩大 ext4 不对（rc=$rc df=${SZ}）"; tail -3 "$W/g2.log"; }
gk3_part_resize "$NP" 1024 >"$W/g3.log" 2>&1 && [ "$(( $(blockdev --getsize64 "$NP") >> 20 ))" = 1024 ] \
    && mount -o ro "$NP" "$mg" && [ "$(sha "$mg/f.bin")" = "$X_SHA" ] && umount "$mg" \
    && ok "缩小 ext4 2 → 1 GiB（走 gk3_shrink）：文件没变" || { bad "缩小不对"; tail -3 "$W/g3.log"; umount "$mg" 2>/dev/null; }
# NTFS 扩大：它后面紧挨着的是新建的那个分区 —— 先删掉，腾出紧挨的空闲
gk3_part_delete "$NP" >/dev/null 2>&1
gk3_part_resize "${DG}p2" 4096 >"$W/g4.log" 2>&1; rc=$?
ntfs-3g -o ro "${DG}p2" "$mg" && GOT=$(sha "$mg/win.bin") && NSZ=$(df -m "$mg" | awk 'NR==2{print $2}') && umount "$mg"
[ "$rc" = 0 ] && [ "$(( $(blockdev --getsize64 "${DG}p2") >> 20 ))" = 4096 ] && [ "$GOT" = "$NT_SHA" ] && [ "$NSZ" -gt 3900 ] \
    && [ "$(sgdisk -i 2 "$DG" | awk '/unique GUID/{print $4}')" = "$PU2" ] \
    && ok "扩大 NTFS 3 → 4 GiB：文件没变，PARTUUID 没变（Windows 的 BCD 靠它）" || { bad "扩大 NTFS 不对（rc=$rc df=${NSZ}）"; tail -3 "$W/g4.log"; }
gfail "扩到比后面的空闲还大" "紧挨着的空闲不够" gk3_part_resize "${DG}p2" 30000

# ★ 2026-09-27 审查：partprobe 没生效（分区被占着）时内核还拿着旧分区表，同号节点指着旧起点 —— -b 看不出来
DS=$(new_disk s 2G); sgdisk -o -n 1:2048:+100M "$DS" >/dev/null 2>&1; partprobe "$DS"; sleep 1
mkfs.ext4 -q -F "${DS}p1"; ms=$W/mnt-s; mkdir -p "$ms"; mount "${DS}p1" "$ms"
sgdisk -d 1 -n 1:411648:+100M "$DS" >/dev/null 2>&1; partprobe "$DS" 2>/dev/null   # p1 挂着：内核改不了它
OUT=$(gk3__node_matches "$DS" "${DS}p1" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q '对不上' && ok "内核还拿着旧分区表：同号节点认出来是旧的（起点对不上）" || bad "旧节点没认出来（rc=${rc}）"
umount "$ms"; partprobe "$DS"; sleep 1
gk3__node_matches "$DS" "${DS}p1" 2>/dev/null && ok "卸下、partprobe 生效之后：对上了" || bad "partprobe 之后还说对不上"

# ── M. MBR 盘与读不出分区表的盘（见 docs/dev/installer-rust-design.md）──────
# 原来的 MBR 检查找 "MBR only"，gdisk 1.0.10 从不这么说 ⇒ MBR 盘上照算双系统方案，挡住写盘的是 ESP 类型检查的
# 副作用（esp-not-esp-type）。现在：方案报 mbr-disk；apply / 缩分区 / 手动调整在动盘前报 ERR mbr-disk touched=no；
# 整盘清空照常（本来就要抹掉分区表）。这块盘上故意放一个 0xEF 分区：证明拦住它的是 MBR 检查，不是 ESP 检查。
# sgdisk -p 读不出时原来把整块盘报成一段空闲。现在 DISK 标 table=unreadable、不列 PART / FREE、ERR disk-unreadable。
echo "═══ M. MBR 盘与读不出分区表的盘：动盘之前明确拒绝 ═══"
DM=$(new_disk m 40G)
printf 'label: dos\n,300M,ef\n,4G,7\n,2G,83\n' | sfdisk -q "$DM"
partprobe "$DM" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 "${DM}p1" >/dev/null; mkntfs -Q -F "${DM}p2" >/dev/null 2>&1; mkfs.ext4 -q -F "${DM}p3"
mfp() {   # MBR 盘的指纹：分区表（sfdisk -d）、PTTYPE、盘头 1 MiB、每个分区头 1 MiB
    local p; sfdisk -d "$1" 2>/dev/null; blkid -p -o value -s PTTYPE "$1"; head -c 1048576 "$1" | sha256sum
    for p in "$1"p*; do head -c 1048576 "$p" | sha256sum; done
}
BEFORE_M=$(mfp "$DM")
[ "$(blkid -p -o value -s PTTYPE "$DM")" = dos ] && [ "$(sgdisk -p "$DM" 2>&1 | grep -ci 'MBR only')" = 0 ] \
    && ok "造出来的是 dos 盘，sgdisk -p 里没有 \"MBR only\"（原来的判据永远不触发）" || bad "测试盘不对"
PM=$(gk3_probe 2>/dev/null)
FM=$(printf '%s\n' "$PM" | grep "^FREE disk=$DM " | tail -1)
printf '%s\n' "$PM" | grep -q "^DISK path=$DM .* table=mbr$" && [ -n "$FM" ] \
    && ok "探测：DISK 标 table=mbr，分区与空闲区照列（界面据此只给整盘清空）" || bad "探测里 MBR 盘没标出来：$(printf '%s\n' "$PM" | grep "^DISK path=$DM ")"
duel_scene "M MBR 盘（带 0xEF 分区）" "$DM"
mplan() { local out rc; out=$(gk3_plan "$@" 2>/dev/null); rc=$?; echo "$rc $out"; }
[ "$(mplan --disk "$DM" --mode alongside --rescue no --region-start "$(gk3__f "$FM" start)" --region-end "$(gk3__f "$FM" end)" --esp "${DM}p1")" = "1 PLANERR msg=mbr-disk" ] \
    && ok "方案（双系统）：PLANERR msg=mbr-disk" || bad "双系统方案没拦住 MBR 盘"
[ "$(mplan --disk "$DM" --mode reinstall --rescue no --esp "${DM}p1")" = "1 PLANERR msg=mbr-disk" ] \
    && ok "方案（重新安装）：PLANERR msg=mbr-disk（原来重新安装绕过这道检查）" || bad "重新安装方案没拦住 MBR 盘"
OUT=$(mplan --disk "$DM" --mode wipe --rescue no)
printf '%s\n' "$OUT" | head -1 | grep -q '^0 PLAN op=wipe ' && printf '%s\n' "$OUT" | grep -q '^PLANSUM mode=wipe ' \
    && ok "方案（整盘清空）：照常 —— MBR 盘唯一能走的路" || bad "整盘清空的方案被 MBR 检查拦了"
mrefuse() {   # $1=说明 $2=期望的 ERR 行开头 $3…=命令：必须失败、ERR 对、盘一个字节没变
    local what=$1 want=$2 out rc; shift 2
    out=$("$@" 2>&1); rc=$?
    if [ "$rc" != 0 ] && printf '%s\n' "$out" | grep -q "^$want" && [ "$(mfp "$DM")" = "$BEFORE_M" ]; then
        ok "${what}：$(printf '%s\n' "$out" | grep -m1 '^ERR ')，盘没动"
    else bad "${what}：rc=${rc}，或 ERR 不对，或盘被改了"; printf '%s\n' "$out" | grep -e '^ERR' -e '^!!' | tail -3 | sed 's/^/      /'; fi
}
MERR="ERR code=mbr-disk disk=$DM touched=no"
mrefuse "apply 双系统（ESP 类型检查之前就拦）" "$MERR" gk3_apply --disk "$DM" --mode alongside --rescue no --release "$REL" \
    --region-start "$(gk3__f "$FM" start)" --region-end "$(gk3__f "$FM" end)" --esp "${DM}p1"
mrefuse "apply 重新安装" "$MERR" gk3_apply --disk "$DM" --mode reinstall --rescue no --release "$REL" --esp "${DM}p1"
mrefuse "缩 NTFS（gk3_shrink）" "$MERR" gk3_shrink "${DM}p2" 2048
mrefuse "删分区" "$MERR" gk3_part_delete "${DM}p3"
mrefuse "格式化分区（原来先 mkfs、之后的 sgdisk -t 才失败）" "$MERR" gk3_part_format "${DM}p3" vfat
mrefuse "新建分区" "$MERR" gk3_part_create --disk "$DM" --start "$(gk3__f "$FM" start)" --size-mib 1024 --fs ext4
mrefuse "扩大分区" "$MERR" gk3_part_resize "${DM}p3" 3072
mrefuse "缩小分区" "$MERR" gk3_part_resize "${DM}p3" 1024
# 整盘清空：照常装完，盘变成 GPT
gk3_apply --disk "$DM" --mode wipe --rescue no --release "$REL" >"$W/m-wipe.log" 2>&1; rc=$?
[ "$rc" = 0 ] && [ "$(blkid -p -o value -s PTTYPE "$DM")" = gpt ] && [ "$(gk3__bylabel "$DM" super 2>/dev/null)" != "" ] \
    && gk3_probe 2>/dev/null | grep -q "^DISK path=$DM .* table=gpt$" \
    && ok "整盘清空 MBR 盘：装完，分区表换成 GPT" || { bad "整盘清空 MBR 盘不对（rc=${rc}）"; grep -e '^ERR' -e '^!!' "$W/m-wipe.log" | tail -3; }

# 读不出：① 真的读不出的设备（dm-error：每次读都 EIO）—— sgdisk 退出码 0、给一张空表，只在 stderr 上说 Read error
EIO=gk3-test-eio-$$
if dmsetup create "$EIO" --table "0 6291456 error" 2>/dev/null; then
    MM=$(dmsetup info -c --noheadings -o major,minor "$EIO" | tr -d ' '); DX=$W/eio
    mknod "$DX" b "${MM%%:*}" "${MM##*:}"
    sgdisk -p "$DX" >/dev/null 2>&1; rc=$?
    gk3__read_table "$DX"
    [ "$rc" = 0 ] && [ "$GK3__PT" = unreadable ] && printf '%s' "$GK3__PT_WHY" | grep -q 'Read error' \
        && ok "整块 EIO 的盘：sgdisk 退出码 0，仍判成 unreadable（${GK3__PT_WHY}）" || bad "整块 EIO 的盘没判成读不出（rc=${rc} PT=${GK3__PT}）"
    OUT=$(gk3_apply --disk "$DX" --mode wipe --rescue no --release "$REL" 2>&1); rc=$?
    [ "$rc" != 0 ] && printf '%s\n' "$OUT" | grep -q "^ERR code=disk-unreadable disk=$DX touched=no$" \
        && ok "apply 整盘清空一块读不出的盘：ERR disk-unreadable touched=no" || { bad "读不出的盘 apply 没拦（rc=${rc}）"; printf '%s\n' "$OUT" | grep -e '^ERR' -e '^!!' | tail -2; }
    OUT=$(gk3_part_create --disk "$DX" --start 2048 --size-mib 100 --fs ext4 2>&1); rc=$?
    [ "$rc" != 0 ] && printf '%s\n' "$OUT" | grep -q "^ERR code=disk-unreadable disk=$DX touched=no$" \
        && ok "在读不出的盘上新建分区：ERR disk-unreadable" || bad "读不出的盘上新建分区没拦（rc=${rc}）"
    rm -f "$DX"; dmsetup remove "$EIO" 2>/dev/null
else bad "建不出 dm-error 设备（容器里没有 dmsetup / device-mapper？）"; fi

# ② 探测：一块有数据的 GPT 盘读不出（假 sgdisk 照 dm-error 上实录的输出回答，其余原样转给真 sgdisk）——
#    gk3_probe 枚举 /sys/block 时跳过 dm-*，真的 dm-error 设备进不了探测，只能这样造
DN=$(new_disk n 20G)
sgdisk -o -n 1:2048:+300M -t 1:ef00 -c 1:esp -n 2:0:+4G -t 2:0700 -c 2:"Basic data partition" "$DN" >/dev/null 2>&1
partprobe "$DN" 2>/dev/null; udevadm settle 2>/dev/null; sleep 1
FB=$W/fakebin; mkdir -p "$FB"
cat > "$FB/sgdisk" <<EOF
#!/bin/bash
if [ "\$1" = -p ] && [ "\$2" = "\${GK3_TEST_EIO_DISK:-}" ]; then
    echo "Warning! Read error 5; strange behavior now likely!" >&2
    echo "Warning! Read error 5; strange behavior now likely!" >&2
    echo "Creating new GPT entries in memory."
    echo "Disk \$2: 41943040 sectors, 20.0 GiB"
    echo "First usable sector is 34, last usable sector is 41943006"
    echo; echo "Number  Start (sector)    End (sector)  Size       Code  Name"
    exit 0
fi
exec $(command -v sgdisk) "\$@"
EOF
chmod +x "$FB/sgdisk"
BEFORE_N=$(fp "$DN")
OUT=$(PATH=$FB:$PATH GK3_TEST_EIO_DISK=$DN gk3_probe 2>"$W/n.err"); rc=$?
[ "$rc" = 1 ] && printf '%s\n' "$OUT" | grep -q "^DISK path=$DN .* table=unreadable$" \
    && ! printf '%s\n' "$OUT" | grep -q -e "^FREE disk=$DN " -e "^PART path=${DN}p" \
    && grep -q "^ERR code=disk-unreadable disk=$DN touched=no$" "$W/n.err" && grep -q '^!! 读不出' "$W/n.err" \
    && printf '%s\n' "$OUT" | grep -q "^DISK path=$DM .* table=gpt$" \
    && ok "探测：读不出的盘标 table=unreadable、不报空闲、ERR disk-unreadable、退出码 1；别的盘照常列" \
    || { bad "探测把读不出的盘报错了（rc=${rc}）"; printf '%s\n' "$OUT" | grep "disk=$DN \|path=$DN"; cat "$W/n.err" | tail -3; }
duel_call "M 读不出的盘" "PATH=$FB:$PATH" "GK3_TEST_EIO_DISK=$DN" gk3_probe
OUT=$(PATH=$FB:$PATH GK3_TEST_EIO_DISK=$DN gk3_part_delete "${DN}p2" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s\n' "$OUT" | grep -q "^ERR code=disk-unreadable disk=$DN touched=no$" && [ "$(fp "$DN")" = "$BEFORE_N" ] \
    && ok "读不出的盘上删分区：ERR disk-unreadable，盘没动" || bad "读不出的盘上删分区没拦（rc=${rc}）"
OUT=$(PATH=$FB:$PATH GK3_TEST_EIO_DISK=$DN gk3_apply --disk "$DN" --mode alongside --rescue no --release "$REL" \
      --region-start 8808448 --region-end 41943006 --esp "${DN}p1" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s\n' "$OUT" | grep -q "^ERR code=disk-unreadable disk=$DN touched=no$" && [ "$(fp "$DN")" = "$BEFORE_N" ] \
    && ok "读不出的盘上装双系统：ERR disk-unreadable，盘没动" || bad "读不出的盘上装双系统没拦（rc=${rc}）"

# ── E. 网络安装 ────────────────────────────────────────────────────────────
echo "═══ E. 网络安装：下载一整套发布文件，再走同一条写盘路径 ═══"
# 迷你 HTTP 服务器：range 模式支持 "Range: bytes=N-"（R2 支持；Python 自带的 http.server 不支持）
cat > "$W/srv.py" <<'SRVEOF'
import http.server, os, sys
import time
# stall：支持 Range；不带 Range 的请求发一半就不动了（连接不断）—— 服务器活着、只是不再发数据（v1.0 计划 GUI-5）
root, port, mode, log = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
rng = mode in ("range", "stall", "e503")
seen = set()   # e503：每个安装文件的第一个请求回 503（服务器暂时出错），之后照常
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_HEAD(self):   # gk3__ota_variant 用 HEAD 量安装文件的大小（R2 支持）
        p = os.path.join(root, self.path.lstrip("/"))
        if not os.path.isfile(p): self.send_error(404); return
        self.send_response(200); self.send_header("Content-Length", str(os.path.getsize(p))); self.end_headers()
    def do_GET(self):
        p = os.path.join(root, self.path.lstrip("/"))
        if not os.path.isfile(p): self.send_error(404); return
        data = open(p, "rb").read(); start = 0
        r = self.headers.get("Range")
        open(log, "a").write("%s %s\n" % (self.path, r or "-"))
        if mode == "e503" and p.endswith((".img", ".zst")) and self.path not in seen:
            seen.add(self.path); self.send_response(503); self.send_header("Content-Length", "0"); self.end_headers(); return
        if rng and r and r.startswith("bytes="):
            start = int(r[6:].split("-")[0])
            if start >= len(data): self.send_response(416); self.end_headers(); return
            self.send_response(206); self.send_header("Content-Range", "bytes %d-%d/%d" % (start, len(data) - 1, len(data)))
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(len(data) - start)); self.end_headers()
        if mode == "stall" and not r and p.endswith((".img", ".zst")):   # 校验清单照常发
            try: self.wfile.write(data[:len(data) // 2]); self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError): pass
            time.sleep(30); return
        try: self.wfile.write(data[start:])
        except (BrokenPipeError, ConnectionResetError): pass   # curl 拿到 200 就放弃续传、主动断开
http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
SRVEOF
SRV=$W/srv; mkdir -p "$SRV/good" "$SRV/bad"
cp "$REL/boot.img" "$REL/super.img.zst" "$REL/install-artifacts.sha256" "$SRV/good/"
# 校验清单里再挂一个 OTA zip 的名字（发版的清单就是这样），好验 version= 能取出来
echo "0000000000000000000000000000000000000000000000000000000000000000  crDroidAndroid-16.0-20260916-gaokun3-v12.11.zip" >> "$SRV/good/install-artifacts.sha256"
cp "$SRV/good/"* "$SRV/bad/"; printf 'X' | dd of="$SRV/bad/boot.img" bs=1 seek=4096 conv=notrunc status=none
python3 "$W/srv.py" "$SRV" 18081 range "$W/srv-range.log" & SRVPID1=$!
python3 "$W/srv.py" "$SRV" 18082 norange "$W/srv-norange.log" & SRVPID2=$!
python3 "$W/srv.py" "$SRV" 18083 stall "$W/srv-stall.log" & SRVPID3=$!
python3 "$W/srv.py" "$SRV" 18084 e503 "$W/srv-e503.log" & SRVPID4=$!
sleep 1
RI=$(gk3_release_info "$SRV/good")
printf '%s' "$RI" | grep -q 'boot=yes super=zst sha256=yes' && printf '%s' "$RI" | grep -q 'version=crDroidAndroid-16.0-20260916-gaokun3-v12.11 ' \
    && ok "gk3_release_info：$(printf '%s' "$RI" | cut -c1-120)" || bad "gk3_release_info 不对：$RI"
DL=$W/dl; OUT=$(gk3_net_release http://127.0.0.1:18081/good/ "$DL" 2>"$W/e.err"); rc=$?
same=1; for f in boot.img super.img.zst; do [ "$(sha "$DL/$f")" = "$(sha "$REL/$f")" ] || same=0; done
[ "$rc" = 0 ] && [ "$same" = 1 ] && printf '%s' "$OUT" | grep -q '^RELEASE .*source=net' \
    && ok "gk3_net_release：两个文件逐字节一致、打出 RELEASE 记录" || { bad "网络下载 rc=$rc"; tail -5 "$W/e.err"; }
P=$(awk '$1=="PROGRESS"{print $2}' "$W/e.err" | tr '\n' ' ')
printf '%s\n' $P | awk 'NR>1 && $1<prev{bad=1} {prev=$1} END{exit bad}' && [ "$(printf '%s\n' $P | tail -1)" = 100 ] \
    && ok "进度单调、走到 100（$(printf '%s\n' $P | wc -l | tr -d ' ') 行）" || bad "进度不对：$P"
# 断点续传：目标目录里先放半截 super，看它是不是真的发了 Range
DL2=$W/dl2; mkdir -p "$DL2"; head -c $(( $(stat -c%s "$REL/super.img.zst") / 3 )) "$REL/super.img.zst" > "$DL2/super.img.zst"
: > "$W/srv-range.log"
gk3_net_release http://127.0.0.1:18081/good/ "$DL2" >/dev/null 2>&1; rc=$?
[ "$rc" = 0 ] && [ "$(sha "$DL2/super.img.zst")" = "$(sha "$REL/super.img.zst")" ] && grep -q '^/good/super.img.zst bytes=[1-9]' "$W/srv-range.log" \
    && ok "断点续传：发了 $(grep '^/good/super.img.zst' "$W/srv-range.log" | cut -d' ' -f2)，续完 sha256 一致" || bad "续传不对（rc=${rc}）：$(tr '\n' ' ' < "$W/srv-range.log")"
# 服务器不支持 Range：半截文件必须被丢掉重下，而不是永远卡在 curl 的 33 上
DL3=$W/dl3; mkdir -p "$DL3"; head -c 12345 "$REL/super.img.zst" > "$DL3/super.img.zst"
gk3_net_release http://127.0.0.1:18082/good/ "$DL3" >/dev/null 2>"$W/e3.err"; rc=$?
[ "$rc" = 0 ] && [ "$(sha "$DL3/super.img.zst")" = "$(sha "$REL/super.img.zst")" ] && grep -q '不支持断点续传' "$W/e3.err" \
    && ok "服务器不支持续传：丢掉半截、从头下完，sha256 一致" || { bad "无 Range 服务器时 rc=$rc"; tail -3 "$W/e3.err"; }
# ★ 2026-09-27 审查：同一个下载目录里先下完 A、再换 B（同一次会话里换版本再装）—— boot.img 一样大，续传 416；
#   super 的新内容接在 A 的前缀后面，sha256 永远对不上。现在：不符就删掉，重试从头下
mkdir -p "$SRV/alt"; cp "$SRV/good/boot.img" "$SRV/alt/"
{ cat "$SRV/good/super.img.zst"; head -c 1048576 /dev/urandom; } > "$SRV/alt/super.img.zst"
# 开头也不同：续传接上的前缀是错的。⚠️ 改成"原字节 +1"，不写死 'Z'：super.img.zst 是随机数据压出来的，
#   第 100 字节恰好是 'Z' 时两份开头一样、续传反而对了（约 1/256）。2026-10-04 有过一次 rc1=0 的偶发失败，这是最说得通的解释（未确证）
B100=$(od -An -tu1 -j100 -N1 "$SRV/alt/super.img.zst" | tr -d ' ')
printf "\\$(printf %o $(( (B100 + 1) % 256 )))" | dd of="$SRV/alt/super.img.zst" bs=1 seek=100 conv=notrunc status=none
( cd "$SRV/alt" && sha256sum boot.img super.img.zst > install-artifacts.sha256 )
# ★ GUI 审查 2026-10-05：下载失败时半截文件是故意留着的、失败页又给"返回修改、换版本" —— 换了版本时 gk3_net_release
#   先比两份校验清单，sha256 变了的文件直接丢掉，第一次就下对（原先第一次必然"sha256 不符"、要再点一次重试）
DL6=$W/dl6; gk3_net_release http://127.0.0.1:18081/good/ "$DL6" >/dev/null 2>&1
head -c 300000 "$DL6/super.img.zst" > "$DL6/s.part" && mv "$DL6/s.part" "$DL6/super.img.zst"   # A 的 super 只下了一半
gk3_net_release http://127.0.0.1:18081/alt/ "$DL6" >/dev/null 2>"$W/e6.err"; rc1=$?
[ "$rc1" = 0 ] && [ "$(sha "$DL6/super.img.zst")" = "$(sha "$SRV/alt/super.img.zst")" ] && grep -q '换了版本：上一次留下的 super.img.zst' "$W/e6.err" \
  && ! grep -q '换了版本：上一次留下的 boot.img' "$W/e6.err" \
    && ok "同一目录换版本：sha256 变了的 super 先丢掉、第一次就下对；没变的 boot.img 留着" || { bad "换版本没有一次下对（rc1=${rc1}）"; tail -3 "$W/e6.err"; }
# 清单没变、本地的半截是坏的（前缀被改过）：续传后 sha256 不符 → 删掉，重试从头下、这次对了
DL6B=$W/dl6b; gk3_net_release http://127.0.0.1:18081/good/ "$DL6B" >/dev/null 2>&1
head -c 300000 "$DL6B/super.img.zst" > "$DL6B/s.part" && mv "$DL6B/s.part" "$DL6B/super.img.zst"
B100=$(od -An -tu1 -j100 -N1 "$DL6B/super.img.zst" | tr -d ' ')
printf "\\$(printf %o $(( (B100 + 1) % 256 )))" | dd of="$DL6B/super.img.zst" bs=1 seek=100 conv=notrunc status=none
gk3_net_release http://127.0.0.1:18081/good/ "$DL6B" >/dev/null 2>"$W/e6b.err"; rc1=$?
gk3_net_release http://127.0.0.1:18081/good/ "$DL6B" >/dev/null 2>&1; rc2=$?
[ "$rc1" != 0 ] && grep -q 'super.img.zst 的 sha256 不符.*已删掉' "$W/e6b.err" && [ "$rc2" = 0 ] && [ "$(sha "$DL6B/super.img.zst")" = "$(sha "$REL/super.img.zst")" ] \
    && ok "本地半截是坏的：第一次 sha256 不符并删掉，重试从头下、这次对了" || bad "坏的半截卡住了（rc1=$rc1 rc2=${rc2}）"
# ★ GUI 审查 2026-10-05：服务器暂时回 503（R2 / CDN 偶尔会）—— 原先 curl --retry 3 会重试，去掉它之后要由外层循环接住；
#   续传中途的 503 不能把半截文件交给 sha256 删掉
DL9=$W/dl9; mkdir -p "$DL9"; head -c 300000 "$REL/super.img.zst" > "$DL9/super.img.zst"; : > "$W/srv-e503.log"
GK3_NET_RETRY_DELAY=0 gk3_net_release http://127.0.0.1:18084/good/ "$DL9" >/dev/null 2>"$W/e9.err"; rc=$?
[ "$rc" = 0 ] && [ "$(sha "$DL9/super.img.zst")" = "$(sha "$REL/super.img.zst")" ] && grep -q 'super.img.zst 中断（curl 退出码 22，HTTP 503）' "$W/e9.err" \
  && [ "$(grep -c '^/good/super.img.zst bytes=300000-' "$W/srv-e503.log")" = 2 ] \
    && ok "服务器暂时 503：重试、从第 300000 字节接着下，sha256 一致" || { bad "503 没被当成暂时的错误（rc=${rc}）"; tail -3 "$W/e9.err"; cat "$W/srv-e503.log"; }
mkdir -p "$SRV/missing"; cp "$SRV/good/install-artifacts.sha256" "$SRV/missing/"   # 清单在、文件不在
OUT=$(GK3_NET_RETRY_DELAY=0 gk3_net_release http://127.0.0.1:18081/missing/ "$W/dl10" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q '下载失败（curl 退出码 22）' && ! printf '%s' "$OUT" | grep -q '接着下' \
    && ok "清单里有、服务器上没有（404）：不重试，直接报失败" || bad "404 的处理不对（rc=${rc}）：$(printf '%s' "$OUT" | tail -2)"
# ★ v1.0 计划 GUI-5：服务器发了一半就不动了（连接还在）。原先 curl 只有 --retry 3，这种停滞永远不超时、进度条一直停着。
#   现在：停滞判死（这里压到 2 秒）→ 按已有长度续传重试 → 下完、sha256 一致
DL7=$W/dl7; : > "$W/srv-stall.log"; T0=$(date +%s)
GK3_NET_SPEED_TIME=2 GK3_NET_RETRY_DELAY=0 gk3_net_release http://127.0.0.1:18083/good/ "$DL7" >/dev/null 2>"$W/e7.err"; rc=$?
[ "$rc" = 0 ] && [ "$(sha "$DL7/super.img.zst")" = "$(sha "$REL/super.img.zst")" ] && grep -q '接着下（第 2/5 次）' "$W/e7.err" \
  && grep -q '^/good/super.img.zst bytes=[1-9]' "$W/srv-stall.log" \
    && ok "下载停滞：$(( $(date +%s) - T0 )) 秒内判死、续传重试、下完 sha256 一致" || { bad "停滞的下载没恢复（rc=${rc}）"; tail -3 "$W/e7.err"; }
# 重试有上限；用完了报失败，但半截文件【留着】（界面上的"重试"要接着它续传，v1.0 计划 GUI-3）
DL8=$W/dl8
OUT=$(GK3_NET_TRIES=1 GK3_NET_SPEED_TIME=2 gk3_net_release http://127.0.0.1:18083/good/ "$DL8" 2>&1); rc=$?
HALF=$(stat -c%s "$DL8/boot.img" 2>/dev/null || echo 0)
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q 'boot.img 没完成（curl 退出码 28，试了 1 次）' && [ "$HALF" -gt 0 ] \
    && printf '%s' "$OUT" | grep -q '^ERR code=dl-incomplete name=boot.img rc=28 http=[0-9]* tries=1 kept_mib=0$' \
    && ok "重试次数用完：报失败（退出码 28），半截的 boot.img（${HALF} 字节）留着" || bad "重试上限不对（rc=${rc}、半截 ${HALF} 字节）：$(printf '%s' "$OUT" | tail -2)"
: > "$W/srv-stall.log"
GK3_NET_SPEED_TIME=2 GK3_NET_RETRY_DELAY=0 gk3_net_release http://127.0.0.1:18083/good/ "$DL8" >/dev/null 2>&1; rc=$?
[ "$rc" = 0 ] && [ "$(sha "$DL8/boot.img")" = "$(sha "$REL/boot.img")" ] && grep -q "^/good/boot.img bytes=$HALF-" "$W/srv-stall.log" \
    && ok "失败之后再来一次（界面上的重试）：boot.img 从第 $HALF 字节接着下，sha256 一致" || bad "重试没有接着半截续传（rc=${rc}）：$(tr '\n' ' ' < "$W/srv-stall.log")"
OUT=$(gk3_net_release http://127.0.0.1:18081/bad/ "$W/dl4" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q 'boot.img 的 sha256 不符' && printf '%s' "$OUT" | grep -q '^ERR code=dl-sha256 name=boot.img$' \
    && ok "服务器上的 boot.img 被改过：拒绝" || bad "被改过的文件居然通过了（rc=${rc}）"
# ★ 2026-09-27 真机：下载 1.2 GiB 的两分半里进度一行没出（tr / mawk 往管道攒块）。按 curl 的样子喂：表头带 \n、
#   每次刷新 "\r<一行>"、同一个百分比重复、最后一行 \n 结尾 —— 第一行进度必须在下一次刷新之前就出来
curl_like() {
    printf '  %% Total    %% Received %% Xferd  Average Speed   Time\n                                 Dload  Upload   Total\n' >&2
    for p in 0 10 10 20; do printf '\r %3d  1221M  %3d  122M    0     0  9000k      0  0:02:18 --:--:--  0:02:18 9000k' "$p" "$p" >&2; sleep 1; done
    printf '\r100  1221M  100 1221M    0     0  9000k      0  0:02:18  0:02:18 --:--:-- 9000k\n' >&2
}
T0=$(date +%s%N)
curl_like 2>&1 | gk3__curl_meter 5 95 super.img.zst | while read -r l; do echo "$(( ($(date +%s%N) - T0) / 1000000 )) $l"; done > "$W/meter.out"
FIRST=$(head -1 "$W/meter.out" | cut -d' ' -f1)
# GUI-5：速度与剩余时间取 curl 的最后两列；剩余时间是 --:--:-- 时（结尾那行）不带 left=
[ "${FIRST:-99999}" -lt 3000 ] && [ "$(cut -d' ' -f2- "$W/meter.out" | tr '\n' '|')" = "PROGRESS 13 dl name=super.img.zst pct=10 speed=9000k left=0:02:18|PROGRESS 22 dl name=super.img.zst pct=20 speed=9000k left=0:02:18|PROGRESS 90 dl name=super.img.zst pct=100 speed=9000k|" ] \
    && ok "下载进度边下边出（第一行 ${FIRST} ms，不等 curl 结束）；重复的百分比只出一次；表头与结尾那行都认对；带速度与剩余时间" \
    || { bad "进度过滤不对（第一行 ${FIRST:-?} ms）"; sed 's/^/      /' "$W/meter.out"; }
# 版本列表：variants.txt 还没发布（真机上 404）→ 退回 OTA 清单，推出"最新发布"，base 指向 install/<zip 名>/
ZN=crDroidAndroid-16.0-20260916-gaokun3-v12.11
mkdir -p "$SRV/ota" "$SRV/install/$ZN"; cp "$SRV/good/"* "$SRV/install/$ZN/"
printf '{"response":[{"filename":"%s.zip","download":"http://127.0.0.1:18081/builds/%s.zip","version":"12.11"}]}' "$ZN" "$ZN" > "$SRV/ota/gaokun3.json"
VM=$(GK3_MANIFEST_URL=http://127.0.0.1:18081/installer/variants.txt GK3_OTA_JSON_URL=http://127.0.0.1:18081/ota/gaokun3.json gk3_net_manifest 2>"$W/vm.err"); rc=$?
WANT_MIB=$(( ( $(stat -c%s "$REL/boot.img") + $(stat -c%s "$REL/super.img.zst") ) / 1048576 ))
[ "$rc" = 0 ] && printf '%s' "$VM" | grep -q "^VARIANT id=latest name=crDroid%2012.11 .*base=http://127.0.0.1:18081/install/$ZN/ size_mib=$WANT_MIB latest=yes" \
    && ok "版本清单 404 → 退回 OTA 清单：最新发布 v12.11，base 与大小（${WANT_MIB} MiB）都对" || { bad "OTA 退回不对（rc=${rc}）：$VM"; tail -3 "$W/vm.err"; }
BASE=$(printf '%s' "$VM" | sed -n 's/.* base=\([^ ]*\).*/\1/p'); DL5=$W/dl5
gk3_net_release "$BASE" "$DL5" >/dev/null 2>&1 && [ "$(sha "$DL5/super.img.zst")" = "$(sha "$REL/super.img.zst")" ] \
    && ok "推出来的 base 能直接交给 gk3_net_release 下载（sha256 一致）" || bad "推出来的 base 下载不了"
OUT=$(GK3_MANIFEST_URL=http://127.0.0.1:18081/nope GK3_OTA_JSON_URL=http://127.0.0.1:18081/nope2 gk3_net_manifest 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q '都不可用' && printf '%s' "$OUT" | grep -q '^ERR code=manifest-unavailable$' \
    && ok "两份清单都取不到：报出两个地址（ERR code=manifest-unavailable）" || bad "两份都取不到时报错不对：$OUT"
# 介质上的变体清单（局域网镜像 / 自建源 / 离线）：先列它；线上两份都取不到时有它就够了
printf 'VARIANT id=lan name=%s desc= base=http://127.0.0.1:18081/install/%s/ size_mib=1\n' "LAN%20mirror" "$ZN" > "$W/local-variants.txt"
VM=$(GK3_LOCAL_MANIFEST=$W/local-variants.txt GK3_MANIFEST_URL=http://127.0.0.1:18081/nope GK3_OTA_JSON_URL=http://127.0.0.1:18081/nope2 gk3_net_manifest 2>/dev/null); rc=$?
[ "$rc" = 0 ] && [ "$(printf '%s\n' "$VM" | grep -c '^VARIANT ')" = 1 ] && printf '%s' "$VM" | grep -q '^VARIANT id=lan ' \
    && ok "介质上有变体清单、线上两份都取不到：照样列出介质上的那个（rc=0）" || bad "介质清单没顶上（rc=${rc}）：$VM"
VM=$(GK3_LOCAL_MANIFEST=$W/local-variants.txt GK3_MANIFEST_URL=http://127.0.0.1:18081/installer/variants.txt GK3_OTA_JSON_URL=http://127.0.0.1:18081/ota/gaokun3.json gk3_net_manifest 2>/dev/null); rc=$?
[ "$rc" = 0 ] && [ "$(printf '%s\n' "$VM" | head -1 | cut -d' ' -f2)" = id=lan ] && printf '%s' "$VM" | grep -q '^VARIANT id=latest ' \
    && ok "介质清单与线上的都有：介质的排在前面，线上的最新发布也在" || bad "合并顺序不对（rc=${rc}）：$VM"
kill $SRVPID1 $SRVPID2 $SRVPID3 $SRVPID4 2>/dev/null
# 下载下来的目录交给 gk3_apply —— 网络安装与 U 盘安装是同一条写盘路径
DN=$(new_disk n 40G); sgdisk -o "$DN" >/dev/null 2>&1
EFVN=$W/efivars-n; mkdir -p "$EFVN"; efw "$EFVN" LoaderEntryDefault gk3boot-android-a.conf; efw "$EFVN" LoaderEntryOneShot auto-windows
GK3_EFIVARS=$EFVN gk3_apply --disk "$DN" --mode wipe --rescue no --release "$DL" >"$W/n.log" 2>&1; rc=$?
[ "$rc" = 0 ] && [ "$(sha_head "$(gk3__bylabel "$DN" super)" "$RAWSZ")" = "$(sha "$W/expect/super.raw")" ] \
    && ok "用下载下来的目录真装一遍：成功，super 逐字节正确" || { bad "网络安装的 apply 失败 rc=$rc"; tail -5 "$W/n.log"; }
[ -z "$(ls -A "$EFVN")" ] && misc_ok "$(gk3__bylabel "$DN" misc)" none \
    && ok "整盘清空：残留的 LoaderEntryDefault（Android 条目的精确 id）与 LoaderEntryOneShot 都删了；misc 是初始状态" || bad "整盘清空后 EFI 变量还在：$(ls "$EFVN")"

# ── J. 写盘进程脱离界面（v1.0 计划 GUI-11）──────────────────────────────────
echo "═══ J. gk3_job_run：写盘放进独立单元，界面死了它照样写完 ═══"
export GK3_JOBDIR=$W/jobs GK3_JOB_POLL=0.1
# 启动介质（ESP 类型的 FAT，像 U 盘那样）：job 结束时要把输出存一份到它的 gaokun3/diag/
DM=$(new_disk m 1G); sgdisk -o -n 1:2048:0 -t 1:ef00 -c 1:esp "$DM" >/dev/null 2>&1; partprobe "$DM"; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 -n GK3LIVE "${DM}p1" >/dev/null; mkdir -p /media/gk3; mount "${DM}p1" /media/gk3; mkdir -p /media/gk3/gaokun3/diag
echo "安装器会话日志" > /media/gk3/gaokun3/diag/installer.log; mount -o remount,ro /media/gk3
WANT=$(gk3_release_info "$REL" 2>/dev/null)
GOT=$(gk3_job_run gk3_release_info "$REL" 2>"$W/j1.err"); rc=$?
[ "$rc" = 0 ] && [ "$GOT" = "$WANT" ] && grep -q '^JOB id=.* mode=setsid ' "$W/j1.err" \
    && ok "gk3_job_run gk3_release_info：stdout 与直接调用逐行相同、退出码 0（容器里没有 systemd ⇒ setsid）" || { bad "job 的输出不对（rc=${rc}）：$GOT"; tail -3 "$W/j1.err"; }
gk3_apply --disk /dev/nonexistent >/dev/null 2>"$W/j2a.err"; rc0=$?
gk3_job_run gk3_apply --disk /dev/nonexistent >/dev/null 2>"$W/j2.err"; rc=$?
[ "$rc" = "$rc0" ] && [ "$rc" != 0 ] && grep -q "$(grep '^!!' "$W/j2a.err" | head -1)" "$W/j2.err" \
    && ok "失败的调用：退出码（${rc}）与 !! 那行与直接调用相同" || bad "失败的调用转发得不对（rc=${rc}，直接调用 rc=${rc0}）"
# 界面崩了：跟读的那个进程被 kill -9，job 照样跑完；重新起来的界面按 id 接着跟，从头拿到全部输出与退出码
( gk3_job_run bash -c 'echo "REC a=1"; echo "PROGRESS 10 写一半" >&2; sleep 3; echo "REC b=2"; exit 3' >"$W/j3.out" 2>"$W/j3.err" ) & FP=$!
sleep 1.5; kill -9 $FP 2>/dev/null; wait $FP 2>/dev/null
JID=$(sed -n 's/^JOB id=\([^ ]*\).*/\1/p' "$W/j3.err" | head -1)
grep -q '^REC a=1$' "$W/j3.out" && [ "$(gk3_job_status "$JID" | sed -n 's/.* state=\([a-z]*\).*/\1/p')" = running ] \
    && ok "跟读的进程（界面）被杀时：已经转发了前半段，job 还在跑（state=running）" || bad "界面被杀前后的状态不对：$(gk3_job_status "$JID")"
for _ in $(seq 50); do [ -f "$GK3_JOBDIR/$JID/rc" ] && break; sleep 0.2; done
OUT=$(gk3_job_follow "$JID" 2>"$W/j3b.err"); rc=$?
[ "$rc" = 3 ] && [ "$OUT" = "$(printf 'REC a=1\nREC b=2')" ] && grep -q '^PROGRESS 10 写一半$' "$W/j3b.err" \
    && gk3_job_status "$JID" | grep -q ' state=done rc=3 ' \
    && ok "界面重新起来后 gk3_job_follow：拿到全部输出（含被杀之后才写的 REC b=2）、退出码 3、状态 done" || { bad "重新跟读不对（rc=${rc}）：$OUT"; gk3_job_status "$JID"; }
grep -q '^REC b=2$' "/media/gk3/gaokun3/diag/job-$JID.log" && findmnt -rno OPTIONS /media/gk3 | grep -q '^ro' \
    && ok "job 的输出存了一份到介质 gaokun3/diag/job-<id>.log，介质改回只读" || bad "介质上没有 job 的日志，或没改回只读"
# 有 systemd 时走 systemd-run：参数（独立单元、--collect、Type=exec、GK3_* 环境、工作目录）
cat > "$W/fake-systemd-run" <<'FSR'
#!/bin/bash
printf '%s\n' "$@" > "$GK3_TEST_SRLOG"
while [ $# -gt 0 ]; do case "$1" in --*) shift ;; *) break ;; esac; done
setsid -f "$@" </dev/null >/dev/null 2>&1
FSR
chmod +x "$W/fake-systemd-run"
GOT=$(cd "$W" && GK3_TEST_SRLOG=$W/sr.args GK3_SYSTEMD_RUN=$W/fake-systemd-run gk3_job_run gk3_release_info "$REL" 2>"$W/j4.err"); rc=$?
[ "$rc" = 0 ] && [ "$GOT" = "$WANT" ] && grep -q '^JOB id=.* mode=systemd-run unit=gk3-job-' "$W/j4.err" \
  && grep -qx -- '--collect' "$W/sr.args" && grep -qx -- '--expand-environment=no' "$W/sr.args" \
  && grep -qx -- '--service-type=exec' "$W/sr.args" && grep -q -- '^--unit=gk3-job-' "$W/sr.args" \
  && grep -qx -- '--setenv=GK3_MACHINE_ID' "$W/sr.args" && grep -qx -- "--working-directory=$W" "$W/sr.args" \
    && ok "有 systemd 时：systemd-run --unit=gk3-job-… --collect --service-type=exec，GK3_* 用 --setenv 带过去，工作目录带过去；输出照样一致" \
    || { bad "systemd-run 那条路不对（rc=${rc}）"; sed 's/^/      /' "$W/sr.args"; }
# job 没写状态就没了（单元被杀 / 机器出错）：follow 不能永远等
mkdir -p "$GK3_JOBDIR/lost-1"; echo gk3_apply > "$GK3_JOBDIR/lost-1/fn"; date +%s > "$GK3_JOBDIR/lost-1/started"
bash -c 'exit 0' & wait $!; echo $! > "$GK3_JOBDIR/lost-1/pid"
OUT=$(gk3_job_follow lost-1 2>&1); rc=$?
[ "$rc" = 125 ] && printf '%s' "$OUT" | grep -q '没写完成状态就没了' && gk3_job_status lost-1 | grep -q ' state=lost ' \
    && printf '%s' "$OUT" | grep -q '^ERR code=job-lost id=lost-1 touched=yes$' \
    && ok "进程没了又没写状态：follow 返回 125 并说明，状态 lost" || bad "lost 的处理不对（rc=${rc}）：$OUT"

# ── L. 失败时把日志另存到用户拿得到的地方（v1.0 计划 GUI-12 的后端）─────────────────
echo "═══ L. gk3_save_logs ═══"
DLG=$(new_disk l 2G); sgdisk -o -n 1:2048:+512M -t 1:0700 -c 1:"Basic data partition" -n 2:0:+512M -t 2:8300 "$DLG" >/dev/null 2>&1
partprobe "$DLG"; udevadm settle 2>/dev/null; sleep 1
mkfs.vfat -F 32 -n STICK "${DLG}p1" >/dev/null; mkfs.ext4 -q -F "${DLG}p2"
TG=$(gk3_log_targets)
printf '%s\n' "$TG" | grep -q "^LOGTARGET part=${DLG}p1 fs=vfat .*removable=yes medium=no esp=no" \
  && ! printf '%s\n' "$TG" | grep -q "part=${DLG}p2 " \
  && [ "$(printf '%s\n' "$TG" | tail -1 | cut -d' ' -f2)" = "part=${DM}p1" ] && printf '%s\n' "$TG" | tail -1 | grep -q 'medium=yes esp=yes' \
    && ok "gk3_log_targets：另插的 FAT 盘在前、ext4 不列、启动介质（ESP 类型）排最后并标 esp=yes" || { bad "gk3_log_targets 不对"; printf '%s\n' "$TG" | sed 's/^/      /'; }
OUT=$(gk3_save_logs "${DLG}p1" 2>&1); rc=$?
mk=$W/mnt-l; mkdir -p "$mk"; mount -o ro "${DLG}p1" "$mk"
LD=$(ls -d "$mk"/gaokun3-logs-* 2>/dev/null | head -1)
[ "$rc" = 0 ] && printf '%s' "$OUT" | grep -q "^LOGSAVED dir=gaokun3-logs-[0-9-]* part=${DLG}p1 files=[1-9][0-9]* esp=no" \
  && [ -s "$LD/dmesg.txt" ] && [ -s "$LD/probe.txt" ] && grep -q '安装器会话日志' "$LD/diag-installer.log" \
  && grep -q 'REC b=2' "$LD/jobs/$JID.log" && ! ls -R "$LD" | grep -q wpa_supplicant \
    && ok "存到另插的 U 盘：dmesg、磁盘探测、会话日志、各 job 的输出都在（$(printf '%s' "$OUT" | sed -n 's/.*files=\([0-9]*\).*/\1/p') 个），没带 WiFi 配置" \
    || { bad "存日志不对（rc=${rc}）：$OUT"; ls -R "$mk" | head -20; }
umount "$mk"
findmnt -rn -S "${DLG}p1" >/dev/null && bad "存完没卸下 ${DLG}p1" || ok "存完卸下了（不留挂载）"
OUT=$(gk3_save_logs "${DLG}p2" 2>&1); rc=$?
[ "$rc" != 0 ] && printf '%s' "$OUT" | grep -q '不是 FAT' && ok "ext4 分区：拒绝（只往 FAT / exFAT 写）" || bad "往 ext4 写日志没被拒（rc=${rc}）"
mkdir -p "$W/logdir"; OUT=$(gk3_save_logs "$W/logdir" 2>&1); rc=$?
[ "$rc" = 0 ] && ls -d "$W"/logdir/gaokun3-logs-* >/dev/null 2>&1 && printf '%s' "$OUT" | grep -q ' part=- ' \
    && ok "给目录：直接写进去（part=-）" || bad "写进目录不对（rc=${rc}）：$OUT"
OUT=$(gk3_save_logs "${DM}p1" 2>&1); rc=$?
[ "$rc" = 0 ] && printf '%s' "$OUT" | grep -q 'esp=yes' && findmnt -rno OPTIONS /media/gk3 | grep -q '^ro' \
    && ok "存到启动介质本身：esp=yes（电脑上不好读，界面要说一句），介质改回只读" || bad "存到介质不对（rc=${rc}）：$OUT"
umount /media/gk3; unset GK3_JOBDIR GK3_JOB_POLL

# ── H. 预检的电量（v1.0 计划 GUI-8）：假的 /sys/class/power_supply ─────────────
echo "═══ H. 预检：电量 <15% 且没接电源才拦 ═══"
PS=$W/ps
mkps() {   # mkps <名字> <type> [capacity] [online] [status]
    mkdir -p "$PS/$1"; echo "$2" > "$PS/$1/type"
    [ -z "${3:-}" ] || echo "$3" > "$PS/$1/capacity"
    [ -z "${4:-}" ] || echo "$4" > "$PS/$1/online"
    [ -z "${5:-}" ] || echo "$5" > "$PS/$1/status"
}
pc() { GK3_POWER_SUPPLY_DIR=$PS gk3__power_check; }
rm -rf "$PS"; mkps gaokun-ec-battery Battery 12 "" Discharging; mkps gaokun-ec-adapter USB "" 0
[ "$(pc)" = "CHECK id=power ok=no value=12 ac=no min=15" ] && ok "12%、没接电源：ok=no" || bad "低电量没拦：$(pc)"
duel_call "H 低电量" "GK3_POWER_SUPPLY_DIR=$PS" gk3_preflight
echo 1 > "$PS/gaokun-ec-adapter/online"
[ "$(pc)" = "CHECK id=power ok=yes value=12 ac=yes min=15" ] && ok "12%、接着电源：放行" || bad "接着电源还拦：$(pc)"
echo 0 > "$PS/gaokun-ec-adapter/online"; echo 15 > "$PS/gaokun-ec-battery/capacity"
[ "$(pc)" = "CHECK id=power ok=yes value=15 ac=no min=15" ] && ok "正好 15%：放行" || bad "15% 被拦：$(pc)"
rm -rf "$PS"; mkps BAT0 Battery 5 "" Charging
[ "$(pc)" = "CHECK id=power ok=yes value=5 ac=yes min=15" ] && ok "认不出名字时按 type 找电池；status=Charging 算接着电源" || bad "按 type 找不对：$(pc)"
duel_call "H 按 type 找电池" "GK3_POWER_SUPPLY_DIR=$PS" gk3_preflight
rm -rf "$PS"; mkdir -p "$PS"
[ "$(pc)" = "CHECK id=power ok=unknown value= ac=no min=15" ] && ok "没有电池：ok=unknown（不拦）" || bad "没电池时：$(pc)"
gk3_preflight 2>/dev/null | grep -q '^CHECK id=power ' && ok "gk3_preflight 带上了 CHECK id=power" || bad "gk3_preflight 里没有 power"

echo
duel_summary || FAIL=$((FAIL + DUEL_FAIL))
echo "═══ 通过 $PASS · 失败 $FAIL ═══"
[ "$FAIL" -eq 0 ]

