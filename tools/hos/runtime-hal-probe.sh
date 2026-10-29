#!/system/bin/sh
# Isolated, current-boot HAL binary probe. Never writes a vendor image.
# apply <thermal|usb> <staged-binary> <old-sha256> <candidate-sha256>
# restore <thermal|usb>
set -eu
[ "$(id -u)" = 0 ]
grep -qw androidboot.force_super_partition=super_hos /proc/cmdline || exit 90
[ "$(getenforce)" = Enforcing ]
[ "$(readlink /proc/1/ns/mnt)" = "$(readlink /proc/self/ns/mnt)" ]
action=${1:?action}; kind=${2:?kind}
case "$kind" in
 thermal) service=vendor.thermal-gaokun3; label=hal_thermal_default_exec;;
 usb) service=vendor.usb-gaokun3; label=hal_usb_default_exec;;
 *) exit 2;;
esac
target=/vendor/bin/hw/android.hardware.$kind-service.gaokun3
root=/data/local/tmp/hos-hal-probe-state
state=$root/$kind
file_sha256() { sha256sum "$1" | cut -d ' ' -f 1; }
is_mounted() { awk -v p="$target" '$2==p {found=1} END {exit !found}' /proc/mounts; }
valid_hash() { [ ${#1} = 64 ] && ! printf '%s' "$1" | grep -q '[^0-9a-f]'; }
case "$action" in
restore)
 [ -d "$state" ]; [ "$(cat "$state/boot")" = "$(cat /proc/sys/kernel/random/boot_id)" ]
 old=$(cat "$state/original-sha"); old_state=$(cat "$state/original-service")
 valid_hash "$old"; is_mounted
 stop "$service"
 umount "$target"
 [ "$(file_sha256 "$target")" = "$old" ]
 case "$old_state" in running) start "$service";; stopped) :;; *) exit 3;; esac
 rm "$state/boot" "$state/original-sha" "$state/original-service"
 rmdir "$state"
 echo "restored $kind; original service state=$old_state"
 ;;
apply)
 candidate=${3:?staged-binary}; old=${4:?old-sha256}; new=${5:?candidate-sha256}
 case "$candidate" in /mnt/hos-hal-candidates/*) ;; *) exit 2;; esac
 [ ! -L "$candidate" ]; [ -f "$candidate" ]; valid_hash "$old"; valid_hash "$new"
 [ "$old" != "$new" ]; [ "$(file_sha256 "$target")" = "$old" ]; [ "$(file_sha256 "$candidate")" = "$new" ]
 ! is_mounted; [ ! -e "$state" ]
 old_state=$(getprop init.svc.$service)
 case "$old_state" in running|stopped) ;; *) exit 3;; esac
 if [ "$kind" = usb ]; then
  case "$(getprop persist.vendor.gaokun3.usbrole.device_only)" in true|1) ;; *) exit 4;; esac
  [ "$(getprop persist.vendor.gaokun3.allow_suspend)" = 0 ]
  [ "$(getprop init.svc.gaokun3_role_host)" != running ]
  # The new init triggers are not loaded during this binary-only probe.
  echo gaokun3_usb_device_only > /sys/power/wake_lock
 fi
 mkdir -p "$root"; chmod 0700 "$root"; mkdir "$state"
 cat /proc/sys/kernel/random/boot_id > "$state/boot"
 printf '%s\n' "$old" > "$state/original-sha"
 printf '%s\n' "$old_state" > "$state/original-service"
 chmod 0755 "$candidate"; chcon u:object_r:$label:s0 "$candidate"
 mounted=0; finished=0
 rollback() {
  if [ "$finished" = 0 ]; then
   stop "$service" || true
   if [ "$mounted" = 1 ]; then umount "$target" || return; fi
   if [ "$(file_sha256 "$target")" = "$old" ] && [ "$old_state" = running ]; then start "$service"; fi
   rm -f "$state/boot" "$state/original-sha" "$state/original-service"
   rmdir "$state" 2>/dev/null || true
  fi
 }
 trap rollback EXIT
 stop "$service"
 mount --bind "$candidate" "$target"; mounted=1
 [ "$(file_sha256 "$target")" = "$new" ]
 start "$service"
 sleep 2
 [ "$(getprop init.svc.$service)" = running ]
 finished=1
 echo "mounted $kind candidate; service running; reboot removes bind mount"
 ;;
*) exit 2;;
esac
