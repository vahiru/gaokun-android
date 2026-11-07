#!/usr/bin/env bash
# 在真机上验证内置麦克风的修复（见 docs/dev/audio-video.md）——
# 【不刷机、不重启】：bind-mount 换上带 0051/0052 的 HAL 二进制与仓库里的策略 XML，重启音频 HAL 与 audioserver，
# 用 gaokun3-mic-smoke 从应用视角录几段，最后撤掉 bind-mount、再重启两个服务。设备重启也会回到原样。
#
#   SER=<ip>:5555 bash scripts/audio/mic-verify.sh <带补丁的 HAL 二进制> [gaokun3-mic-smoke]
#   可选环境变量：
#     HAL_B=<第二份 HAL>  第一份录完后换上第二份，同样的用例再录一遍（A/B，#127 §6：H1 = 0051+0052，H2 = 再加 0063）
#     EXTRA=1             每份带补丁的 HAL 额外跑三组（都不出声）：120 s 长录、启停 20 次、HAL 进程停顿 0.12 s / 0.3 s
#     EXTRA=stall         只跑停顿那两组
#   ⚠️ 停顿实验有两种，意义不同：
#     * 停 HAL 进程：in_0 正阻塞在 pcm_read 里时被信号打断，读返回不完整（sound/core/pcm_lib.c 返回已传帧数）→ tinyalsa 报 -EIO →
#       proxy 整块重读，已拷的 L 帧（0 ≤ L < 4096）静默丢掉。丢帧以后按帧号算的 D / E 整体抬高 L/48 ms，像延迟、其实是丢帧；
#       0.3 s 还超过 ALSA 环（160 ms）会被覆盖。这一组只检查"停/续之后不挂死、洞数"：H2 超过 3 块期限（256 ms）时允许 1 个洞。
#     * 停 audioserver：HAL 的两个线程都照常跑、不丢帧，积压留在管道（2 块）和 ALSA（160 ms）里 ——
#       这一组的 D 才是"读端被拖住之后积压能不能追回来"的判据：H1 应 skipping ≥ 1、之后 D 永久抬高；H2 应 0 条 incomplete / skipping、
#       之后 D 回到停顿前的水平。停 0.12 s（辅助：H1 会不会触发取决于相位）与 0.2 s（主判据；超过约 0.24 s 写端节流会拖到 ALSA 溢出边缘）。
#     两组都数 framework 侧的丢帧（RecordThread buffer overflow / overrun on read from pipe）：非 0 时这一轮的 D / E 不作数。
#   每个用例都带 -T（mic-smoke 的计时：交付延迟 D、时间戳误差 E），结果表后面附 D / E 的中位数。
#
#   HAL 二进制：构建机上 crdroid-tree-fixes.py 打好 [13]/[14] 之后 `m com.android.hardware.audio`，取
#     out/target/product/gaokun3/apex/com.android.hardware.audio/bin/hw/android.hardware.audio.service-aidl.example
#   gaokun3-mic-smoke（device/huawei/gaokun3/audio/tools/mic-smoke.c）：`m gaokun3-mic-smoke`，或直接用 NDK：
#     $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang -O2 -Wall -Werror \
#         -o gaokun3-mic-smoke mic-smoke.c -laaudio -lm
#
# ★ 顺序：先在【原版】上录一遍（复现 44 字节空录音 —— 基线不对，后面的"修好了"就没有意义），再换、再录。
# ★ 录音不出声，不打扰旁人（MEMORY：宿舍环境）；录下的 WAV 留在设备的 /data/local/tmp/micverify/，拉回本机 out/micverify/。
# ⚠️ bind-mount 要做在 init 的挂载命名空间里（nsenter -t 1 -m），否则 init 拉起的 HAL / audioserver 看不见。
set -uo pipefail
SER=${SER:?SER=<ip>:5555}
HAL=${1:?用法：$0 <带补丁的 HAL 二进制> [gaokun3-mic-smoke]}
SMOKE=${2:-$(cd "$(dirname "$0")/../.." && pwd)/out/micverify/gaokun3-mic-smoke}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
XML=$REPO/device/huawei/gaokun3/audio/primary_audio_policy_configuration.xml
OUT=$REPO/out/micverify/$(date +%Y%m%d-%H%M%S)
D=/data/local/tmp/micverify
HAL_ON_DEV=/apex/com.android.hardware.audio/bin/hw/android.hardware.audio.service-aidl.example
XML_ON_DEV=/vendor/etc/primary_audio_policy_configuration.xml
S() { adb -s "$SER" shell "$@" | tr -d '\r'; }
die() { echo "✗ $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }
mkdir -p "$OUT"

[ -f "$HAL" ] && [ -f "$SMOKE" ] && [ -f "$XML" ] || die "缺文件：$HAL / $SMOKE / $XML"
S true >/dev/null 2>&1 || die "adb 连不上 $SER"
[ "$(S getprop ro.crdroid.device)" = gaokun3 ] || die "$SER 不是 gaokun3"
[ "$(S id -u)" = 0 ] || die "需要 root：开发构建 adb shell setprop service.adb.root 1 && adb root；发布构建（ro.debuggable=0）上 KSU 的 adb root 是否还能用待上机核实（libadbroot 是否依赖 ro.debuggable），这些开发脚本只保证在开发构建上可用"

say "0. 设备与硬件的事实（只读）"
S "getprop ro.build.version.incremental; getprop ro.boot.slot_suffix; getenforce" | sed 's/^/   /'
for d in 2 3; do echo "   tinypcminfo -D 0 -d $d:"; S "tinypcminfo -D 0 -d $d 2>&1 | grep -iE 'channels|rate|capture' | head -6" | sed 's/^/     /'; done
S "ls -la $HAL_ON_DEV $XML_ON_DEV" | sed 's/^/   /'

adb -s "$SER" push "$SMOKE" $D/gaokun3-mic-smoke >/dev/null 2>&1 || { S "mkdir -p $D"; adb -s "$SER" push "$SMOKE" $D/gaokun3-mic-smoke >/dev/null; }
S "chmod 755 $D/gaokun3-mic-smoke"

# 录音矩阵：系统录音机那种（44.1k 单声道）、原生（48k 双声道）、VoIP（16k 单声道 communication）、AAudio 独占低延迟（原先会去 MMAP 的 stub）
CASES=(
  "rec441m|-r 44100 -c 1 -t 5 -T"
  "raw48s|-r 48000 -c 2 -t 5 -T"
  "voip16m|-r 16000 -c 1 -p communication -t 5 -T"
  "lowlat48m|-r 48000 -c 1 -m lowlat -s exclusive -t 5 -T"
)
run_cases() {   # $1=标签（before/after）
    local tag=$1 c name args
    S "logcat -c" >/dev/null
    for c in "${CASES[@]}"; do
        name=${c%%|*}; args=${c#*|}
        echo "   ── ${name}（${args}）" | tee -a "$OUT/$tag.txt"
        S "$D/gaokun3-mic-smoke $args $D/$tag-$name.wav 2>&1" | sed 's/^/     /' | tee -a "$OUT/$tag.txt"
    done
    S "logcat -d" > "$OUT/$tag-logcat.txt"
    echo "   logcat 里的关键行："
    grep -E 'getCardAndDeviceId|MonoPipe capacity|incomplete data|cannot set hw params|pcm_is_ready|proxy_open|MMAP|mmap' "$OUT/$tag-logcat.txt" | sed 's/^/     /' | tail -20
    S "dumpsys media.audio_flinger 2>/dev/null | grep -E '^Output thread|^Input thread|Latency|HAL frame count|Sample rate' | head -30" > "$OUT/$tag-flinger.txt"
}
logcat_count() {   # $1=文件 → "incomplete N · skipping N · ALSA 读错 N · framework 丢帧 N"
    printf 'incomplete %s · skipping %s · ALSA 读错 %s · framework 丢帧 %s' "$(grep -c 'incomplete data received' "$1")" \
        "$(grep -c 'skipping transfer' "$1")" "$(grep -c 'Error reading from ALSA' "$1")" \
        "$(grep -cE 'buffer overflow|overrun on read from pipe' "$1")"
}
run_extra() {   # $1=标签；都不出声
    local tag=$1 f
    [ "${EXTRA}" = stall ] || run_long_and_startstop $tag
    run_stall $tag
}
run_long_and_startstop() {
    local tag=$1
    echo "   ── ${tag}：120 s 长录（-r 48000 -c 2 -t 120 -T）" | tee -a "$OUT/$tag.txt"
    S "logcat -c" >/dev/null
    S "$D/gaokun3-mic-smoke -r 48000 -c 2 -t 120 -T 2>&1" | sed 's/^/     /' | tee -a "$OUT/$tag.txt" | grep -E 'RESULT|TIMING: (偏移|交付|时间戳|ADC|ALSA)'
    S "logcat -d" > "$OUT/$tag-long-logcat.txt"; echo "     logcat：$(logcat_count "$OUT/$tag-long-logcat.txt")" | tee -a "$OUT/$tag.txt"

    echo "   ── ${tag}：启停 20 次（每次 1 s）" | tee -a "$OUT/$tag.txt"
    S "logcat -c" >/dev/null
    S "for i in \$(seq 1 20); do $D/gaokun3-mic-smoke -r 48000 -c 2 -t 1 -T 2>&1 | grep -E '^RESULT|第一次含真实数据'; done" > "$OUT/$tag-startstop.txt"
    S "logcat -d" > "$OUT/$tag-startstop-logcat.txt"
    echo "     $(grep -c 'RESULT: PASS' "$OUT/$tag-startstop.txt")/20 PASS；开头静音：$(grep -o '开头静音 [0-9]* ms' "$OUT/$tag-startstop.txt" | awk '{print $2}' | sort -n | uniq -c | awk '{printf "%s ms×%s ", $2, $1}')" | tee -a "$OUT/$tag.txt"
    echo "     首块到达（HAL 日志）：$(grep -o 'first capture block [a-z ]*[0-9]* ms' "$OUT/$tag-startstop-logcat.txt" | awk '{print $(NF-1)}' | sort -n | tr '\n' ' ')" | tee -a "$OUT/$tag.txt"
    echo "     logcat：$(logcat_count "$OUT/$tag-startstop-logcat.txt")" | tee -a "$OUT/$tag.txt"
}
run_stall() {
    local tag=$1
    echo "   ── ${tag}：HAL 进程停顿（第 8 s 停 0.12 s、第 14 s 停 0.3 s；-t 20 -T）" | tee -a "$OUT/$tag.txt"
    S "logcat -c" >/dev/null
    S "p=\$(pidof android.hardware.audio.service-aidl.example); ( sleep 8; kill -STOP \$p; sleep 0.12; kill -CONT \$p; sleep 6; kill -STOP \$p; sleep 0.3; kill -CONT \$p ) & $D/gaokun3-mic-smoke -r 48000 -c 2 -t 20 -T 2>&1; wait; kill -CONT \$p" \
        | sed 's/^/     /' | tee -a "$OUT/$tag.txt" | grep -E 'RESULT|开头静音|TIMING: (偏移|ALSA 积压 分)'
    S "logcat -d" > "$OUT/$tag-stall-logcat.txt"; echo "     logcat：$(logcat_count "$OUT/$tag-stall-logcat.txt")" | tee -a "$OUT/$tag.txt"

    echo "   ── ${tag}：audioserver 停顿（第 8 s 停 0.12 s、第 14 s 停 0.2 s；-t 20 -T）" | tee -a "$OUT/$tag.txt"
    S "logcat -c" >/dev/null
    S "p=\$(pidof audioserver); ( sleep 8; kill -STOP \$p; sleep 0.12; kill -CONT \$p; sleep 6; kill -STOP \$p; sleep 0.2; kill -CONT \$p ) & $D/gaokun3-mic-smoke -r 48000 -c 2 -t 20 -T 2>&1; wait; kill -CONT \$p" \
        | sed 's/^/     /' | tee -a "$OUT/$tag.txt" | grep -E 'RESULT|开头静音|TIMING: (偏移|交付|时间戳|ALSA)'
    S "logcat -d" > "$OUT/$tag-asstall-logcat.txt"; echo "     logcat：$(logcat_count "$OUT/$tag-asstall-logcat.txt")" | tee -a "$OUT/$tag.txt"
}
restart_audio() {
    # init 会把两个都拉起来；HAL 先、audioserver 后（audioserver 起来时去连 HAL）。
    # 先 CONT：停顿实验若中途断了，被 SIGSTOP 的 HAL 收到 TERM 也不会退出，音频就一直是死的
    S "kill -CONT \$(pidof android.hardware.audio.service-aidl.example) \$(pidof audioserver) 2>/dev/null; kill \$(pidof android.hardware.audio.service-aidl.example) 2>/dev/null; sleep 1; kill \$(pidof audioserver) 2>/dev/null; sleep 4"
    S "pidof android.hardware.audio.service-aidl.example >/dev/null && pidof audioserver >/dev/null" \
        || { sleep 4; S "pidof android.hardware.audio.service-aidl.example >/dev/null && pidof audioserver >/dev/null" || die "音频服务没起来（重启设备即可恢复）"; }
}
# 设备上"真正在用"的那份：init 命名空间里的文件 + 正在跑的 HAL 进程的 exe（adb 的命名空间不算数）
dev_sha() { S "nsenter -t 1 -m -- sha256sum $1 | cut -c1-16"; }
hal_exe_sha() { S "sha256sum /proc/\$(pidof android.hardware.audio.service-aidl.example)/exe | cut -c1-16"; }
undo() {
    # ⚠️ 先停 HAL 再卸：HAL 正从被 bind 的那个文件上跑着，普通 umount 会 EBUSY（2026-09-28 第一次跑就这样，
    #    脚本没看出来、还以为撤掉了）。停了 init 也可能立刻把它拉起来，所以再用 umount -l 兜底。
    S "kill -CONT \$(pidof android.hardware.audio.service-aidl.example) 2>/dev/null; kill \$(pidof android.hardware.audio.service-aidl.example) 2>/dev/null
       nsenter -t 1 -m -- umount $HAL_ON_DEV 2>/dev/null || nsenter -t 1 -m -- umount -l $HAL_ON_DEV 2>/dev/null
       nsenter -t 1 -m -- umount $XML_ON_DEV 2>/dev/null || nsenter -t 1 -m -- umount -l $XML_ON_DEV 2>/dev/null; true" >/dev/null
    restart_audio
}

say "1. 基线：原版 HAL + 原版策略"
run_cases before

say "2. 换上带补丁的 HAL 与仓库里的策略 XML（bind-mount，不改盘上任何东西）"
ORIG_HAL=$(dev_sha $HAL_ON_DEV); ORIG_XML=$(dev_sha $XML_ON_DEV)
echo "   设备原版：HAL ${ORIG_HAL} · 策略 XML ${ORIG_XML}"
adb -s "$SER" push "$XML" $D/primary_audio_policy_configuration.xml >/dev/null || die "推 XML 失败"
S "chmod 644 $D/primary_audio_policy_configuration.xml; chcon \$(ls -Z $XML_ON_DEV | cut -d' ' -f1) $D/primary_audio_policy_configuration.xml"
trap undo EXIT
TAGS=()
n=0
for H in "$HAL" ${HAL_B:+"$HAL_B"}; do
    n=$((n + 1)); tag=after; [ $n -gt 1 ] && tag=after-b
    NEW_HAL=$(shasum -a 256 "$H" | cut -c1-16)
    [ "$NEW_HAL" != "$ORIG_HAL" ] || die "设备上跑的已经是这份 HAL（上次没撤干净？先重启设备或手动 umount）"
    [ $n -gt 1 ] && undo   # 先撤掉上一份
    adb -s "$SER" push "$H" $D/hal.bin >/dev/null || die "推 HAL 失败"
    S "chmod 755 $D/hal.bin; chcon \$(ls -Z $HAL_ON_DEV | cut -d' ' -f1) $D/hal.bin"
    S "nsenter -t 1 -m -- mount --bind $D/hal.bin $HAL_ON_DEV && nsenter -t 1 -m -- mount --bind $D/primary_audio_policy_configuration.xml $XML_ON_DEV" || die "bind-mount 失败"
    restart_audio
    [ "$(hal_exe_sha)" = "$NEW_HAL" ] || die "换上之后跑的 HAL 不是 ${H}（$(hal_exe_sha)）"
    say "3.${n} ${tag}：$(basename "$H")（${NEW_HAL}）"
    run_cases $tag
    case "${EXTRA:-0}" in 1|stall) run_extra $tag ;; esac
    TAGS+=("$tag")
done

say "4. 撤掉 bind-mount、重启音频服务 → 回到原样"
trap - EXIT
undo
H1=$(dev_sha $HAL_ON_DEV); X1=$(dev_sha $XML_ON_DEV); E1=$(hal_exe_sha)
if [ "$H1" = "$ORIG_HAL" ] && [ "$E1" = "$ORIG_HAL" ] && [ "$X1" = "$ORIG_XML" ]; then
    echo "   回到原样 ✓（HAL 文件 / 正在跑的 HAL = ${H1}，XML = ${X1}）"
else
    echo "   ✗ 没回到原样：HAL 文件 ${H1} · 正在跑 ${E1} · XML ${X1}（原版 ${ORIG_HAL} / ${ORIG_XML}）—— 重启设备即可恢复" >&2
    RESTORE_FAIL=1
fi

adb -s "$SER" pull $D "$OUT/wav" >/dev/null 2>&1 && S "rm -f $D/*.wav" >/dev/null
say "结果（${OUT}）"
r() { awk -v n="── ${2}（" 'index($0, n) { f = 1; next } f && /^   ── / { exit } f && /RESULT:/ { sub(/.*RESULT: /, ""); print; p = 1; exit } END { if (!p) print (f ? "（无 RESULT）" : "（没跑）") }' "$OUT/$1.txt"; }
med() {   # $1=标签 $2=用例 $3=交付|时间戳 → 中位数
    awk -v n="── ${2}（" -v k="TIMING: ${3}（" 'index($0, n) { f = 1; next } f && /^   ── / { exit } f && index($0, k) { split($0, a, "= "); split(a[2], b, " / "); print b[2]; exit }' "$OUT/$1.txt"
}
printf '   %-12s %-26s' 用例 before; for t in "${TAGS[@]}"; do printf ' %-30s' "$t"; done; echo
for c in "${CASES[@]}"; do
    name=${c%%|*}
    printf '   %-12s %-26s' "$name" "$(r before "$name")"
    for t in "${TAGS[@]}"; do printf ' %-30s' "$(r $t "$name")"; done; echo
done
echo "   交付延迟 D / 时间戳误差 E 的中位数（ms，只有 48 kHz 用例有）："
for c in "${CASES[@]}"; do
    name=${c%%|*}
    printf '   %-12s before %s / %s' "$name" "$(med before "$name" '交付延迟 D')" "$(med before "$name" '时间戳误差 E')"
    for t in "${TAGS[@]}"; do printf ' · %s %s / %s' "$t" "$(med $t "$name" '交付延迟 D')" "$(med $t "$name" '时间戳误差 E')"; done; echo
done
exit ${RESTORE_FAIL:-0}
