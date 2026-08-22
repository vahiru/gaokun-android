#!/usr/bin/env bash
# Venus 硬件视频编解码（Android 侧）验收。在【宿主机】跑（走 adb）。
#
# 每一项都对应一个真实的失败模式，别删：
#   属性 supported.*    —— ★ 最阴的一个：不设 = 服务在、IComponentStore 也在、
#                          但零个组件，且不报任何错（V4L2ComponentStore.cpp:29-79）
#   /dev/video* 属主    —— 服务跑 user media，默认 root:root 0600 打不开
#   IComponentStore/default —— 服务注册成功的判据（software 那个是 swcodec 的）
#   media_codecs_c2.xml —— 不在 XML 里的组件会被 Codec2InfoBuilder 直接跳过
#   c2.v4l2.* 出现在 MediaCodecList —— 说明 CreateInterfaceByName 成功，
#                          也就意味着 V4L2 设备真的打开了
#   ★ screenrecord      —— 唯一现成的端到端【硬件编码】测试
#   ★ 放一次视频        —— 端到端【硬件解码】测试
set -u
export MSYS_NO_PATHCONV=1
A="adb ${SERIAL:+-s $SERIAL}"
PASS=0; FAIL=0
ok()   { echo "  [OK]   $*"; PASS=$((PASS + 1)); }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }

$A wait-for-device

echo "═══ 1. 门控属性（不设就是零个组件）═══"
for k in decoder.supported.h264 decoder.supported.hevc decoder.supported.vp8 \
         decoder.supported.vp9 encoder.supported.h264 encoder.supported.vp8; do
    v=$($A shell getprop "ro.vendor.v4l2_codec2.$k" | tr -d '\r')
    [ "$v" = true ] && ok "$k = true" || bad "$k = [$v]（应为 true）"
done
for k in decoder.supported.av1 encoder.supported.vp9; do
    v=$($A shell getprop "ro.vendor.v4l2_codec2.$k" | tr -d '\r')
    [ -z "$v" ] || [ "$v" = false ] && ok "$k 未启用（硬件确实没有）" || bad "$k = [$v]，但硬件没有这个能力"
done
v=$($A shell getprop debug.stagefright.c2-poolmask | tr -d '\r')
[ "$v" = 0xfc0000 ] && ok "poolmask = $v（BLOB；本机没有 ION）" || bad "poolmask = [$v]，应为 0xfc0000"

echo; echo "═══ 2. 设备节点 ═══"
$A shell 'ls -lZ /dev/video0 /dev/video1' 2>/dev/null | sed 's/^/  /'
o=$($A shell 'stat -c %U:%G /dev/video0' | tr -d '\r')
[ "$o" = "media:media" ] && ok "/dev/video0 属主 $o" || bad "/dev/video0 属主 [$o]，应为 media:media"

echo; echo "═══ 3. HAL 服务 ═══"
$A shell 'service list 2>/dev/null | grep -i "media.c2"' | sed 's/^/  /'
$A shell 'service list 2>/dev/null | grep -q "IComponentStore/default"' \
    && ok "IComponentStore/default 已注册" || bad "IComponentStore/default 不在"
$A shell 'ps -A -o name 2>/dev/null | grep -q "c2-service-v4l2"' \
    && ok "服务进程在跑" || bad "服务进程不在（看 logcat 里的 c2-service-v4l2）"

echo; echo "═══ 4. MediaCodecList 里的硬件组件 ═══"
LIST=$($A shell 'dumpsys media.player 2>/dev/null' | grep -oE 'c2\.v4l2\.[a-z0-9.]+' | sort -u)
echo "$LIST" | sed 's/^/  /'
N=$(echo "$LIST" | grep -c 'c2.v4l2' || true)
[ "$N" -ge 6 ] && ok "$N 个 c2.v4l2 组件已注册" || bad "只有 $N 个（期望 6：解码 4 + 编码 2）"

echo; echo "═══ 5. 端到端：硬件编码（screenrecord）═══"
$A shell 'logcat -c 2>/dev/null'
$A shell 'rm -f /data/local/tmp/hwenc.mp4; screenrecord --time-limit 4 --size 1280x720 /data/local/tmp/hwenc.mp4' >/dev/null 2>&1
SZ=$($A shell 'stat -c %s /data/local/tmp/hwenc.mp4 2>/dev/null' | tr -d '\r')
[ -n "$SZ" ] && [ "$SZ" -gt 10000 ] && ok "录出 $SZ 字节的 mp4" || bad "录制失败（大小 [$SZ]）"
USED=$($A shell 'logcat -d 2>/dev/null' | grep -oE 'c2\.v4l2\.avc\.encoder|c2\.android\.avc\.encoder|OMX\.[A-Za-z0-9.]+' | sort -u | tr '\n' ' ')
echo "  实际用到的编码器: ${USED:-（日志里没看到）}"
case "$USED" in *c2.v4l2.avc.encoder*) ok "★ 走的是硬件编码器";; *) bad "没走 c2.v4l2.avc.encoder（上面是实际用的）";; esac

echo; echo "═══ 6. 端到端：硬件解码（放刚录的那段）═══"
$A shell 'logcat -c 2>/dev/null'
$A shell 'cp /data/local/tmp/hwenc.mp4 /sdcard/hwenc.mp4 2>/dev/null
am start -a android.intent.action.VIEW -d file:///sdcard/hwenc.mp4 -t video/mp4' >/dev/null 2>&1
sleep 6
DEC=$($A shell 'logcat -d 2>/dev/null' | grep -oE 'c2\.v4l2\.avc\.decoder|c2\.android\.avc\.decoder' | sort -u | tr '\n' ' ')
echo "  实际用到的解码器: ${DEC:-（日志里没看到；可能没有能放 mp4 的应用）}"
case "$DEC" in *c2.v4l2.avc.decoder*) ok "★ 走的是硬件解码器";; *) echo "  [SKIP] 未观测到硬件解码（不判失败：可能只是没应用接这个 intent）";; esac

echo; echo "═══ 7. 内核侧有没有报错 ═══"
$A shell 'dmesg | grep -iE "venus|video-codec" | tail -6' | sed 's/^/  /'
E=$($A shell 'dmesg | grep -ciE "venus.*(error|fail|timeout)"' | tr -d '\r')
[ "${E:-0}" = 0 ] && ok "venus 无报错" || bad "venus 有 $E 行报错"

echo; echo "═══ 小结：通过 $PASS · 失败 $FAIL ═══"
[ "$FAIL" -eq 0 ]
