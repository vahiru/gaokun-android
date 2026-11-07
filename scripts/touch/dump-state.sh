#!/usr/bin/env bash
# 一次性抓下触摸驱动的全部可观测状态。需要内核 #23 及以上（patches/kernel/input/0007）。
#
#   bash scripts/touch/dump-state.sh [输出目录]
#
# 抓什么：
#   stats           逐级计数器（哪一级放行/丢弃了候选）
#   contacts_log    最近 16 个触点出生时的 x/y/面积/信号/是否在边缘
#   frame_raw.bin   面板【产出】的 40×60 网格（仅去基线）
#   frame.bin       流水线【判定】的同一张网格
#   knobs           54 个算法参数的当前值
#   params          模块参数（fuzz / disable_pressure）
#   absinfo         框架看到的轴信息（fuzz / resolution 是否生效）
#   interrupts      触摸 IRQ 计数（状态指纹：≈面板刷新率=正常，0=IC 停摆，乱=模式错乱）
set -uo pipefail
SER=${SER:-gaokun3}
OUT=${1:-touchdump-$(date +%Y%m%d-%H%M%S)}
S() { adb -s "$SER" shell "$@"; }
mkdir -p "$OUT"

ALGO=$(S 'for d in /sys/bus/spi/devices/*/algo; do [ -d "$d" ] && echo "$d" && break; done' | tr -d '\r')
[ -n "$ALGO" ] || { echo "✗ 找不到 algo 目录 —— 触摸驱动没起来？" >&2; exit 1; }
DBG=/sys/kernel/debug/himax-hx83121a
# ★ 节点号会变（重启后 event7 曾变成键盘）—— 按名字找
EV=$(S "grep -A8 'Name=\"Himax' /proc/bus/input/devices | grep -o 'event[0-9]*' | head -1" | tr -cd 'a-z0-9')
[ -n "$EV" ] || { echo '✗ 找不到 Himax 触摸节点' >&2; exit 1; }
echo "触摸节点: /dev/input/$EV"

S "cat $ALGO/stats"        > "$OUT/stats"        2>/dev/null
S "cat $ALGO/contacts_log" > "$OUT/contacts_log" 2>/dev/null
S "for f in $ALGO/*; do [ -f \$f ] && printf '%-24s %s\n' \"\$(basename \$f)\" \"\$(cat \$f 2>/dev/null | head -1)\"; done" \
                           > "$OUT/knobs"        2>/dev/null
S 'for f in /sys/module/himax_hx83121a_spi/parameters/*; do printf "%-20s %s\n" "$(basename $f)" "$(cat $f)"; done' \
                           > "$OUT/params"       2>/dev/null
S "getevent -lp /dev/input/$EV" > "$OUT/absinfo" 2>/dev/null
S 'grep -i himax /proc/interrupts' > "$OUT/interrupts" 2>/dev/null

# ★ 整帧走 debugfs（4800 字节，sysfs 的 PAGE_SIZE 装不下）。
#   用 adb exec-out 保二进制原样 —— adb shell 会把 \n 变成 \r\n，把帧毁掉。
if S "[ -r $DBG/frame_raw ]"; then
    adb -s "$SER" exec-out "cat $DBG/frame_raw" > "$OUT/frame_raw.bin"
    adb -s "$SER" exec-out "cat $DBG/frame"     > "$OUT/frame.bin"
    for f in frame_raw frame; do
        sz=$(wc -c < "$OUT/$f.bin" | tr -d ' ')
        [ "$sz" = 4800 ] || echo "⚠️ $f.bin 是 $sz 字节，应为 4800 —— 传输被改过？"
    done
else
    echo "⚠️ $DBG 不存在 —— 跑的不是带 patches/kernel/input/0007 的内核（#23 起）"
fi

echo "✓ 已存到 $OUT/"
echo
sed -n '1,8p' "$OUT/stats" 2>/dev/null
echo "…（完整内容见 $OUT/stats）"
[ -s "$OUT/frame_raw.bin" ] && echo && python3 "$(dirname "$0")/grid.py" "$OUT/frame_raw.bin" 2>/dev/null | tail -3
