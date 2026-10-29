#!/usr/bin/env bash
# Temporary diagnostic overlay only: this donor removes it on reboot.
# Use overlays/recents in the vendor image for a persistent repair.
set -euo pipefail
action=${1:-status}
serial=${2:-gaokun3}
case "$action" in apply|disable|status) ;; *) echo 'Usage: recents-compat.sh apply|disable|status [serial]' >&2; exit 2;; esac
adb -s "$serial" shell sh -s -- "$action" <<'DEVICE'
set -eu
grep -qw androidboot.force_super_partition=super_hos /proc/cmdline || exit 90
resource=android:string/config_recentsComponentName
overlay=com.android.shell:HosRecentsCompat
expected=com.miui.home/com.miui.home.recents.RecentsActivity
case "$1" in
apply)
    original=$(cmd overlay lookup --user 0 android "$resource")
    case "$original" in
        com.android.launcher3/com.android.quickstep.RecentsActivity|"$expected") ;;
        *) echo "Unexpected Recents resource: $original" >&2; exit 91;;
    esac
    resolved=$(cmd package resolve-activity --brief -n "$expected")
    echo "$resolved" | grep -qx 'com.miui.home/.recents.RecentsActivity'
    services=$(cmd package query-services --brief -a android.intent.action.QUICKSTEP_SERVICE)
    echo "$services" | grep -q 'com.miui.home/com.rust.hyper_launcher.recents.TouchInteractionService'
    cmd overlay fabricate --target android --name HosRecentsCompat "$resource" 0x03 "$expected"
    cmd overlay enable --user 0 "$overlay"
    [ "$(cmd overlay lookup --user 0 android "$resource")" = "$expected" ]
    echo 'Applied temporarily. This donor removes fabricated shell overlays on reboot; install the static RRO for persistence.'
    ;;
disable)
    cmd overlay disable --user 0 "$overlay"
    echo 'Disabled. Reboot is needed to refresh cached consumers.'
    ;;
esac
cmd overlay lookup --user 0 android "$resource"
cmd overlay dump "$overlay"
DEVICE
