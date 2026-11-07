#!/usr/bin/env bash
# 真实游戏里的播放回归（patches/android/hardware-interfaces/0006，见 docs/dev/audio-video.md）——【不出声】：媒体音量调 0（混音照走 HAL，只是内容为零），
# 原版 HAL 与带补丁的 HAL（bind-mount，不刷机不重启）各跑一遍同样的剧本，比 AudioFlinger 自己的时间戳统计与 HAL 日志。
#
#   SER=<adb 序列号> bash scripts/audio/game-audio-check.sh <带补丁的 HAL 二进制> [包名，默认 moe.low.arc]
#
# 剧本（每份 HAL）：重启音频服务（AF 统计清零）→ 冷启动游戏 → 菜单音乐跑 60 s → 切后台 / 切回 ×2（暂停-继续）
#   → 再跑 20 s → 停 audioserver 0.2 s ×3（模拟游戏负载下 AF 晚到）→ 再跑 20 s → 采样。
# 判据：
#   * AudioOut_D 的 `Timestamp stats`：disc（时间戳不连续次数）、jitterMs 的 std / min / max。原版扔一块 = jitter 一个 −85 ms 的点；
#   * HAL 日志：skipping transfer（原版扔块）/ incomplete data sent / error writing into ALSA（补丁版写失败）/ 崩溃；
#   * 游戏的轨道 Underruns、AF 的 `Delayed writes`；游戏进程还活着。
# ⚠️ 要先解锁；脚本结束会恢复音量、撤掉 bind-mount、关掉游戏。设备重启也会回到原样。
set -uo pipefail
SER=${SER:?SER=<adb 序列号>}
HAL=${1:?用法：$0 <带补丁的 HAL 二进制> [包名]}
PKG=${2:-moe.low.arc}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$REPO/out/playprobe/game-$(date +%Y%m%d-%H%M%S)
D=/data/local/tmp/playprobe
HAL_ON_DEV=/apex/com.android.hardware.audio/bin/hw/android.hardware.audio.service-aidl.example
S() { adb -s "$SER" shell "$@" | tr -d '\r'; }
die() { echo "✗ $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }
mkdir -p "$OUT"

[ -f "$HAL" ] || die "缺 $HAL"
[ "$(S getprop ro.crdroid.device)" = gaokun3 ] || die "$SER 不是 gaokun3"
[ "$(S id -u)" = 0 ] || die "需要 root：开发构建 adb shell setprop service.adb.root 1 && adb root；发布构建（ro.debuggable=0）上 KSU 的 adb root 是否还能用待上机核实（libadbroot 是否依赖 ro.debuggable），这些开发脚本只保证在开发构建上可用"
S "pm path $PKG" | grep -q package: || die "设备上没有 $PKG"
S "dumpsys window | grep -q 'mDreamingLockscreen=false'" || echo "   ⚠️ 看起来没解锁（mDreamingLockscreen 不是 false），游戏可能起不来"

VOL0=$(S "cmd media_session volume --stream 3 --get" | grep -o 'volume is [0-9]*' | grep -o '[0-9]*$')
echo "   原来的媒体音量：${VOL0:-?}"
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
}
silence() { S "cmd media_session volume --stream 3 --set 0" >/dev/null; }
cleanup() {
    S "am force-stop $PKG; kill \$(pidof android.hardware.audio.service-aidl.example) 2>/dev/null
       nsenter -t 1 -m -- umount $HAL_ON_DEV 2>/dev/null || nsenter -t 1 -m -- umount -l $HAL_ON_DEV 2>/dev/null; true" >/dev/null
    S "sleep 1; kill \$(pidof audioserver) 2>/dev/null; sleep 6" >/dev/null
    [ -n "${VOL0:-}" ] && S "cmd media_session volume --stream 3 --set $VOL0" >/dev/null
}

play_script() {  # $1 = 标签
    local tag=$1
    S "am force-stop $PKG"
    restart_audio
    silence
    S "logcat -c"
    S "monkey -p $PKG -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1
    echo "   冷启动 ${PKG}，菜单音乐跑 60 s…"; sleep 60
    silence  # 游戏可能自己调过音量
    for i in 1 2; do
        echo "   切后台 / 切回（${i}）…"
        S "input keyevent KEYCODE_HOME"; sleep 5
        S "monkey -p $PKG -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1; sleep 10
    done
    sleep 20
    echo "   停 audioserver 0.2 s ×3…"
    S "p=\$(pidof audioserver); for i in 1 2 3; do kill -STOP \$p; sleep 0.2; kill -CONT \$p; sleep 4; done"
    sleep 20
    S "dumpsys media.audio_flinger" > "$OUT/$tag-af.txt"
    S "logcat -d" > "$OUT/$tag-logcat.txt"
    S "pidof $PKG" > "$OUT/$tag-pid.txt"
    local thr; thr=$(grep -A60 "name AudioOut_D," "$OUT/$tag-af.txt")
    {
        echo "── ${tag}（HAL $(hal_sha)）"
        echo "   $(echo "$thr" | grep -m1 'Timestamp stats' | sed 's/^ *//')"
        echo "   $(echo "$thr" | grep -m1 'Delayed writes' | sed 's/^ *//') · $(echo "$thr" | grep -m1 'Total writes' | sed 's/^ *//')"
        echo "   游戏轨道（Underruns 列）："; echo "$thr" | grep -E "^ +[0-9]+ +yes" | sed 's/^/     /'
        echo "   HAL 日志：skipping $(LC_ALL=C grep -ac 'skipping transfer' "$OUT/$tag-logcat.txt") · incomplete $(LC_ALL=C grep -ac 'incomplete data sent' "$OUT/$tag-logcat.txt") · ALSA 写错误 $(LC_ALL=C grep -ac 'error writing into ALSA' "$OUT/$tag-logcat.txt") · 崩溃 $(LC_ALL=C grep -acE 'Fatal signal|F DEBUG' "$OUT/$tag-logcat.txt") · HAL 日志总数 $(LC_ALL=C grep -ac 'AHAL_' "$OUT/$tag-logcat.txt")"
        echo "   游戏进程：$( [ -s "$OUT/$tag-pid.txt" ] && echo 活着 || echo ⚠️ 不在了)"
    } | tee -a "$OUT/summary.txt"
}

trap cleanup EXIT
say "1. 原版 HAL"
play_script orig

say "2. 带补丁的 HAL（bind-mount）"
adb -s "$SER" push "$HAL" $D/hal.bin >/dev/null
S "chmod 755 $D/hal.bin; chcon \$(ls -Z $HAL_ON_DEV | cut -d' ' -f1) $D/hal.bin"
S "nsenter -t 1 -m -- mount --bind $D/hal.bin $HAL_ON_DEV" || die "bind-mount 失败"
WANT=$(shasum -a 256 "$HAL" | cut -c1-16)
S "am force-stop $PKG"; restart_audio
[ "$(hal_sha)" = "$WANT" ] || die "HAL 没换上"
play_script patched

say "3. 收尾：关游戏、撤 bind-mount、恢复音量"
trap - EXIT
cleanup
echo "   HAL 现在：$(hal_sha)"
echo; echo "结果在 $OUT/（summary.txt）"
