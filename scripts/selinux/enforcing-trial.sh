#!/system/bin/sh
# 运行期 enforcing 试跑：不重启，临时 setenforce 1，跑一串操作，再退回 permissive。（#126）
#
# 用法（在 Mac 上）：
#   adb push scripts/selinux/enforcing-trial.sh /data/local/tmp/
#   adb shell 'su -c "setsid sh /data/local/tmp/enforcing-trial.sh </dev/null >/dev/null 2>&1 &"'
#   ……约 3 分钟后看 /data/local/tmp/enf-trial.log，denial 用 avc-summary.py --enforcing 汇总。
#
# ★ 为什么敢这么做（2026-09-27 实测）：本机的 adbd 与 su 都跑在 KernelSU 的 ksu 域，
#   它是 permissive 域且全放行（policy-query.sh 查到 flags=1）⇒ 全局 enforcing 也锁不住 adb，
#   随时能 setenforce 0 退回来。这是【开发机】的性质，不是产品的。
# ★ 为什么值得做：enforcing 下每次拒绝都记（不像 permissive 同一元组只记一次），
#   能看到 permissive 普查里被去重藏起来的对象；而且直接看到"拒了之后功能怎么坏"。
# ⚠️ 看不到的：只在开机 / 服务启动那一瞬发生的访问（试跑时它们早就发生过了）。
#   那一半要靠完整开机的 permissive 普查（logd 的 kernel 缓冲从 0 秒起，dmesg 会滚掉）。
# ⚠️ 试跑中 USB 端口 HAL 若被拒绝读 UDC 状态，会误判角色、USB adb 掉线。结束后
#   `setprop ctl.restart vendor.usb-gaokun3` 让它重新判断。TCP adb 不受影响。
# 看门狗：无论中间发生什么，T+300s 一定 setenforce 0。
L=/data/local/tmp/enf-trial.log
echo "start $(cat /proc/uptime)" > $L
( sleep 300; setenforce 0; echo "watchdog setenforce0 $(cat /proc/uptime)" >> $L ) &
/system/bin/auditctl -r 1000 >> $L 2>&1          # 审计限速开大（logd 开机完成后设成 5/秒）
setprop ctl.stop hangdump                         # 它每分钟一批 denial，先停
sleep 2
setenforce 1
echo "enforcing $(getenforce) $(cat /proc/uptime)" >> $L
sleep 5
step() { echo "== $1 $(cat /proc/uptime)" >> $L; }
step touch_daily; setprop persist.sys.gaokun3.touch_mode daily; sleep 4
step touch_game;  setprop persist.sys.gaokun3.touch_mode game;  sleep 4
step kbd_on;      setprop persist.sys.gaokun3.keyboard 1; sleep 4
step kbd_off;     setprop persist.sys.gaokun3.keyboard 0; sleep 4
step bootctl;     /system/bin/bootctl get-current-slot >> $L 2>&1; /system/bin/bootctl is-slot-marked-successful 0 >> $L 2>&1
step dumpsys;     dumpsys thermalservice | grep -m3 Temperature >> $L 2>&1; dumpsys battery | head -12 >> $L 2>&1
step wifi_scan;   cmd wifi start-scan >> $L 2>&1; sleep 6; cmd wifi list-scan-results 2>&1 | head -3 >> $L
step decode;      timeout 40 /system/bin/gaokun3-decode-test /data/local/tmp/avc720.mp4 >> $L 2>&1; echo "rc=$?" >> $L
step cam_rear;    timeout 60 /data/local/tmp/gaokun3-ncam-smoke >> $L 2>&1; echo "rc=$?" >> $L
step cam_front;   timeout 40 /data/local/tmp/gaokun3-ncam-smoke front >> $L 2>&1; echo "rc=$?" >> $L
step idle;        sleep 20
step done
setenforce 0
echo "permissive $(getenforce) $(cat /proc/uptime)" >> $L
setprop ctl.start hangdump
/system/bin/auditctl -r ${AUDIT_RATE_AFTER:-5} >> $L 2>&1
echo "end $(cat /proc/uptime)" >> $L
