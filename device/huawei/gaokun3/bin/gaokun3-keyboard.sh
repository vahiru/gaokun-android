#!/vendor/bin/sh
# 开/关磁吸键盘（键盘 + 触控板一起）。
#
# 用法: gaokun3-keyboard.sh on|off
#
# 机制：内核的 /sys/class/input/inputN/inhibited。写 1 之后设备还在、
# evdev 节点还在，但不再上报事件也不再唤醒系统 —— 这正是"关掉键盘"该有的语义，
# 比解绑 USB 干净得多（解绑会让重新插拔才能恢复）。
#
# ★ 为什么按名字匹配而不是写死 eventN：本机这套键盘一共注册【6 个】input 设备
#   （两个 interface 各自的 keyboard / mouse / touchpad / consumer-control），
#   编号还会随插拔顺序变。实测名字全部以 "HID 12d1:10b8" 开头。
#
# ⚠️ 触控板一起关是有意的：只关键盘会留下一个能动光标却打不了字的半残状态。

WANT="$1"
case "$WANT" in
    on)  VAL=0 ;;
    off) VAL=1 ;;
    *)   log -t gaokun3-keyboard "用法: $0 on|off"; exit 2 ;;
esac

N=0
for d in /sys/class/input/input*; do
    [ -f "$d/name" ] || continue
    case "$(cat "$d/name" 2>/dev/null)" in
        "HID 12d1:10b8"*) ;;
        *) continue ;;
    esac
    [ -w "$d/inhibited" ] || { log -t gaokun3-keyboard "$d/inhibited 不可写"; continue; }
    echo "$VAL" > "$d/inhibited" 2>/dev/null && N=$((N + 1))
done

if [ "$N" = 0 ]; then
    log -t gaokun3-keyboard "键盘已 $WANT：没有匹配的设备（键盘没接？）"
else
    log -t gaokun3-keyboard "键盘已 $WANT（inhibited=$VAL，共 $N 个 input 设备）"
fi
