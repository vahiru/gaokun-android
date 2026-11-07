#!/vendor/bin/sh
#
# 音频/蓝牙死锁取证看门狗。
#
# 为什么要有它：用户实机曾报告"长期运行后音频与蓝牙可能死锁"，
# 而我们**一次都没复现过**。它的诊断建议全是推导出来的，不是观测。
# 现实是死锁发生时用户只会重启，证据就没了 —— 所以证据必须【自动】留下。
#
# 探针刻意做得很便宜：只读 /proc 里的线程状态。每 60 秒一次、每次几毫秒。
# 取证部分删掉了 logcat 与 dumpsys —— 这个脚本跑在 vendor 域
#   （gaokun3_hangdump），而 Treble 的 neverallow 不许 vendor 域执行 /system/bin/logcat
#   （logcat_exec）、也不许它打开 /dev/binder（dumpsys 要）。permissive 下它们能跑，
#   enforcing 下只会留下空文件。binder 那一段改读 binderfs 的 state（见 sepolicy/gaokun3_scripts.te 末尾）。
#
# 判据不是"出现 D 状态"（短暂的 D 很正常），而是
# **同一个 tid 连续三次采样都在 D**（= 卡住至少两分钟）。
# 不可中断睡眠正是内核侧死锁/DSP 通路卡住的特征。
#
# ★ v1.0 AV-6（2026-10-05）补的两条（D 状态判据 6 周一次没触发过，而它抓不到"数据面停了、
#   线程却在 S 状态里等"的那一类：ALSA 写 / 读阻塞在可中断的等待里，线程是 S 不是 D）：
#   ① PCM 停滞：/proc/asound/card*/pcm*/sub*/status 处于 RUNNING、而 hw_ptr 在连续两次采样
#      （= 至少 60 秒，再确认一次共约 2 分钟）里一动不动 ⇒ DSP 不再交付周期，取证。
#      读 status 本身要拿 PCM 的锁（ASoC 的 nonatomic 流用的是 mutex）—— 死锁时这一读会卡在 D 里、
#      连带把看门狗自己卡死。所以放到后台子 shell 里读、主循环最多等 10 秒；连续两次没读完
#      （= 状态读卡住 ≥ 70 秒）本身就算一次停滞，直接取证（取证时不再去碰 PCM 的 status）。
#   ② remoteproc 状态变化：每次采样记一个签名（各 remoteproc 的 state + /proc/interrupts 里
#      q6v5 fatal / wdog 的计数 + ADSP / CDSP 的 ready 计数），变了就写 logcat 和
#      $DIR/rproc-events.log（只留最近 400 行）。只记录、不取证、不退出 —— SSR 后几秒就自愈，
#      60 秒的采样看 state 会漏，所以加中断计数。SLPI 的 ready / handover 不算（它平时就一直在涨）。
#   A1 的候选成因之一正是 ADSP SSR 后路由丢失（v1.0-plan AV-5 / AV-6），出事时这两样能把时间线对上。
#   ⚠️ 未上机：PCM status 的字段格式（"state: RUNNING"、"hw_ptr      : N"）按内核 sound/core/pcm.c 的
#     proc 输出写，1.0.0-dev.1 上当时没有流在跑、只看到 "closed"；上机时放一段音乐核对一次（判据见文末）。
#
# 每次启动最多产出一份 dump：服务是 oneshot + disabled、只在 boot_completed 时起一次，取完证就 exit。
# 盘上只留最近 5 份（KEEP），不会把 /data 撑爆。
# ⓘ 2026-09-29 删掉了 `.done` 标记：它在 /data 上跨重启保留、从没人删 ⇒ 第一次取证之后看门狗
#   就永久不跑了（与"每次启动一份"相反）；而"每次启动一份"本来就由上面那两点保证，标记是多余的。

PATH=/system/bin:/vendor/bin
export PATH

DIR=/data/vendor/gaokun3
KEEP=5
INTERVAL=60
STRIKES_NEEDED=3

mkdir -p $DIR 2>/dev/null
# 只留最近 KEEP 份（ls -t 新的在前）
ls -dt $DIR/hangdump-* 2>/dev/null | tail -n +$((KEEP + 1)) | while read -r old; do rm -rf "$old"; done

# 关注的进程：音频服务端、蓝牙、以及我们自己的 DSP 文件服务器
# （#38 的推断是三者共用 QRTR/FastRPC 那条通路）。
watched_pids() {
    # ⚠️ 2026-09-29：原先的 android.hardware.bluetooth 一个进程都匹配不上（pidof 按完整名），
    #   蓝牙 HAL 真名是 android.hardware.bluetooth-service.default；并补上音频 HAL 两个进程。
    for n in audioserver com.android.bluetooth android.hardware.bluetooth-service.default \
             android.hardware.audio.service-aidl.example android.hardware.audio.effect.service-aidl.example \
             hexagonrpcd; do
        pidof "$n" 2>/dev/null
    done
}

# 返回当前处于 D 状态的 tid 列表
d_state_tids() {
    for p in $(watched_pids); do
        for t in /proc/$p/task/*; do
            [ -d "$t" ] || continue
            # /proc/<tid>/stat 第 3 个字段是状态；进程名里可能有空格，
            # 所以从右括号之后再切。
            st=$(sed -e 's/.*) //' -e 's/ .*//' "$t/stat" 2>/dev/null)
            [ "$st" = "D" ] && basename "$t"
        done
    done
}

# ── ① PCM 停滞（AV-6）─────────────────────────────────────────────────────
# 后台子 shell 把"处于 RUNNING 的子流=hw_ptr"逐行写进 $PCM_OUT（先写 .part 再改名，主循环只认改完名的）。
# 返回 0 = 读完了；1 = 10 秒还没读完（上一轮的探针若还卡着，不再另起一个，只接着等它）。
PCM_OUT=$DIR/.pcm-probe
PCM_PENDING=0
pcm_probe() {
    if [ $PCM_PENDING = 0 ]; then
        rm -f "$PCM_OUT" "$PCM_OUT.part"
        (
            for f in /proc/asound/card*/pcm*/sub*/status; do
                [ -f "$f" ] || continue
                st=$(cat "$f" 2>/dev/null)
                case "$st" in "state: RUNNING"*) ;; *) continue ;; esac
                hp=$(echo "$st" | sed -n 's/^hw_ptr[[:space:]]*:[[:space:]]*//p')
                [ -n "$hp" ] && echo "$f=$hp"
            done > "$PCM_OUT.part" 2>/dev/null
            mv -f "$PCM_OUT.part" "$PCM_OUT"
        ) &
        PCM_PENDING=1
    fi
    i=0
    while [ ! -f "$PCM_OUT" ] && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
    [ -f "$PCM_OUT" ] || return 1
    PCM_PENDING=0
    return 0
}

# ── ② remoteproc 签名（AV-6）──────────────────────────────────────────────
# 例：slpi=running adsp=running cdsp=running 209:0 210:0 214:0 215:0 216:2 …（"中断号:各 CPU 计数之和"）
rproc_sig() {
    for r in /sys/class/remoteproc/remoteproc*; do
        [ -d "$r" ] || continue
        n=""; st=""
        read -r n 2>/dev/null < "$r/name"
        read -r st 2>/dev/null < "$r/state"
        printf '%s=%s ' "$n" "$st"
    done
    grep -E 'q6v5 (fatal|wdog)|smp2p-(adsp|nsp[0-9]).*q6v5 ready' /proc/interrupts 2>/dev/null |
    while read -r irq rest; do
        sum=0
        for x in $rest; do
            case "$x" in *[!0-9]*) break ;; esac
            sum=$((sum + x))
        done
        printf '%s%s ' "$irq" "$sum"
    done
}

RPROC_LOG=$DIR/rproc-events.log
rproc_event() {
    log -t hangdump "remoteproc 状态变化：[$1] → [$2]"
    {
        echo "=== uptime $(cat /proc/uptime)"
        echo "前: $1"
        echo "后: $2"
        dmesg -S 2>/dev/null | grep -iE 'remoteproc|q6v5|qcom_q6v5|ssr|fatal|crash|apr|gpr|q6apm|qrtr' | tail -40
    } >> "$RPROC_LOG" 2>&1
    # 只留最近 400 行，不会把 /data 撑大
    if [ "$(wc -l < "$RPROC_LOG" 2>/dev/null)" -gt 400 ] 2>/dev/null; then
        tail -n 400 "$RPROC_LOG" > "$RPROC_LOG.new" && mv -f "$RPROC_LOG.new" "$RPROC_LOG"
    fi
}

collect() {
    U=$(cut -d. -f1 /proc/uptime)
    O=$DIR/hangdump-$U
    mkdir -p $O
    log -t hangdump "检测到疑似死锁，取证到 $O"

    { echo "uptime: $(cat /proc/uptime)"; echo "触发原因: $REASON"; echo "卡住的 tid: $STUCK"
      [ -n "$STUCK_PCM" ] && echo "停滞的 PCM（子流=hw_ptr）: $STUCK_PCM"; } > $O/00-summary.txt
    for t in $STUCK; do
        {
            echo "=== tid $t ==="
            cat /proc/$t/comm    2>/dev/null
            cat /proc/$t/wchan   2>/dev/null; echo
            echo "--- status ---"; cat /proc/$t/status 2>/dev/null
        } >> $O/01-stuck-threads.txt 2>&1
    done

    # 只列被看的那几个进程：vendor 域读不到别的进程的 /proc（`ps -A` 会每个进程报一条 denial、
    #   结果也只剩这几个）。/proc/<tid>/stack 要 CAP_SYS_ADMIN + ptrace，vendor 域拿不到，已删。
    L=$(watched_pids | tr -s ' \n' ',' | sed 's/^,//;s/,$//')
    [ -n "$L" ] && ps -T -o PID,TID,S,NAME -p "$L" > $O/02-ps.txt 2>&1
    # -S：走 syslog(2)（sepolicy 给了 kernel:system syslog_read）；默认的 /dev/kmsg 是 kmsg_device，不给。
    dmesg -S | tail -800 > $O/03-dmesg.txt 2>&1

    # QRTR 服务表：#38 第 3 步。少了哪个服务就指向哪个 DSP。
    timeout 10 gaokun3-qrtr-lookup > $O/04-qrtr.txt 2>&1

    # 音频数据面还活着吗（内核侧 vs 上层的分水岭，#38 第 1 步）
    # ⚠️ AV-6：探针读 status 已经卡住时不再读 —— 那会把取证本身卡在 D 里，后面的 binder 一节就永远写不出来
    #   （timeout 杀不掉 D 状态的 cat，它会一直等）。
    if [ $PCM_PENDING = 1 ]; then
        echo "PCM status 读不出来（探针卡住 ≥ 10 秒，见 00-summary），本节跳过" > $O/05-pcm.txt
    else
        for f in /proc/asound/card0/pcm*/sub0/status; do
            { echo "== $f"; timeout 5 cat "$f"; } >> $O/05-pcm.txt 2>&1
        done
    fi

    # binder：只读 binderfs 的 state —— 每个进程的 binder 线程与挂起中的事务，
    #   "谁在等谁"正是查死锁要的。它的类型是 binderfs_logs，userdebug 上可读
    #   （system/sepolicy private/domain.te:739 的豁免；本机只编 userdebug）。
    # ⚠️ 2026-09-27 改（#126）：此前读的 transactions / failed_transaction_log 在 domain.te:558-568
    #   里只许 dumpstate / system_server 等读；debugfs 回落那条 neverallow 没有任何豁免。都删了。
    BL=/dev/binderfs/binder_logs
    { echo "== $BL/state（前 2000 行）"; timeout 5 head -2000 $BL/state; } > $O/06-binder.txt 2>&1

    # remoteproc（AV-6）：此刻的签名 + 本次开机记下的全部变化
    { echo "当前: $(rproc_sig)"; echo; cat "$RPROC_LOG" 2>/dev/null; } > $O/07-rproc.txt 2>&1

    # ✗ logcat / dumpsys 已删（vendor 域做不到，见文件开头）。死锁现场的 logcat 要靠用户
    #   事后 `adb logcat -d` 或 bug report —— 取证目录里的时间戳（目录名就是 uptime）用来对齐。

    sync
    log -t hangdump "取证完成：${O}（本次启动不再重复采集）"
}

PREV=""
STRIKES=0
REASON=""
STUCK=""
STUCK_PCM=""
PCM_PREV=""
PCM_STRIKES=0
PCM_STRIKES_NEEDED=2          # hw_ptr 连续两次对比不动 = 至少 60 秒、再确认一次（约 2 分钟）
PCM_HANG=0
PCM_HANG_NEEDED=2             # status 连续两轮读不完 = 读卡住 ≥ 70 秒
# 本次开机的 remoteproc 变化记录从头开始（上一次开机的那份留作 .prev）
[ -f "$RPROC_LOG" ] && mv -f "$RPROC_LOG" "$RPROC_LOG.prev" 2>/dev/null
RPROC_PREV=$(rproc_sig)
log -t hangdump "启动：D 状态 / PCM 停滞判据，remoteproc 基线 [$RPROC_PREV]"
while true; do
    sleep $INTERVAL

    # ② remoteproc：只记录
    RPROC_NOW=$(rproc_sig)
    if [ "$RPROC_NOW" != "$RPROC_PREV" ]; then
        rproc_event "$RPROC_PREV" "$RPROC_NOW"
        RPROC_PREV=$RPROC_NOW
    fi

    # ① PCM 停滞
    if pcm_probe; then
        PCM_HANG=0
        NOWP=$(cat "$PCM_OUT" 2>/dev/null)
        SAMEP=""
        for x in $NOWP; do
            for y in $PCM_PREV; do [ "$x" = "$y" ] && SAMEP="$SAMEP $x"; done
        done
        PCM_PREV="$NOWP"
        if [ -n "$SAMEP" ]; then
            PCM_STRIKES=$((PCM_STRIKES + 1))
            log -t hangdump "PCM 在 RUNNING 但 hw_ptr 不动（第 $PCM_STRIKES/$PCM_STRIKES_NEEDED 次）:$SAMEP"
            if [ $PCM_STRIKES -ge $PCM_STRIKES_NEEDED ]; then
                REASON="PCM 停滞（RUNNING 但 hw_ptr 连续 $PCM_STRIKES 次采样不动）"
                STUCK_PCM="$SAMEP"
                collect
                exit 0
            fi
        else
            PCM_STRIKES=0
        fi
    else
        PCM_HANG=$((PCM_HANG + 1))
        log -t hangdump "读 PCM status 卡住（第 $PCM_HANG/$PCM_HANG_NEEDED 轮，后台探针 10 秒没返回）"
        if [ $PCM_HANG -ge $PCM_HANG_NEEDED ]; then
            REASON="PCM status 读不出来（探针连续 $PCM_HANG 轮卡住 —— PCM 的锁被占着不放）"
            collect
            exit 0
        fi
    fi

    # 原判据：同一 tid 持续 D 状态
    NOW=$(d_state_tids)
    [ -z "$NOW" ] && { PREV=""; STRIKES=0; continue; }

    # 与上一次采样求交集：只有【同一个 tid】持续卡住才算数
    SAME=""
    for t in $NOW; do
        for q in $PREV; do [ "$t" = "$q" ] && SAME="$SAME $t"; done
    done
    PREV="$NOW"

    if [ -z "$SAME" ]; then STRIKES=0; continue; fi
    STRIKES=$((STRIKES + 1))
    log -t hangdump "同一 tid 持续 D 状态（第 $STRIKES/$STRIKES_NEEDED 次）:$SAME"
    if [ $STRIKES -ge $STRIKES_NEEDED ]; then
        REASON="同一 tid 持续 D 状态"
        STUCK="$SAME"
        collect
        exit 0
    fi
done

# 上机判据（AV-6，⬜ 未上机）：
#   · logcat -s hangdump 开机后有"启动：… remoteproc 基线 [slpi=running adsp=running cdsp=running …]"，
#     签名里有 q6v5 fatal / wdog 与 ADSP / CDSP ready 的计数；
#   · 放音乐时 `cat /proc/asound/card0/pcm0p/sub0/status` 的第一行是 "state: RUNNING"、有 "hw_ptr      :" 一行
#     （字段名对不上的话 PCM 判据永远不触发 —— 这是要核的第一件事）；放音期间 logcat 不出现"hw_ptr 不动"；
#   · 暂停 / 停止播放后不触发；/data/vendor/gaokun3/.pcm-probe 每分钟刷新一次；
#   · 自然发生的 SSR（例如 B21 / #121 那类 SLPI 崩溃）会在 /data/vendor/gaokun3/rproc-events.log 留一条。
#     不要为了验它去手动触发 SSR。
