#!/bin/bash
# 诊断：开机自动置 host 为什么没生效？单元没跑，还是写失败？
R=/var/log/s2diag.log
say() { echo "[$(date +%H:%M:%S) up=$(cut -d. -f1 /proc/uptime)] $*" >> $R; sync; }
rm -f /etc/systemd/system/multi-user.target.wants/s71test.service
: > $R
say "START 诊断"
sleep 25
S=/sys/class/usb_role/a600000.usb-role-switch/role
say "当前 role=[$(cat $S 2>/dev/null)]"
say "---- systemd 单元 ----"
say "  is-enabled: $(systemctl is-enabled gaokun3-usb-role.service 2>&1)"
say "  is-active:  $(systemctl is-active gaokun3-usb-role.service 2>&1)"
systemctl status gaokun3-usb-role.service --no-pager -l 2>&1 | head -12 | sed 's/^/    /' >> $R
say "---- journal 里的 gaokun3-usb-role ----"
journalctl -t gaokun3-usb-role --no-pager -n 10 2>&1 | sed 's/^/    /' >> $R
say "---- 手动跑一次，抓 stderr ----"
OUT=$( { /usr/local/bin/gaokun3-usb-role-host.sh ; } 2>&1 ); RC=$?
say "  rc=$RC 输出=[$OUT]"
say "  跑完 role=[$(cat $S)]  子xhci=$(ls /sys/bus/platform/devices/a600000.usb/ | grep -c ^xhci)"
say "---- 直接写，抓 stderr ----"
E2=$( { echo host > $S ; } 2>&1 ); R2=$?
say "  rc=$R2 [$E2]  → role=[$(cat $S)]"
say "---- udev 规则匹配情况 ----"
udevadm test /sys/class/usb_role/a600000.usb-role-switch 2>&1 | grep -iE "gaokun3|role|ATTR" | head -8 | sed 's/^/    /' >> $R
say "---- dmesg 里 dwc3/role 相关 ----"
dmesg | grep -iE "dwc3|role|a600000" | tail -8 | sed 's/^/    /' >> $R
say "DONE —— 10 秒后回 Android"
sync; sleep 10
systemctl reboot
exit 0
