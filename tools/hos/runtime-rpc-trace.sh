#!/system/bin/sh
# Temporary read-only RPC diagnostic; never enables ALS or writes remoteproc state.
# apply CANDIDATE EXPECTED_NEW_SHA; restore
set -eu
[ "$(id -u)" = 0 ]
grep -qw androidboot.force_super_partition=super_hos /proc/cmdline || exit 90
[ "$(getenforce)" = Enforcing ]
[ "$(readlink /proc/1/ns/mnt)" = "$(readlink /proc/self/ns/mnt)" ]
state=/data/local/tmp/hos-rpc-trace-state
target=/vendor/bin/hexagonrpcd
rpc=vendor.hexagonrpcd-sdsp
hal=vendor.sensors-gaokun3
original=122b57f908ad74c02fb9b43621fe8aa2f3a46c410c075bef0e5f570579bbf69c
file_sha() { sha256sum "$1" | cut -d ' ' -f1; }
mounted() { awk -v p="$target" '$2==p {found=1} END {exit !found}' /proc/mounts; }
wait_state() {
 n=0
 while [ "$n" -lt 10 ]; do
  [ "$(getprop "init.svc.$1")" = "$2" ] && return 0
  sleep 1; n=$((n+1))
 done
 return 1
}
stop_pair() {
 stop "$hal"; wait_state "$hal" stopped
 stop "$rpc"; wait_state "$rpc" stopped
 [ -z "$(pidof hexagonrpcd 2>/dev/null || true)" ]
}
start_pair() {
 start "$rpc"; wait_state "$rpc" running
 start "$hal"; wait_state "$hal" running
}
restore_state() {
 stop_pair || return 1
 if mounted; then umount "$target" || return 1; fi
 [ "$(file_sha "$target")" = "$original" ] || return 1
 start_pair || return 1
 setprop persist.vendor.gaokun3.sscrecover "$(cat "$state/recovery")"
 echo gaokun3_rpc_probe > /sys/power/wake_unlock
 rm "$state/boot" "$state/recovery" "$state/candidate"
 rmdir "$state"
}
case "${1:-}" in
apply)
 candidate=${2:?candidate}; new=${3:?sha}
 case "$candidate" in /mnt/hos-rpc-trace/*) ;; *) exit 2;; esac
 [ ${#new} = 64 ]; ! printf '%s' "$new" | grep -q '[^0-9a-f]'
 [ ! -L "$candidate" ]; [ -f "$candidate" ]; [ "$(file_sha "$candidate")" = "$new" ]
 [ "$(file_sha "$target")" = "$original" ]; ! mounted; [ ! -e "$state" ]
 [ "$(file_sha /vendor/lib64/libhexagonrpc.so)" = 16f700635c88226f585ce9277d154631ae3e741149740851be857733f73023f2 ]
 [ "$(getprop init.svc.$rpc)" = running ]; [ "$(getprop init.svc.$hal)" = running ]
 [ "$(getprop init.svc.gaokun3_sscrecover)" != running ]
 count=0
 for r in /sys/class/remoteproc/remoteproc*; do
  if [ "$(cat "$r/name")" = slpi ]; then [ "$(cat "$r/state")" = running ]; count=$((count+1)); fi
 done
 [ "$count" = 1 ]
 mkdir -m 0700 "$state"
 cat /proc/sys/kernel/random/boot_id > "$state/boot"
 getprop persist.vendor.gaokun3.sscrecover > "$state/recovery"
 printf '%s\n' "$new" > "$state/candidate"
 finished=0
 cleanup() {
  if [ "$finished" = 0 ]; then
   echo 'Probe failed: attempting restoration of original service pair' >&2
   restore_state || echo 'Restoration incomplete; retained state requires review' >&2
  fi
 }
 trap cleanup EXIT
 echo gaokun3_rpc_probe > /sys/power/wake_lock
 setprop persist.vendor.gaokun3.sscrecover 0
 stop_pair
 chmod 0755 "$candidate"; chcon u:object_r:hexagonrpcd_exec:s0 "$candidate"
 mount --bind "$candidate" "$target"
 [ "$(file_sha "$target")" = "$new" ]
 start_pair
 finished=1
 echo 'RPC trace running temporarily; automatic SSC recovery held until restore; no ALS request sent'
 ;;
restore)
 [ -d "$state" ]
 [ "$(cat "$state/boot")" = "$(cat /proc/sys/kernel/random/boot_id)" ]
 [ "$(file_sha "$target")" = "$(cat "$state/candidate")" ]; mounted
 restore_state
 echo 'Original RPC and sensor services restored; confirm fresh IMU events after settling'
 ;;
*) echo 'Usage: sh runtime-rpc-trace.sh apply CANDIDATE SHA | restore' >&2; exit 2;;
esac
