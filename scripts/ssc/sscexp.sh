#!/system/bin/sh
# SSC 实验循环：停 SLPI -> 起 SLPI -> 用自定义根目录起 hexagonrpcd -> 测传感器
# 用法: sscexp.sh <sensor名> [等待秒数]
# 前提: /data/local/tmp/hr 已按本次实验准备好
R=/sys/class/remoteproc/remoteproc0
ROOT=/data/local/tmp/hr
SENSOR="$1"
WAIT="${2:-45}"

[ -d "$ROOT" ] || { echo "FAIL: $ROOT 不存在"; exit 1; }

stop vendor.hexagonrpcd-sdsp 2>/dev/null
sleep 1
pkill hexagonrpcd 2>/dev/null
sleep 1
echo stop > $R/state 2>/dev/null
sleep 3
ST=$(cat $R/state)
[ "$ST" = "offline" ] || { echo "FAIL: SLPI 没停下 (state=$ST)"; exit 1; }

echo start > $R/state || { echo "FAIL: 写 start 失败"; exit 1; }
sleep 6
ST=$(cat $R/state)
[ "$ST" = "running" ] || { echo "FAIL: SLPI 没起来 (state=$ST)"; exit 1; }
[ -e /dev/fastrpc-sdsp ] || { echo "FAIL: 无 /dev/fastrpc-sdsp"; exit 1; }

setsid nohup /vendor/bin/hexagonrpcd -f /dev/fastrpc-sdsp -d sdsp -s -R "$ROOT" >/dev/null 2>&1 </dev/null &
sleep "$WAIT"
pgrep hexagonrpcd >/dev/null || { echo "FAIL: hexagonrpcd 没活着"; exit 1; }

echo "--- 测 $SENSOR ---"
timeout 90 gaokun3-ssc-test "$SENSOR" 2>&1 | tail -4
