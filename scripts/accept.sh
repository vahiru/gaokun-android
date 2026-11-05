#!/usr/bin/env bash
# 发版装机验收的 A 档（无人值守）一键跑。逐条对应 docs/dev/release-checklist.md 的 A1–A20。
# 在【宿主机】跑，走 adb；以只读检查为主，B / C 档（要人在场、长稳与测量）不在这里。
#
#   SER=gaokun3 bash scripts/accept.sh [选项]
#     --readonly          只做只读检查：跳过相机（A12）、麦克风（A13）与真解视频（A11 的第 5 步）
#     --stamp <戳>        期望的 ro.build.date.utc（候选版的构建戳）；不给就只记录、不判
#     --profile release   默认。A3（G1 安全默认值）不过判 FAIL
#     --profile dev       开发构建（adb 免授权、TCP 5555 是故意的）：A3 只记录
#     --soak <秒>         A15 跑 verify-turnip.sh 时浸泡这么久再复查（PERF-12 的 A 档是 600）
#     --video <片子.mp4>  A11 真解一段（verify-hw-codec2.sh 的第 5 步）
#     --out <目录>        报告目录，默认 out/accept/<戳>-<时间>/
#   退出码：0 = 没有 FAIL（WARN 要人看一眼，但不挡），1 = 有 FAIL，2 = adb 不通 / 参数错 / 中途掉线
#   中途掉线也照样写汇总行，并记一个 A0 FAIL —— 读汇总行的人（release.sh）不会把半截报告当成全绿。
#
# 退出码就是本脚本自己的，报告是另外写进文件的 —— 别 `bash accept.sh | tail` 再取 $?，
#   要看结果就看退出码，或者看报告目录里的 report.txt 最后一行。
# 要 root（qcom_stats、dropbox、/proc/<pid>/maps）：adb 本身是 root 就直接跑；否则走 su -c（KernelSU 要先给 shell 授权）。
# 判据都是这台机器上【实测过】的路径与字符串（2026-10-04，1791053208）；新增检查请同样先在实机上核对再写。
set -u
export MSYS_NO_PATHCONV=1
HERE=$(cd "$(dirname "$0")" && pwd)
SER=${SER:-${SERIAL:-}}
ADB="adb ${SER:+-s $SER}"
RO=0; STAMP=; PROFILE=release; SOAK=0; VIDEO=; OUT=
while [ $# -gt 0 ]; do
    case $1 in
        --readonly) RO=1 ;;
        --stamp)    STAMP=${2:?}; shift ;;
        --profile)  PROFILE=${2:?}; shift ;;
        --soak)     SOAK=${2:?}; shift ;;
        --video)    VIDEO=${2:?}; shift ;;
        --out)      OUT=${2:?}; shift ;;
        *) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
    shift
done
case $PROFILE in release|dev) ;; *) echo "--profile 只认 release / dev"; exit 2 ;; esac

$ADB get-state >/dev/null 2>&1 || { echo "adb 不通（SER=${SER:-未设}）"; exit 2; }
if [ "$($ADB shell id -u 2>/dev/null | tr -d '\r')" = 0 ]; then
    RSH() { $ADB shell "$1"; }
else
    RSH() { printf '%s\n' "$1" | $ADB shell su -c sh; }
fi
R() { RSH "$1" 2>/dev/null | tr -d '\r'; }

BUILD=$(R 'getprop ro.build.date.utc')
OUT=${OUT:-out/accept/${BUILD:-unknown}-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
REPORT=$OUT/report.txt
: > "$REPORT"

NP=0; NF=0; NW=0; NS=0; FAILS=; WARNS=
say()  { echo "$*" | tee -a "$REPORT"; }
pass() { say "  [PASS] $1 $2"; NP=$((NP + 1)); }
fail() { say "  [FAIL] $1 $2"; NF=$((NF + 1)); FAILS="$FAILS $1"; }
warn() { say "  [WARN] $1 $2"; NW=$((NW + 1)); WARNS="$WARNS $1"; }
skip() { say "  [SKIP] $1 $2"; NS=$((NS + 1)); }
info() { say "         $*"; }
# adb 中途掉线：记 A0 FAIL、照样出汇总行，退出 2（不让半截报告看起来是 FAIL 0）
lost() {
    fail A0 "adb 中途掉线（${*}），后面的检查没跑"
    say ""
    say "═══ 汇总：PASS $NP · FAIL $NF · WARN $NW · SKIP $NS · 中断（adb 掉线）═══"
    say "报告：$REPORT"
    exit 2
}
sect() { $ADB get-state >/dev/null 2>&1 || lost "进 ${1} 之前"; say ""; say "── $* ──"; }
# 设备上读回的整数：空串或非数字 = adb 掉线（bash 3.2 下空值进 $(( )) 是语法错误，脚本直接以 1 退出、不出汇总行）
num()  { case $1 in ''|*[!0-9-]*) lost "${2} 读回 [${1}]" ;; esac; }
# 判断等式：chk ID 实际值 期望值 说明
chk()  { if [ "$2" = "$3" ]; then pass "$1" "$4 = $2"; else fail "$1" "$4 = [$2]，应为 $3"; fi; }

say "gaokun3 装机验收 A 档 · $(date '+%F %T') · SER=${SER:-默认} · profile=$PROFILE$([ $RO = 1 ] && echo ' · 只读')"
say "报告目录：$OUT"

# ── A1 构建与内核 ──
sect "A1 构建与内核"
INC=$(R 'getprop ro.build.version.incremental')
if [ -n "$STAMP" ]; then chk A1 "$BUILD" "$STAMP" "ro.build.date.utc"
else pass A1 "ro.build.date.utc = ${BUILD}（没给 --stamp，只记录）"; fi
info "incremental = ${INC}，槽位 = $(R 'getprop ro.boot.slot_suffix')"
info "$(R 'cat /proc/version' | cut -c1-160)"
chk A1 "$(R 'getprop ro.build.characteristics')" tablet "ro.build.characteristics"

# ── A2 开机状态 ──
sect "A2 开机状态"
UP=$(R 'cut -d. -f1 /proc/uptime')
chk A2 "$(R 'getprop sys.boot_completed')" 1 "sys.boot_completed"
if [ "${UP:-0}" -ge 600 ]; then pass A2 "已开机 $((UP / 60)) 分钟"
else warn A2 "才开机 $((UP / 60)) 分钟：denial 普查与 GPU 判据要开机 ≥10 分钟才算数，建议稍后重跑"; fi
info "sys.boot.reason = $(R 'getprop sys.boot.reason')；history：$(R 'getprop persist.sys.boot.reason.history' | tr '\n' ' ')"

# ── A3 发布构建的安全默认值（v1.0-plan G1 / 决定 D1）──
sect "A3 安全默认值（G1）"
a3() {  # ID 实际 期望 说明：release 判 FAIL，dev 只记录
    if [ "$2" = "$3" ]; then pass A3 "$4 = [$2]"
    elif [ "$PROFILE" = dev ]; then info "（dev）$4 = [$2]，发布构建应为 [$3]"
    else fail A3 "$4 = [$2]，发布构建应为 [$3]"; fi
}
a3 A3 "$(R 'getprop ro.adb.secure')" 1 "ro.adb.secure"
a3 A3 "$(R 'getprop ro.debuggable')" 0 "ro.debuggable"
a3 A3 "$(R 'getprop persist.adb.tcp.port')" "" "persist.adb.tcp.port"
# 5555 = 0x15B3，状态 0A = LISTEN
a3 A3 "$(R 'cat /proc/net/tcp /proc/net/tcp6 2>/dev/null | awk "\$2 ~ /:15B3\$/ && \$4 == \"0A\"" | wc -l' | tr -d ' ')" 0 "TCP 5555 监听数"
a3 A3 "$(R 'test -e /product/etc/security/adb_keys && echo 在 || echo 无')" 无 "/product/etc/security/adb_keys"

# ── A4 待机默认值（S1）──
sect "A4 待机默认值（S1）"
chk A4 "$(R 'grep "^persist.vendor.gaokun3.allow_suspend=" /vendor/build.prop | cut -d= -f2')" 1 "镜像默认 allow_suspend（/vendor/build.prop）"
info "设备当前 persist.vendor.gaokun3.allow_suspend = [$(R 'getprop persist.vendor.gaokun3.allow_suspend')]（开发机持久 0 是故意的）"

# ── A5 自研服务与 SELinux 域 ──
sect "A5 自研服务与域"
for s in smmustall hangdump vendor.usb-gaokun3 vendor.boot-gaokun3 vendor.camera-provider-gaokun3 \
         vendor.sensors-gaokun3 vendor.light-gaokun3 vendor.thermal-gaokun3; do
    chk A5 "$(R "getprop init.svc.$s")" running "init.svc.$s"
done
PSZ=$(R 'ps -AZ -o LABEL,PID,ARGS 2>/dev/null')
for e in "gaokun3_smmustall:smmu-nostall.sh" "hal_usb_default:android.hardware.usb-service.gaokun3"; do
    d=${e%%:*}; c=${e#*:}
    if echo "$PSZ" | grep -F "$c" | grep -q "u:r:$d:s0"; then pass A5 "$c 跑在 u:r:$d:s0"
    else fail A5 "$c 不在 u:r:$d:s0（实际：$(echo "$PSZ" | grep -F "$c" | awk '{print $1}' | head -1)）"; fi
done

# ── A6 三颗 DSP ──
sect "A6 remoteproc"
RP=$(R 'for r in /sys/class/remoteproc/remoteproc*; do echo "$(cat $r/name) $(cat $r/state)"; done')
for n in adsp cdsp slpi; do
    st=$(echo "$RP" | awk -v n=$n '$1 == n {print $2}')
    chk A6 "$st" running "$n"
done

# ── A7 传感器 ──
sect "A7 传感器"
SL=$(R 'dumpsys sensorservice 2>/dev/null | sed -n "/^Sensor List:/,/^Fusion States:/p"')
for s in "SH3001 Accelerometer" "SH3001 Gyroscope"; do
    echo "$SL" | grep -q "$s" && pass A7 "sensorservice 列出 $s" || fail A7 "sensorservice 里没有 ${s}（SSC / SLPI 挂了？）"
done

# ── A8 Wi-Fi / 热点 ──
sect "A8 Wi-Fi / 热点"
# B16 的 RRO（rro/Gaokun3WifiOverlay，target com.android.wifi.resources）：先查资源值，再查活动网络上的实际值
TB=$(R 'cmd overlay lookup com.android.wifi.resources com.android.wifi.resources:string/config_wifi_tcp_buffers')
case $TB in *8388608*) pass A8 "config_wifi_tcp_buffers = $TB" ;; *) fail A8 "config_wifi_tcp_buffers = [$TB]，应含 8388608" ;; esac
TBA=$(R 'dumpsys connectivity 2>/dev/null | grep -m1 -o "TcpBufferSizes: [0-9,]*"')
if [ -z "$TBA" ]; then skip A8 "没有活动网络，dumpsys connectivity 里没有 TcpBufferSizes（连上 Wi-Fi 后重跑这一项）"
else case $TBA in *8388608*) pass A8 "活动网络 $TBA" ;; *) fail A8 "活动网络 ${TBA}，应含 8388608" ;; esac; fi
R 'dumpsys wifi 2>/dev/null' > "$OUT/dumpsys-wifi.txt"
grep -q "Resource Name: config_wifiSaeUpgradeEnabled, value: false" "$OUT/dumpsys-wifi.txt" \
    && pass A8 "config_wifiSaeUpgradeEnabled = false（混合 WPA2/WPA3 路由器）" \
    || fail A8 "config_wifiSaeUpgradeEnabled 不是 false"
chk A8 "$(R 'test -x /vendor/bin/hw/hostapd && echo 在 || echo 无')" 在 "/vendor/bin/hw/hostapd（#11）"
info "$(grep -m1 -E '^Wi-Fi is' "$OUT/dumpsys-wifi.txt")；$(grep -m1 -o 'current SSID(s):{[^}]*}' "$OUT/dumpsys-wifi.txt")"

# ── A9 USB 服务（#13）──
sect "A9 USB 服务"
chk A9 "$(R 'service check usb' | sed 's/.*: //')" found "service usb"
R 'pm list features' | grep -qx "feature:android.hardware.usb.host" && pass A9 "feature android.hardware.usb.host" || fail A9 "缺 feature android.hardware.usb.host"

# ── A10 NTP 与时间 ──
sect "A10 NTP"
NS_=$(R 'settings get global ntp_server')
[ "$NS_" = null ] && pass A10 "ntp_server 没有手动设（走镜像的服务器列表）" || warn A10 "ntp_server = [$NS_]（开发机手动设过？验 B22 时要先 settings delete）"
DS=$(R 'date +%s'); num "$DS" "设备 date +%s"
DT=$(( DS - $(date +%s) )); DT=${DT#-}
if [ "$DT" -le 5 ]; then pass A10 "设备与宿主机时间差 ${DT} 秒"
elif [ "$DT" -le 60 ]; then warn A10 "设备与宿主机时间差 ${DT} 秒"
else fail A10 "设备与宿主机时间差 ${DT} 秒（NTP 没对上）"; fi

# ── A11 硬解 ──
sect "A11 硬件视频解码（verify-hw-codec2.sh）"
V=; [ $RO = 0 ] && [ -n "$VIDEO" ] && V=$VIDEO
SERIAL=$SER bash "$HERE/verify-hw-codec2.sh" $V > "$OUT/verify-hw-codec2.txt" 2>&1; rc=$?
info "$(grep '小结' "$OUT/verify-hw-codec2.txt")"
[ $rc = 0 ] && pass A11 "verify-hw-codec2.sh 全过" || { fail A11 "verify-hw-codec2.sh 有失败项（见 verify-hw-codec2.txt）"; grep FAIL "$OUT/verify-hw-codec2.txt" | sed 's/^/         /' | tee -a "$REPORT"; }
[ -z "$V" ] && skip A11 "没有真解视频（$([ $RO = 1 ] && echo '--readonly' || echo '没给 --video')）—— 组件在列表里不等于能解码"

# ── A12 相机 ──
sect "A12 相机（gaokun3-ncam-smoke）"
if [ $RO = 1 ]; then skip A12 "--readonly：不开相机"
elif [ "$(R 'test -x /data/local/tmp/gaokun3-ncam-smoke && echo y')" != y ]; then
    skip A12 "设备上没有 /data/local/tmp/gaokun3-ncam-smoke（m gaokun3-ncam-smoke 后 push）"
else
    for w in "" front; do
        R "/data/local/tmp/gaokun3-ncam-smoke $w" > "$OUT/ncam-${w:-back}.txt" 2>&1
        r=$(grep -a '^RESULT:' "$OUT/ncam-${w:-back}.txt" | tail -1)
        [ "$r" = "RESULT: PASS" ] && pass A12 "${w:-后摄} $r" || fail A12 "${w:-后摄} ${r:-没有 RESULT 行}（见 ncam-${w:-back}.txt）"
    done
fi

# ── A13 麦克风 ──
sect "A13 麦克风（gaokun3-mic-smoke，不出声、不落文件）"
MS=/data/local/tmp/micverify/gaokun3-mic-smoke
if [ $RO = 1 ]; then skip A13 "--readonly：不录音"
elif [ "$(R "test -x $MS && echo y")" != y ]; then skip A13 "设备上没有 $MS"
else
    T0=$(R 'date "+%m-%d %H:%M:%S.000"')
    R "$MS -r 48000 -c 2 -t 5 -T" > "$OUT/mic-smoke.txt" 2>&1
    r=$(grep -a '^RESULT:' "$OUT/mic-smoke.txt" | tail -1)
    # 成功行是 "RESULT: PASS (开头静音 N ms)"（device/huawei/gaokun3/audio/tools/mic-smoke.c:511），不能精确比较
    case $r in
        "RESULT: PASS"*) pass A13 "$r" ;;
        *) fail A13 "${r:-没有 RESULT 行}（见 mic-smoke.txt）" ;;
    esac
    grep -a -E '开头|TIMING:' "$OUT/mic-smoke.txt" | head -6 | sed 's/^/         /' | tee -a "$REPORT"
    # A17 的 adbd 会把每条 shell 命令原样打进 logcat（"adbd service requested '…raw:<命令>'"），
    # 设备上 grep 关键字会先匹配到这条命令自己 ⇒ 一律先滤掉（10-26 首跑因此误判 A11 / A13）
    LG=$(R "logcat -b all -d -T '$T0' 2>/dev/null | grep -v 'adbd service requested' | grep -E 'first capture block ready|incomplete data received'")
    echo "$LG" | grep -q "first capture block ready" && pass A13 "logcat：$(echo "$LG" | grep -m1 -o 'first capture block ready.*')" || warn A13 "logcat 里没看到 first capture block ready"
    echo "$LG" | grep -q "incomplete data received" && fail A13 "logcat 有 incomplete data received（Issue #9 那种丢块）" || true
fi

# ── A14 扬声器增强（Histen）效果注册 ──
sect "A14 扬声器增强"
EP=$(R 'pidof android.hardware.audio.effect.service-aidl.example')
if [ -z "$EP" ]; then fail A14 "effect HAL 进程不在"
else
    R "grep -c libgaokunhisteneffect.so /proc/${EP%% *}/maps" | grep -qv '^0$' \
        && pass A14 "effect HAL 已加载 libgaokunhisteneffect.so" || fail A14 "effect HAL 的 maps 里没有 libgaokunhisteneffect.so"
fi
# 只数 effect 相关的行（原始报错的形状见 device/huawei/gaokun3/effects/README.md:265-267：soundfx 路径 + effect HAL 进程）；
# App / 游戏自己的 linker 命名空间报错不归这一项，只记一笔
NSRE='soundfx|histen|audio\.effect'
NSL=$(R 'logcat -b all -d 2>/dev/null | grep -v "adbd service requested" | grep "not accessible for the namespace"')
NA=$(printf '%s\n' "$NSL" | grep -ciE "$NSRE"); NO=$(printf '%s\n' "$NSL" | grep -c .)
[ "$NA" = 0 ] && pass A14 "logcat 没有 effect 相关的 not accessible for the namespace" \
    || fail A14 "logcat 有 $NA 行 effect 相关的 not accessible for the namespace（0068 的放行失效？）"
[ "$((NO - NA))" = 0 ] || info "另有 $((NO - NA)) 行别的进程的 namespace 报错（App 自己的，不算 A14），例：$(printf '%s\n' "$NSL" | grep -viE "$NSRE" | head -1 | cut -c1-160)"

# ── A15 GPU ──
# A18 要的是浸泡期间的增量（PERF-12 A 档）：A15 前后各存一份频率 / 温度快照 —— 开机以来的累计值受开机时长影响，跨版本没法比
FREQ_CMD='for p in /sys/devices/system/cpu/cpufreq/policy*; do echo "== $p"; cat $p/stats/time_in_state; done; echo "== gpu"; cat /sys/class/devfreq/3d00000.gpu/trans_stat'
THERM_CMD='for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type) $(cat $z/temp)"; done; for c in /sys/class/thermal/cooling_device*; do echo "$(cat $c/type) cur_state=$(cat $c/cur_state)"; done'
R "$FREQ_CMD" > "$OUT/freq-before.txt"
R "$THERM_CMD" > "$OUT/thermal-before.txt"
T15=$(date +%s)
sect "A15 GPU（verify-turnip.sh$([ "$SOAK" != 0 ] && echo "，浸泡 ${SOAK}s")）"
SER=$SER bash "$HERE/verify-turnip.sh" "$SOAK" > "$OUT/verify-turnip.txt" 2>&1; rc=$?
info "$(grep '小结' "$OUT/verify-turnip.txt")"
[ $rc = 2 ] && lost "verify-turnip.sh 报 adb 不通"
[ $rc = 0 ] && pass A15 "verify-turnip.sh 全过" || { fail A15 "verify-turnip.sh 有失败项（见 verify-turnip.txt）"; grep FAIL "$OUT/verify-turnip.txt" | sed 's/^/         /' | tee -a "$REPORT"; }
R "$FREQ_CMD" > "$OUT/freq-after.txt"
R "$THERM_CMD" > "$OUT/thermal-after.txt"
T15=$(( $(date +%s) - T15 ))
[ "$SOAK" = 0 ] && info "（没浸泡；PERF-12 的 A 档要 --soak 600）"

# ── A16 本次开机以来的崩溃 ──
sect "A16 崩溃（dropbox，本次开机以来）"
# 开机时刻（毫秒）在宿主机上乘：设备的 mksh 算术是 32 位，秒 × 1000 会溢出
BOOTS=$(R 'echo $(( $(date +%s) - $(cut -d. -f1 /proc/uptime) ))'); num "$BOOTS" "开机时刻"
BOOTMS=$(( BOOTS * 1000 ))
R 'ls /data/system/dropbox' > "$OUT/dropbox-ls.txt"
since() { awk -F@ -v t0="$BOOTMS" -v tag="$1" '$1 == tag { split($2, a, "."); if (a[1] + 0 >= t0 + 0) print }' "$OUT/dropbox-ls.txt"; }
N=$(since system_server_crash | wc -l | tr -d ' ')
[ "$N" = 0 ] && pass A16 "system_server_crash 0" || fail A16 "system_server_crash $N 次"
# 按 tombstone 头里 ">>> 进程 <<<" 分三类：
#   测试工具 —— /data/local/tmp 下的：判 WARN
#   App      —— tombstone 的 "uid:" ≥ 10000（应用 / isolated 进程），或 /data/app 下的可执行文件：判 WARN
#   系统进程 —— 其余（镜像里的路径、zygote64、media.codec 这类裸名、读不出来的）：判 FAIL
# 不按"名字带点"认 App：media.codec / media.swcodec 这些系统服务的进程名也带点。
# 头两行的格式见 refs/lineage-system-core/debuggerd/libdebuggerd/tombstone_proto_to_text.cpp:112-114
# B9 冒烟、C1 跑游戏之后再跑 accept 时，游戏自己的 native 崩溃不该被算成系统回归
NT=0; NTT=0; NTA=0
for f in $(since SYSTEM_TOMBSTONE); do
    hd=$(R "(zcat /data/system/dropbox/$f 2>/dev/null || cat /data/system/dropbox/$f) | grep -m2 -oE '>>> .* <<<|^uid: [0-9]+'")
    who=$(echo "$hd" | grep -m1 '^>>> '); w=${who#>>> }; w=${w% <<<}
    uid=$(echo "$hd" | sed -n 's/^uid: //p' | head -1)
    case $w in
        /data/local/tmp/*) NTT=$((NTT + 1)); info "测试工具的 tombstone：$f $who" ;;
        /data/app/*) NTA=$((NTA + 1)); info "App 的 tombstone：$f $who" ;;
        *) if [ "${uid:-0}" -ge 10000 ] 2>/dev/null; then NTA=$((NTA + 1)); info "App 的 tombstone：$f $who uid=$uid"
           else NT=$((NT + 1)); info "系统进程 tombstone：$f ${who:-（读不出进程名，按系统进程算）} uid=${uid:-?}"; fi ;;
    esac
done
[ "$NT" = 0 ] && pass A16 "系统进程 tombstone 0" || fail A16 "系统进程 tombstone $NT 个"
[ "$NTT" = 0 ] || warn A16 "/data/local/tmp 下的测试工具崩了 $NTT 次（工具本身要修稳，不算系统回归）"
[ "$NTA" = 0 ] || warn A16 "App 自己的 native 崩溃 $NTA 次（不算系统回归；同一个 App 反复崩要看是不是图形栈 / GPU）"
NA=$(since system_app_crash | wc -l | tr -d ' '); ND=$(since data_app_crash | wc -l | tr -d ' ')
info "system_app_crash ${NA}、data_app_crash ${ND}（按包名看 dropbox-ls.txt；GMS 在未认证设备上会崩，见 TODO v0.7.0 第 8 项）"
NANR=$(since system_server_anr | wc -l | tr -d ' ')
[ "$NANR" = 0 ] || warn A16 "system_server_anr $NANR 次"

# ── A17 SELinux ──
sect "A17 SELinux"
info "getenforce = $(R getenforce)"
R 'logcat -b all -d -v monotonic' > "$OUT/logcat.txt"
R 'dmesg' > "$OUT/dmesg.txt"
info "logd kernel 缓冲从 $(grep -m1 -A1 'beginning of kernel' "$OUT/logcat.txt" | tail -1 | awk '{print $1}') 秒起（判 denial 要覆盖到开机）"
# 计数用 avc-summary.py 去重后的（主体, 目标类型, 类, 权限）元组：它的正则不依赖 audit() 戳，所以 servicemanager
# 经 logcat 打的用户态 service_manager 拒绝（没有 audit 序号）也算进来 —— #126 里 HWC / gatekeeper 注册失败就是这一类，
# 切 enforcing 之前恰恰要看它们。permissive=1 与 permissive=0 各跑一遍（后者来自 enforcing 的域 / 运行期试跑）
{ echo "### permissive=1"; python3 "$HERE/selinux/avc-summary.py" "$OUT/logcat.txt" "$OUT/dmesg.txt"
  echo "### permissive=0"; python3 "$HERE/selinux/avc-summary.py" --enforcing "$OUT/logcat.txt" "$OUT/dmesg.txt"; } > "$OUT/avc-summary.txt" 2>&1
AV=$(grep -c '^  ' "$OUT/avc-summary.txt")
RAW=$(cat "$OUT/logcat.txt" "$OUT/dmesg.txt" | grep -cE 'avc: +denied')
AL=$(grep -c 'audit_lost' "$OUT/dmesg.txt")
if [ "$AV" = 0 ] && [ "$RAW" = 0 ]; then pass A17 "avc denial 0"
elif [ "$AV" = 0 ]; then
    warn A17 "有 $RAW 行 avc denied，avc-summary.py 却一条都没解析出来（格式变了？直接看 logcat.txt / dmesg.txt）"
else
    warn A17 "avc denial $AV 个元组（原始 $RAW 行；permissive 下要人看有没有新的大面积 denial —— 明细见 avc-summary.txt）"
    grep -v '^###' "$OUT/avc-summary.txt" | head -8 | sed 's/^/         /' | tee -a "$REPORT"
fi
[ "${AL:-0}" = 0 ] || warn A17 "dmesg 有 $AL 行 audit_lost（审计限速丢了记录，普查不完整）"

# ── A18 待机计数与性能快照（存档，供下一版对比）──
sect "A18 待机计数与性能快照"
R 'for f in /sys/power/suspend_stats/*; do echo "${f##*/}=$(cat $f)"; done' > "$OUT/suspend_stats.txt"
R 'for f in /sys/kernel/debug/qcom_stats/*; do echo "== ${f##*/}"; cat $f; done' > "$OUT/qcom_stats.txt"
R "$FREQ_CMD" > "$OUT/freq.txt"
R "$THERM_CMD" > "$OUT/thermal.txt"
info "suspend_stats：$(grep -E '^(success|fail|last_failed_dev)=' "$OUT/suspend_stats.txt" | tr '\n' ' ')"
info "qcom_stats 次数：$(awk '/^== /{n=$2} /^Count:/{printf "%s=%s ", n, $2}' "$OUT/qcom_stats.txt")"
TMAX=$(awk '$1 ~ /-thermal$/ {t = $2 / 1000; if (t > m) m = t} END {printf "%.1f", m}' "$OUT/thermal.txt")
info "最高温 ${TMAX} °C"
# A15 期间（freq-before → freq-after）的频率驻留增量：跨版本比这一行，别比开机以来的累计值。
# 各档的最后一列是驻留时间（time_in_state：10 ms；trans_stat：ms），trans_stat 的解析同 scripts/perf/game-perf.sh 的 delta()
info "A15 期间 ${T15} 秒的平均频率（驻留加权）：$(awk '
    FNR == 1 { f++ }
    /^== / { sec = $2; n = sec; sub(/.*\//, "", n); next }
    sec != "gpu" && NF == 2 { k = sec SUBSEP $1; if (f == 1) p[k] = $2; else { d = $2 - p[k]; t[n] += d; w[n] += $1 * d } }
    sec == "gpu" && /^[ *]*[0-9]+:/ { fq = $1; sub(/^\*/, "", fq); if (fq == "") fq = $2; sub(/:$/, "", fq)
        k = "gpu" SUBSEP fq; if (f == 1) p[k] = $NF; else { d = $NF - p[k]; t["gpu"] += d; w["gpu"] += fq * d } }
    END { for (s in t) printf "%s %.0f MHz  ", s, t[s] ? w[s] / t[s] / (s == "gpu" ? 1e6 : 1e3) : 0 }
    ' "$OUT/freq-before.txt" "$OUT/freq-after.txt")"
CD=$(grep -c 'cur_state=[1-9]' "$OUT/thermal.txt")
[ "$CD" = 0 ] && pass A18 "cooling_device 全为 0（空闲时没有压频）" || warn A18 "$CD 个 cooling_device > 0（空闲时就在压频？看 thermal.txt）"
[ -s "$OUT/qcom_stats.txt" ] && pass A18 "快照已存：suspend_stats / qcom_stats / time_in_state / trans_stat / thermal" || fail A18 "qcom_stats 读不出来（要 root / debugfs）"

# ── A19 GApps（D7：只发 GApps 版）──
sect "A19 GApps"
R 'pm list packages com.google.android.gms' | grep -qx "package:com.google.android.gms" && pass A19 "com.google.android.gms 已装" || fail A19 "没有 com.google.android.gms（D7 只发 GApps 版）"

# ── A20 root（D6：保留 KSU 并披露）──
sect "A20 root"
[ "$(R 'id -u')" = 0 ] && pass A20 "root 可用（$(R 'id -Z' 2>/dev/null)）" || fail A20 "拿不到 root（KSU 坏了，或没给 shell 授权）"

# ── 汇总 ──
say ""
say "═══ 汇总：PASS $NP · FAIL $NF · WARN $NW · SKIP $NS ═══"
[ -n "$FAILS" ] && say "FAIL 项：$(echo $FAILS | tr ' ' '\n' | uniq | tr '\n' ' ')"
[ -n "$WARNS" ] && say "WARN 项：$(echo $WARNS | tr ' ' '\n' | uniq | tr '\n' ' ')（要人看一眼）"
say "报告：$REPORT"
[ "$NF" -eq 0 ] && exit 0 || exit 1
