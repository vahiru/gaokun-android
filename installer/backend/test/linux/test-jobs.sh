#!/usr/bin/env bash
# gk3l job（写盘进程脱离界面）与 log-targets / save-logs 的容器测试。
#
#   bash scripts/live/test-in-container.sh installer/backend/test/linux/test-jobs.sh
#
# 容器里没有 systemd：直接跑是 setsid 方式；systemd-run 方式用一个假的 systemd-run（GK3_SYSTEMD_RUN）。
set -u
cd "$(dirname "$0")/../../../.."
[ "$(id -u)" = 0 ] || { echo "要 root（用 scripts/live/test-in-container.sh 跑）"; exit 2; }
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ✓ $*"; }
bad() { FAIL=$((FAIL+1)); echo "  ✗ $*"; }
field() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }

W=$(mktemp -d /tmp/gk3l-jobs.XXXX)
cleanup() {
    local m img l
    for m in $(findmnt -rno TARGET | grep -e "^$W" -e '^/media/gk3$' | sort -r); do umount "$m" 2>/dev/null; done
    for img in "$W"/*.img; do
        for l in $(losetup -j "$img" 2>/dev/null | cut -d: -f1); do losetup -d "$l" 2>/dev/null; done
    done
    rm -rf "$W"
}
trap cleanup EXIT
export GK3_RUNDIR=$W/run GK3_JOB_POLL=0.05

echo "═══ 编 gk3l ═══"
GK3L_CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    sh installer/backend/linux/build.sh "$W/gk3l" gcc || { echo "编不过"; exit 1; }
G=$W/gk3l
ok "ASan 版编出来了"

echo "═══ 1. job run 与直接调用逐行相同（setsid 方式） ═══"
P=(plan --sector 512 --region-start 2048 --region-end 100000000 --disk /dev/x)
"$G" "${P[@]}" >"$W/d.out" 2>"$W/d.err"; DR=$?
"$G" job run "${P[@]}" >"$W/j.out" 2>"$W/j.err"; JR=$?
J=$(grep '^JOB ' "$W/j.err")
[ "$DR" = "$JR" ] && cmp -s "$W/d.out" "$W/j.out" && [ "$(field "$J" mode)" = setsid ] && [ "$(field "$J" state)" = running ] \
    && ok "stdout 逐字节相同、退出码相同（$JR），stderr 多一行 JOB（mode=setsid）" || { bad "job run 与直接调用不同：rc $DR/$JR"; diff "$W/d.out" "$W/j.out" | head; echo "$J"; }
# 容器里没有 systemd 与 logind：去掉 JOB 行与这两种情况的提示
grep -v '^JOB \|^warning: not a systemd\|^warning: cannot take a systemd-inhibit' "$W/j.err" > "$W/j.err2"
cmp -s "$W/d.err" "$W/j.err2" && ok "stderr（去掉 JOB 与 setsid 提示）也相同" || { bad "stderr 不同"; diff "$W/d.err" "$W/j.err2" | head; }
"$G" job run plan --sector 512 --region-start 2048 --region-end 1000000 >"$W/j.out" 2>"$W/j.err"; JR=$?
[ "$JR" = 1 ] && grep -q '^PLANERR msg=not-enough-space' "$W/j.out" && grep -q '^ERR code=plan ' "$W/j.err" \
    && ok "失败的命令：退出码 1、PLANERR 与 ERR 照样转发" || bad "失败的命令：rc=$JR"
ID=$(field "$(grep '^JOB ' "$W/j.err")" id)
S=$("$G" job status "$ID")
[ "$(field "$S" state)" = done ] && [ "$(field "$S" rc)" = 1 ] && printf '%s' "$S" | grep -q 'cmd=plan%20--sector' \
    && ok "job status：state=done rc=1，cmd 记下了" || bad "job status：$S"
[ "$("$G" job status | grep -c '^JOB ')" = 2 ] && ok "job status 不带 id：列出两个 job" || bad "job status：$("$G" job status)"

echo "═══ 2. systemd-run 方式（假的 systemd-run） ═══"
cat > "$W/fake-sr" <<'EOF'
#!/bin/bash
# 记下参数，去掉 systemd-run 自己的选项后在后台跑
echo "$@" > "$GK3_RUNDIR/sr-args"
while [ $# -gt 0 ] && [[ $1 == --* ]]; do shift; done
setsid -f "$@" </dev/null >/dev/null 2>&1
EOF
chmod +x "$W/fake-sr"
GK3_SYSTEMD_RUN=$W/fake-sr GK3_TEST_MARK=hello "$G" job run "${P[@]}" >"$W/j.out" 2>"$W/j.err"; JR=$?
J=$(grep '^JOB ' "$W/j.err"); A=$(cat "$W/run/sr-args" 2>/dev/null)
[ "$JR" = 0 ] && cmp -s "$W/d.out" "$W/j.out" && [ "$(field "$J" mode)" = systemd-run ] && [ "$(field "$J" unit)" = "gk3-job-$(field "$J" id)" ] \
    && printf '%s' "$A" | grep -q -- '--collect' && printf '%s' "$A" | grep -q -- '--setenv=GK3_TEST_MARK=hello' \
    && printf '%s' "$A" | grep -q -- '--setenv=GK3_RUNDIR=' && printf '%s' "$A" | grep -q " job body $W/run/jobs/" \
    && ok "mode=systemd-run，单元名带 id，GK3_* 用 --setenv 带过去，单元里跑 job body" || { bad "systemd-run 方式：rc=$JR $J"; echo "      $A"; }
GK3_SYSTEMD_RUN=/bin/false "$G" job run "${P[@]}" >"$W/j.out" 2>"$W/j.err"; JR=$?
[ "$JR" != 0 ] && grep -q '^ERR code=job-start ' "$W/j.err" && ok "systemd-run 起不来 ⇒ ERR job-start" || bad "systemd-run 失败：rc=$JR $(cat "$W/j.err")"

echo "═══ 3. job 没写状态就没了 ⇒ job-lost；没有这个 job ⇒ job-missing ═══"
# 一个不存在的进程号（大于任何 pid_max）
L=$W/run/jobs/lost-1; mkdir -p "$L"; echo apply > "$L/cmd"; echo 1 > "$L/started"; echo 2147483647 > "$L/pid"
"$G" job follow lost-1 >/dev/null 2>"$W/j.err"; JR=$?
[ "$JR" = 125 ] && grep -q '^ERR code=job-lost id=lost-1 touched=yes' "$W/j.err" && ok "进程不在了、没有 rc ⇒ ERR job-lost touched=yes，退出码 125" || bad "lost：rc=$JR $(cat "$W/j.err")"
"$G" job follow nope >/dev/null 2>"$W/j.err"; JR=$?
[ "$JR" != 0 ] && grep -q '^ERR code=job-missing' "$W/j.err" && ok "没有这个 job ⇒ job-missing" || bad "missing：rc=$JR"
"$G" job follow ../jobs >/dev/null 2>"$W/j.err"; [ $? != 0 ] && grep -q '^ERR code=job-missing' "$W/j.err" && ok "id 带斜杠 ⇒ job-missing" || bad "id 带斜杠没拦住"

echo "═══ 4. job 结束时把输出存一份到安装介质（介质只读挂着：临时改成读写、写完改回去） ═══"
truncate -s 64M "$W/medium.img"; mkfs.vfat "$W/medium.img" >/dev/null; mmd -i "$W/medium.img" ::/gaokun3
MD=$(losetup -f --show "$W/medium.img"); mkdir -p /media/gk3; mount -o ro -t vfat "$MD" /media/gk3
"$G" job run "${P[@]}" >/dev/null 2>"$W/j.err"
ID=$(field "$(grep '^JOB ' "$W/j.err")" id)
[ -f "/media/gk3/gaokun3/diag/job-$ID.log" ] && grep -q '^PLAN op=mkpart' "/media/gk3/gaokun3/diag/job-$ID.log" \
    && findmnt -rno OPTIONS /media/gk3 | grep -q '^ro' && ok "diag/job-<id>.log 写进介质，介质改回只读" || bad "介质上没有 job 日志或没改回只读：$(findmnt -rno OPTIONS /media/gk3)"

echo "═══ 5. log-targets / save-logs ═══"
export GK3_ALLOW_LOOP=1
umount /media/gk3; losetup -d "$MD"
truncate -s 2G "$W/usb.img"; U=$(losetup -fP --show "$W/usb.img")
sgdisk -o -n 1:2048:+500M -t 1:0700 -n 2:0:+500M -t 2:0700 "$U" >/dev/null; partprobe "$U"; sleep 1
mkfs.vfat -n USBLOGS "${U}p1" >/dev/null; mkntfs -Q -F "${U}p2" >/dev/null 2>&1
# 启动介质：另一块盘上的 ESP 分区，挂在 /media/gk3
truncate -s 2G "$W/boot.img"; BT=$(losetup -fP --show "$W/boot.img")
sgdisk -o -n 1:2048:+500M -t 1:ef00 "$BT" >/dev/null; partprobe "$BT"; sleep 1; mkfs.vfat -n GK3LIVE "${BT}p1" >/dev/null
mount -o ro -t vfat "${BT}p1" /media/gk3
T=$("$G" log-targets)
printf '%s\n' "$T" | head -1 | grep -q "^LOGTARGET part=${U}p1 fs=vfat .*label=USBLOGS removable=yes medium=no esp=no" \
    && ! printf '%s\n' "$T" | grep -q "part=${U}p2 " && printf '%s\n' "$T" | tail -1 | grep -q "part=${BT}p1 .*medium=yes esp=yes" \
    && ok "U 盘的 FAT 分区排第一，NTFS 不列，启动介质排最后" || { bad "log-targets："; printf '%s\n' "$T"; }
S=$("$G" save-logs --to "${U}p1"); rc=$?
mkdir -p "$W/chk"; mount -o ro "${U}p1" "$W/chk"
D=$(field "$S" dir)
[ "$rc" = 0 ] && [ "$(field "$S" part)" = "${U}p1" ] && [ -f "$W/chk/$D/dmesg.txt" ] && grep -q '^DISK ' "$W/chk/$D/probe.txt" \
    && [ "$(ls "$W/chk/$D/jobs" | wc -l)" -ge 3 ] && [ "$(field "$S" files)" -ge 5 ] \
    && ok "存到 U 盘：$S（dmesg、probe、$(ls "$W/chk/$D/jobs" | wc -l) 个 job 的输出）" || { bad "save-logs：rc=$rc $S"; ls -la "$W/chk/$D" 2>&1 | head; }
umount "$W/chk"
findmnt -rn -S "${U}p1" >/dev/null && bad "save-logs 自己挂的分区没卸下" || ok "自己挂的分区用完卸下了"
S=$("$G" save-logs --to "${U}p2" 2>"$W/s.err"); [ $? != 0 ] && grep -q '^ERR code=logs-not-fat' "$W/s.err" && ok "NTFS ⇒ logs-not-fat" || bad "NTFS：$S"
mkdir -p "$W/dir"; S=$("$G" save-logs --to "$W/dir"); [ $? = 0 ] && [ -d "$W/dir/$(field "$S" dir)" ] && [ "$(field "$S" part)" = - ] && ok "存到目录：$S" || bad "存到目录：$S"
S=$("$G" save-logs); [ $? = 0 ] && [ "$(field "$S" part)" = "${U}p1" ] && ok "不给 --to：用第一个（U 盘）" || bad "默认目标：$S"
umount /media/gk3; losetup -d "$BT"; losetup -d "$U"

echo
echo "合计：通过 $PASS，失败 $FAIL"
[ "$FAIL" = 0 ]
