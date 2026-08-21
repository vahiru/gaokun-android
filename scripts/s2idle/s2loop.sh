#!/bin/bash
# pm_test=devices 连跑 N 次真实试验（或直到整板复位）。
# ⚠️ 上一版的坑：pm_test 周期结束后会留下 pending 唤醒事件，紧接着再挂起会
#    立刻 -EBUSY 返回 rc!=0 —— 那不是"通过"，是"根本没跑"。必须只把 rc=0 计数。
R=/var/log/s71.log
N=10
say() { echo "[$(date +%H:%M:%S) up=$(cut -d. -f1 /proc/uptime)] $*" >> $R; sync; }
rm -f /etc/systemd/system/multi-user.target.wants/s71test.service
: > $R
say "START ${1:-未命名实验}"
sleep 35
say "cmdline pcie/nvme: [$(cat /proc/cmdline | tr ' ' '\n' | grep -E 'pcie|nvme' | tr '\n' ' ')]"
say "aspm=[$(cat /sys/module/pcie_aspm/parameters/policy 2>/dev/null)] apst=[$(cat /sys/module/nvme_core/parameters/default_ps_max_latency_us 2>/dev/null)]"
OK=0
for i in $(seq 1 $N); do
  RC=1; TRY=0
  while [ $RC -ne 0 ] && [ $TRY -lt 6 ]; do
    TRY=$((TRY+1))
    echo devices > /sys/power/pm_test
    sync
    T0=$(cut -d. -f1 /proc/uptime)
    say "PRE  第$i/$N 次（尝试 $TRY）"
    echo mem > /sys/power/state
    RC=$?
    T1=$(cut -d. -f1 /proc/uptime)
    if [ $RC -ne 0 ]; then
      say "  rc=$RC 用时 $((T1-T0))s —— 没真跑（多半 -EBUSY），等 10 秒重试。dmesg: [$(dmesg | tail -1 | cut -c1-90)]"
      sleep 10
    fi
  done
  if [ $RC -eq 0 ]; then
    OK=$((OK+1))
    say "★ 真实通过 第$i/$N 次（用时 $((T1-T0))s）—— 累计真实通过 $OK 次"
  else
    say "⚠️ 第$i/$N 次 6 次尝试都是 -EBUSY，放弃这一轮"
  fi
  sleep 5
done
echo none > /sys/power/pm_test
say "★★★ 结束：真实通过 $OK / $N"
say "DONE —— 10 秒后重启回 Android"
sync; sleep 10
systemctl reboot
