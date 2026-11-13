#!/bin/bash
# OTA postinstall（device/huawei/gaokun3/bin/gaokun3-ota-postinstall.sh）的离线测试：选直连条目（OTA-9 + S9 的排除规则）
# 与统一启动入口一节（gk3_deploy）；T 组：双系统的 loader.conf timeout（U13）。
#
#   bash tools/gk3boot/test/postinstall/run.sh     （或 make -C tools/gk3boot postinstall-test）
#
# 做法（harness.sh）：脚本原样拷出来，只把 /dev/block/by-name/、/mnt/gaokun3_ota_esp、/mnt/gaokun3_esp_probe 换成测试目录；
# mount / umount / getprop / df / stat / sync / toybox 用桩（mount 不真挂，ESP 就是个目录），解包器换成写死内容的桩。
# 每个场景用 mksh（设备上的 /system/bin/sh 是 mksh）、dash、ksh 各跑一遍 —— 找不到的 shell 跳过（brew install mksh）。
# GK3_PI_SHELLS="…" 换一组（可以是绝对路径，例如一个把脚本交给容器里 busybox sh 的包装脚本）。
# 最后（hal-test 编过的话）把 postinstall 第一次部署写出的 ESP 交给 HAL 的开机完成逻辑：同版本同模式应零写入
# （= 两边条目正文逐字节一致、"是否已是想要的样子"判据一致）。交叉 2 在 action + 执行端下再做一遍
# （gk3boot-tools.conf 与 fastboot.img 也要两边一致）。F1–F9：执行端与 tools 条目（GK3_PI_FAIL_CP 模拟写失败）。
#
# ⚠️ 测的是 shell 逻辑：真 vfat、toybox 的各命令细节、SELinux（postinstall 跑在旧槽策略下）要上机看。
set -u
G=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
H=$G/test/postinstall
export GK3_PI_WORK=$G/build/postinstall-test
P=$GK3_PI_WORK; rm -rf "$P"; mkdir -p "$P"
EFI=$P/fake.efi; { printf 'MZ'; head -c 3000 /dev/zero | tr '\0' x; } > "$EFI"
# 执行端 fastboot.img 的替身：gzip 魔数开头（HAL 要认）；FBBIG 用来测空间账（约 3000 KB）
FB=$P/fake-fastboot.img; { printf '\037\213\010'; head -c 65536 /dev/zero | tr '\0' f; } > "$FB"
FBBIG=$P/fake-fastboot-big.img; { printf '\037\213\010'; head -c 3072000 /dev/zero | tr '\0' g; } > "$FBBIG"
TOOLS_V() { printf 'title      Android fastboot / boot menu\nversion    gk3boot-%s\nsort-key   0gk3tools\nefi        /EFI/gk3boot/%s/gk3boot.efi\noptions    gk3.action=fastboot' "$1" "$1"; }
V1=0.9.0-test.g0123456789ab
MID=8a29534fa802480d9fbb71aa18c01d7b
E=$P/run/esp/loader/entries
PASS=0; FAIL=0
ok()  { echo "    ✓ $*"; PASS=$((PASS+1)); }
bad() { echo "    ✗ $*"; FAIL=$((FAIL+1)); }
chk() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
sc() {   # sc <名字> <mode> <vendor 版本或空> [vendor 的 fastboot.img]
    S=$P/sc-$1; rm -rf "$S"; mkdir -p "$S"; bash "$H/mkesp.sh" "$S"
    printf '%s' "$2" > "$S/mode"
    if [ -n "$3" ]; then mkdir -p "$S/vendor/boot/gk3boot"; cp "$EFI" "$S/vendor/boot/gk3boot/gk3boot.efi"; echo "$3" > "$S/vendor/boot/gk3boot/version"; fi
    if [ -n "${4:-}" ]; then cp "$4" "$S/vendor/boot/gk3boot/fastboot.img"; fi
}
act() {   # act <场景> <版本> <observe> <条目文件名…>：放现役 / 上一版条目（与 HAL 的 EntryText 同格式）
    S=$P/sc-$1; v=$2; o=$3; shift 3
    mkdir -p "$S/esp/EFI/gk3boot/$v"; cp "$EFI" "$S/esp/EFI/gk3boot/$v/gk3boot.efi"
    for n in "$@"; do x=${n#gk3*-android-}; x=${x%%[.+]*}
      printf 'title      Android\nversion    gk3boot-%s\nsort-key   0gk3\nefi        /EFI/gk3boot/%s/gk3boot.efi\noptions    gk3.observe=%s gk3.hint=%s\n' "$v" "$v" "$o" "$x" > "$S/esp/loader/entries/$n"
    done
}
go() { OUT=$(bash "$H/harness.sh" "$SH" "$P/sc-$1" 1 2>&1); RC=$(echo "$OUT" | LC_ALL=C sed -n 's/^RC=//p'); }
gk() { (cd "$E" && ls | grep -E '^gk3' | tr '\n' ' '); }

for SH in ${GK3_PI_SHELLS:-mksh dash ksh}; do
command -v "$SH" >/dev/null || { echo "════════ ${SH}：没装，跳过"; continue; }
echo "════════ $SH"

echo "  D1 直连 a/b + 祝福过的 gk3boot-android-b.conf + gk3prev-android-b.conf + 一个前缀多一位的假直连条目（OTA-9 只认 <32 位 hex>）"
sc d1 observe ""
act d1 "$V1" 1 gk3boot-android-b.conf gk3prev-android-b.conf
cp "$P/sc-d1/esp/loader/entries/$MID-android-b.conf" "$P/sc-d1/esp/loader/entries/X$MID-android-b.conf"
go d1
chk "RC=0、选中的是直连条目" '[ "$RC" = 0 ] && echo "$OUT" | grep -q "启动项 = $MID-android-b.conf"'
chk "直连条目 options 同步、内核写进 slot_b" 'grep -q "^options    console=tty0 foo=bar androidboot.slot_suffix=_b" "$E/$MID-android-b.conf" && [ "$(cat "$P/run/esp/$MID/android/slot_b/Image")" = IMAGE ]'
chk "gk3boot / gk3prev 条目没被碰（vendor 不带入口）" 'grep -q "^options    gk3.observe=1 gk3.hint=b" "$E/gk3boot-android-b.conf" && [ -f "$E/gk3prev-android-b.conf" ]'

echo "  P1 没设属性（off）、ESP 上没有入口 ⇒ 什么都不做"
sc p1 "" "$V1"; go p1
chk "RC=0、没有 gk3 条目、没有 EFI/gk3boot" '[ "$RC" = 0 ] && [ -z "$(gk)" ] && [ ! -e "$P/run/esp/EFI/gk3boot" ]'

echo "  P2 observe、第一次部署"
sc p2 observe "$V1"; go p2
chk "RC=0、条目 = +3 ×2" '[ "$RC" = 0 ] && [ "$(gk)" = "gk3boot-android-a+3.conf gk3boot-android-b+3.conf " ]'
chk "二进制写好、与 vendor 一致、没有 .new" 'cmp -s "$EFI" "$P/run/esp/EFI/gk3boot/$V1/gk3boot.efi" && ! find "$P/run/esp" -name "*.new" | grep -q .'
chk "条目正文" '[ "$(cat "$E/gk3boot-android-b+3.conf")" = "$(printf "title      Android (gk3boot observe)\nversion    gk3boot-%s\nsort-key   0gk3\nefi        /EFI/gk3boot/%s/gk3boot.efi\noptions    gk3.observe=1 gk3.hint=b" $V1 $V1)" ]'
chk "直连条目照常同步、内核照常写" 'grep -q "^options    console=tty0 foo=bar androidboot.slot_suffix=_b" "$E/$MID-android-b.conf" && [ "$(cat "$P/run/esp/$MID/android/slot_b/Image")" = IMAGE ]'
rm -rf "$P/p2-esp"; cp -R "$P/run/esp" "$P/p2-esp"

echo "  P3 observe、ESP 上已是这一版这个模式（一个祝福过、一个 +2-1）、有陈旧 .staged ⇒ 条目不动、清 .staged"
sc p3 observe "$V1"; act p3 "$V1" 1 gk3boot-android-a.conf gk3boot-android-b+2-1.conf
printf 'x\n' > "$P/sc-p3/esp/loader/entries/gk3boot-android-a.conf.staged"
go p3
chk "条目不变、陈旧 .staged 被清" '[ "$RC" = 0 ] && [ "$(gk)" = "gk3boot-android-a.conf gk3boot-android-b+2-1.conf " ]'

echo "  P4 action、ESP 上是同版本 observe ⇒ 写 .staged（observe=0），现役不动"
sc p4 action "$V1"; act p4 "$V1" 1 gk3boot-android-a.conf gk3boot-android-b.conf; go p4
chk "现役 + .staged ×2" '[ "$(gk)" = "gk3boot-android-a.conf gk3boot-android-a.conf.staged gk3boot-android-b.conf gk3boot-android-b.conf.staged " ]'
chk ".staged 是 observe=0、title Android" 'grep -q "^options    gk3.observe=0 gk3.hint=a gk3.dispatch=1$" "$E/gk3boot-android-a.conf.staged" && grep -q "^title      Android$" "$E/gk3boot-android-a.conf.staged"'

echo "  P5 vendor 是新版 V2、ESP 上现役 V1 ⇒ V2 目录 + .staged，V1 原样"
sc p5 action V2; act p5 "$V1" 0 gk3boot-android-a.conf gk3boot-android-b.conf; go p5
chk "V1、V2 目录都在" '[ -f "$P/run/esp/EFI/gk3boot/$V1/gk3boot.efi" ] && [ -f "$P/run/esp/EFI/gk3boot/V2/gk3boot.efi" ]'
chk ".staged 指 V2、现役仍指 V1" 'grep -qF "/EFI/gk3boot/V2/gk3boot.efi" "$E/gk3boot-android-b.conf.staged" && grep -qF "/EFI/gk3boot/$V1/" "$E/gk3boot-android-b.conf"'

echo "  P6 off、ESP 上有现役 / +3 / gk3prev / .staged ⇒ 条目全删、目录留着"
sc p6 off "$V1"; act p6 "$V1" 0 gk3boot-android-a.conf gk3boot-android-b+3.conf gk3prev-android-a.conf gk3boot-android-b.conf.staged; go p6
chk "没有 gk3 条目、目录还在、直连条目还在" '[ "$RC" = 0 ] && [ -z "$(gk)" ] && [ -d "$P/run/esp/EFI/gk3boot/$V1" ] && [ -f "$E/$MID-android-a.conf" ]'

echo "  P7 非法模式 ⇒ 不动"
sc p7 Action "$V1"; act p7 "$V1" 0 gk3boot-android-a.conf; go p7
chk "条目不变、RC=0" '[ "$RC" = 0 ] && [ "$(gk)" = "gk3boot-android-a.conf " ]'

echo "  P8 vendor 不带入口 ⇒ 不动"
sc p8 action ""; act p8 "$V1" 0 gk3boot-android-a.conf; go p8
chk "条目不变、RC=0" '[ "$RC" = 0 ] && [ "$(gk)" = "gk3boot-android-a.conf " ]'

echo "  P9 空间账：可用 42000 KB（够内核、不够内核 + 入口）"
sc p9 observe "$V1"; FREE_KB=42000 go p9
chk "observe ⇒ 空间不足、RC=1" '[ "$RC" = 1 ]'
sc p9 off "$V1"; FREE_KB=42000 go p9
chk "off ⇒ 照常通过" '[ "$RC" = 0 ]'

echo "  P10 写二进制失败（EFI/gk3boot 是个文件）⇒ 不部署、OTA 照样成功"
sc p10 action "$V1"; mkdir -p "$P/sc-p10/esp/EFI"; echo x > "$P/sc-p10/esp/EFI/gk3boot"; go p10
chk "RC=0、没有 gk3 条目" '[ "$RC" = 0 ] && [ -z "$(gk)" ]'

echo "  P11 版本串不合法 ⇒ 不部署"
sc p11 action 'bad/ver'; go p11
chk "RC=0、没有条目" '[ "$RC" = 0 ] && [ -z "$(gk)" ]'

echo "  F1 action、第一次部署、vendor 带 fastboot.img ⇒ 二进制 + 执行端 + +3 ×2 + gk3boot-tools.conf"
sc f1 action "$V1" "$FB"; go f1
chk "RC=0、条目 = +3 ×2 + tools" '[ "$RC" = 0 ] && [ "$(gk)" = "gk3boot-android-a+3.conf gk3boot-android-b+3.conf gk3boot-tools.conf " ]'
chk "fastboot.img 写好、与 vendor 一致、没有 .new" 'cmp -s "$FB" "$P/run/esp/EFI/gk3boot/$V1/fastboot.img" && ! find "$P/run/esp" -name "*.new" | grep -q .'
chk "gk3boot-tools.conf 正文逐字节" '[ "$(cat "$E/gk3boot-tools.conf")" = "$(TOOLS_V "$V1")" ]'
rm -rf "$P/f1-esp"; cp -R "$P/run/esp" "$P/f1-esp"

echo "  F2 observe、第一次部署、带 fastboot.img ⇒ 执行端照样铺（规则同 gk3boot.efi），但不写 tools"
sc f2 observe "$V1" "$FB"; go f2
chk "RC=0、条目 = +3 ×2、没有 tools、fastboot.img 在" '[ "$RC" = 0 ] && [ "$(gk)" = "gk3boot-android-a+3.conf gk3boot-android-b+3.conf " ] && cmp -s "$FB" "$P/run/esp/EFI/gk3boot/$V1/fastboot.img"'

echo "  F3 action、第一次部署、vendor 不带 fastboot.img ⇒ 入口照常、没有 tools"
sc f3 action "$V1"; go f3
chk "RC=0、条目 = +3 ×2、没有 fastboot.img" '[ "$RC" = 0 ] && [ "$(gk)" = "gk3boot-android-a+3.conf gk3boot-android-b+3.conf " ] && [ ! -e "$P/run/esp/EFI/gk3boot/$V1/fastboot.img" ]'
chk "日志说这一版不带执行端" 'echo "$OUT" | grep -q "不带执行端"'

echo "  F4 现役 V1（action、tools → V1），vendor V2 带执行端 ⇒ V2/ 铺齐 + .staged，tools 仍指 V1"
sc f4 action V2 "$FB"; act f4 "$V1" 0 gk3boot-android-a.conf gk3boot-android-b.conf
TOOLS_V "$V1" > "$P/sc-f4/esp/loader/entries/gk3boot-tools.conf"; go f4
chk "V2/ 有 gk3boot.efi + fastboot.img" '[ -f "$P/run/esp/EFI/gk3boot/V2/gk3boot.efi" ] && cmp -s "$FB" "$P/run/esp/EFI/gk3boot/V2/fastboot.img"'
chk ".staged ×2、tools 没动（仍指 V1）" '[ "$(gk)" = "gk3boot-android-a.conf gk3boot-android-a.conf.staged gk3boot-android-b.conf gk3boot-android-b.conf.staged gk3boot-tools.conf " ] && [ "$(cat "$E/gk3boot-tools.conf")" = "$(TOOLS_V "$V1")" ]'

echo "  F5 action、ESP 上已是 V1 但没有执行端也没有 tools（上次没带 / 没写上），vendor 这次带了 ⇒ 补上，现役不动"
sc f5 action "$V1" "$FB"; act f5 "$V1" 0 gk3boot-android-a.conf gk3boot-android-b.conf; go f5
chk "条目 = 现役 ×2 + tools，没有 .staged" '[ "$(gk)" = "gk3boot-android-a.conf gk3boot-android-b.conf gk3boot-tools.conf " ]'
chk "fastboot.img 补上、tools → V1" 'cmp -s "$FB" "$P/run/esp/EFI/gk3boot/$V1/fastboot.img" && [ "$(cat "$E/gk3boot-tools.conf")" = "$(TOOLS_V "$V1")" ]'

echo "  F6 observe、ESP 上已是 V1 observe、却留着一个 tools ⇒ 删掉它"
sc f6 observe "$V1" "$FB"; act f6 "$V1" 1 gk3boot-android-a.conf gk3boot-android-b.conf; TOOLS_V "$V1" > "$P/sc-f6/esp/loader/entries/gk3boot-tools.conf"; go f6
chk "没有 tools、现役不动" '[ "$(gk)" = "gk3boot-android-a.conf gk3boot-android-b.conf " ]'

echo "  F7 off、ESP 上有 tools ⇒ 与其他入口条目一起删"
sc f7 off "$V1" "$FB"; act f7 "$V1" 0 gk3boot-android-a.conf gk3prev-android-a.conf; TOOLS_V "$V1" > "$P/sc-f7/esp/loader/entries/gk3boot-tools.conf"; go f7
chk "RC=0、没有任何 gk3 条目、日志数到 3 个" '[ "$RC" = 0 ] && [ -z "$(gk)" ] && echo "$OUT" | grep -q "删掉 3 个入口条目"'

echo "  F8 写 fastboot.img 失败（cp 报 ESP 满）⇒ 入口照常部署、没有 tools、没有 .new、OTA 成功"
sc f8 action "$V1" "$FB"; OUT=$(GK3_PI_FAIL_CP=fastboot.img.new bash "$H/harness.sh" "$SH" "$P/sc-f8" 1 2>&1); RC=$(echo "$OUT" | LC_ALL=C sed -n 's/^RC=//p')
chk "RC=0、条目 = +3 ×2、没有 tools、没有 fastboot.img / .new" '[ "$RC" = 0 ] && [ "$(gk)" = "gk3boot-android-a+3.conf gk3boot-android-b+3.conf " ] && [ ! -e "$P/run/esp/EFI/gk3boot/$V1/fastboot.img" ] && ! find "$P/run/esp" -name "*.new" | grep -q .'
chk "gk3boot.efi 照样写好、日志说没有执行端" '[ -f "$P/run/esp/EFI/gk3boot/$V1/gk3boot.efi" ] && echo "$OUT" | grep -q "只是没有执行端"'

echo "  F9 空间账算上 fastboot.img 的真实大小：可用 43000 KB（够内核 + 入口，不够再加 3000 KB 的执行端）"
sc f9 action "$V1"; FREE_KB=43000 go f9
chk "不带执行端 ⇒ 通过" '[ "$RC" = 0 ]'
sc f9 action "$V1" "$FBBIG"; FREE_KB=43000 go f9
chk "带 3000 KB 执行端 ⇒ 空间不足、RC=1" '[ "$RC" = 1 ] && echo "$OUT" | grep -q "空间不足"'
sc f9 off "$V1" "$FBBIG"; FREE_KB=43000 go f9
chk "off ⇒ 不算入口、照常通过" '[ "$RC" = 0 ]'

# ── R：救援条目（v1.0 计划 OTA-10 / GUI-10 / INST-17）──────────────────────────────────
# 期望值按旧版安装器写救援条目的规则对同一份 cmdline 算一遍（postinstall 必须与它逐字相同）
RCL="androidboot.hardware=gaokun3 init=/init firmware_class.path=/vendor/firmware/ console=tty0 clk_ignore_unused gk3.x=1"
RWANT="$(printf '%s\n' "$RCL" | tr ' ' '\n' | grep -v -e '^androidboot\.' -e '^init=' -e '^firmware_class\.path=' -e '^$' | tr '\n' ' ')loglevel=3 panic=10 gk3.squash=/gaokun3/rescue.squashfs"
resc() {   # resc <场景> <文件名> <槽> <title> <options>：放一条安装器样式的救援条目
    printf 'title      %s\nversion    gaokun3-rescue\nsort-key   linux1\nlinux      /%s/android/slot_%s/Image\ndevicetree /%s/android/slot_%s/gaokun3.dtb\ninitrd     /%s/rescue/initramfs.img\noptions    %s\n' \
        "$4" "$MID" "$3" "$MID" "$3" "$MID" "$5" > "$P/sc-$1/esp/loader/entries/$2"
}
gor() { OUT=$(GK3_PI_CMDLINE="$RCL" bash "$H/harness.sh" "$SH" "$P/sc-$1" "$2" 2>&1); RC=$(echo "$OUT" | LC_ALL=C sed -n 's/^RC=//p'); }
opt() { sed -n 's/^options *//p' "$E/$1"; }

echo "  R1 老机器（v1.0 之前装的：只有借 slot_a 的 rescue.conf、旧标题、旧 options），OTA 到 b ⇒ 派生 rescue-b.conf"
sc r1 off ""; resc r1 "$MID-rescue.conf" a "gaokun3 rescue (runs from RAM)" "old loglevel=4 panic=10 gk3.squash=/gaokun3/rescue.squashfs"
gor r1 1
chk "RC=0、期望值算得出来（${RWANT}）" '[ "$RC" = 0 ] && [ -n "$RWANT" ] && ! echo "$RWANT" | grep -q androidboot'
chk "rescue-b.conf：借 slot_b 的内核与 dtb、initramfs 不变" 'grep -q "^linux      /$MID/android/slot_b/Image$" "$E/$MID-rescue-b.conf" && grep -q "^devicetree /$MID/android/slot_b/gaokun3.dtb$" "$E/$MID-rescue-b.conf" && grep -q "^initrd     /$MID/rescue/initramfs.img$" "$E/$MID-rescue-b.conf"'
chk "rescue-b.conf：options 按安装器的规则从新 cmdline 派生、标题 / version / sort-key 改好" '[ "$(opt "$MID-rescue-b.conf")" = "$RWANT" ] && grep -q "^title      gaokun3 rescue (slot b kernel)$" "$E/$MID-rescue-b.conf" && grep -q "^version    gaokun3-rescue-b$" "$E/$MID-rescue-b.conf" && grep -q "^sort-key   linux2$" "$E/$MID-rescue-b.conf"'
chk "rescue.conf（借 slot_a，内核没换）原样不动" '[ "$(opt "$MID-rescue.conf")" = "old loglevel=4 panic=10 gk3.squash=/gaokun3/rescue.squashfs" ] && grep -q "(runs from RAM)" "$E/$MID-rescue.conf"'
chk "没有留下 .new" '! find "$P/run/esp" -name "*.new" | grep -q .'

echo "  R2 两条都在，OTA 到 a ⇒ 只同步 rescue.conf（并把旧标题改成 slot a kernel），rescue-b.conf 不动"
sc r2 off ""; resc r2 "$MID-rescue.conf" a "gaokun3 rescue (runs from RAM)" "old gk3.squash=/gaokun3/rescue.squashfs"
resc r2 "$MID-rescue-b.conf" b "gaokun3 rescue (slot b kernel)" "oldb gk3.squash=/gaokun3/rescue.squashfs"
gor r2 0
chk "RC=0、rescue.conf 的 options = 安装器规则、标题改成 slot a kernel" '[ "$RC" = 0 ] && [ "$(opt "$MID-rescue.conf")" = "$RWANT" ] && grep -q "^title      gaokun3 rescue (slot a kernel)$" "$E/$MID-rescue.conf"'
chk "rescue-b.conf 不动、直连条目 a 照常同步" '[ "$(opt "$MID-rescue-b.conf")" = "oldb gk3.squash=/gaokun3/rescue.squashfs" ] && [ "$(opt "$MID-android-a.conf")" = "$RCL androidboot.slot_suffix=_a" ]'

echo "  R3 两条都在，OTA 到 b ⇒ 同步 rescue-b.conf，不再派生第二份"
sc r3 off ""; resc r3 "$MID-rescue.conf" a "gaokun3 rescue (slot a kernel)" "olda gk3.squash=/gaokun3/rescue.squashfs"
resc r3 "$MID-rescue-b.conf" b "gaokun3 rescue (slot b kernel)" "oldb gk3.squash=/gaokun3/rescue.squashfs"
gor r3 1
chk "rescue-b.conf 同步、rescue.conf 不动、条目数不变" '[ "$RC" = 0 ] && [ "$(opt "$MID-rescue-b.conf")" = "$RWANT" ] && [ "$(opt "$MID-rescue.conf")" = "olda gk3.squash=/gaokun3/rescue.squashfs" ] && [ "$(ls "$E" | grep -c rescue)" = 2 ]'

echo "  R4 不是安装器写的（手写的、没有 gk3.squash=/gaokun3/rescue.squashfs）⇒ 不碰、不派生"
sc r4 off ""; resc r4 "$MID-rescue.conf" a "my rescue" "console=tty0 gk3.squash=/my/own.squashfs"
gor r4 1
chk "RC=0、没有派生 rescue-b.conf、原条目不变" '[ "$RC" = 0 ] && [ ! -e "$E/$MID-rescue-b.conf" ] && [ "$(opt "$MID-rescue.conf")" = "console=tty0 gk3.squash=/my/own.squashfs" ]'
gor r4 0
chk "OTA 到 a 也不碰它" '[ "$RC" = 0 ] && [ "$(opt "$MID-rescue.conf")" = "console=tty0 gk3.squash=/my/own.squashfs" ]'

# ── T：loader.conf 的 timeout：只把旧安装器写的 15 改成 5，别的一概不动 ────────────────────
win() { mkdir -p "$P/sc-$1/esp/EFI/Microsoft/Boot"; printf 'MZwin' > "$P/sc-$1/esp/EFI/Microsoft/Boot/bootmgfw.efi"; }
LCF=$P/run/esp/loader/loader.conf
echo "  T1 双系统、loader.conf 是安装器写的 timeout 15 ⇒ 改成 5，其余行原样"
sc t1 off ""; win t1; go t1
chk "RC=0、timeout 5、console-mode / editor / default 原样、没有 .new" '[ "$RC" = 0 ] && [ "$(cat "$LCF")" = "$(printf "timeout 5\nconsole-mode keep\neditor no\ndefault *-android-a.conf")" ] && [ ! -e "$LCF.new" ] && echo "$OUT" | grep -q "从安装器原来写的 15 改成 5"'
echo "  T2 双系统、用户改过（timeout 10）⇒ 不动"
sc t2 off ""; win t2; sed -i.bak 's/^timeout 15$/timeout 10/' "$P/sc-t2/esp/loader/loader.conf"; rm -f "$P/sc-t2/esp/loader/loader.conf.bak"; go t2
chk "RC=0、还是 timeout 10" '[ "$RC" = 0 ] && grep -qx "timeout 10" "$LCF" && ! grep -q "timeout 5" "$LCF"'
echo "  T3 纯 Android（没有 bootmgfw.efi）、timeout 15 ⇒ 同样改成 5"
sc t3 off ""; go t3
chk "RC=0、timeout 5、其余行原样" '[ "$RC" = 0 ] && [ "$(cat "$LCF")" = "$(printf "timeout 5\nconsole-mode keep\neditor no\ndefault *-android-a.conf")" ] && [ ! -e "$LCF.new" ] && echo "$OUT" | grep -q "从安装器原来写的 15 改成 5"'
echo "  T4 双系统、已是 timeout 5（1.0 安装器装的）⇒ 不动、不写"
sc t4 off ""; win t4; sed -i.bak 's/^timeout 15$/timeout 5/' "$P/sc-t4/esp/loader/loader.conf"; rm -f "$P/sc-t4/esp/loader/loader.conf.bak"; go t4
chk "RC=0、timeout 5、日志里没说改" '[ "$RC" = 0 ] && grep -qx "timeout 5" "$LCF" && ! echo "$OUT" | grep -q "改成 5"'
echo "  T5 双系统、timeout 15 后面多了空格（不是安装器写的原样）⇒ 不动"
sc t5 off ""; win t5; sed -i.bak 's/^timeout 15$/timeout 15 /' "$P/sc-t5/esp/loader/loader.conf"; rm -f "$P/sc-t5/esp/loader/loader.conf.bak"; go t5
chk "RC=0、原样" '[ "$RC" = 0 ] && grep -qx "timeout 15 " "$LCF"'

echo "  R5 没装救援系统 ⇒ 什么都不多出来"
sc r5 off ""; gor r5 1
chk "RC=0、没有任何 rescue 条目" '[ "$RC" = 0 ] && ! ls "$E" | grep -q rescue'
done

D=$G/build/hal-test/driver
if [ -x "$D" ] && [ -d "$P/p2-esp" ] && [ -d "$P/f1-esp" ]; then
    echo "════════ 交叉：postinstall 第一次部署的 ESP → HAL 开机完成逻辑（${D}）"
    T=$G/build/hal-test/t
    rm -rf "$T"; mkdir -p "$T/vendor/boot/gk3boot"; cp -R "$P/p2-esp" "$T/esp"
    cp "$EFI" "$T/vendor/boot/gk3boot/gk3boot.efi"; echo "$V1" > "$T/vendor/boot/gk3boot/version"
    "$D" nomisc "$T/misc.img"
    mv "$T/esp/loader/entries/gk3boot-android-b+3.conf" "$T/esp/loader/entries/gk3boot-android-b+2-1.conf"
    printf 'persist.vendor.gaokun3.gk3boot=observe\nro.boot.gk3boot.entry=gk3boot-android-b+2-1.conf\n' > "$T/props"
    OUT=$(GK3T_PROPS="$T/props" "$D" run 2>/dev/null)
    chk "第一次经入口开机：只 bless（条目 = a+3、b）" '[ "$(cd "$T/esp/loader/entries" && ls | grep ^gk3 | tr "\n" " ")" = "gk3boot-android-a+3.conf gk3boot-android-b.conf " ]'
    printf 'persist.vendor.gaokun3.gk3boot=observe\nro.boot.gk3boot.entry=gk3boot-android-b.conf\n' > "$T/props"
    OUT=$(GK3T_PROPS="$T/props" "$D" run 2>/dev/null)
    chk "再开一次：HAL 判'已是想要的样子'、只读挂、零写入" 'echo "$OUT" | grep -q "^mounts: ro=1 rw=0$" && echo "$OUT" | grep -q "^bootentry.version=$V1$"'

    echo "════════ 交叉 2：postinstall 在 action + 执行端下第一次部署的 ESP（F1）→ HAL：bless 之后零写入（tools / fastboot.img 两边判据一致）"
    rm -rf "$T"; mkdir -p "$T/vendor/boot/gk3boot"; cp -R "$P/f1-esp" "$T/esp"
    cp "$EFI" "$T/vendor/boot/gk3boot/gk3boot.efi"; echo "$V1" > "$T/vendor/boot/gk3boot/version"; cp "$FB" "$T/vendor/boot/gk3boot/fastboot.img"
    "$D" nomisc "$T/misc.img"
    mv "$T/esp/loader/entries/gk3boot-android-a+3.conf" "$T/esp/loader/entries/gk3boot-android-a+2-1.conf"
    printf 'persist.vendor.gaokun3.gk3boot=action\nro.boot.gk3boot.entry=gk3boot-android-a+2-1.conf\n' > "$T/props"
    OUT=$(GK3T_PROPS="$T/props" "$D" run 2>/dev/null)
    chk "第一次经入口开机：只 bless（条目 = a、b+3、tools）" '[ "$(cd "$T/esp/loader/entries" && ls | grep ^gk3 | tr "\n" " ")" = "gk3boot-android-a.conf gk3boot-android-b+3.conf gk3boot-tools.conf " ]'
    printf 'persist.vendor.gaokun3.gk3boot=action\nro.boot.gk3boot.entry=gk3boot-android-a.conf\n' > "$T/props"
    OUT=$(GK3T_PROPS="$T/props" "$D" run 2>/dev/null)
    chk "再开一次：HAL 判'已是想要的样子'、只读挂、零写入、error 空" 'echo "$OUT" | grep -q "^mounts: ro=1 rw=0$" && echo "$OUT" | grep -q "^bootentry.error=$"'
else
    echo "════════ 交叉：跳过（先 bash tools/gk3boot/test/hal/run.sh 编出 HAL 的主机测试程序）"
fi
echo "══ postinstall：通过 ${PASS}，失败 ${FAIL}"
[ "$FAIL" = 0 ]
