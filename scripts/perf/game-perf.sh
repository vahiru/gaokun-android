#!/usr/bin/env bash
# PERF-2：游戏性能采样器（只读）。在【宿主机】跑，走 adb；游戏要已经在前台跑着（要用户在场解锁、进固定场景）。
#
#   SER=gaokun3 bash scripts/perf/game-perf.sh [-p 包名] [-t 总秒数] [-i 采样间隔秒] [-o 输出目录]
#     -p  不给就取当前前台 Activity 的包名
#     -t  默认 1200（三角洲那一档是 20 分钟）；中途 Ctrl-C 也会出摘要
#     -i  默认 5：每 5 秒出一行 CSV（帧数据每 0.5 秒抓一次，见下）
#     -o  默认 out/perf/<包名>-<时间>/
#   产物：samples.csv（每行一个间隔）、frames.txt（每帧的上屏时刻，ns）、summary.txt、header.txt
#
# 每一列从哪来（都是只读的 sysfs / dumpsys，2026-10-04 在 1791053208 上逐个核对过路径）：
#   帧          dumpsys SurfaceFlinger --latency <图层名>：desired / actual present / frame ready 三列（ns），首行是 vsync 周期。
#               crDroid 16（bp4a）开着 deprecate_frame_tracker ⇒ 数据来自 FrameTimeline 最近 **64 个 SF 合成帧**
#               （FrameTimeline.h:559），只含已上屏的帧，120 Hz 下约 0.53 秒；旧 FrameTracker 路径才是 127 行、含 0 / INT64_MAX
#               （两种都按第 2 列 >0 且 <9e18 取，解析不变）。所以两次抓取的间隔必须 < 64 / 刷新率：每轮 sleep 0.2 再加一次 adb
#               （USB 约 30 ms，TCP 更慢）与宿主机上的 awk / sort。图层计数每 65536 帧回绕一次（Layer.h:444），那一轮会短暂变空、记成 GAP。
#               （10-18 构建机按源码核过，⬜ 上机数一次行数应 ≤ 64。）
#               按时间戳去重拼接；两次之间没有重叠 = 中间丢了帧，frames.txt 里记一行 GAP，
#               跨 GAP 的间隔不进帧时间统计（CSV 的 gap 列 = 1 说明这一行的 fps 偏低是采样丢的，不是游戏掉的）。
#               图层名从 --list 里找 "SurfaceView[包名/…](BLAST)"（游戏都画在 SurfaceView 上，2026-10-04 timestats 里
#               三角洲 / 卡拉彼丘 / 明日方舟 / Phigros / Arcaea 都是这个形状），找不到再退回包名的普通窗口。
#   交叉核对    dumpsys SurfaceFlinger --timestats -dump 里同名图层的 totalFrames：开头、结尾各读一次，Δ/Δt 是整段平均帧率。
#               （只读 dump，不用 -enable / -clear；timestats 在本机开机就开着，statsStart = 开机时刻）
#   CPU         /sys/devices/system/cpu/cpufreq/policy{0,4}/stats/time_in_state（kHz、10 ms 为单位）的增量：
#               平均频率 = Σf·Δt / ΣΔt，top% = 最高档的 Δt 占比。注意这是【频率驻留】，不是忙闲（idle 也算在当时的频率上）。
#   GPU         /sys/class/devfreq/3d00000.gpu/trans_stat 每档最后一列 time(ms) 的增量，分母 = 各档 Δt 的合计
#               （复核 PERF-2：本机 simple_ondemand 几乎只在 270 / 690 两档之间跳，所以要和 fps 一起看）。
#               msm 没有导出 GPU 忙闲百分比，这里只有频率驻留。
#   温度        /sys/class/thermal/thermal_zone*：cpu*-thermal 取最大、gpu-thermal、mem-thermal（°C）；内核带板载热敏电阻温区时
#               （patches/kernel/dts/0031），另记 sys-therm1–4 的最大值（t_board）。降频主要由这四路触发，但各路的降频点不同；没有这些温区时该列为空。
#   降频        /sys/class/thermal/cooling_device*/cur_state（cpufreq-cpu0 / cpufreq-cpu4 / devfreq-3d00000.gpu），>0 = 温控在压频。
#   电池        gaokun-ec-battery 的 capacity 与 current_now（µA，原始值，符号以 EC 为准）。
#
# 帧率上限要先弄清（复核 PERF-2）：ro.surface_flinger.game_default_frame_rate_override=60 同时
# debug.graphics.game_default_frame_rate.disabled=true —— header.txt 里会记下这两个属性、当前刷新率与图层的 frameRate 投票，
# 平均 fps 贴着 60 还是 120，要对照它们解读。
#
# ⚠️ 只读：不 setprop、不 --latency-clear、不 timestats -clear、不改刷新率。每秒两次 adb shell，开销很小但不是零。
set -u
export MSYS_NO_PATHCONV=1
SER=${SER:-${SERIAL:-}}
ADB="adb ${SER:+-s $SER}"
PKG=; DUR=1200; INT=5; OUT=
while getopts "p:t:i:o:h" o; do
    case $o in
        p) PKG=$OPTARG ;; t) DUR=$OPTARG ;; i) INT=$OPTARG ;; o) OUT=$OPTARG ;;
        *) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done
A() { $ADB shell "$1" 2>/dev/null | tr -d '\r'; }

[ "$(A 'getprop sys.boot_completed')" = 1 ] || { echo "adb 不通或没开机完成"; exit 2; }
if [ -z "$PKG" ]; then
    PKG=$(A 'dumpsys activity activities | grep -m1 -E "ResumedActivity"' | sed -n 's/.* u[0-9]* \([^/ ]*\)\/.*/\1/p')
    [ -n "$PKG" ] || { echo "认不出前台包名，用 -p 指定"; exit 2; }
fi

# 在 --list 里找图层：去掉 "RequestedLayerState{" 外壳与尾部的 parentId= / relativeParentId= / z= 字段
find_layer() {
    A 'dumpsys SurfaceFlinger --list' \
      | sed -e 's/^RequestedLayerState{//' -e 's/\( parentId=-\{0,1\}[0-9]*\)\{0,1\}\( relativeParentId=-\{0,1\}[0-9]*\)\{0,1\}\( z=-\{0,1\}[0-9]*\)\{0,1\}}$//' \
      > "$TMPD/list.txt"
    l=$(grep -F "SurfaceView[$PKG/" "$TMPD/list.txt" | grep -F "(BLAST)" | tail -1)
    [ -n "$l" ] || l=$(grep -F " $PKG/" "$TMPD/list.txt" | grep -vE "^ActivityRecord|InputSink|animation-leash|^Surface\(name=" | tail -1)
    echo "$l"
}

TMPD=$(mktemp -d)
LAYER=$(find_layer)
[ -n "$LAYER" ] || { echo "SurfaceFlinger 里找不到 $PKG 的图层（游戏在前台吗？）"; rm -rf "$TMPD"; exit 2; }
OUT=${OUT:-out/perf/$PKG-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
case $LAYER in *"'"*) echo "图层名里有单引号，不支持：$LAYER"; exit 2 ;; esac

# 时间结构：一次 adb 调用里抓帧；到采样点再顺带抓 CPU / GPU / 温度 / 降频 / 电池 / uptime
SNAP='echo @CPU0; cat /sys/devices/system/cpu/cpufreq/policy0/stats/time_in_state
echo @CPU4; cat /sys/devices/system/cpu/cpufreq/policy4/stats/time_in_state
echo @GPU; cat /sys/class/devfreq/3d00000.gpu/trans_stat
echo @THERM; for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type) $(cat $z/temp)"; done
echo @COOL; for c in /sys/class/thermal/cooling_device*; do echo "$(cat $c/type) $(cat $c/cur_state)"; done
echo @BAT; cat /sys/class/power_supply/gaokun-ec-battery/capacity /sys/class/power_supply/gaokun-ec-battery/current_now
echo @UP; cat /proc/uptime'
timestats_frames() {  # 该图层在 timestats 里所有分桶的 totalFrames 之和（同一图层按刷新率 / 渲染率分了好几块）
    A 'dumpsys SurfaceFlinger --timestats -dump' \
      | awk -v L="layerName = $LAYER" '$0 == L {hit = 1; next} /^layerName = / {hit = 0} hit && /^totalFrames = / {s += $3; hit = 0} END {print s + 0}'
}

{
    echo "包名：$PKG"
    echo "图层：$LAYER"
    echo "开始：$(date '+%F %T')  时长 ${DUR}s  间隔 ${INT}s"
    echo "构建戳：$(A 'getprop ro.build.date.utc')  内核：$(A 'uname -v')"
    echo "ro.surface_flinger.game_default_frame_rate_override = $(A 'getprop ro.surface_flinger.game_default_frame_rate_override')"
    echo "debug.graphics.game_default_frame_rate.disabled = $(A 'getprop debug.graphics.game_default_frame_rate.disabled')"
    echo "peak_refresh_rate = $(A 'settings get system peak_refresh_rate')  min_refresh_rate = $(A 'settings get system min_refresh_rate')"
    A 'dumpsys SurfaceFlinger' | grep -m1 "activeMode=" | sed 's/^ */SF: /'
    A 'dumpsys SurfaceFlinger' | grep -m1 "^GLES:"
    A 'dumpsys SurfaceFlinger --timestats -dump' | awk -v L="layerName = $LAYER" '$0 == L {hit = 1} hit && /^(frameRate|frameRateCompatibility|gameMode) = / {print "timestats: " $0} hit && /^averageFPS/ {exit}'
} > "$OUT/header.txt"
cat "$OUT/header.txt"

# 两次快照之差 → CSV 的一段（awk 读 prev 与 cur 两个文件）
delta() {
    awk '
    FNR == 1 { f++ }
    /^@/ { sec = $1; next }
    sec == "@CPU0" || sec == "@CPU4" { k = sec ":" $1; if (f == 1) p[k] = $2; else { d = $2 - p[k]; fr[sec, ++nf[sec]] = $1; dt[sec, nf[sec]] = d } }
    sec == "@GPU" && /^[ *]*[0-9]+:/ { fq = $1; sub(/^\*/, "", fq); if (fq == "") fq = $2; sub(/:$/, "", fq); k = "G:" fq
        if (f == 1) p[k] = $NF; else { gf[++ng] = fq; gd[ng] = $NF - p[k] } }
    f == 2 && sec == "@THERM" { t = $2 / 1000; if ($1 ~ /^cpu[0-9]+-thermal$/ && t > tc) tc = t; if ($1 == "gpu-thermal") tg = t; if ($1 == "mem-thermal") tm = t
        if ($1 ~ /^sys-therm[1-4]-thermal$/ && (!hb || t > tb)) { tb = t; hb = 1 } }
    f == 2 && sec == "@COOL" { cool[$1] = $2 }
    f == 2 && sec == "@BAT" { b[++nb] = $1 }
    sec == "@UP" { up[f] = $1 }
    END {
        for (s = 0; s <= 1; s++) {
            sec = s ? "@CPU4" : "@CPU0"; sw = 0; tot = 0; top = 0; mx = 0
            for (i = 1; i <= nf[sec]; i++) { tot += dt[sec, i]; sw += fr[sec, i] * dt[sec, i]; if (fr[sec, i] + 0 > mx) { mx = fr[sec, i] + 0; top = dt[sec, i] } }
            printf "%.0f,%.1f,", tot ? sw / tot / 1000 : 0, tot ? 100 * top / tot : 0
        }
        tot = 0; sw = 0; mx = 0; top = 0
        for (i = 1; i <= ng; i++) { tot += gd[i]; sw += gf[i] * gd[i]; if (gf[i] + 0 > mx) { mx = gf[i] + 0; top = gd[i] } }
        printf "%.0f,%.1f,", tot ? sw / tot / 1e6 : 0, tot ? 100 * top / tot : 0
        printf "%.1f,%.1f,%.1f,%s,", tc, tg, tm, hb ? sprintf("%.1f", tb) : ""
        printf "%d,%d,%d,", cool["cpufreq-cpu0"], cool["cpufreq-cpu4"], cool["devfreq-3d00000.gpu"]
        printf "%s,%s,%.2f\n", b[1], b[2], up[2] - up[1]
    }' "$1" "$2"
}

echo "t_s,fps,frames,gap,cpu0_mhz,cpu0_top_pct,cpu4_mhz,cpu4_top_pct,gpu_mhz,gpu_max_pct,t_cpu_max,t_gpu,t_mem,t_board,cool_cpu0,cool_cpu4,cool_gpu,bat_cap,bat_cur_ua,dt_s" > "$OUT/samples.csv"
: > "$OUT/frames.txt"
TS0=$(timestats_frames)
A "$SNAP" > "$TMPD/first.txt"; cp "$TMPD/first.txt" "$TMPD/prev.txt"
LAST=0; NEWN=0; GAPIN=0; EMPTY=0; START=$(date +%s); NEXT=$((START + INT)); STOP=0
trap 'STOP=1' INT TERM

while [ "$STOP" = 0 ] && [ $(( $(date +%s) - START )) -lt "$DUR" ]; do
    # 采样点按墙钟走（每 INT 秒一次），不按轮数：一轮的耗时随 adb 与快照大小变
    # （落后了就从现在重新起算，不连发几行补课 —— 那几行的 dt 很小、fps 很吵）
    now=$(date +%s); snap=0
    if [ "$now" -ge "$NEXT" ]; then snap=1; NEXT=$((NEXT + INT)); [ "$NEXT" -gt "$now" ] || NEXT=$((now + INT)); fi
    if [ $snap = 1 ]; then A "echo @LAT; dumpsys SurfaceFlinger --latency '$LAYER'; $SNAP" > "$TMPD/tick.txt"
    else A "echo @LAT; dumpsys SurfaceFlinger --latency '$LAYER'" > "$TMPD/tick.txt"; fi
    # 第 2 列 = 实际上屏时刻；0 = 还没记、≥ 9e18 = INT64_MAX（还没上屏）
    awk '/^@/ {sec = $1; next} sec == "@LAT" && NF == 3 && $2 + 0 > 0 && $2 + 0 < 9e18 {print $2}' "$TMPD/tick.txt" | sort -n > "$TMPD/ts.txt"
    if [ -s "$TMPD/ts.txt" ]; then
        EMPTY=0
        MIN=$(head -1 "$TMPD/ts.txt")
        if [ "$LAST" != 0 ] && [ "$MIN" -gt "$LAST" ]; then echo GAP >> "$OUT/frames.txt"; GAPIN=1; fi
        awk -v last="$LAST" '$1 + 0 > last + 0' "$TMPD/ts.txt" > "$TMPD/new.txt"
        n=$(wc -l < "$TMPD/new.txt" | tr -d ' ')
        if [ "$n" -gt 0 ]; then cat "$TMPD/new.txt" >> "$OUT/frames.txt"; LAST=$(tail -1 "$TMPD/new.txt"); NEWN=$((NEWN + n)); fi
    else
        # 连续 6 轮（约 3 秒）一帧都没有：游戏暂停 / 切走，或者图层没了（游戏重启后图层名里的 #序号会变）。重新找一次
        EMPTY=$((EMPTY + 1))
        if [ "$EMPTY" -ge 6 ]; then
            EMPTY=0; L2=$(find_layer)
            if [ -n "$L2" ] && [ "$L2" != "$LAYER" ]; then
                LAYER=$L2; echo "图层换成：$LAYER" | tee -a "$OUT/header.txt"; echo GAP >> "$OUT/frames.txt"; LAST=0; GAPIN=1
            fi
        fi
    fi
    if [ $snap = 1 ]; then
        sed -n '/^@CPU0/,$p' "$TMPD/tick.txt" > "$TMPD/cur.txt"
        D=$(delta "$TMPD/prev.txt" "$TMPD/cur.txt")
        DT=${D##*,}
        FPS=$(awk -v n="$NEWN" -v dt="$DT" 'BEGIN {printf "%.1f", (dt > 0 ? n / dt : 0)}')
        echo "$(( $(date +%s) - START )),$FPS,$NEWN,$GAPIN,$D" | tee -a "$OUT/samples.csv"
        mv "$TMPD/cur.txt" "$TMPD/prev.txt"; NEWN=0; GAPIN=0
    fi
    sleep 0.2   # 必须让一轮 < 64 / 刷新率（120 Hz 下 0.53 s），见头注释；macOS 与 Linux 的 sleep 都认小数
done
trap - INT TERM

# ── 摘要 ──
TS1=$(timestats_frames)
WALL=$(( $(date +%s) - START ))
A "$SNAP" > "$TMPD/last.txt"
{
    echo "═══ $PKG · $(date '+%F %T') · ${WALL}s ═══"
    # 帧时间：相邻两帧上屏时刻之差（跨 GAP 的不算）
    awk 'BEGIN {p = 0} /^GAP/ {p = 0; next} { if (p) printf "%.3f\n", ($1 - p) / 1e6; p = $1 }' "$OUT/frames.txt" | sort -n > "$TMPD/ft.txt"
    awk '{ a[NR] = $1; s += $1 }
         END {
           if (NR == 0) { print "帧：没有抓到（图层名不对，或游戏不在前台）"; exit }
           k = int(NR / 100); if (k < 1) k = 1; w = 0; for (i = NR - k + 1; i <= NR; i++) w += a[i]
           printf "帧：%d 个间隔；平均 %.1f fps（平均帧时间 %.2f ms）\n", NR, 1000 * NR / s, s / NR
           printf "1%% low：%.1f fps（最慢 1%% 帧的平均帧时间 %.2f ms）；p99 帧时间 %.2f ms；最长 %.1f ms\n", 1000 * k / w, w / k, a[(int(NR * 0.99) > 0 ? int(NR * 0.99) : 1)], a[NR]
         }' "$TMPD/ft.txt"
    echo "GAP（采样丢帧）次数：$(grep -c '^GAP' "$OUT/frames.txt")"
    awk -v a="$TS0" -v b="$TS1" -v w="$WALL" 'BEGIN { if (b > a && w > 0) printf "timestats 交叉核对：totalFrames %d → %d，平均 %.1f fps\n", a, b, (b - a) / w; else print "timestats 交叉核对：图层不在 timestats 里或没有新增（不影响上面的数）" }'
    echo "整段（首个快照 → 最后一个）："
    D=$(delta "$TMPD/first.txt" "$TMPD/last.txt")
    echo "$D" | awk -F, '{ printf "  CPU0 平均 %s MHz（最高档 %s%%），CPU4 平均 %s MHz（最高档 %s%%）\n  GPU 平均 %s MHz（最高档 %s%%）\n", $1, $2, $3, $4, $5, $6 }'
    awk -F, 'NR > 1 { if ($11 > mc) mc = $11; if ($12 > mg) mg = $12; if ($13 > mm) mm = $13; if ($14 != "" && (!hb || $14 > mb)) { mb = $14; hb = 1 }
                      if ($15 + $16 + $17 > 0) { thr++; ts += $20 } }
             END { printf "  最高温：CPU %.1f °C、GPU %.1f °C、内存 %.1f °C", mc, mg, mm
                   if (hb) printf "、板载热敏电阻（sys-therm1–4）%.1f °C", mb
                   printf "\n  温控压频：%d 个采样行（约 %.0f 秒）cooling_device > 0\n", thr, ts }' "$OUT/samples.csv"
} | tee "$OUT/summary.txt"
rm -rf "$TMPD"
echo "产物：$OUT/"
