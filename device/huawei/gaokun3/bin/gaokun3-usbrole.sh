#!/vendor/bin/sh
# 挂起前把 a600000.usb 的 USB role 切到 host，恢复后切回 device。
#
# 为什么：那个控制器停在 role=device 时，设备挂起阶段会【整板复位】，不留任何日志
# （固件/TZ 级复位，pstore 抓不到）。实测双臂对照：role=host 5/5 通过、
# role=device 第 1 次就复位。而 USB device-mode adb 的 UDC 就在它上面
# （sys.usb.controller=a600000.usb），所以不能简单改成 host 了事。
# 案卷：docs/stage4-findings.md #52 / #54 / #56。
#
# ★ 不变量：wakelock `gaokun3_usbrole` 一直持有，【除非已确认 role 真的是 host】。
#   任何失败路径都保持持有 → 结果只是"不挂起"，绝不会"带着 device 模式去挂起"。
#
# ⚠️ dwc3 的模式切换是【异步】的（dwc3_set_mode 只 queue_work），
#    写完 sysfs 就走会漏掉切换未完成的窗口 —— 必须轮询确认。

WANT="$1"
S=/sys/class/usb_role/a600000.usb-role-switch/role
D=/sys/bus/platform/devices/a600000.usb
WL=gaokun3_usbrole
TAG=gaokun3-usbrole

say() { log -t $TAG "$*"; }

case "$WANT" in
    host|device) ;;
    *) say "用法: $0 host|device"; exit 2 ;;
esac

if [ ! -e "$S" ]; then
    say "没有 $S —— 不做任何事（wakelock 保持原状）"
    exit 0
fi

# ★ 先把门关上，再动 role。失败路径全都停在这个状态。
echo $WL > /sys/power/wake_lock

echo "$WANT" > "$S" 2>/dev/null

# 轮询确认（最多约 6 秒）
i=0
OK=0
while [ $i -lt 60 ]; do
    CUR=$(cat "$S" 2>/dev/null)
    if [ "$CUR" = "$WANT" ]; then
        if [ "$WANT" = host ]; then
            # host 模式的判据是 xhci 子设备真的实例化了，不是 role 读回来对
            NX=$(ls "$D"/ 2>/dev/null | grep -c '^xhci')
            [ "$NX" -ge 1 ] && { OK=1; break; }
        else
            [ -e /sys/class/udc/a600000.usb ] && { OK=1; break; }
        fi
    fi
    sleep 0.1
    i=$((i + 1))
done

NX=$(ls "$D"/ 2>/dev/null | grep -c '^xhci')
if [ "$WANT" = host ]; then
    if [ "$OK" = 1 ]; then
        echo $WL > /sys/power/wake_unlock
        say "已确认 role=host（子 xhci=$NX，耗时 $((i * 100))ms）→ 放行挂起"
    else
        say "⚠️ 切 host 失败：role=[$(cat $S 2>/dev/null)] 子xhci=$NX —— 保持 wakelock，不放行挂起"
    fi
else
    say "role=[$(cat $S 2>/dev/null)] UDC=[$(ls /sys/class/udc/ 2>/dev/null)] 确认=$OK —— wakelock 保持持有"
fi
