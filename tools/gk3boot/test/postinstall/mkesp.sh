#!/bin/bash
# mkesp.sh <目录>：造一份"安装器装出来的" ESP（与 installer/backend/src/esp.c 的 gk3w_esp_android_entry 同形）
set -eu
E=$1/esp; MID=8a29534fa802480d9fbb71aa18c01d7b
mkdir -p "$E/loader/entries" "$E/EFI/BOOT" "$E/$MID/android/slot_a" "$E/$MID/android/slot_b"
printf 'timeout 15\nconsole-mode keep\neditor no\ndefault *-android-a.conf\n' > "$E/loader/loader.conf"
for s in a b; do
  printf 'OLDIMG' > "$E/$MID/android/slot_$s/Image"
  cat > "$E/loader/entries/$MID-android-$s.conf" <<X
title      crDroid 16.0 (gaokun3) — slot _$s
version    gaokun3-slot-$s
sort-key   zandroid$s
options    old androidboot.slot_suffix=_$s
linux      /$MID/android/slot_$s/Image
devicetree /$MID/android/slot_$s/gaokun3.dtb
initrd     /$MID/android/slot_$s/ramdisk.img
X
done
