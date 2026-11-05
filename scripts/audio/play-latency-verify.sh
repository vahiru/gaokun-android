#!/usr/bin/env bash
# 在真机上验证播放时间戳的修复（patches/0069，见 docs/dev/audio-video.md）——
# 【不刷机、不重启、不出声】：bind-mount 换上带补丁的 HAL 二进制，重启音频 HAL 与 audioserver，
# 用 gaokun3-play-probe（播全零）从应用视角开流 N 次，最后撤掉 bind-mount、再重启两个服务。设备重启也会回到原样。
#
#   SER=<ip>:5555 bash scripts/audio/play-latency-verify.sh <带补丁的 HAL 二进制> [gaokun3-play-probe]
#   可选环境变量：N=<每份 HAL 开流次数，默认 16>
#
#   HAL 二进制：构建机上 crdroid-tree-fixes.py 打好 [17] 之后 `m com.android.hardware.audio`，取
#     out/target/product/gaokun3/apex/com.android.hardware.audio/bin/hw/android.hardware.audio.service-aidl.example
#   gaokun3-play-probe（device/huawei/gaokun3/audio/tools/play-probe.c）用 NDK 编：
#     $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang -O2 -Wall -Werror \
#         -o gaokun3-play-probe play-probe.c -laaudio -lm
#
# 判据（每份 HAL 各一组）：
#   * ★ 卡顿：播放中途停 audioserver 0.2 s。原版 HAL 按墙钟追赶、扔掉 2–3 整块（logcat "skipping transfer"），
#     之后应用帧号与硬件永久错开 170–256 ms（U 永久跳一截）；修好后 skipping = 0、卡顿前后 U 不变（只多一段欠载静音）。
#   * 每次开流的 U 中位数（时间戳说已播的帧 − 同一时刻 ALSA hw_ptr，ms）。⚠️ 探针假设"应用第 0 帧 = 硬件第 0 帧"，
#     而 AudioFlinger 有时在轨道填满前先写一块别的 ⇒ 开流之间差整块（85.3 ms）是这个假设的假象，不是时间戳的错
#     （#130 §3）。所以按 85.3 ms 折叠后再比：折叠后的极差才是开流之间时间戳真正的差别。修好后 U 折叠值应 ≈ 0
#     （位置按 hw_ptr 上报）；U 的绝对值不是出声延迟（hw_ptr 本身领先 DAC，DSP 预读）。
#   * 流内：跳变 > 5 ms 的次数；logcat 里 incomplete data sent / skipping transfer / error writing into ALSA 的条数。
# ⚠️ bind-mount 要做在 init 的挂载命名空间里（nsenter -t 1 -m），否则 init 拉起的 HAL / audioserver 看不见。
set -uo pipefail
SER=${SER:?SER=<ip>:5555}
HAL=${1:?用法：$0 <带补丁的 HAL 二进制> [gaokun3-play-probe]}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
PROBE=${2:-$REPO/out/playprobe/gaokun3-play-probe}
N=${N:-16}
OUT=$REPO/out/playprobe/$(date +%Y%m%d-%H%M%S)
D=/data/local/tmp/playprobe
HAL_ON_DEV=/apex/com.android.hardware.audio/bin/hw/android.hardware.audio.service-aidl.example
S() { adb -s "$SER" shell "$@" | tr -d '\r'; }
die() { echo "✗ $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }
mkdir -p "$OUT"

[ -f "$HAL" ] && [ -f "$PROBE" ] || die "缺文件：$HAL / $PROBE"
S true >/dev/null 2>&1 || die "adb 连不上 $SER"
[ "$(S getprop ro.crdroid.device)" = gaokun3 ] || die "$SER 不是 gaokun3"
[ "$(S id -u)" = 0 ] || die "需要 root：开发构建 adb shell setprop service.adb.root 1 && adb root；发布构建（ro.debuggable=0）上 KSU 的 adb root 是否还能用待上机核实（libadbroot 是否依赖 ro.debuggable），这些开发脚本只保证在开发构建上可用"
S "mkdir -p $D"
adb -s "$SER" push "$PROBE" $D/gaokun3-play-probe >/dev/null && S "chmod 755 $D/gaokun3-play-probe"

restart_audio() {
    S "kill -CONT \$(pidof android.hardware.audio.service-aidl.example) \$(pidof audioserver) 2>/dev/null; kill \$(pidof android.hardware.audio.service-aidl.example) 2>/dev/null; sleep 1; kill \$(pidof audioserver) 2>/dev/null; sleep 4"
    S "pidof android.hardware.audio.service-aidl.example >/dev/null && pidof audioserver >/dev/null" \
        || { sleep 4; S "pidof android.hardware.audio.service-aidl.example >/dev/null && pidof audioserver >/dev/null" || die "音频服务没起来（重启设备即可恢复）"; }
    sleep 4  # 让 audioserver 把策略、输出线程都建好再开流
}
# HAL 刚被 init 拉起时 pidof 可能为空 —— 空串也"不等于原版"，上一版脚本就这样把没换上的 HAL 当成换上了（#130 §4）
hal_exe_sha() {
    local s i
    for i in 1 2 3 4 5; do
        s=$(S "p=\$(pidof android.hardware.audio.service-aidl.example) && sha256sum /proc/\$p/exe | cut -c1-16")
        [ -n "$s" ] && { echo "$s"; return; }
        sleep 2
    done
}
undo() {
    # 先停 HAL 再卸：HAL 正从被 bind 的文件上跑着，普通 umount 会 EBUSY（mic-verify.sh 2026-09-28 踩过）
    S "kill \$(pidof android.hardware.audio.service-aidl.example) 2>/dev/null
       nsenter -t 1 -m -- umount $HAL_ON_DEV 2>/dev/null || nsenter -t 1 -m -- umount -l $HAL_ON_DEV 2>/dev/null; true" >/dev/null
    restart_audio
}

run_set() {  # $1 = 标签
    local tag=$1 f="$OUT/$1.txt"
    S "logcat -c"
    echo "   $N 次开流（每次 6 s，间隔 5 s 让输出线程 standby）…"
    S "for i in \$(seq 1 $N); do sleep 5; $D/gaokun3-play-probe -t 6 | grep RESULT; done" > "$f"
    echo "   卡顿：播放第 4 s 停 audioserver 0.2 s…"
    S "sleep 5; p=\$(pidof audioserver); ( sleep 4; kill -STOP \$p; sleep 0.2; kill -CONT \$p ) & $D/gaokun3-play-probe -t 10; wait; kill -CONT \$p" > "$OUT/$tag-stall.txt"
    S "logcat -d" > "$OUT/$tag-logcat.txt"
    local us; us=$(grep -o 'U_median=[-0-9.]*' "$f" | cut -d= -f2)
    echo "   每次开流的 U 中位数（ms）：$(echo $us | tr '\n' ' ')" | tee -a "$OUT/summary.txt"
    # 按一块（4096 帧 @ 48 kHz = 85.333 ms）折叠到 (-42.7, 42.7]
    echo "$us" | awk -v t="$tag" '{b=85.3333; v=$1-b*int($1/b); if(v>b/2)v-=b; if(v<=-b/2)v+=b; if(NR==1){mn=v;mx=v} if(v<mn)mn=v; if(v>mx)mx=v} END{printf "   %s：%d 次 · 按整块折叠后 %.1f … %.1f ms（极差 %.1f）\n", t, NR, mn, mx, mx-mn}' | tee -a "$OUT/summary.txt"
    echo "   流内跳变 >5ms：$(grep -o 'jumps=[0-9]*' "$f" | cut -d= -f2 | sort -n | uniq -c | awk '{printf "%s 次×%s ", $2, $1}')" | tee -a "$OUT/summary.txt"
    local st; st=$(grep -E '每秒' -A1 "$OUT/$tag-stall.txt" | tail -1 | tr -s ' ')
    echo "   卡顿那一次（每秒 U，第 4 s 停 0.2 s）：$st" | tee -a "$OUT/summary.txt"
    echo "$st" | awk '{printf "   卡顿前（第 2–4 s）→ 后（第 6–10 s）U 变化 %+.1f ms\n", ($NF+$(NF-1))/2-($2+$3)/2}' | tee -a "$OUT/summary.txt"
    echo "   logcat：incomplete $(grep -c 'incomplete data sent' "$OUT/$tag-logcat.txt") · skipping $(grep -c 'skipping transfer' "$OUT/$tag-logcat.txt") · ALSA 写错误 $(grep -c 'error writing into ALSA' "$OUT/$tag-logcat.txt") · HAL 崩溃 $(grep -cE 'F DEBUG|Fatal signal.*audio' "$OUT/$tag-logcat.txt")" | tee -a "$OUT/summary.txt"
}

say "0. 设备（只读）"
S "getprop ro.build.version.incremental; getprop ro.boot.slot_suffix; cat /proc/asound/card0/pcm1p/sub0/status | head -1" | sed 's/^/   /'
ORIG_HAL=$(hal_exe_sha)

[ -n "$ORIG_HAL" ] || die "读不到当前 HAL 的指纹"
say "1. 原版 HAL（${ORIG_HAL}）" | tee -a "$OUT/summary.txt"
run_set orig

say "2. 换上带补丁的 HAL（bind-mount，不改盘上任何东西）"
trap undo EXIT
adb -s "$SER" push "$HAL" $D/hal.bin >/dev/null
# toybox 的 chcon 没有 --reference：按 ls -Z 取原文件的标签（mic-verify.sh 那一行一直是静默失败的）
S "chmod 755 $D/hal.bin; chcon \$(ls -Z $HAL_ON_DEV | cut -d' ' -f1) $D/hal.bin"
S "nsenter -t 1 -m -- mount --bind $D/hal.bin $HAL_ON_DEV" || die "bind-mount 失败"
restart_audio
NEW_HAL=$(hal_exe_sha)
WANT_HAL=$(shasum -a 256 "$HAL" | cut -c1-16)
[ "$NEW_HAL" = "$WANT_HAL" ] || die "HAL 没换上：在跑的是 '${NEW_HAL}'，要的是 ${WANT_HAL}"
say "3. 带补丁的 HAL（${NEW_HAL}）" | tee -a "$OUT/summary.txt"
run_set patched

say "4. 撤掉 bind-mount、重启音频服务 → 回到原样"
trap - EXIT
undo
[ "$(hal_exe_sha)" = "$ORIG_HAL" ] && echo "   ✓ HAL 已回到原版" || echo "   ⚠️ HAL 的 sha 不是原版 —— 重启设备即可恢复"
echo; echo "结果在 $OUT/（summary.txt）"
