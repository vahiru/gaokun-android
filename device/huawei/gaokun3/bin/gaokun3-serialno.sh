#!/vendor/bin/sh
# 读取整机序列号，写入vendor.gaokun3.serialno（etc/serialno.rc在init阶段同步运行它）。
#
# 依次读SMBIOS的product_serial（CONFIG_DMIID=y导出的/sys/class/dmi/id/）、chassis_serial与SoC序列号，
# 都不可用时用字面量gaokun3。只接受ASCII字母、数字与._-，其余值（空、None、Default…、"To be filled…"）跳过。
#
# 不直接写ro.serialno：核心策略只允许白名单里的域设置和读取serialno_prop（private/domain.te），本脚本的域
# gaokun3_serialno不在其中。etc/serialno.rc再由白名单内的vendor_init把这个值写进ro.boot.serialno与ro.serialno。
# 序列号不从引导程序（androidboot.serialno）取：不经过gk3boot的启动方式也要有同一个序列号。

sn=""
for f in /sys/class/dmi/id/product_serial /sys/class/dmi/id/chassis_serial /sys/devices/soc0/serial_number; do
    [ -r "$f" ] || continue
    read -r raw < "$f" 2>/dev/null
    case "$raw" in
        ""|None|Default*|"To be filled"*|*[!A-Za-z0-9._-]*) continue ;;
        *) sn="$raw"; break ;;
    esac
done
[ -n "$sn" ] || sn=gaokun3
setprop vendor.gaokun3.serialno "$sn"
