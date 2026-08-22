#!/usr/bin/env bash
# Venus 硬件视频【解码】（Android 侧）验收。在宿主机跑（走 adb）。
#
# 用法: [SERIAL=xxx] bash scripts/verify-venus-codec2.sh [测试视频.mp4]
#   不给视频就只做静态检查；给了就真解一遍（这才是唯一算数的判据）。
#
# 每一项都对应一个真实踩过的失败模式，别删：
#   门控属性        —— ★最阴：不设 = 服务在、IComponentStore 也在、但零个组件，
#                      而且不报任何错（V4L2ComponentStore.cpp:29-79）
#   /dev/video* 属主 —— 服务跑 user media，默认 root:root 0600 打不开
#   扩展 seccomp     —— 不装就在真干活时被 SIGSYS 打死（blocked syscall: eventfd2）
#   编码器【必须没有】—— 它走不通 surface 输入，开着会让应用失败而不是回退软编
#   ★ 真解一段     —— 组件"在列表里"只证明能实例化，不证明能解码
set -u
export MSYS_NO_PATHCONV=1
A="adb ${SERIAL:+-s $SERIAL}"
VIDEO=${1:-}
PASS=0; FAIL=0
ok()  { echo "  [OK]   $*"; PASS=$((PASS + 1)); }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }

$A wait-for-device

echo "═══ 1. 门控属性 ═══"
for k in h264 hevc vp8 vp9; do
    v=$($A shell getprop "ro.vendor.v4l2_codec2.decoder.supported.$k" | tr -d '\r')
    [ "$v" = true ] && ok "decoder.$k = true" || bad "decoder.$k = [$v]（应为 true）"
done
for k in decoder.supported.av1 encoder.supported.h264 encoder.supported.vp8 encoder.supported.vp9; do
    v=$($A shell getprop "ro.vendor.v4l2_codec2.$k" | tr -d '\r')
    if [ -z "$v" ] || [ "$v" = false ]; then ok "$k 未启用（有意为之）"; else bad "$k = [$v]，不该启用"; fi
done
v=$($A shell getprop debug.stagefright.c2-poolmask | tr -d '\r')
[ "$v" = 0xfc0000 ] && ok "poolmask = $v（BLOB；本机没有 ION）" || bad "poolmask = [$v]，应为 0xfc0000"
v=$($A shell getprop debug.stagefright.c2inputsurface | tr -d '\r')
[ "$v" = "-1" ] && ok "c2inputsurface = -1（绕开框架的空指针崩溃）" || bad "c2inputsurface = [$v]，应为 -1"

echo; echo "═══ 2. 设备节点与 seccomp 策略 ═══"
o=$($A shell 'stat -c %U:%G /dev/video0' | tr -d '\r')
[ "$o" = "media:media" ] && ok "/dev/video0 属主 $o" || bad "/dev/video0 属主 [$o]，应为 media:media"
$A shell 'test -f /vendor/etc/seccomp_policy/android.hardware.media.c2-extended-seccomp_policy' \
    && ok "扩展 seccomp 策略已装" || bad "缺扩展 seccomp 策略（会在解码时 SIGSYS）"

echo; echo "═══ 3. HAL 服务 ═══"
$A shell 'service list 2>/dev/null | grep -q "IComponentStore/default"' \
    && ok "IComponentStore/default 已注册" || bad "IComponentStore/default 不在"
$A shell 'ps -A -o name 2>/dev/null | grep -q "c2-service-v4l2"' \
    && ok "服务进程在跑" || bad "服务进程不在"
S=$($A shell 'logcat -b all -d 2>/dev/null | grep -c "received SIGSYS"' | tr -d '\r')
[ "${S:-0}" = 0 ] && ok "没有 SIGSYS（seccomp 没打死它）" || bad "有 $S 次 SIGSYS —— 看 blocked syscall"

echo; echo "═══ 4. MediaCodecList 里的硬件组件 ═══"
LIST=$($A shell 'dumpsys media.player 2>/dev/null' | grep -oE 'c2\.v4l2\.[a-z0-9.]+' | sort -u)
echo "$LIST" | sed 's/^/  /'
N=$(echo "$LIST" | grep -c 'decoder' || true)
[ "$N" -ge 4 ] && ok "$N 个解码组件已注册" || bad "只有 $N 个解码组件（期望 4）"
echo "$LIST" | grep -q encoder && bad "居然有编码组件（应该被属性关掉）" || ok "没有编码组件（有意为之）"

echo; echo "═══ 5. ★ 真解一段（唯一算数的判据）═══"
if [ -z "$VIDEO" ]; then
    echo "  [SKIP] 没给测试视频。用法：bash $0 <某个.mp4>"
else
    $A push "$VIDEO" /data/local/tmp/dt.mp4 >/dev/null 2>&1
    $A shell 'logcat -c' >/dev/null 2>&1
    OUT=$($A shell '/system/bin/gaokun3-decode-test /data/local/tmp/dt.mp4 c2.v4l2.avc.decoder 2>&1')
    echo "$OUT" | sed 's/^/  /'
    case "$OUT" in
        *"通过：真的解出了帧"*) ok "★ 硬件解码器真的解出了帧" ;;
        *"创建解码器失败"*)     bad "连组件都创建不出来" ;;
        *)                      bad "解码失败（上面是输出）" ;;
    esac
    echo "  --- 内核侧同期日志 ---"
    $A shell 'logcat -b all -d 2>/dev/null | grep -iE "V4L2Device|DecodeComponent|blocked syscall" | tail -6' | sed 's/^/  /'
fi

echo; echo "═══ 6. 内核侧有没有报错 ═══"
E=$($A shell 'dmesg | grep -ciE "venus.*(error|fail|timeout)"' | tr -d '\r')
[ "${E:-0}" = 0 ] && ok "venus 无报错" || bad "venus 有 $E 行报错"

echo; echo "═══ 小结：通过 $PASS · 失败 $FAIL ═══"
[ "$FAIL" -eq 0 ]
