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
#   它只约束这个脚本自己：vreg_l9d 常开（patches/0086）以后 device 模式挂起不再复位，
#   USB 端口 HAL（usb/Follow.h）按对端切角色时不看屏幕，也不拿这把锁。
#
# ⚠️ dwc3 的模式切换是【异步】的（dwc3_set_mode 只 queue_work），
#    写完 sysfs 就走会漏掉切换未完成的窗口 —— 必须轮询确认。

WANT="$1"
S=/sys/class/usb_role/a600000.usb-role-switch/role
D=/sys/bus/platform/devices/a600000.usb
UDC=/sys/class/udc/a600000.usb/state
ADAPTER=/sys/class/power_supply/gaokun-ec-adapter/online
WL=gaokun3_usbrole
TAG=gaokun3-usbrole

say() { log -t $TAG "$*"; }

# xhci 下有没有下游设备（根集线器 usbN 下出现 `1-1` 这类子设备目录）。device 分支用。
# ★ 这是"对面是不是设备"的电气事实：只有 host 模式、xhci 绑上驱动、真枚举出东西时才成立。
#   不用 typec 的 power_role 判断：实机上没插东西的 port1 也报 [source]，那是空口的默认值（v1.0 计划 STOR-2 复核）。
has_downstream() {
    for u in "$D"/xhci-hcd.*/usb*; do
        [ -d "$u" ] && ls "$u" 2>/dev/null | grep -qE '^[0-9]+-[0-9.]+$' && return 0
    done
    return 1
}

# PWR-4 止损：切 host 失败（A6：待机后回插 port0，xhci 一律 -110）时置 1，确认 host 成功时清 0。
#   USB 端口 HAL 也会设它（我方供电、切 host 后 xhci 没起来，usb/Follow.cpp）。
#   不持久 —— 口坏了要重启才好，重启后它自然没了。Parts 据此发"USB 口异常，重启后恢复"的通知（UsbPortNotifier）。
BROKEN=vendor.gaokun3.usbrole.broken

# ── USB-2（v1.0，2026-10-05）：port0 在 host 时让 adbd 的 USB 传输停下 ──────────────
# 现象（1.0.0-dev.1 实机 dmesg）：port0 停在 host 时每秒一组 init 的 "symlink … File exists" +
#   "write …/UDC … Device or resource busy"，外加 f_fs 的 "read descriptors" / "bcdVersion"。
# 谁在每秒重设 sys.usb.ffs.ready —— adbd 自己（LineageOS packages_modules_adb lineage-23.2 的上游副本）：
#   · daemon/usb.cpp:274-282：UsbFfsConnection 的监视线程等 FUNCTIONFS_BIND 只等 1 秒，等不到就断开重来；
#   · daemon/usb.cpp:742-766：usb_ffs_open_thread 随即重开 functionfs —— daemon/usb_ffs.cpp:282-302 重写描述符
#     （f_fs 打 "read descriptors"）并 SetProperty("sys.usb.ffs.ready", "1")；
#   · 同值 setprop 也会触发（init 无条件 NotifyPropertyChange），于是 init.usb.configfs.rc:20-24 每秒重跑一遍：
#     symlink 已存在、写 UDC 失败 —— host 模式下 a600000.usb 这个 UDC 根本不存在，configfs 的 gadget 驱动是
#     match_existing_only（linux v7.2-rc2 drivers/usb/gadget/configfs.c:1985），注册时找不到 UDC 就返回 -EBUSY
#     （drivers/usb/gadget/udc/core.c:1733-1737），udc_name 随即清空（configfs.c:295-303）⇒ 不会挂起等待，下一秒再来。
#     patches/0112 起写 UDC 不再失败：驱动挂起等 UDC 出现，再写同名就重新注册；adbd 照样每秒重开 functionfs、重写描述符。
# 上游早就留了开关，正是为这种情况：usb.cpp:728-740 —— "When the device is acting as a USB host, we'll be unable
#   to bind to the USB gadget…"，读 sys.usb.adb.disabled，为真就在 open_functionfs 之后停在 PropertyMonitor 里等它
#   变回假（:751-755）；init.usb.rc:23-24（refs/lineage-system-core/rootdir/）把 vendor.sys.usb.adb.disabled 拷过去。
# ⇒ 我们按【实际角色】设它：host ⇒ 1，device ⇒ 0。只停 adbd 的 USB 传输：不动 sys.usb.config（B1 的「USB 调试」
#   开关语义和 UsbDeviceManager 选的「USB 用途」都不受影响），TCP / 无线调试照常。
#   切回 device、设回 0 之后：adbd 起连接 → 1 秒等不到 BIND → 重开 ffs → ffs.ready=1 → init 写 UDC（此时 UDC 已在）
#   → 绑上，USB adb 约 1–2 秒回来 —— 与现在"切回 device 后靠每秒重试碰上"是同一条路，只是 host 期间不再空转。
# 属性链：本脚本 setprop vendor.gaokun3.usbrole.adb_pause（vendor_gaokun3_prop，本域有 set_prop）→ etc/usbrole.rc
#   用两条字面量触发器 setprop vendor.sys.usb.adb.disabled（vendor_default_prop：只有 init / vendor_init 能设，
#   refs/lineage-sepolicy/private/domain.te:802；vendor_init 有 set_prop，vendor_init.te:306）→ init.usb.rc 拷成
#   sys.usb.adb.disabled（system_prop，adbd 能读：core_property_type，private/domain.te:461/467）。
# ⚠️ 读源码得出、未上机。判据见 etc/usbrole.rc 的 USB-2 一段。
ADB_PAUSE=vendor.gaokun3.usbrole.adb_pause
adb_gate() {
    r=""
    read -r r 2>/dev/null < "$S"
    case "$r" in
        host)   want=1 ;;
        device) want=0 ;;
        *) return 0 ;;
    esac
    [ "$(getprop $ADB_PAUSE)" = "$want" ] && return 0
    setprop $ADB_PAUSE $want
    if [ $want = 1 ]; then
        say "role=host ⇒ $ADB_PAUSE=1（adbd 的 USB 传输停下，不再每秒重绑 UDC，USB-2）"
    else
        say "role=device ⇒ $ADB_PAUSE=0（adbd 的 USB 传输恢复，USB-2）"
    fi
}

case "$WANT" in
    host|device) ;;
    *) say "用法: $0 host|device"; exit 2 ;;
esac

if [ ! -e "$S" ]; then
    say "没有 $S —— 不做任何事（wakelock 保持原状）"
    exit 0
fi

# ★ 2026-09-14（#112）：插着 USB 主机（PC 在用 adb）时【不切 host、不放行挂起】。
#   依据：#56 实测 device 模式带着已枚举的 gadget 挂起照样整板复位，所以"插着线睡"在这块板子上
#   目前不可能安全；而切 host 就等于把用户正在用的 adb 拔掉。折中：插着主机 → 息屏但不睡（反正在充电），
#   拔线后再切 host 放行挂起（role_host 进程原地每 2 秒看一次，见下面的 host_unplugged）。判据用 UDC 的 state：
#   configured/addressed = 有主机在总线另一端；not attached = 没有。
#   ⚠️ 这不是根治。根治是让 dwc3 device 模式的挂起不复位（见 docs/stage4-findings.md #112）。
#   ⚠️ 拔线之后 UDC 的 state 不会变：一直停在 configured，直到重新插上、主机再次枚举（dwc3 没有得知
#   VBUS 消失）。只看 UDC 的话，被主机枚举过、再拔掉线以后（在息屏前拔还是息屏期间拔都一样），每次息屏
#   都会当成主机还在、一直等下去，wakelock 不放，到重启为止整机都不再睡。EC 报的适配器在线
#   （gaokun-ec-adapter/online）拔线当即变 0，所以先看它：不在线 ⇒ 没有主机。读不到时（节点不在或
#   被拒）按在线处理，退回只看 UDC 的判断，不变量不受影响。
vbus_present() {
    [ "$(cat $ADAPTER 2>/dev/null)" != 0 ]
}

host_attached() {
    vbus_present || return 1
    case "$(cat $UDC 2>/dev/null)" in
        configured|addressed|default) return 0 ;;
        *) return 1 ;;
    esac
}

# 确认插着主机以后，等拔线时只看适配器在线（#41 评审）：切"USB 用途"时 gadget 要重绑，dwc3 断开上拉，
#   UDC 会短暂变成 not attached；2 秒一次的轮询碰上这一瞬，就会当成拔线、切 host，把正在用的连接断掉。
#   适配器节点读不到时只能照旧看 UDC。主机让总线休眠（UDC=suspended，比如电脑睡了）也接着等，直到拔线或亮屏。
host_unplugged() {
    case "$(cat $ADAPTER 2>/dev/null)" in
        0) return 0 ;;
        1) return 1 ;;
        *) ! host_attached ;;
    esac
}

if [ "$WANT" = host ] && host_attached; then
    # 息屏期间插着主机：先关门，再【就在这个进程里】等到拔线（或亮屏）再切 host。
    # ⚠️★ 2026-09-29 改（SELinux 第七轮审计）：原先是 `(setsid "$0" watch &)` 另起一个 watch 进程、
    #   写 pid 文件到 /data/vendor/gaokun3，拔线后再 `exec "$0" host`。三处都站不住：
    #   ① enforcing 下重新执行自己要 execute_no_trans（init_daemon_domain 不给），pid 文件所在的
    #     vendor_data_file 也没给写 —— policy-query 全是 DENY；
    #   ② 更根本的：oneshot 服务的主进程退出时，init 对 vendor API ≥ R 会 SIGKILL 整个进程组
    #     （system/core/init/service.cpp:264-276；本机 ro.board.api_level=202504），setsid 出不了
    #     init 的 cgroup ⇒ watch 进程多半在 role_host 退出那一刻就被杀了（从源码推断，未实测 ——
    #     开发机 allow_suspend=0，这条路径从没跑过）。
    #   现在：role_host 自己留着等；亮屏时 usbrole.rc 先 `stop gaokun3_role_host` 再起 role_device。
    echo $WL > /sys/power/wake_lock
    say "USB 主机在线（UDC=$(cat $UDC 2>/dev/null)，适配器在线=$(cat $ADAPTER 2>/dev/null)）→ 保持 device、不放行挂起（充电中，息屏不睡）；拔线后自动切 host"
    until host_unplugged; do
        [ "$(getprop debug.tracing.screen_state)" = 2 ] && exit 0
        sleep 2
    done
    say "USB 主机已拔掉（UDC=$(cat $UDC 2>/dev/null)，适配器在线=$(cat $ADAPTER 2>/dev/null)）→ 现在切 host 放行挂起"
fi

# ★ 先把门关上，再动 role。失败路径全都停在这个状态。
echo $WL > /sys/power/wake_lock

# ★ v1.0（PWR-5 / STOR-2）：切 device 之前看对面是不是设备。
#   亮屏（usbrole.rc）和开机（init.gaokun3.usb.rc）都走这里。原先无条件写 device ⇒ port0 上由我们供电的
#   U 盘 / 鼠标 / 手柄一亮屏就掉；带 PD 的扩展坞每次亮屏断约 6 秒。
#   现在：已是 host 且 xhci 下枚举着东西 ⇒ 保持 host、不写 device。对面是 PC 时 host 模式下枚举不出任何东西，照旧切 device。
#   wakelock 照旧持有（上面刚拿）：亮屏时本来就不睡，息屏时 role_host 会重新确认 host 再放行 —— #52 的不变量不受影响。
#   ⚠️ 开机那次可能早于 U 盘枚举完成 ⇒ 那时仍会切 device（与改之前一样），这一点没有办法在不拖慢开机的前提下根治。
#   ⚠️ 源码推断，未实测：要用户在场插 U 盘做息屏→亮屏（v1.0 计划 PWR-5 的验收）。
if [ "$WANT" = device ] && [ "$(cat "$S" 2>/dev/null)" = host ] && has_downstream; then
    say "role=host 且 xhci 下有下游设备（$(ls "$D"/xhci-hcd.*/usb* 2>/dev/null | grep -E '^[0-9]+-[0-9.]+$' | tr '\n' ' ')）→ 保持 host、不切 device；wakelock 保持持有"
    adb_gate     # USB-2：停在 host ⇒ adbd 的 USB 传输停下（开机那次尤其要紧：USB 端口 HAL 可能还没起来）
    exit 0
fi

echo "$WANT" > "$S" 2>/dev/null

# 轮询确认（最多约 6 秒）
i=0
OK=0
while [ $i -lt 60 ]; do
    CUR=$(cat "$S" 2>/dev/null)
    if [ "$CUR" = "$WANT" ]; then
        if [ "$WANT" = host ]; then
            # host 模式的判据是 xhci 【绑上了驱动】，不是 role 读回来对。
            # ⚠️ 2026-09-23 以前数的是 `ls $D | grep ^xhci`（平台设备）—— xhci probe 失败（-110）时
            #    平台设备照样在，这个判据从来没真正验过 host 起来了（#118 §7）。
            NX=$(ls -d "$D"/xhci-hcd.*/driver 2>/dev/null | wc -l)
            [ "$NX" -ge 1 ] && { OK=1; break; }
        else
            [ -e /sys/class/udc/a600000.usb ] && { OK=1; break; }
        fi
    fi
    sleep 0.1
    i=$((i + 1))
done

NX=$(ls -d "$D"/xhci-hcd.*/driver 2>/dev/null | wc -l)
adb_gate         # USB-2：按读回来的实际角色同步（切不成时角色没变，它也就不动）
if [ "$WANT" = host ]; then
    if [ "$OK" = 1 ]; then
        echo $WL > /sys/power/wake_unlock
        [ "$(getprop $BROKEN)" = 1 ] && setprop $BROKEN 0
        say "已确认 role=host（子 xhci=${NX}，耗时 $((i * 100))ms）→ 放行挂起"
    else
        # 这次开机余下的时间每次息屏都会走到这里 ⇒ 整机不再睡（PWR-4）。不变量优先，只把状态报出去。
        setprop $BROKEN 1
        say "⚠️ 切 host 失败：role=[$(cat $S 2>/dev/null)] 子xhci=$NX —— 保持 wakelock，不放行挂起；已置 $BROKEN=1"
    fi
else
    say "role=[$(cat $S 2>/dev/null)] UDC=[$(ls /sys/class/udc/ 2>/dev/null)] 确认=$OK —— wakelock 保持持有"
fi
