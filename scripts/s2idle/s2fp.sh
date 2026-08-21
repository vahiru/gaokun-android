#!/bin/bash
# 每次开机：先拍一份完整"开机指纹"，再跑 pm_test=devices 循环，最后重启回 Android。
# 目的：同一条 cmdline 有的开机连过 10 次、有的第 1 次就死 ——
#       差异必然在开机时就定下来了。攒若干次开机后比对"能过"与"不能过"的指纹。
# ⚠️ 只用 pm_test（5 秒自动返回、不需要唤醒源），不会把机器睡死。
B=/var/log/s2fp
mkdir -p $B
ID=$(( $(cat $B/counter 2>/dev/null || echo 0) + 1 )); echo $ID > $B/counter
D=$B/boot-$ID; mkdir -p $D
L=$B/trials.log
say() { echo "[boot-$ID $(date +%H:%M:%S) up=$(cut -d. -f1 /proc/uptime)] $*" >> $L; sync; }
N_TARGET=6
# 机器码现场读，不写死（换机器/重装 systemd-boot 都会变）
MID=$(basename $(ls /boot/efi/loader/entries/*-plain72.conf 2>/dev/null | head -1) 2>/dev/null | sed "s/-plain72.conf//")
[ -z "$MID" ] && MID=$(cat /etc/machine-id)
say "==== 开机 #$ID / $N_TARGET 开始 ===="
# ★ 先把下一轮安排好，再做任何可能整板复位的事 ——
#   这样无论本轮是跑完还是中途复位，都会继续进下一轮。
if [ $ID -lt $N_TARGET ]; then
    ln -sf /etc/systemd/system/s71test.service /etc/systemd/system/multi-user.target.wants/s71test.service
    if bootctl set-oneshot $MID-plain72.conf 2>>$L; then
        say "下一轮已安排（oneshot -> plain72，武装已就位）"
    else
        say "⚠️ bootctl set-oneshot 失败 —— 链条会停在本轮"
    fi
else
    rm -f /etc/systemd/system/multi-user.target.wants/s71test.service
    say "这是最后一轮（$ID/$N_TARGET），跑完回 Android 停下"
fi
sync
sleep 35

# ---------- 指纹 ----------
cat /proc/cmdline > $D/cmdline
uname -a > $D/uname
for b in platform pci i2c spi auxiliary usb serio hid; do
  for drv in /sys/bus/$b/drivers/*; do
    [ -d "$drv" ] || continue
    for d in $drv/*; do
      [ -L "$d" ] && [ -e "$d/uevent" ] && echo "$b/$(basename $drv)/$(basename $d)"
    done
  done
done | sort > $D/bound-devices.txt
for w in /sys/class/wakeup/*/; do
  [ -d "$w" ] || continue
  echo "$(cat $w/name 2>/dev/null) active=$(cat $w/active_count 2>/dev/null) event=$(cat $w/event_count 2>/dev/null) abort=$(cat $w/wakeup_count 2>/dev/null)"
done | sort > $D/wakeup.txt
awk '{n=$1; c=0; for(i=2;i<=NF;i++) if ($i ~ /^[0-9]+$/) c+=$i; $1=""; print n, c, $0}' /proc/interrupts | sed 's/  */ /g' > $D/interrupts.txt
find /sys/devices -name runtime_status -maxdepth 8 2>/dev/null | while read f; do
  echo "$(dirname $(dirname $f) | sed 's|/sys/devices/||') $(cat $f 2>/dev/null)"
done | sort > $D/runtime.txt
for p in /sys/bus/pci/devices/*/; do
  [ -d "$p" ] || continue
  echo "$(basename $p) class=$(cat $p/class 2>/dev/null) speed=$(cat $p/current_link_speed 2>/dev/null) width=$(cat $p/current_link_width 2>/dev/null) ctrl=$(cat $p/power/control 2>/dev/null) rt=$(cat $p/power/runtime_status 2>/dev/null) d3cold=$(cat $p/d3cold_allowed 2>/dev/null)"
done | sort > $D/pci.txt
{ echo "--- aspm ---"; cat /sys/module/pcie_aspm/parameters/policy 2>/dev/null
  echo "--- link state ---"; for p in /sys/bus/pci/devices/*/link/; do [ -d "$p" ] && echo "$p $(cat $p/l1_aspm 2>/dev/null)"; done
  echo "--- apst ---"; cat /sys/module/nvme_core/parameters/default_ps_max_latency_us 2>/dev/null
  echo "--- nvme ---"; for f in /sys/class/nvme/nvme0/{model,firmware_rev,state,cntrltype}; do [ -r "$f" ] && echo "$(basename $f)=$(cat $f)"; done
} > $D/pcie-nvme.txt
{ for n in /sys/class/net/*/; do echo "$(basename $n) oper=$(cat $n/operstate 2>/dev/null) carrier=$(cat $n/carrier 2>/dev/null)"; done
  command -v iw >/dev/null && iw dev 2>/dev/null | grep -E "Interface|ssid|channel"
} > $D/net.txt
cat /sys/kernel/debug/pm_genpd/pm_genpd_summary > $D/genpd.txt 2>/dev/null || echo "无 genpd debugfs" > $D/genpd.txt
dmesg > $D/dmesg.txt
sync
say "指纹已采集：绑定设备 $(wc -l < $D/bound-devices.txt) 项 / 唤醒源 $(wc -l < $D/wakeup.txt) 个 / PCI $(wc -l < $D/pci.txt) 个"

# ---------- 试验 ----------
OK=0
echo "PENDING" > $D/RESULT
for i in $(seq 1 10); do
  RC=1; TRY=0
  while [ $RC -ne 0 ] && [ $TRY -lt 6 ]; do
    TRY=$((TRY+1))
    echo devices > /sys/power/pm_test; sync
    T0=$(cut -d. -f1 /proc/uptime)
    say "PRE 第$i/10（尝试 $TRY）"
    echo mem > /sys/power/state
    RC=$?
    T1=$(cut -d. -f1 /proc/uptime)
    [ $RC -ne 0 ] && { say "  rc=$RC 用时 $((T1-T0))s —— 没真跑，等 10 秒重试"; sleep 10; }
  done
  if [ $RC -eq 0 ]; then
    OK=$((OK+1)); echo "PASS=$OK" > $D/RESULT; sync
    say "★ 真实通过 $i/10（用时 $((T1-T0))s）"
  else
    echo "EBUSY_GAVEUP=$OK" > $D/RESULT; sync
    say "⚠️ 第$i/10 六次尝试全 -EBUSY，放弃"
  fi
  sleep 5
done
echo none > /sys/power/pm_test
echo "DONE PASS=$OK/10" > $D/RESULT; sync
say "==== 开机 #$ID 结束：真实通过 $OK/10 ===="
say "10 秒后重启回 Android"
sync; sleep 10
systemctl reboot
