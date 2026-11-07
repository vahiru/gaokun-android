#!/vendor/bin/sh
# remoteproc 兜底：ramdisk 里已带固件，DSP 在早期 probe 时就已 running；
# 万一哪颗没起来（固件路径变动等），这里补一刀。ADSP 不起来就没有声卡。
#
# ⚠️★ 为什么不再在 init rc 里直接 `write .../state start`（docs/dev/sensors-ssc-protocol.md）：
#   对【已在运行】的 DSP 写 start，内核只把 rproc->power 加一（rproc_boot），而 sysfs 的
#   stop 只减一、减不到 0 就静默返回成功（remoteproc_core.c:1996-1998，不打日志）。
#   旧的 rc 触发器挂在 sys.boot_completed=1 上，每次 framework 软重启都会再触发一次 ⇒
#   引用越积越多，SLPI 的实验循环（scripts/ssc/sscexp.sh）写一次 stop 根本停不下来。
#   ⇒ 只对【不在 running】的写 start，脚本幂等，触发多少次都一样。
for r in /sys/class/remoteproc/remoteproc*; do
    [ -e "$r/state" ] || continue
    st=$(cat "$r/state" 2>/dev/null)
    name=$(cat "$r/name" 2>/dev/null)
    case "$st" in
        running|attached) ;;
        *)
            log -t gaokun3-rproc "$name（$r）状态是 $st，补一次 start"
            echo start > "$r/state" 2>/dev/null ||
                log -t gaokun3-rproc "$name start 失败"
            ;;
    esac
done
exit 0
