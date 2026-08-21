#!/bin/bash
# 安全挂起助手：没有 rtc0 就拒绝挂起（否则会睡死，只能长按电源键）。
L="${1:-未命名}"
R=/var/log/sx.log
say() { echo "[$(date +%H:%M:%S) up=$(cut -d. -f1 /proc/uptime)] $*" | tee -a $R; }
if [ ! -e /sys/class/rtc/rtc0/wakealarm ]; then
    say "拒绝挂起：/sys/class/rtc/rtc0/wakealarm 不存在，没有唤醒源"
    exit 2
fi
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +40 > /sys/class/rtc/rtc0/wakealarm
A=$(cat /sys/class/rtc/rtc0/wakealarm)
[ -z "$A" ] && { say "拒绝挂起：闹钟没设上"; exit 2; }
sync
say "PRE  $L  (闹钟=$A)"
echo mem > /sys/power/state
RC=$?
say "★★★ POST $L  rc=$RC success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail) dev=[$(cat /sys/power/suspend_stats/last_failed_dev)] step=[$(cat /sys/power/suspend_stats/last_failed_step)] errno=[$(cat /sys/power/suspend_stats/last_failed_errno)]"
