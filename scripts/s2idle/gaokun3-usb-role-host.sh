#!/bin/sh
# gaokun3：把 a600000.usb 的 USB role switch 置为 host —— s2idle 能工作的前提。
# 见 docs/stage4-findings.md #52 / #53 / #54，以及同目录 README。
# ⚠️ 只用于【救援 Ubuntu】。Android 不能用：USB adb 的 UDC 就在这个控制器上。
S=/sys/class/usb_role/a600000.usb-role-switch/role
[ -e "$S" ] || { logger -t gaokun3-usb-role "没有 $S，跳过"; exit 0; }
CUR=$(cat "$S" 2>/dev/null)
if [ "$CUR" = host ]; then
    logger -t gaokun3-usb-role "已经是 host，无需改动"
    exit 0
fi
echo host > "$S" 2>/dev/null
NEW=$(cat "$S" 2>/dev/null)
NX=$(ls /sys/bus/platform/devices/a600000.usb/ 2>/dev/null | grep -c '^xhci')
if [ "$NEW" = host ]; then
    logger -t gaokun3-usb-role "role: $CUR -> host（子 xhci = $NX）"
else
    logger -t gaokun3-usb-role "⚠️ 置 host 失败，仍是 $NEW —— 此时挂起会整板复位"
    exit 1
fi
