#!/usr/bin/env bash
# 硬件视频编解码（Android侧）验收。在宿主机跑（走adb）。
# 内核驱动 2026-09-28 起是 qcom-iris（#128；此前是 qcom-venus，本脚本原名 verify-venus-codec2.sh）。
#
# 用法: [SERIAL=xxx] bash scripts/verify-hw-codec2.sh [测试视频.mp4 [组件名]]
#   不给视频就不做真解；给了就真解一遍（这才是解码唯一算数的判据）。
#   确认内核已建立CP非像素上下文、且屏幕亮着时，还会用screenrecord经c2.v4l2.avc.encoder在1280x720和640x480各真编一段
#   （熄屏时视频硬件会挂起，见docs/dev/audio-video.md）；设了NOENC就跳过这一步（accept.sh --readonly）。
#   内核日志要root才能读：adb shell不是uid 0时经su读，读不到的检查记为跳过。
#   组件名默认 c2.v4l2.avc.decoder；HEVC / VP9 的片子要显式给 c2.v4l2.hevc.decoder / c2.v4l2.vp9.decoder。
#
# 每一项都对应一个真实踩过的失败模式，别删：
#   门控属性        —— ★最阴：不设 = 服务在、IComponentStore 也在、但零个组件，
#                      而且不报任何错（V4L2ComponentStore.cpp:29-79）
#   /dev/video* 属主 —— 服务跑 user media，默认 root:root 0600 打不开
#   扩展 seccomp     —— 不装就在真干活时被 SIGSYS 打死（blocked syscall: eventfd2）
#   编码器只开avc/hevc、并发1 —— 编码器ARP在固定IOVA上，第二路要在分配时被拒、回退软编；
#                      VP8/VP9编码iris没有，开着的话应用拿到失败而不是回退软编
#   CP非像素上下文   —— 内核没有patches/kernel/iommu/0001时第一次编码就整板复位，dmesg里要有CB15 ready
#   VP8【必须没有】  —— iris gen1 不解 VP8（iris_platform_vpu2.c 的解码格式表只有 H264/HEVC/VP9），
#                      门控开着的话组件建得出来、start() 失败，应用拿到的是失败而不是回退软解
#   按名字找节点     —— camss 占了 video0-31，编解码节点的编号取决于谁先 probe，不能写死 video0
#   ★ 真解一段     —— 组件"在列表里"只证明能实例化，不证明能解码
set -u
export MSYS_NO_PATHCONV=1
A="adb ${SERIAL:+-s $SERIAL}"
VIDEO=${1:-}
COMP=${2:-c2.v4l2.avc.decoder}
PASS=0; FAIL=0
ok()  { echo "  [OK]   $*"; PASS=$((PASS + 1)); }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }

$A wait-for-device

# 内核日志：发布版的adb shell没有syslog_read，经su读；都读不到时返回空。
if [ "$($A shell id -u 2>/dev/null | tr -d '\r')" = 0 ]; then
    KLOG() { $A shell dmesg 2>/dev/null | tr -d '\r'; }
else
    KLOG() { printf 'dmesg\n' | $A shell su -c sh 2>/dev/null | tr -d '\r'; }
fi

echo "═══ 1. 门控属性 ═══"
for k in h264 hevc vp9; do
    v=$($A shell getprop "ro.vendor.v4l2_codec2.decoder.supported.$k" | tr -d '\r')
    [ "$v" = true ] && ok "decoder.$k = true" || bad "decoder.$k = [$v]（应为 true）"
done
for k in h264 hevc; do
    v=$($A shell getprop "ro.vendor.v4l2_codec2.encoder.supported.$k" | tr -d '\r')
    [ "$v" = true ] && ok "encoder.$k = true" || bad "encoder.$k = [$v]（应为true）"
done
v=$($A shell getprop ro.vendor.v4l2_codec2.encode_concurrent_instances | tr -d '\r')
[ "$v" = 1 ] && ok "encode_concurrent_instances = 1（编码器ARP在固定IOVA，只能有一个编码会话）" \
    || bad "encode_concurrent_instances = [$v]，应为1"
for k in decoder.supported.vp8 decoder.supported.av1 encoder.supported.vp8 encoder.supported.vp9; do
    v=$($A shell getprop "ro.vendor.v4l2_codec2.$k" | tr -d '\r')
    if [ -z "$v" ] || [ "$v" = false ]; then ok "$k 未启用（有意为之）"; else bad "$k = [$v]，不该启用"; fi
done
v=$($A shell getprop debug.stagefright.c2-poolmask | tr -d '\r')
[ "$v" = 0xfe0000 ] && ok "poolmask = ${v}（BLOB + GRALLOC；本机没有 ION）" || bad "poolmask = [$v]，应为 0xfe0000"
v=$($A shell getprop debug.stagefright.c2inputsurface | tr -d '\r')
[ "$v" = "-1" ] && ok "c2inputsurface = -1（绕开框架的空指针崩溃）" || bad "c2inputsurface = [$v]，应为 -1"

echo; echo "═══ 2. 设备节点与 seccomp 策略 ═══"
# 按 sysfs 名字找解码节点（iris_probe.c 注册的名字），再查属主（ueventd.gaokun3.rc 的 /dev/video* 规则）。
DEC=$($A shell 'for v in /sys/class/video4linux/video*; do [ "$(cat $v/name)" = qcom-iris-decoder ] && echo /dev/${v##*/}; done' | tr -d '\r')
if [ -z "$DEC" ]; then
    bad "找不到名为 qcom-iris-decoder 的 video 节点（iris 没绑上？看 dmesg | grep aa00000）"
else
    o=$($A shell "stat -c %U:%G $DEC" | tr -d '\r')
    [ "$o" = "media:camera" ] && ok "${DEC}（qcom-iris-decoder）属主 ${o}" || bad "${DEC} 属主 [${o}]，应为 media:camera"
fi
ENC=$($A shell 'for v in /sys/class/video4linux/video*; do [ "$(cat $v/name)" = qcom-iris-encoder ] && echo /dev/${v##*/}; done' | tr -d '\r')
if [ -z "$ENC" ]; then
    bad "找不到名为qcom-iris-encoder的video节点"
else
    o=$($A shell "stat -c %U:%G $ENC" | tr -d '\r')
    [ "$o" = "media:camera" ] && ok "${ENC}（qcom-iris-encoder）属主${o}" || bad "${ENC}属主[${o}]，应为media:camera"
fi
# patches/kernel/iommu/0001在启动时打印这一行；没有它，第一次硬件编码会让整板复位。
# 只有在这里确认过，第5b步才真编。日志从开机后已经滚动过（第一行不是0秒）时看不到这一行，记为跳过。
CPNP=0
K=$(KLOG)
if [ -z "$K" ]; then
    echo "  [SKIP] 读不到内核日志（没有root），无法确认CP非像素上下文，第5b步不真编"
elif echo "$K" | grep -q "cp-nonpixel: CB15 ready"; then
    ok "内核已建立CP非像素上下文（cp-nonpixel: CB15 ready）"; CPNP=1
elif ! echo "$K" | head -1 | grep -q '^\[ *0\.0'; then
    echo "  [SKIP] 内核日志已经滚动，看不到开机时的cp-nonpixel: CB15 ready，第5b步不真编"
else
    bad "内核日志里没有cp-nonpixel: CB15 ready —— 这个内核上不能开硬件编码"
fi
v=$($A shell 'cat /sys/module/qcom_iris/parameters/venus_compat_gfmt 2>/dev/null' | tr -d '\r')
echo "  [INFO] qcom_iris.venus_compat_gfmt = [${v}]（Y = 与 venus 同一条路，patches/kernel/iris/0003）"
$A shell 'test -f /vendor/etc/seccomp_policy/android.hardware.media.c2-extended-seccomp_policy' \
    && ok "扩展 seccomp 策略已装" || bad "缺扩展 seccomp 策略（会在解码时 SIGSYS）"

echo; echo "═══ 3. HAL 服务 ═══"
$A shell 'service list 2>/dev/null | grep -q "IComponentStore/default"' \
    && ok "IComponentStore/default 已注册" || bad "IComponentStore/default 不在"
$A shell 'ps -A -o name 2>/dev/null | grep -q "c2-service-v4l2"' \
    && ok "服务进程在跑" || bad "服务进程不在"
S=$($A shell 'logcat -b all -d 2>/dev/null | grep -v "adbd service requested" | grep -c "received SIGSYS"' | tr -d '\r')
[ "${S:-0}" = 0 ] && ok "没有 SIGSYS（seccomp 没打死它）" || bad "有 $S 次 SIGSYS —— 看 blocked syscall"

echo; echo "═══ 4. MediaCodecList 里的硬件组件 ═══"
LIST=$($A shell 'dumpsys media.player 2>/dev/null' | grep -oE 'c2\.v4l2\.[a-z0-9.]+' | sort -u)
echo "$LIST" | sed 's/^/  /'
N=$(echo "$LIST" | grep -c 'decoder' || true)
[ "$N" -eq 3 ] && ok "$N 个解码组件已注册（avc / hevc / vp9）" || bad "$N 个解码组件（期望恰好 3：avc / hevc / vp9）"
echo "$LIST" | grep -q 'vp8' && bad "居然有 VP8 组件（iris 不解 VP8，应被属性与 XML 关掉）" || ok "没有 VP8 组件（有意为之）"
N=$(echo "$LIST" | grep -c 'encoder' || true)
[ "$N" -eq 2 ] && ok "${N}个编码组件已注册（avc/hevc）" || bad "${N}个编码组件（期望恰好2个：avc/hevc）"

echo; echo "═══ 5. ★ 真解一段（唯一算数的判据）═══"
if [ -z "$VIDEO" ]; then
    echo "  [SKIP] 没给测试视频。用法：bash $0 <某个.mp4>"
else
    $A push "$VIDEO" /data/local/tmp/dt.mp4 >/dev/null 2>&1
    $A shell 'logcat -c' >/dev/null 2>&1
    OUT=$($A shell "/system/bin/gaokun3-decode-test /data/local/tmp/dt.mp4 $COMP 2>&1")
    echo "$OUT" | sed 's/^/  /'
    case "$OUT" in
        *"通过：真的解出了帧"*) ok "★ 硬件解码器真的解出了帧" ;;
        *"创建解码器失败"*)     bad "连组件都创建不出来" ;;
        *)                      bad "解码失败（上面是输出）" ;;
    esac
    echo "  --- 内核侧同期日志 ---"
    $A shell 'logcat -b all -d 2>/dev/null | grep -v "adbd service requested" | grep -iE "V4L2Device|DecodeComponent|blocked syscall" | tail -6' | sed 's/^/  /'
fi

echo; echo "═══ 5b. ★ 真编一段（H.264，screenrecord）═══"
# screenrecord只会用avc，--codec-name指定硬件组件，不让它悄悄落到软编上。画面不动时只出很少几帧。
# 编码器在第一帧之后出错时screenrecord只打印Encoder failed、退出码仍是0，所以还要看--verbose最后报的帧数。
# 熄屏时视频硬件会挂起（docs/dev/audio-video.md），所以只在亮屏时做；超时30秒算失败。
# 编两个尺寸：1280x720的一帧正好是4 KiB的倍数；640x480不是，iris要求的缓冲比一帧的数据大2048字节，
# 这个尺寸靠patches/android/v4l2-codec2/0006才能送进编码器。
W=$($A shell 'dumpsys power 2>/dev/null | grep -m1 -o "mWakefulness=[A-Za-z]*"' | tr -d '\r')
if [ -n "${NOENC:-}" ]; then
    echo "  [SKIP] 设了NOENC，不做真编"
elif [ "$CPNP" != 1 ]; then
    echo "  [SKIP] 没有确认内核已建立CP非像素上下文（见第2步），不做真编"
elif [ "$W" != "mWakefulness=Awake" ]; then
    echo "  [SKIP] 屏幕没亮（${W:-未知}），不做真编。点亮屏幕后重跑"
else
    for RES in 1280x720 640x480; do
        $A shell 'rm -f /data/local/tmp/vhw.mp4' >/dev/null 2>&1
        $A shell 'logcat -c' >/dev/null 2>&1
        OUT=$($A shell "timeout 30 screenrecord --verbose --codec-name c2.v4l2.avc.encoder --size $RES --bit-rate 4000000 --time-limit 3 /data/local/tmp/vhw.mp4 2>&1; echo rc=\$?" | tr -d '\r')
        SZ=$($A shell 'stat -c %s /data/local/tmp/vhw.mp4 2>/dev/null' | tr -d '\r')
        NF=$(echo "$OUT" | sed -n 's/.*recorded \([0-9][0-9]*\) frames.*/\1/p' | tail -1)
        if echo "$OUT" | grep -q '^rc=0$' && ! echo "$OUT" | grep -q 'Encoder failed' \
                && [ "${NF:-0}" -gt 0 ] && [ "${SZ:-0}" -gt 1024 ]; then
            ok "★ c2.v4l2.avc.encoder在${RES}编出了${NF}帧、${SZ}字节"
        else
            bad "${RES}硬件编码失败（帧数[${NF}]，大小[${SZ}]；rc=124是超时）："
            echo "$OUT" | sed 's/^/    /'
            $A shell 'logcat -d 2>/dev/null | grep -E "EncodeComponent|V4L2Encoder|QBUF" | tail -8' | sed 's/^/    /'
        fi
    done
    $A shell 'rm -f /data/local/tmp/vhw.mp4' >/dev/null 2>&1
fi

echo; echo "═══ 6. 内核侧有没有报错 ═══"
# iris 的 dev_err 都带设备名 aa00000.video-codec；另有 arm-smmu 对它的流 ID 报的 context fault ——
# iommus 是 <0x2a00 0x400>（掩码 0x400），SID 0x2a00 与 0x2e00 都落在这个上下文里，两个都要认。
# （arm-smmu 报两行：Unhandled context fault … cbfrsynra=… 与 SID=…；不放宽到整个 15000000.iommu —— 显示 / 相机也挂在它上面。）
# 编码器ARP所在的CP非像素上下文（SID 0x2a04）出错时由patches/kernel/iommu/0001报"cp-nonpixel fault"等，
# ARP的页交不回HLOS时patches/kernel/iris/0013报"enc ARP: stuck"。
RE='aa00000\.video-codec.*(error|fail|timeout|invalid|watchdog|unsupported)|cbfrsynra=0x2[ae]00|SID=0x2[ae]00|cp-nonpixel( fault|: leaking|: no fault irq|: no free SME)|enc ARP: stuck'
K=$(KLOG)
if [ -z "$K" ]; then
    echo "  [SKIP] 读不到内核日志（没有root）"
else
    E=$(echo "$K" | grep -ciE "$RE")
    [ "${E:-0}" = 0 ] && ok "iris无报错" || { bad "iris有${E}行报错："; echo "$K" | grep -iE "$RE" | tail -8 | sed 's/^/    /'; }
fi

echo; echo "═══ 小结：通过 $PASS · 失败 $FAIL ═══"
[ "$FAIL" -eq 0 ]
