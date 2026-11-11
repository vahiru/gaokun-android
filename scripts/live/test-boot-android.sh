#!/usr/bin/env bash
# 救援系统里的 gk3-boot-android（scripts/live/overlay-common/usr/bin/）的离线测试：任何机器，不要 root。
#
#   bash scripts/live/test-boot-android.sh
#
# 用 GK3_ESP_DIR（一个目录当 ESP）+ GK3_EFIVARS（一个目录当 efivarfs）跑，逐字节核写进去的变量。
# ⚠️ 测的是选条目与编码：真 efivarfs（chattr -i、一次 write、uefisecapp 后端）要在真机的救援系统里看（TODO V16）。
set -u
cd "$(dirname "$0")/../.."
H=scripts/live/overlay-common/usr/bin/gk3-boot-android
W=$(mktemp -d /tmp/gk3-bootandroid.XXXX); trap 'rm -rf "$W"' EXIT
PASS=0; FAIL=0
ok()  { echo "  ✓ $*"; PASS=$((PASS+1)); }
bad() { echo "  ✗ $*"; FAIL=$((FAIL+1)); }
MID=0123456789abcdef0123456789abcdef
VARN=LoaderEntryOneShot-4a67b082-0a4c-41cf-b6c7-440b29bb8c4f
mkesp() {   # 安装器样式的 ESP：两个直连条目 + 救援 + 统一启动入口的条目
    rm -rf "$W/esp" "$W/efi"; mkdir -p "$W/esp/loader/entries" "$W/efi"
    printf 'timeout 15\ndefault *-android-%s.conf\n' "${1:-a}" > "$W/esp/loader/loader.conf"
    for s in a b; do
        mkdir -p "$W/esp/$MID/android/slot_$s"; echo K > "$W/esp/$MID/android/slot_$s/Image"; echo D > "$W/esp/$MID/android/slot_$s/gaokun3.dtb"
        printf 'title x\nlinux      /%s/android/slot_%s/Image\ndevicetree /%s/android/slot_%s/gaokun3.dtb\noptions o\n' "$MID" "$s" "$MID" "$s" > "$W/esp/loader/entries/$MID-android-$s.conf"
        printf 'title Android\nefi /EFI/gk3boot/v/gk3boot.efi\n' > "$W/esp/loader/entries/gk3boot-android-$s.conf"
    done
    printf 'title r\nlinux /%s/android/slot_a/Image\n' "$MID" > "$W/esp/loader/entries/$MID-rescue.conf"
}
run() { OUT=$(GK3_ESP_DIR=$W/esp GK3_EFIVARS=$W/efi bash "$H" "$@" 2>&1); RC=$?; }
# 期望的字节另起一套算（iconv 的 UTF-16LE），不抄被测脚本的写法
want_bytes() { { printf '\007\000\000\000'; printf '%s' "$1" | iconv -f UTF-8 -t UTF-16LE; printf '\000\000'; } | od -An -tx1 | tr -d ' \n'; }
got_bytes() { od -An -tx1 "$W/efi/$VARN" 2>/dev/null | tr -d ' \n'; }

echo "═══ gk3-boot-android ═══"
mkesp a; run b
[ "$RC" = 0 ] && [ "$(got_bytes)" = "$(want_bytes "$MID-android-b.conf")" ] \
    && ok "槽 b：写的是直连条目 $MID-android-b.conf（属性 07 + UTF-16LE + 双 NUL，逐字节），不是 gk3boot-android-b.conf" \
    || { bad "槽 b 写得不对（rc=${RC}）"; echo "$OUT" | sed 's/^/      /'; }
[ "$(printf '%s' "$(want_bytes x)" | cut -c1-8)" = 07000000 ] && ok "前 4 字节 = 07 00 00 00（NV|BS|RT）" || bad "属性字节不对"
mkesp b; rm -f "$W/efi/$VARN"; run
[ "$RC" = 0 ] && [ "$(got_bytes)" = "$(want_bytes "$MID-android-b.conf")" ] && ok "不给槽：取 loader.conf 的 default（*-android-b.conf）" || bad "默认槽不对（rc=${RC}）：$OUT"
mkesp a; mv "$W/esp/loader/entries/$MID-android-a.conf" "$W/esp/loader/entries/$MID-android-a+2-1.conf"; run a
[ "$RC" = 0 ] && [ "$(got_bytes)" = "$(want_bytes "$MID-android-a.conf")" ] && ok "带启动计数的文件名（+2-1）：写去掉计数后的 ID（systemd-boot 按它匹配）" || bad "计数后缀处理不对（rc=${RC}）：$OUT"
mkesp a; cp "$W/esp/loader/entries/$MID-android-a.conf" "$W/esp/loader/entries/fedcba9876543210fedcba9876543210-android-a.conf"; rm -f "$W/efi/$VARN"; run a
[ "$RC" != 0 ] && echo "$OUT" | grep -q 'more than one direct entry for slot a' && [ ! -e "$W/efi/$VARN" ] && ok "同一个槽两个直连条目：拒绝、不写" || bad "重复条目没拒绝（rc=${RC}）"
mv "$W/esp/loader/entries/fedcba9876543210fedcba9876543210-android-a.conf" "$W/esp/loader/entries/fedcba9876543210fedcba9876543210-android-a.conf.disabled"; run a
[ "$RC" = 0 ] && ok "另一个改名成 .conf.disabled 之后：放行" || bad ".disabled 没被忽略（rc=${RC}）：$OUT"
mkesp a; rm -f "$W/esp/loader/entries/$MID-android-b.conf" "$W/efi/$VARN"; run b
[ "$RC" != 0 ] && echo "$OUT" | grep -q 'no direct entry for slot b' && [ ! -e "$W/efi/$VARN" ] && ok "槽 b 没有直连条目（只有 gk3boot-android-b.conf）：拒绝、不写" || bad "缺条目没拒绝（rc=${RC}）"
mkesp a; rm -f "$W/esp/$MID/android/slot_b/Image"; run b
[ "$RC" != 0 ] && echo "$OUT" | grep -q 'which is not on the ESP' && [ ! -e "$W/efi/$VARN" ] && ok "条目指的内核不在 ESP 上：拒绝、不写" || bad "缺内核没拒绝（rc=${RC}）"
mkesp a; run a; run --clear
[ "$RC" = 0 ] && [ ! -e "$W/efi/$VARN" ] && ok "--clear 撤掉 OneShot" || bad "--clear 不对（rc=${RC}）"
run --list
[ "$RC" = 0 ] && echo "$OUT" | grep -q "direct entries for slot a: $MID-android-a.conf" && ok "--list 列出两个槽的直连条目" || bad "--list 不对：$OUT"
run c
[ "$RC" != 0 ] && ok "不认识的参数：拒绝" || bad "坏参数没拒绝"

echo
echo "═══ 通过 $PASS · 失败 $FAIL ═══"
[ "$FAIL" -eq 0 ]
