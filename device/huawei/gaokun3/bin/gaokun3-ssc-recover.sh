#!/vendor/bin/sh
# SLPI 崩溃 / 自愈之后把传感器整条链收拾回来（v1.0 DISP-14 / HW-1，TODO B21）。
#
# 现象（docs/dev/sensors-ssc-protocol.md，实测）：SLPI 崩溃自愈时 init 只把 vendor.hexagonrpcd-sdsp
#   按常规重启了（pid 741 退出 → 9002 退出码 4 → 9188），而 SEE 要在 SLPI【起来之后】hexagonrpcd
#   【再重启一次】才注册传感器（#118 §4）⇒ SSC 里连 accel 都没有，自动旋转失效，直到重启。
# 收拾办法就是 #121 §3 手工做过、实测有效的那一套：停 HAL → 停 hexagonrpcd（并清掉残留进程）
#   → 起 hexagonrpcd → 起 HAL（HAL 自己会在同一个 client 上等 registry 与传感器注册，约 20 秒）。
#
# 谁来叫我（etc/sscrecover.rc）：
#   1. init.svc.vendor.hexagonrpcd-sdsp=restarting（开机完成之后）—— SLPI 一崩，fastrpc 会话断开，
#      hexagonrpcd 退出，init 把它置成 restarting（system/core/init/service.cpp:412）。
#      我们自己 stop 它时走的是 stopping → stopped（同文件 :941-1004），不会再把自己叫起来。
#   2. vendor.gaokun3.sscrecover.req —— 传感器 HAL 的看门狗：流该开着却 60 秒没有读数、
#      或会话建好 60 秒还没有任何传感器注册（sensors-hal/SscHub.cpp）。HAL 不再自己重建会话。
#   本服务是 oneshot：运行期间再来的触发，init 的 start 是空操作 —— 天然不重入。
#
# ★ 停 / 起服务不在本脚本里直接做（那要给本域开 ctl.* 属性的写权限），而是设
#   vendor.gaokun3.sscrecover.step=stop|start-rpc|start-hal|done，由 rc 里的 vendor_init 动作执行，
#   与 keyboard.rc / boot HAL 的 bootreq 同一种写法。
#
# ⚠️★ 一行命令里永远不用 pkill -f / pgrep -f：残留进程用 pidof（按可执行文件名精确匹配）。
# ⚠️★ 绝不碰 ambient_light（#37 / #121：一使能 SLPI 的 sensor_process 就整个崩溃）—— 本脚本不开任何 SSC 会话。
#
# 关掉自动收拾（做 SLPI 实验时）：setprop persist.vendor.gaokun3.sscrecover 0（root）。
#   scripts/ssc/sscexp.sh 先 stop 再动 SLPI，不经过 restarting，本来就不会触发它。

TAG=gaokun3-sscrecover
RPC=vendor.hexagonrpcd-sdsp
HAL=vendor.sensors-gaokun3
MAX_RUNS=5          # 一次开机最多收拾这么多次，之后只记日志（SLPI 若反复崩，别陪它一起抖）
MIN_GAP=120         # 两次收拾之间至少隔这么多秒

say() { log -t "$TAG" "$*"; }
uptime_s() { v=$(cat /proc/uptime 2>/dev/null); echo "${v%%.*}"; }
svc() { getprop "init.svc.$1"; }

# 等某个服务进入某个状态，最多 $3 秒；到了返回 0
wait_svc() {
    i=0
    while [ "$i" -lt "$3" ]; do
        [ "$(svc "$1")" = "$2" ] && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# SLPI 的 remoteproc 状态（running / crashed / offline …）；找不到返回空
slpi_state() {
    for r in /sys/class/remoteproc/remoteproc*; do
        [ "$(cat "$r/name" 2>/dev/null)" = slpi ] || continue
        cat "$r/state" 2>/dev/null
        return
    done
}

step() {
    setprop vendor.gaokun3.sscrecover.step "$1"
    say "步骤 $1"
}

if [ "$(getprop persist.vendor.gaokun3.sscrecover)" = 0 ]; then
    say "已关闭（persist.vendor.gaokun3.sscrecover=0），不收拾"
    exit 0
fi

now=$(uptime_s)
runs=$(getprop vendor.gaokun3.sscrecover.runs)
runs=${runs:-0}
last=$(getprop vendor.gaokun3.sscrecover.last_end)
last=${last:-0}
why="hexagonrpcd=$(svc $RPC) req=$(getprop vendor.gaokun3.sscrecover.req) slpi=$(slpi_state)"

if [ "$runs" -ge "$MAX_RUNS" ]; then
    say "本次开机已收拾 $runs 次，不再收拾（$why）—— 重启才能恢复传感器"
    exit 0
fi
if [ "$last" -gt 0 ] && [ $((now - last)) -lt "$MIN_GAP" ]; then
    say "距上次收拾只有 $((now - last)) 秒（< $MIN_GAP），这次跳过（$why）"
    exit 0
fi
runs=$((runs + 1))
setprop vendor.gaokun3.sscrecover.runs "$runs"
say "开始第 $runs 次收拾（uptime ${now}s，$why）"

# 1. 等 SLPI 回到 running（自愈一般几秒；最多等 60 秒）。
#    #118 §4：hexagonrpcd 必须在 SLPI【起来之后】重启那一次才有用，提前重启等于白做。
i=0
while [ "$(slpi_state)" != running ] && [ "$i" -lt 60 ]; do
    sleep 1
    i=$((i + 1))
done
st=$(slpi_state)
if [ "$st" != running ]; then
    # 不在这里替 SLPI 写 start：那是 gaokun3-rproc-kick.sh 的事，而且对 crashed 状态写 start 的后果没验证过。
    say "SLPI 60 秒内没回到 running（$st），放弃这次收拾"
    setprop vendor.gaokun3.sscrecover.last_end "$(uptime_s)"
    exit 1
fi
# 让 init 那次常规重启先落地，免得和它抢（#121 里 init 自己连着重启了两次）
sleep 5

# 2. 停 HAL 与 hexagonrpcd。先停 HAL：#37 的教训 —— 别在 HAL 还握着会话时动底下。
step stop
wait_svc $HAL stopped 15 || say "HAL 15 秒没停下（$(svc $HAL)）"
wait_svc $RPC stopped 15 || say "hexagonrpcd 15 秒没停下（$(svc $RPC)）"
# #121 §3 的手工步骤里有一句 pkill hexagonrpcd：stop 之后可能还有不归 init 管的残留进程
#（例如 sscexp.sh 用 setsid 起的那种）。只按名字精确匹配。
pids=$(pidof hexagonrpcd)
if [ -n "$pids" ]; then
    say "清掉残留的 hexagonrpcd：$pids"
    kill $pids 2>/dev/null
    sleep 2
    pids=$(pidof hexagonrpcd)
    [ -n "$pids" ] && kill -9 $pids 2>/dev/null
fi
sleep 2

# 3. 起 hexagonrpcd —— 这就是 SEE 要的"SLPI 起来之后的那一次重启"
step start-rpc
wait_svc $RPC running 15 || say "hexagonrpcd 15 秒没起来（$(svc $RPC)）"
# 给它一点时间连上 DSP；registry 与传感器注册（约 20 秒）由 HAL 在同一个 client 上等
sleep 5

# 4. 起 HAL。system_server 会重新 linkToDeath、重新登记传感器（#121 §3 实测：登记 5 个）。
step start-hal
wait_svc $HAL running 15 || say "HAL 15 秒没起来（$(svc $HAL)）"
step done

setprop vendor.gaokun3.sscrecover.last_end "$(uptime_s)"
say "第 $runs 次收拾做完（uptime $(uptime_s)s）；是否恢复看 logcat 的 SscHub: accel=1 gyro=1"
exit 0
