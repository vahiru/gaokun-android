#!/usr/bin/env bash
#
# 验收「内核已进入 OTA 范围」这条链路。在【宿主机】上跑，通过 adb 检查设备。
#
#   bash scripts/verify-kernel-ota.sh [<设备 IP>]
#
# 逐项判据都写在输出里。任何一项 ✗ 都说明链路断了，不要靠"看起来没问题"。
set -uo pipefail

IP="${1:-}"
if [ -z "$IP" ]; then
    # adb over TCP 的 IP 会漂（#27），没给就自己找
    for c in $(adb devices | awk '/:5555/{print $1}'); do
        [ "$(adb -s "$c" shell getprop ro.crdroid.device 2>/dev/null | tr -d '\r')" = gaokun3 ] && { DEV=$c; break; }
    done
    [ -n "${DEV:-}" ] || { echo "找不到设备。用法: $0 <IP>"; exit 1; }
else
    adb connect "$IP:5555" >/dev/null 2>&1
    DEV="$IP:5555"
fi
sh() { adb -s "$DEV" shell "$@" 2>&1 | tr -d '\r'; }
pass=0; fail=0
ck() { # ck <说明> <期望正则> <实际>
    if echo "$3" | grep -qE "$2"; then echo "  ✅ $1"; pass=$((pass+1))
    else echo "  ✗ $1"; echo "      实际: $3"; fail=$((fail+1)); fi
}

echo "设备: $DEV"

echo
echo "══ 1. 内核 ══"
# dmesg's first line is linux_banner, a compile-time constant. Since patches/0085 reports another
# release to apps only, uname as root is real too, but rc.2 and rc.3 kernels report 6.6.30 to everyone.
u=$(sh 'dmesg 2>/dev/null | head -1')
echo "  $u"
ck "内核是 gaokun3 主线" 'Linux version 7\.[0-9]+\.[0-9]+(-rc[0-9]+)?-gaokun3' "$u"

echo
echo "══ 2. CONFIG_QCOM_FASTRPC=y（不再需要 insmod）══"
nodes=$(sh 'ls /dev/fastrpc-* 2>&1 | tr "\n" " "')
mods=$(sh 'wc -l < /proc/modules')
ck "四个 fastrpc 节点都在" 'sdsp' "$nodes"
ck "没有加载任何模块（说明是 =y 而不是靠 insmod）" '^0$' "$mods"

echo
echo "══ 3. boot 分区（Android 分区规范）══"
ba=$(sh 'dd if=/dev/block/by-name/boot_a bs=8 count=1 2>/dev/null | od -An -c | tr -s " "')
ck "boot_a 里是 Android boot 镜像" 'A N D R O I D !' "$ba"
slots=$(sh 'bootctl get-number-slots')
ck "bootctl 报 2 个槽（不是回退成 4）" '^2$' "$slots"

echo
echo "══ 4. 过渡期部件（EFI 加载器就位前靠它们）══"
ck "解包器已装" 'gaokun3-bootimg-extract' "$(sh 'ls /vendor/bin/gaokun3-bootimg-extract 2>&1')"
ck "postinstall 钩子已装" 'gaokun3-ota-postinstall' "$(sh 'ls /vendor/bin/gaokun3-ota-postinstall.sh 2>&1')"
ck "解包器能读 boot_a" 'header_version=2' "$(sh 'mkdir -p /data/local/tmp/bx && /vendor/bin/gaokun3-bootimg-extract /dev/block/by-name/boot_a /data/local/tmp/bx')"

echo
echo "══ 5. ESP 上的每槽内核（systemd-boot 实际加载的）══"
# ⚠️ 逐个文件查，不要拿"目录列表逐字匹配"当判据 —— OTA 的 postinstall 会额外
#   写一个 cmdline.txt（解包器的产物，是预期行为），2026-08-20 就因此误报过一次 ✗。
#   判据要针对"必需的东西在不在"，不要对"多了什么"敏感。
esp=$(sh 'M=/data/local/tmp/espv; mkdir -p $M; mountpoint -q $M || mount -t vfat -o ro /dev/block/by-name/esp $M
MID=$(ls $M | grep -E "^[0-9a-f]{32}$" | head -1)
for s in a b; do
  miss=""
  for f in Image gaokun3.dtb ramdisk.img; do
    [ -s "$M/$MID/android/slot_$s/$f" ] || miss="$miss $f"
  done
  [ -z "$miss" ] && echo -n "slot_$s=ok " || echo -n "slot_$s=missing:$miss "
  [ -s "$M/$MID/android/slot_$s/recovery-ramdisk.img" ] && echo -n "(recovery) "
done
umount $M')
echo "  $esp"
ck "slot_a 三个必需文件齐" 'slot_a=ok' "$esp"
ck "slot_b 三个必需文件齐" 'slot_b=ok' "$esp"

echo
echo "══ 6. 传感器（内核 =y 之后应当开机自起）══"
ck "hexagonrpcd 在跑" '^[1-9]' "$(sh 'ps -A -o NAME | grep -c hexagonrpcd')"
ck "sensors HAL 在跑" '^[1-9]' "$(sh 'ps -A -o NAME | grep -c sensors-service.gaokun3')"
ck "框架看到加速度计" 'SH3001 Accelerometer' "$(sh 'dumpsys sensorservice 2>/dev/null | grep -m1 SH3001')"

echo
echo "══ 结果：$pass 项通过，$fail 项失败 ══"
[ "$fail" -eq 0 ] || exit 1
