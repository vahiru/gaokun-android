#!/bin/bash
# 验收：udev 规则是否开机自动把 role 置成 host，以及在此之上真实挂起是否稳定。
# ★ 脚本【不】自己写 role —— 那正是要验的东西。不是 host 就拒绝挂起。
R=/var/log/s2verify.log
say() { echo "[$(date +%H:%M:%S) wall=$(date +%s) up=$(cut -d. -f1 /proc/uptime)] $*" >> $R; sync; }
rm -f /etc/systemd/system/multi-user.target.wants/s71test.service
: > $R
say "START 验收 udev 规则  内核=$(uname -v | cut -d' ' -f1)"
sleep 25
S=/sys/class/usb_role/a600000.usb-role-switch/role
say "★ 开机后 role=[$(cat $S 2>/dev/null)]（udev 规则应已生效，期望 host）"
say "  子xhci=$(ls /sys/bus/platform/devices/a600000.usb/ | grep -c ^xhci)"
if [ "$(cat $S 2>/dev/null)" != host ]; then
    say "⚠️ role 不是 host —— udev 规则没生效，拒绝挂起。8 秒后回 Android"
    sync; sleep 8; systemctl reboot
fi
A=$(readlink -f /sys/class/power_supply/gaokun-ec-adapter 2>/dev/null)
[ -n "$A" ] && echo disabled > $A/power/wakeup 2>/dev/null && say "已关 ec-adapter 唤醒"
echo none > /sys/power/pm_test
OK=0
for i in 1 2 3; do
    echo 0 > /sys/class/rtc/rtc0/wakealarm; echo +40 > /sys/class/rtc/rtc0/wakealarm
    say "PRE 真实挂起 第$i/3（闹钟 $(cat /sys/class/rtc/rtc0/wakealarm)）"
    sync
    echo mem > /sys/power/state
    RC=$?
    [ $RC -eq 0 ] && OK=$((OK+1))
    say "POST 第$i/3 rc=$RC success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail)"
    sleep 10
done
say "==== 验收结果：真实挂起 $OK/3，success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail) ===="
say "DONE —— 10 秒后回 Android"
sync; sleep 10
systemctl reboot
