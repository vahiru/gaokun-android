#!/system/bin/sh
# B7 / PERF-3 / PERF-4：待机与长稳采样器（【设备侧】，只读系统状态，只往自己的目录追加日志）。
# 宿主机上用 scripts/perf/standby.sh 启停与取回，一般不直接跑这个文件。
#
#   sh standby-sampler.sh once            # 打一行到 stdout，不写任何文件（验采样本身）
#   sh standby-sampler.sh run [目录]       # 常驻（root；setsid nohup 拉起，拔线后照跑）
#
# 每行一条记录，空格分隔的 key=value（值里没有空格）。每行都带：时间、suspend_stats、电池（charge_now 等）、
# qcom_stats 各项、boot reason 与 persist.sys.boot.reason.history（用它区分断电和正常重启，
# 不要只看有没有 SHUTDOWN 记录 —— v1.0-plan B7）。ev= 说明为什么打这一行：
#   start —— 采样开始（另带上一次关机原因、构建戳、内核版本）
#   wake  —— 发现 suspend_stats/success 变了 = 刚从一次（或几次）挂起里醒来
#   hb    —— 心跳：HB 秒没打过行（不睡的时候也有数据；附带关键进程 RSS / fd 与 dropbox 计数，供 72 小时狗粮看趋势）
#   stop  —— 收到 TERM（standby.sh stop）
#
# ★ 为什么这样判"唤醒"：`sleep` 用 CLOCK_MONOTONIC，挂起期间不走，所以这个循环【本身不会唤醒机器】；
#   醒来后最多 POLL 秒内看到 success 变化。代价：醒来不到 POLL 秒就又睡下去的短唤醒（闹钟 / dark resume）
#   会被下一行合并 —— 每行的 ss_ok 是累计值，两行之差就是中间挂起了几次，不会丢。
# ★ 每行之后 sync：断电 / 复位时日志要留到最后一次醒来（PERF-4 / BATT：电量耗尽时的行为没有数据）。
# ⚠️ qcom_stats 在 debugfs，要 root。插着 USB 时 CX 塌缩本来就进不去（复核 PERF-3），计数为 0 不算异常 ——
#   这份采样的意义在于【拔线】之后。
# ⚠️ 不 setprop、不碰 wakelock、不开关任何服务：allow_suspend 由人来设（standby.sh start 会提示）。
# ⚠️ screen= 是 debug.tracing.screen_state 的原始值，即 Android 的 Display.STATE_*：2 = 亮屏，1 = 息屏
#   —— 别把 1 读成亮屏。
# ⚠️ setsid 扛得住拔线，但出不了 init 给 adbd 建的进程组 cgroup：期间开关 USB 调试 / adb root / unroot 让 adbd 重启，
#   采样器会被一起杀、不留 stop 行（standby.sh pull 会提示"中途死了"）。

QS=/sys/kernel/debug/qcom_stats
SS=/sys/power/suspend_stats
BAT=/sys/class/power_supply/gaokun-ec-battery
ADP=/sys/class/power_supply/gaokun-ec-adapter
POLL=${POLL:-5}          # 秒：多久看一次 suspend_stats/success（只是 read 一个 sysfs 文件 + 一次 sleep）
HB=${HB:-1800}           # 秒：心跳间隔（按 /proc/uptime，含挂起时间）

rd() { v=; read -r v < "$1" 2>/dev/null; echo "${v:--}" | tr ' ' '_'; }
up() { u=; read -r u _ < /proc/uptime; echo "${u%%.*}"; }

# qcom_stats：每个有 "Count:" 的子系统记 名=次数/累计时长（原始单位，取两行之差用）；ddr_stats 的 LPM 次数合计
qcom() {
    [ -d $QS ] || { echo "qcom=unreadable"; return; }
    for f in $QS/*; do
        case ${f##*/} in ddr_stats) continue ;; esac
        awk -v n=${f##*/} '/^Count:/{c=$2} /^Accumulated Duration:/{d=$3} END{if (c!="") printf "%s=%s/%s ", n, c, d}' $f 2>/dev/null
    done
    awk '/LPM Stat/{for(i=1;i<=NF;i++) if($i ~ /^count:/){split($i,a,":"); s+=a[2]}} END{printf "ddr_lpm=%d", s}' $QS/ddr_stats 2>/dev/null
}

# 为什么不睡 / 被谁叫醒（B7 排查用）：
#   wl=      /sys/power/wake_lock 里的用户态锁（gaokun3_nosuspend = allow_suspend 没放开，gaokun3_usbhost = 主机枚举了平板，USB 端口 HAL 持有）
#   ws_act=  此刻持有中的内核唤醒源（active_since > 0）
#   ws_top=  累计 wakeup_count 前 5 的唤醒源（名:次数；首末两行相减 = 这段时间谁叫醒了机器）
#   ⚠️ debugfs 的 wakeup_sources 一行是 名字 + 9 列数字，名字可能全是空格（实测有一个 active_count 1 万多的匿名源）⇒
#      按"从行尾数第几列"取，NF = 9 的记成 anon。列序：wakeup_count = $(NF-6)，active_since = $(NF-4)。
#   lpass=   LPASS_HW_MACRO / LPASS_HW_DCODEC 的 enable_count（q6prm 投票）。2026-10-09 rc.1 息屏、无 PCM 打开、
#            macro 全 runtime suspended 时仍是 4/4，同时 ADSP 25.7 小时只睡了 5 秒 —— 怀疑二者有关，未验证。
wake_why() {
    w=$(cat /sys/power/wake_lock 2>/dev/null | tr -s ' \n' ',' | sed 's/,$//')
    WS=/sys/kernel/debug/wakeup_sources
    a=$(awk 'NR > 1 && $(NF-4) > 0 { n = (NF == 10) ? $1 : "anon"; s = s (s ? "," : "") n } END { print s }' $WS 2>/dev/null)
    t=$(awk 'NR > 1 && $(NF-6) > 0 { print $(NF-6), (NF == 10) ? $1 : "anon" }' $WS 2>/dev/null |
        sort -rn | head -5 | awk '{ s = s (s ? "," : "") $2 ":" $1 } END { print s }')
    m=$(rd /sys/kernel/debug/clk/LPASS_HW_MACRO/clk_enable_count)
    c=$(rd /sys/kernel/debug/clk/LPASS_HW_DCODEC/clk_enable_count)
    echo -n "wl=${w:--} ws_act=${a:--} ws_top=${t:--} lpass=$m/$c"
}

# 关键进程 PID/RSS(kB)/fd 数；标签:进程名，名字按 2026-10-04 实机 ps 核对过。PID 变了 = 中途崩溃重启过
procs() {
    for e in ss:system_server sf:surfaceflinger audioserver:audioserver \
             audiohal:android.hardware.audio.service-aidl.example \
             camhal:android.hardware.camera.provider-service.gaokun3 \
             senshal:android.hardware.sensors-service.gaokun3; do
        n=${e#*:}; p=$(pidof $n 2>/dev/null); p=${p%% *}
        if [ -n "$p" ]; then
            r=$(awk '/^VmRSS/{print $2}' /proc/$p/status 2>/dev/null)
            fd=$(ls /proc/$p/fd 2>/dev/null | wc -l)
            echo -n "p_${e%%:*}=$p/${r:-?}/${fd} "
        else
            echo -n "p_${e%%:*}=dead "
        fi
    done
}

# dropbox：采样开始以来新增的崩溃类条目（文件名里的 @ 后是毫秒时间戳；T0S 是秒 ——
# ⚠️ mksh 的算术是 32 位，别在 shell 里把秒乘成毫秒，交给 awk 的浮点去比）
dropbox() {
    ls /data/system/dropbox 2>/dev/null | awk -v t0="${T0S:-0}" -F@ '
        { split($2, a, "."); if (a[1] / 1000 < t0 + 0) next }
        /^system_server_crash@/ {ss++} /^SYSTEM_TOMBSTONE@/ {tb++} /^system_app_crash@/ {sa++}
        /^system_server_anr@/ {an++} /^SYSTEM_BOOT@/ {bt++}
        END { printf "db_ss_crash=%d db_tombstone=%d db_app_crash=%d db_ss_anr=%d db_boot=%d", ss, tb, sa, an, bt }'
}

line() {  # $1 = ev；整条记录只占一行（start / hb / stop 的附加字段接在同一行尾）
    x=
    case $1 in
        start)   x="boot_last=$(getprop sys.boot.reason.last) build=$(getprop ro.build.date.utc) kernel=$(uname -v | tr ' ' '_')" ;;
        hb|stop) x="$(procs)$(dropbox) vendor_gaokun3=$(ls /data/vendor/gaokun3 2>/dev/null | wc -l)" ;;
    esac
    echo "t=$(date +%Y-%m-%dT%H:%M:%S%z) epoch=$(date +%s) up=$(up) ev=$1" \
         "ss_ok=$(rd $SS/success) ss_fail=$(rd $SS/fail) ss_lastdev=$(rd $SS/last_failed_dev) ss_lasterr=$(rd $SS/last_failed_errno)" \
         "wirq=$(rd /sys/power/pm_wakeup_irq)" \
         "cap=$(rd $BAT/capacity) charge_now=$(rd $BAT/charge_now) cur=$(rd $BAT/current_now) volt=$(rd $BAT/voltage_now) bstat=$(rd $BAT/status) ac=$(rd $ADP/online)" \
         "screen=$(getprop debug.tracing.screen_state) allow_suspend=$(getprop persist.vendor.gaokun3.allow_suspend) wlan=$(rd /sys/class/net/wlan0/operstate)" \
         "$(qcom)" \
         "$(wake_why)" \
         "boot_reason=$(getprop sys.boot.reason) boot_history=$(getprop persist.sys.boot.reason.history | tr '\n' ';')" \
         "$x"
}

case ${1:-} in
once)
    T0S=0 line start
    T0S=0 line hb
    ;;
run)
    D=${2:-/data/local/tmp/gk3-standby}
    mkdir -p $D || exit 1
    L=$D/standby.log
    echo $$ > $D/pid
    T0S=$(date +%s); export T0S
    emit() { line $1 >> $L; sync; }
    trap 'emit stop; rm -f $D/pid; exit 0' TERM INT HUP
    emit start
    # 循环体只用 read 内建读两个文件（不 fork），每轮唯一的子进程是 sleep
    read -r last < $SS/success; read -r u _ < /proc/uptime; lasthb=${u%%.*}
    while :; do
        sleep $POLL
        read -r s < $SS/success; read -r u _ < /proc/uptime; now=${u%%.*}
        if [ "$s" != "$last" ]; then
            emit wake; last=$s; lasthb=$now
        elif [ $((now - lasthb)) -ge $HB ]; then
            emit hb; lasthb=$now
        fi
    done
    ;;
*)
    echo "用法：sh $0 once | run [目录]（一般经由 scripts/perf/standby.sh 调用）" >&2
    exit 2
    ;;
esac
