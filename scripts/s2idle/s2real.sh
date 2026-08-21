#!/bin/bash
# ★ 真实 s2idle 挂起测试。前提：我们带补丁的内核 #31 + role=host。
# 两道已知的坎（USB role 复位、EC suspend_noirq -110）都已修好，
# 这一步验的是最后没验过的东西：真正睡下去、被 RTC 叫醒、恢复回来。
# 每一步都 sync，所以即使整板复位或睡死，证据也已经在盘上。
R=/var/log/s2real.log
say() { echo "[$(date +%H:%M:%S) wall=$(date +%s) up=$(cut -d. -f1 /proc/uptime)] $*" >> $R; sync; }
rm -f /etc/systemd/system/multi-user.target.wants/s71test.service
: > $R
say "START 真实 s2idle 挂起测试"
sleep 30
say "内核: $(uname -r) $(uname -v | cut -d' ' -f1)"

A=$(readlink -f /sys/class/power_supply/gaokun-ec-adapter 2>/dev/null)
[ -n "$A" ] && echo disabled > $A/power/wakeup 2>/dev/null && say "已关 ec-adapter 唤醒（否则 pm_wakeup_pending 会挡住挂起）"

S=/sys/class/usb_role/a600000.usb-role-switch/role
D=/sys/bus/platform/devices/a600000.usb
echo host > $S 2>>$R; sleep 3
say "★ role=[$(cat $S)]  子xhci=$(ls $D/ | grep -c ^xhci)"
[ "$(cat $S)" != host ] && { say "⚠️ role 没改成 host，中止（不冒睡死风险）"; sync; sleep 5; systemctl reboot; }

echo none > /sys/power/pm_test
say "pm_test=[$(cat /sys/power/pm_test)]  ← 真实挂起，不是测试模式"
say "挂起前 suspend_stats: success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail)"

for i in 1 2 3; do
    echo 0 > /sys/class/rtc/rtc0/wakealarm
    echo +40 > /sys/class/rtc/rtc0/wakealarm
    AL=$(cat /sys/class/rtc/rtc0/wakealarm)
    [ -z "$AL" ] && { say "⚠️ 闹钟没设上，中止"; break; }
    say "RTC 闹钟=$AL（$(( AL - $(date +%s) )) 秒后）"
    say "★★★ PRE 第$i 次真实挂起 —— 下一行如果出现，就是醒回来了"
    sync
    echo mem > /sys/power/state
    RC=$?
    say "★★★ POST 第$i 次  rc=$RC  success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail) step=[$(cat /sys/power/suspend_stats/last_failed_step)] dev=[$(cat /sys/power/suspend_stats/last_failed_dev)]"
    dmesg | grep -iE "PM: suspend|Restarting tasks|PM: resume" | tail -5 >> $R
    sync
    sleep 10
done
say "==== 三次做完 ===="
say "最终 success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail)"
say "DONE —— 12 秒后回 Android"
sync; sleep 12
systemctl reboot
