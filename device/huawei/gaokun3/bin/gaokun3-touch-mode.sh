#!/vendor/bin/sh
# 触摸手感模式：game（不做手掌识别）/ daily（识别手掌）
#
# 后端是 himax hx83121a 驱动自己暴露的算法参数
# （/sys/bus/spi/devices/spi0.0/algo/，共 54 项），运行期可改、立刻生效。
# 这里动六项（原来只动三项，取自上游 EGoTouchRev 的 game_preset；hand_enabled 来自 patches/0082）：
#   track_smoothing      基于速度预测的坐标平滑。开着更稳，代价是多一层延迟。
#                        实现是 x = (3*旧 + 新)/4，稳态滞后 3 帧 ≈ 25 ms —— 不小。
#                        ★ 两版都给 0：驱动对上报的位置做一欧滤波（track_filter_*，patches/0120 加入、0122 起默认打开），
#                        慢时平滑、快时几乎不滞后；再叠这层固定平滑只会多出滞后。
#   track_start_debounce touch_active 要连续几帧才确认。2 帧 ≈ 按下多 1 帧（8 ms）延迟，
#                        换来的是噪声不那么容易变成一次"按下"。
#   track_jump_dist2     跳点检测阈值的平方。⚠️★★ **两版预设一律给 0**，见下。
#   hand_enabled         手的位置图（手掌识别）。game 关：贴着屏幕边缘平放的拇指会被掌压规则认成
#                        手掌，位置图会把它周围、以及手抬起后约 300 ms 内的点按一起丢掉。daily 开。
#
# ⚠️★★★ 2026-09-14：`track_jump_dist2=6400` 是本仓自己制造的重大缺陷，已从两版预设清掉。
#    驱动的跳点检测拿【原始位移】跟它比，于是它等于一条 **1.0 m/s 的限速线**；越过之后
#    轨迹被复位、新槽的 debounce 永远回不到 0 ⇒ **手指只要持续快过它，驱动一个点都不上报**。
#    实测（同一个人、同样的甩动、录 event7 原始流）：
#        jump=6400  手指在屏 8.0 s / 路径 3.99 m → 系统收到 **87 条**轨迹，37% 是 ≤5 帧碎片
#        jump=0     手指在屏 10.3 s / 路径 4.07 m → 系统收到 **6 条**，0 碎片
#    一次连续滑动被切成十几次独立触摸：滑动惯性反复清零（列表甩不动）、手势被判取消，
#    而沿途多出来的那些"按下"**就是用户报的"幽灵触摸"**。两个症状同一个根因。
#    内核 `patches/0038` 把判据改成【与预测位置的偏差】之后这个功能才真正可用；
#    在带 0038 的内核上实机验过之前，**这里保持 0**。说明见 `scripts/touch/README.md`。
#
# ⚠️ 只动这几项是【故意的】。其余参数（基线 / CMF 共模滤波 / 检测门限 / 掌压 / 边缘补偿）
#    的默认值是按这块面板标定、实机验过的，乱改会带来幽灵触点或断触，属于要实机逐项验的活。

MODE="$1"
case "$MODE" in
    # game 与 daily 只差 hand_enabled：游戏里不要手掌识别。坐标平滑交给驱动的一欧滤波，
    # 两版都关 track_smoothing。
    # DEB=1：2026-09-16 实测（#116）36 次点击 + 甩动，0 条 ≤2 帧的短触点、guard_kill=0，
    #        按下延迟 25 → 17 ms 无代价。空载 14636 帧一个像素都没越过阈值，噪声离阈值 11.6 倍。
    game)  SMOOTH=0; DEB=1; JUMP=0; HAND=0 ;;
    daily) SMOOTH=0; DEB=1; JUMP=0; HAND=1 ;;
    *) log -t gaokun3-touch "用法: $0 game|daily"; exit 2 ;;
esac

# ★ 不写死那条很长的 DT 路径（/sys/devices/platform/soc@0/9c0000.geniqup/...）：
#   换内核或 SPI 编号变了它就失效，而且失效时【毫无征兆】。按 algo 目录找。
ALGO=""
for d in /sys/bus/spi/devices/*/algo; do
    [ -d "$d" ] && ALGO="$d" && break
done
[ -n "$ALGO" ] || { log -t gaokun3-touch "找不到 algo 目录 —— 触摸驱动没起来？"; exit 1; }

fail=0
# ★ debounce_base 与 track_start_debounce 一起降；pressure_enabled 在轴存在时必须为 1
#   （否则驱动报常数 1/4095，每个触点都成"针尖"，比没有轴还糟 —— patches/0047 起默认已跟着轴走，这里兜底）。
for kv in "track_smoothing=$SMOOTH" "track_start_debounce=$DEB" "debounce_base=$DEB" "track_jump_dist2=$JUMP" "pressure_enabled=1"; do
    k="${kv%%=*}"; v="${kv##*=}"
    if ! echo "$v" > "$ALGO/$k" 2>/dev/null; then
        log -t gaokun3-touch "写 $k 失败"; fail=1; continue
    fi
    # ★ 回读校验：sysfs 写入成功不代表值被接受（驱动可能钳位或拒绝）
    got="$(cat "$ALGO/$k" 2>/dev/null)"
    [ "$got" = "$v" ] || { log -t gaokun3-touch "$k 回读 $got != 期望 $v"; fail=1; }
done

# ★ hand_enabled 单独写：没有 patches/0082 的内核没有这个节点，跳过而不算失败。
#   daily 也要显式写 1，否则从 game 切回来时它还停在 0。
if [ -e "$ALGO/hand_enabled" ]; then
    if ! echo "$HAND" > "$ALGO/hand_enabled" 2>/dev/null ||
       [ "$(cat "$ALGO/hand_enabled" 2>/dev/null)" != "$HAND" ]; then
        log -t gaokun3-touch "hand_enabled 写入或回读失败（期望 $HAND）"; fail=1
    fi
fi

if [ "$fail" = 0 ]; then
    log -t gaokun3-touch "触摸模式 = ${MODE}（${ALGO}）"
else
    log -t gaokun3-touch "触摸模式 $MODE 未完全应用"
fi
exit $fail
