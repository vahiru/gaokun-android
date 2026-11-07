#!/usr/bin/env bash
# 声学回环对照（patches/android/hardware-interfaces/0006，见 docs/dev/audio-video.md）：镜像自带的 HAL 与另一份 HAL（bind-mount）各跑同一套，
# 用 gaokun3-loopback 量"应用时间戳预测的出声时刻 → 麦克风实际收到"的偏移 e，看它在开流之间、卡顿前后稳不稳。
# ⚠️ 会出声（扬声器，每 0.5 s 一声 2 ms 的 3 kHz 短音，默认 -24 dBFS × 当前媒体音量）。宿舍环境：要有人在场、同意，并显式 LOUD=1。
#
#   LOUD=1 SER=<adb 序列号> bash scripts/audio/loopback-check.sh <对照 HAL 二进制> [gaokun3-loopback]
#   可选：RUNS=<每份 HAL 开流次数，默认 4>  AMP=<dBFS，默认 -24>  VOL=<测试时的媒体音量，默认不改>
#   对照 HAL：例如原版 out/micverify/hal-0063b.bin（= 1790702971 镜像里的那份，a5803b1d…）
#
# 每份 HAL：RUNS 次独立开流（每次 8 s，间隔 5 s 让输出线程 standby）+ 1 次 16 s、第 5 / 10 s 各停 audioserver 0.2 s。
# 判据：同一份 HAL 各次开流的 e 中位数之差（音游校准一次就要它不变）；卡顿那一次 e 前后有没有永久跳。
set -uo pipefail
[ "${LOUD:-}" = 1 ] || { echo "✗ 这个测试会出声：确认有人在场、同意后用 LOUD=1 再跑" >&2; exit 1; }
SER=${SER:?SER=<adb 序列号>}
ALT=${1:?用法：$0 <对照 HAL 二进制> [gaokun3-loopback]}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
TOOL=${2:-$REPO/out/playprobe/gaokun3-loopback}
RUNS=${RUNS:-4}
AMP=${AMP:--24}
OUT=$REPO/out/playprobe/loop-$(date +%Y%m%d-%H%M%S)
D=/data/local/tmp/playprobe
HAL_ON_DEV=/apex/com.android.hardware.audio/bin/hw/android.hardware.audio.service-aidl.example
S() { adb -s "$SER" shell "$@" | tr -d '\r'; }
die() { echo "✗ $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }
mkdir -p "$OUT"

[ -f "$ALT" ] && [ -f "$TOOL" ] || die "缺文件：$ALT / $TOOL"
[ "$(S getprop ro.crdroid.device)" = gaokun3 ] || die "${SER} 不是 gaokun3"
[ "$(S id -u)" = 0 ] || die "需要 root：开发构建 adb shell setprop service.adb.root 1 && adb root；发布构建（ro.debuggable=0）上 KSU 的 adb root 是否还能用待上机核实（libadbroot 是否依赖 ro.debuggable），这些开发脚本只保证在开发构建上可用"
S "mkdir -p $D"
adb -s "$SER" push "$TOOL" $D/gaokun3-loopback >/dev/null && S "chmod 755 $D/gaokun3-loopback"
VOL0=$(S "cmd media_session volume --stream 3 --get" | grep -o 'volume is [0-9]*' | grep -o '[0-9]*$')
echo "   媒体音量：${VOL0:-?}（结束时恢复）"

hal_sha() {
    local s i
    for i in 1 2 3 4 5; do
        s=$(S "p=\$(pidof android.hardware.audio.service-aidl.example) && sha256sum /proc/\$p/exe | cut -c1-16")
        [ -n "$s" ] && { echo "$s"; return; }
        sleep 2
    done
}
restart_audio() {
    S "kill \$(pidof android.hardware.audio.service-aidl.example) 2>/dev/null; sleep 1; kill \$(pidof audioserver) 2>/dev/null; sleep 6"
    S "pidof android.hardware.audio.service-aidl.example >/dev/null && pidof audioserver >/dev/null" || die "音频服务没起来（重启设备即可恢复）"
    sleep 3
}
set_vol() { [ -n "${VOL:-}" ] && S "cmd media_session volume --stream 3 --set $VOL" >/dev/null; true; }
cleanup() {
    S "kill \$(pidof android.hardware.audio.service-aidl.example) 2>/dev/null
       nsenter -t 1 -m -- umount $HAL_ON_DEV 2>/dev/null || nsenter -t 1 -m -- umount -l $HAL_ON_DEV 2>/dev/null; true" >/dev/null
    S "sleep 1; kill \$(pidof audioserver) 2>/dev/null; sleep 6" >/dev/null
    [ -n "${VOL0:-}" ] && S "cmd media_session volume --stream 3 --set $VOL0" >/dev/null
}

run_set() {  # $1 = 标签
    local tag=$1
    set_vol
    echo "── ${tag}（HAL $(hal_sha)）" | tee -a "$OUT/summary.txt"
    S "for i in \$(seq 1 $RUNS); do sleep 5; $D/gaokun3-loopback -t 8 -a $AMP | grep -E 'RESULT|^  '; done" > "$OUT/$tag-runs.txt"
    grep RESULT "$OUT/$tag-runs.txt" | sed 's/^/   /' | tee -a "$OUT/summary.txt"
    grep -o 'e_median=[-0-9.]*' "$OUT/$tag-runs.txt" | cut -d= -f2 \
        | awk '{if(NR==1){mn=$1;mx=$1} if($1<mn)mn=$1; if($1>mx)mx=$1} END{printf "   开流之间 e 中位数的极差 %.1f ms（%.1f … %.1f）\n", mx-mn, mn, mx}' | tee -a "$OUT/summary.txt"
    S "sleep 5; p=\$(pidof audioserver); ( sleep 5; kill -STOP \$p; sleep 0.2; kill -CONT \$p; sleep 5; kill -STOP \$p; sleep 0.2; kill -CONT \$p ) & $D/gaokun3-loopback -t 16 -a $AMP; wait; kill -CONT \$p" > "$OUT/$tag-stall.txt"
    # 每声的 e 在 "  " 开头那一行；第 5 / 10 s 停顿，每 0.5 s 一声 ⇒ 前 8 声在第一次停顿前、后 8 声在第二次停顿后
    awk '/^  /{n=split($0,a," "); m=0; for(i=1;i<=n;i++) if(a[i]!="×") v[++m]=a[i];
         b=0; for(i=2;i<=8&&i<=m;i++) b+=v[i]; b/=7; c=0; for(i=m-7;i<=m;i++) c+=v[i]; c/=8;
         printf "   卡顿 ×2：前（第 2–8 声）%.1f ms → 后（最后 8 声）%.1f ms，变化 %+.1f ms\n", b, c, c-b}' "$OUT/$tag-stall.txt" | tee -a "$OUT/summary.txt"
    grep RESULT "$OUT/$tag-stall.txt" | sed 's/^/   卡顿那一次：/' | tee -a "$OUT/summary.txt"
}

trap cleanup EXIT
say "1. 镜像自带的 HAL"
restart_audio
run_set image

say "2. 对照 HAL（bind-mount）"
adb -s "$SER" push "$ALT" $D/hal.bin >/dev/null
S "chmod 755 $D/hal.bin; chcon \$(ls -Z $HAL_ON_DEV | cut -d' ' -f1) $D/hal.bin"
S "nsenter -t 1 -m -- mount --bind $D/hal.bin $HAL_ON_DEV" || die "bind-mount 失败"
restart_audio
[ "$(hal_sha)" = "$(shasum -a 256 "$ALT" | cut -c1-16)" ] || die "对照 HAL 没换上"
run_set alt

say "3. 收尾"
trap - EXIT
cleanup
echo "   HAL 现在：$(hal_sha) · 媒体音量：$(S "cmd media_session volume --stream 3 --get" | grep -o 'volume is [0-9]*')"
echo; echo "结果在 $OUT/（summary.txt）"
