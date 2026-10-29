#!/system/bin/sh
# Run on the isolated experiment, after pushing the candidate XML to /data/local/tmp.
# Diagnostic only: mount disappears on reboot; no recording capability acceptance.
set -eu

profile=${1:?usage: sh runtime-startup-probe.sh /data/local/tmp/profile.xml}
case "$profile" in /data/local/tmp/*.xml) ;; *) exit 2 ;; esac
[ "$(id -u)" = 0 ]
grep -q 'androidboot.force_super_partition=super_hos' /proc/cmdline
[ "$(getprop ro.product.device)" = gaokun3 ]
[ "$(getenforce)" = Enforcing ]
grep -q '^/dev/block/nvme0n1p12 /data ' /proc/mounts
[ -s "$profile" ]
[ ! -e /mnt/hos-media-profiles ]

# Apply before exposing XML: its appearance lets zygote start system_server.
# These persist only in userdata_hos, never in production userdata.
setprop persist.debug.wfd.enable 0
# Keep the diagnostic session available through framework crashes.
# Restore to false after framework stability has been established.
setprop persist.sys.disable_rescue true
setprop persist.sys.background_blur_supported false

mounted=false
published=false
cleanup() {
    if [ "$published" = false ]; then
        if [ "$mounted" = true ]; then umount /mnt/hos-media-profiles; fi
        rmdir /mnt/hos-media-profiles 2>/dev/null || true
    fi
}
trap cleanup EXIT
mkdir /mnt/hos-media-profiles
mount -t tmpfs -o mode=0755 tmpfs /mnt/hos-media-profiles
mounted=true
chcon u:object_r:vendor_configs_file:s0 /mnt/hos-media-profiles
for name in piano vendor; do
    dest=/mnt/hos-media-profiles/media_profiles_${name}.xml
    cp "$profile" "$dest"
    chmod 0644 "$dest"
    chcon u:object_r:vendor_configs_file:s0 "$dest"
done
mount -t overlay overlay -o ro,lowerdir=/mnt/hos-media-profiles:/vendor/etc /vendor/etc
published=true
ls -lZ /vendor/etc/media_profiles_piano.xml /vendor/etc/media_profiles_vendor.xml
getprop persist.debug.wfd.enable
getprop persist.sys.disable_rescue
getenforce
