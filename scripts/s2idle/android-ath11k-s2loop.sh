#!/system/bin/sh
# issue #16 / patches/kernel/wifi/0001 验证：ath11k 断电挂起/恢复循环，看固件每次都能重新下载、Wi-Fi 每次都能恢复。
# ⚠️ 必须是 pm_test=platform：devices 档不跑 suspend_late/resume_early，ath11k 既不断电也不重载、停了影子定时器 ⇒ Wi-Fi 必坏（2026-10-04 踩过）
# 用法（root）：setsid nohup sh android-ath11k-s2loop.sh <pm_test 次数> <真实 s2idle 次数> <同网段可 ping 的主机> &
# 详见 docs/dev/deep-sleep.md。
# ⚠️ role=device 时挂起会整板复位（gaokun3-usbrole.sh 顶部）⇒ 先切 host，结束切回 device（USB adb 期间断开）。
N=${1:-10}; NR=${2:-3}; PEER=${3:?第三个参数：同网段一台能 ping 的主机（别猜网关：Android 的默认路由在策略路由表里）}
D=/data/local/tmp/mhi-test; mkdir -p $D; L=$D/log; : > $L
S=/sys/class/usb_role/a600000.usb-role-switch/role
# ★ WOW=1（2026-10-18，#40 / #44 / #46 之后）：开着 WoWLAN 时挂起走 WoW、芯片不断电、连接不断 ⇒ 不重载固件、
#   也不会重新关联。判据改成：挂起成功 + ping 通 + 本轮没有 WoW / 恢复类错误。固件拒绝进 WoW（0105 让那次挂起
#   返回 -EBUSY、cycle 里重试）单独计数（declined=），不算失败。WOW=0（默认）是 #16 的断电挂起判据。
WOW=${WOW:-0}
say() { echo "[$(date +%H:%M:%S) up=$(cut -d' ' -f1 /proc/uptime)] $*" >> $L; sync; }
mark() { echo "MHITEST $*" > /dev/kmsg; }
since() { dmesg | sed -n "/MHITEST $1 begin/,\$p"; }   # ★ 只看本轮标记之后：dmesg 环形缓冲会写满，按全量计数会失真
wifi_ok() {  # $1=label。最多等 60 秒：本轮之后重新关联 + ping 通 PEER
  i=0; while [ $i -lt 30 ]; do
    { [ "$WOW" = 1 ] || since $1 | grep -q "wlan0: associated"; } && ping -c1 -W1 $PEER >/dev/null 2>&1 && { echo "$((i*2))s"; return 0; }
    sleep 2; i=$((i+1)); done; echo FAIL; return 1; }
cycle() {  # $1 = platform | none
  case "$(cat $S)" in *"[host]"*|host) ;; *) say "ABORT: 挂起前 role=[$(cat $S)] 不是 host —— device 角色挂起会整板复位"; finish; exit 1 ;; esac
  echo $1 > /sys/power/pm_test
  RC=1; TRY=0
  while [ $RC -ne 0 ] && [ $TRY -lt 6 ]; do
    TRY=$((TRY+1))
    [ "$1" = none ] && { echo 0 > /sys/class/rtc/rtc0/wakealarm; echo +20 > /sys/class/rtc/rtc0/wakealarm; }
    T0=$(cut -d. -f1 /proc/uptime); echo mem > /sys/power/state; RC=$?; T1=$(cut -d. -f1 /proc/uptime)
    [ $RC -ne 0 ] && { say "   rc=$RC（尝试 $TRY，多半 -EBUSY）"; sleep 5; }
  done
  echo none > /sys/power/pm_test
  return $RC
}
say "START N=$N NR=$NR WOW=$WOW kernel=$(uname -v) peer=$PEER"
say "buddyinfo DMA: $(grep DMA /proc/buddyinfo | grep -v DMA32)"
say "wake_lock: $(cat /sys/power/wake_lock)"
# ★ 测试要固定在 host：USB 端口 HAL（vendor.usb-gaokun3）按对端决定角色，插着电脑或充电器（我方受电）时
#   host 下约 6 秒没有下游设备就会切回 device，测试期间停掉它。原先做这件事的是 gaokun3_usbfollow；
#   vreg_l9d 常开之前 device 角色下挂起 = 整板静默复位，2026-10-04 前两轮的"第 2 次循环复位"就是它。
setprop ctl.stop vendor.usb-gaokun3; sleep 1
say "usbhal=[$(getprop init.svc.vendor.usb-gaokun3)]"
echo host > $S; sleep 3
say "role=[$(cat $S)] udc=[$(ls /sys/class/udc 2>/dev/null)]"
case "$(cat $S)" in *host*) ;; *) say "ABORT: 切不到 host"; setprop ctl.start vendor.usb-gaokun3; exit 1 ;; esac
pass=0; fail=0
finish() {
  echo device > $S; sleep 3; setprop ctl.start vendor.usb-gaokun3
  say "role=[$(cat $S)] udc=[$(ls /sys/class/udc 2>/dev/null)] state=[$(cat /sys/class/udc/a600000.usb/state 2>/dev/null)] usbhal=[$(getprop init.svc.vendor.usb-gaokun3)]"
}
run() {  # $1=mode $2=label
  mark "$2 begin"
  cycle $1; rc=$?
  w=$(wifi_ok $2)
  fw=$(since $2 | grep -c "ath11k_pci.*fw_version")
  bad=$(since $2 | grep -c -E "page allocation failure|failed to power up mhi|failed to early resume|DPM device timeout|did not load|failed to wakeup wow|failed to resume hif|failed to suspend hif|firmware crashed")
  # 断电路径（WOW=0）本来就会断开重连；WoW 下连接不该断
  [ "$WOW" = 1 ] && bad=$((bad + $(since $2 | grep -c "wlan0: deauthenticating")))
  dec=$(since $2 | grep -c "firmware declined wow")
  mark "$2 end"
  if [ $rc -eq 0 ] && [ "$w" != FAIL ] && [ $bad -eq 0 ] && { [ "$WOW" = 1 ] || [ $fw -ge 1 ]; }; then pass=$((pass+1)); r=PASS; else fail=$((fail+1)); r=FAIL; fi
  say "$r $2 rc=$rc wifi=$w fw_reload=$fw errlines=$bad declined=$dec"
  dmesg > $D/dmesg-$2.txt; logcat -d -b main,system -t 400 > $D/logcat-$2.txt 2>/dev/null
  say "   wlan0=[$(ip -4 addr show wlan0 | awk '/inet/{print $2}')] supplicant=[$(pidof wpa_supplicant)] link=[$(cat /sys/class/net/wlan0/operstate 2>/dev/null)]"; sync
}
# ★ 节拍（2026-10-05 dev.7 实测）：Android Wi-Fi 框架对自动重连有限速 —— 连续约 5 次 connectToNetwork 之后
#   约 6 分钟一次都不发（logcat 里 SupplicantStaIfaceHalAidlImpl: connectToNetwork 停发），于是第 6 轮起
#   "wifi=FAIL"，而内核侧（挂起 rc、固件重载、errlines）全好 —— 是测试节拍太密，不是回归。
#   每轮至少隔 PACE 秒（默认 75，≈4 分钟 ≤ 3–4 次重连）；PACE=0 退回旧行为（只测内核侧时更快）。
PACE=${PACE:-75}
paced() { t0=$(cut -d. -f1 /proc/uptime); run "$1" "$2"; t1=$(cut -d. -f1 /proc/uptime); [ $((t1 - t0)) -lt $PACE ] && sleep $((PACE - (t1 - t0))); }
i=1; while [ $i -le $N ]; do paced platform "pmtest-$i"; i=$((i+1)); done
i=1; while [ $i -le $NR ]; do paced none "real-$i"; i=$((i+1)); done
say "buddyinfo DMA: $(grep DMA /proc/buddyinfo | grep -v DMA32)"
say "suspend_stats: success=$(cat /sys/kernel/debug/suspend_stats 2>/dev/null | grep -m1 success)"
finish
dmesg | grep -E "MHITEST|ath11k|mhi |PM: suspend|page allocation" > $D/dmesg-filtered.txt
say "DONE pass=$pass fail=$fail"
