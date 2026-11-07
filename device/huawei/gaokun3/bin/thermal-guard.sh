#!/vendor/bin/sh
#
# ⚠️ 已退役（2026-08-20，M6）。本文件【不再随镜像安装】。
#
# 它存在的理由是主线 sc8280xp.dtsi 的 CPU 温区没有任何 cooling device。
# 那个缺口现在由 patches/kernel/dts/0002-arm64-dts-sc8280xp-add-cpu-cooling-maps.patch
# 在 DTS 里根治了：8 个 cpuN-thermal 各加一条 85C passive trip 并绑到本簇的
# cpufreq cooling device（实机验证 cdev 0->1、trip 1->2）。
#
# ★ 不要在已打 0009 的 DTB 上再跑这个脚本：温区绑定之后 cur_state 归内核
#   thermal core 管，用户态再写同一个节点会与 step_wise 互相覆盖。
#   文件留在这里只为还在用旧 DTB 的人。
#
# 用户态 CPU 温控 —— 补主线 sc8280xp.dtsi 的缺口。
#
# ★为什么需要它（2026-08-20 M4 实测，用户在跑原神时发现）：
#   主线 `arch/arm64/boot/dts/qcom/sc8280xp.dtsi` 里**总共只有一个
#   `cooling-maps`，就在 `gpu-thermal` 下面**。所有 CPU 相关 zone
#   （cpu0..cpu7-thermal、cluster0-thermal）都只有一条
#       cpu-crit { temperature = <110000>; type = "critical"; }
#   —— 没有 passive trip，也没有绑定任何 cooling device。实测：
#       gpu-thermal   : trip 85C passive -> devfreq-3d00000.gpu   ✅ 有温控
#       cpu0-7/cluster: 只有 110C critical，cdev 一个都没绑        ❌ 没温控
#       mem-thermal   : 90C hot，也没绑
#   后果：CPU 会一路满频跑到 110°C，然后撞 critical trip ——
#   那不是降频，是**内核紧急关机**，中间没有任何渐进保护。
#   这台是**被动散热无风扇**平板，长时间游戏真会撞上。
#
#   ⚠️ 这是**主线本身的缺口**，不是本项目弄坏的；所有 sc8280xp 主线设备
#      （含 ThinkPad X13s）都一样。根治要改 DTS：给 CPU zone 加 passive trip
#      + cooling-maps（可只重编 DTB，不必重编内核）。本脚本是在那之前的安全网。
#
# 做法：`cpufreq-cpu0`(max_state 20) 与 `cpufreq-cpu4`(max_state 17) 这两个
# cooling device **确实存在**（DTS 的 CPU 节点有 `#cooling-cells = <2>`），
# 只是没有任何 thermal zone 接管它们 —— 所以用户态可以直接写 cur_state。
# 比改 scaling_max_freq 干净：频率映射交给内核 cpufreq-cooling 驱动，
# 退出时归零即可完全恢复。
# 实测可控：cd0 cur_state 0->1 使 policy0 scaling_max_freq 2438400 -> 2342400。
#
# ⚠️ PATH 必须钉 /system/bin —— 与 smmu-nostall.sh / audio-route.sh 同一个坑：
#    本脚本以 `#!/vendor/bin/sh` 起，PATH 默认优先 /vendor/bin，于是 log/sleep
#    每次都去 exec /vendor/bin/toybox_vendor，而服务跑在 u:r:shell:s0 域下对
#    vendor_toolbox_exec 无权限 → permissive 下功能正常但疯狂刷 avc denied。
PATH=/system/bin:/system/xbin
export PATH

CD0=/sys/class/thermal/cooling_device0   # cpufreq-cpu0
CD1=/sys/class/thermal/cooling_device1   # cpufreq-cpu4

# 认名字而不是认编号（编号可能随内核变动）
for c in /sys/class/thermal/cooling_device*; do
    case "$(cat $c/type 2>/dev/null)" in
        cpufreq-cpu0) CD0=$c ;;
        cpufreq-cpu4) CD1=$c ;;
    esac
done
M0=$(cat $CD0/max_state 2>/dev/null || echo 0)
M1=$(cat $CD1/max_state 2>/dev/null || echo 0)
[ "$M0" -gt 0 ] 2>/dev/null || { log -t thermalguard "找不到 cpufreq-cpu0 cooling device，放弃"; exit 1; }

# 只盯**没有** cooling-map 保护的 zone；GPU 有内核自己的 passive trip，不插手
ZONES=""
for z in /sys/class/thermal/thermal_zone*; do
    case "$(cat $z/type 2>/dev/null)" in
        cpu*-thermal|cluster*-thermal|mem-thermal) ZONES="$ZONES $z/temp" ;;
    esac
done

# 温度 -> 目标限制百分比。110C 是 critical，所以 105C 以上也只压到 90%，
# 留一点余量让机器还能用（压到 100% 会把 CPU 钉在最低频）。
target_pct() {
    t=$1
    if   [ "$t" -lt  80000 ]; then echo 0
    elif [ "$t" -lt  85000 ]; then echo 10
    elif [ "$t" -lt  90000 ]; then echo 25
    elif [ "$t" -lt  95000 ]; then echo 45
    elif [ "$t" -lt 100000 ]; then echo 65
    elif [ "$t" -lt 105000 ]; then echo 80
    else                             echo 90
    fi
}

restore() { echo 0 > $CD0/cur_state 2>/dev/null; echo 0 > $CD1/cur_state 2>/dev/null; }
trap 'restore; log -t thermalguard "退出，已解除限频"; exit 0' TERM INT HUP

cur=0; round=0; peak=0
log -t thermalguard "启动：接管 $(cat $CD0/type)(max=$M0) + $(cat $CD1/type)(max=$M1)"

while true; do
    MT=0
    for f in $ZONES; do
        v=$(cat "$f" 2>/dev/null)
        [ -n "$v" ] && [ "$v" -gt "$MT" ] 2>/dev/null && MT=$v
    done
    [ "$MT" -gt "$peak" ] 2>/dev/null && peak=$MT

    P=$(target_pct "$MT")
    # step-wise：一次只挪 5%，避免帧率突变
    if   [ "$P" -gt "$cur" ]; then cur=$((cur + 5)); [ "$cur" -gt "$P" ] && cur=$P
    elif [ "$P" -lt "$cur" ]; then cur=$((cur - 5)); [ "$cur" -lt "$P" ] && cur=$P
    fi

    S0=$((M0 * cur / 100)); S1=$((M1 * cur / 100))
    O0=$(cat $CD0/cur_state 2>/dev/null)
    if [ "$S0" != "$O0" ]; then
        echo $S0 > $CD0/cur_state 2>/dev/null
        echo $S1 > $CD1/cur_state 2>/dev/null
        log -t thermalguard "温度 $((MT/1000))C -> 限制 ${cur}% (cpu0=$S0/$M0 cpu4=$S1/$M1)"
    fi

    round=$((round + 1))
    [ $((round % 150)) -eq 0 ] && log -t thermalguard "心跳 当前 $((MT/1000))C 峰值 $((peak/1000))C 限制 ${cur}%"
    sleep 2
done
