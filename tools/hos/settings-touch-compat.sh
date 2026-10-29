#!/usr/bin/env bash
# Temporary diagnostic resource overrides. Donor removes these on reboot.
set -euo pipefail
action=${1:-status}
serial=${2:-gaokun3}
case "$action" in apply|restore|status) ;; *) echo 'Usage: settings-touch-compat.sh apply|restore|status [serial]' >&2; exit 2;; esac
adb -s "$serial" shell sh -s -- "$action" <<'DEVICE'
set -eu
grep -qw androidboot.force_super_partition=super_hos /proc/cmdline || exit 90
for pair in HosSettingsCardTint:miuix_recyclerview_card_tint_color_light HosSettingsPreferenceTint:miuix_preference_item_tint_color_light; do
 name=${pair%%:*}; resource=com.android.settings:color/${pair#*:}
 case "$1" in
 apply)
  value=$(cmd overlay lookup --user 0 com.android.settings "$resource")
  case "$value" in *'#ff000000'|*'#ff007aff') ;; *) echo "Unexpected original resource: $resource=$value" >&2; exit 91;; esac
  ;;
 esac
done
if [ "$1" = apply ]; then
 # One resource per fabricate command: trailing values are not separate items.
 cmd overlay fabricate --target com.android.settings --name HosSettingsCardTint com.android.settings:color/miuix_recyclerview_card_tint_color_light 0x1c 0xff007aff
 cmd overlay fabricate --target com.android.settings --name HosSettingsPreferenceTint com.android.settings:color/miuix_preference_item_tint_color_light 0x1c 0xff007aff
 cmd overlay enable --user 0 com.android.shell:HosSettingsCardTint
 cmd overlay enable --user 0 com.android.shell:HosSettingsPreferenceTint
elif [ "$1" = restore ]; then
 listing=$(cmd overlay list --user 0 com.android.settings)
 for name in HosSettingsCardTint HosSettingsPreferenceTint; do
  if echo "$listing" | grep -Fq "com.android.shell:$name"; then cmd overlay disable --user 0 "com.android.shell:$name"; fi
 done
fi
if [ "$1" != status ]; then
 am force-stop com.android.settings
 am start -a android.settings.SETTINGS
fi
cmd overlay lookup --user 0 com.android.settings com.android.settings:color/miuix_recyclerview_card_tint_color_light
cmd overlay lookup --user 0 com.android.settings com.android.settings:color/miuix_preference_item_tint_color_light
DEVICE
