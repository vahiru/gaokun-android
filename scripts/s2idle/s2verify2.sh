#!/bin/bash
# 验收（正确版）：用 systemctl suspend —— 只有它才会跑 /usr/lib/systemd/system-sleep 钩子。
# `echo mem > /sys/power/state` 绕过钩子，所以用它验证这个修复是【无效的判据】。
R=/var/log/s2verify2.log
say() { echo "[$(date +%H:%M:%S) wall=$(date +%s) up=$(cut -d. -f1 /proc/uptime)] $*" >> $R; sync; }
rm -f /etc/systemd/system/multi-user.target.wants/s71test.service
: > $R
say "START 验收 system-sleep 钩子（用 systemctl suspend）"
sleep 25
S=/sys/class/usb_role/a600000.usb-role-switch/role
say "挂起前 role=[$(cat $S)] 子xhci=$(ls /sys/bus/platform/devices/a600000.usb/ | grep -c ^xhci)"
say "  （开机单元置过 host，但 typec/UCSI 会改回 device —— 钩子的作用就是挂起前再置一次）"
A=$(readlink -f /sys/class/power_supply/gaokun-ec-adapter 2>/dev/null)
[ -n "$A" ] && echo disabled > $A/power/wakeup 2>/dev/null
echo none > /sys/power/pm_test

OK=0
for i in 1 2 3; do
    echo 0 > /sys/class/rtc/rtc0/wakealarm; echo +40 > /sys/class/rtc/rtc0/wakealarm
    B4=$(date +%s); S0=$(cat /sys/power/suspend_stats/success)
    say "PRE 第$i/3  role=[$(cat $S)]  闹钟=$(cat /sys/class/rtc/rtc0/wakealarm)"
    sync
    systemctl suspend           # ← 走钩子；这是异步的，所以下面要等
    sleep 75                    # 40 秒睡 + 余量
    S1=$(cat /sys/power/suspend_stats/success)
    DT=$(( $(date +%s) - B4 ))
    if [ "$S1" -gt "$S0" ]; then OK=$((OK+1)); say "★ 第$i/3 挂起成功（success $S0→$S1，墙钟 ${DT}s）role 现在=[$(cat $S)]"
    else say "  第$i/3 没成功（success 仍 $S0，墙钟 ${DT}s）fail=$(cat /sys/power/suspend_stats/fail) dev=[$(cat /sys/power/suspend_stats/last_failed_dev)]"; fi
    dmesg | grep -iE "PM: suspend entry|PM: suspend exit" | tail -2 >> $R
    sync; sleep 5
done
say "==== 验收结果：$OK/3  success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail) ===="
say "钩子日志:"; journalctl -t gaokun3-usb-role --no-pager -n 6 2>&1 | sed 's/^/    /' >> $R
say "DONE —— 10 秒后回 Android"
sync; sleep 10
systemctl reboot
exit 0
