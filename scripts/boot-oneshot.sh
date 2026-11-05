#!/usr/bin/env bash
# 让下一次（且仅下一次）启动走指定的 systemd-boot 条目，不改 ESP 上的 default。
#
#   bash scripts/boot-oneshot.sh --list                    # 列出可选条目与当前状态
#   bash scripts/boot-oneshot.sh <条目名.conf>             # 写 oneshot（不重启）
#   bash scripts/boot-oneshot.sh --clear                   # 撤销
#
# ★ 为什么值得有这个脚本：本仓每次测新内核都要做这件事，而做法有三个不显然的点，
#   忘一个就静默失败（#42）：
#     1. Android 侧**能**写 EFI 变量 —— 尽管 cmdline 里有 `efi=noruntime`。
#        本机的 EFI 变量走高通 TrustZone 的 uefisecapp 后端，不依赖 EFI 运行时服务。
#        （M4/M6 曾判定"写不了、只能先进救援 Ubuntu"，#42 推翻了。）
#     2. 格式 = 4 字节属性（NV|BS|RT = 0x07，小端）+ 条目名的 **UTF-16LE** + 双字节 NUL。
#     3. 覆盖已存在的变量前要 **`chattr -i`**，否则 write 直接 EPERM。
#
# 本脚本**不重启**。重启前请确认现场有人能按电源键。
set -uo pipefail
SER=${SER:-gaokun3}
GUID=4a67b082-0a4c-41cf-b6c7-440b29bb8c4f      # systemd-boot 的厂商 GUID
VAR=LoaderEntryOneShot-$GUID
EFI=/data/local/tmp/efivars
ESP=/mnt/gaokun3_oneshot
S() { adb -s "$SER" shell "$@"; }
die() { echo "✗ $*" >&2; exit 1; }

S true >/dev/null 2>&1 || die "adb 连不上 $SER"
[ "$(S id -u | tr -d '\r')" = "0" ] || die "需要 root"

S "mkdir -p $EFI; mountpoint -q $EFI || mount -t efivarfs none $EFI" >/dev/null 2>&1
S "[ -d $EFI ]" || die "挂不上 efivarfs"

case "${1:---list}" in
--list)
    echo "── ESP 上的条目 ──"
    S "mkdir -p $ESP; mount -t vfat /dev/block/by-name/esp $ESP 2>/dev/null
       ls $ESP/loader/entries/; echo; grep ^default $ESP/loader/loader.conf
       umount $ESP 2>/dev/null; rmdir $ESP 2>/dev/null" | tr -d '\r'
    echo "  （名字带 +N[-M] 的是 systemd-boot 的启动计数条目；OneShot / default 匹配的是去掉计数之后的名字）"
    echo "── 当前 EFI 变量 ──"
    # 跳过前 4 字节属性，UTF-16LE 转 ASCII
    for v in LoaderEntryOneShot LoaderEntrySelected LoaderEntryDefault; do
        printf "  %-20s %s\n" "$v" \
          "$(S "[ -f $EFI/$v-$GUID ] && dd if=$EFI/$v-$GUID bs=1 skip=4 2>/dev/null | tr -d '\\0'" | tr -d '\r')"
    done
    echo "── misc（gk3-misc 解码，只读）──"
    SER="$SER" bash "$(dirname "$0")/misc-dump.sh" 2>&1 | sed 's/^/  /'
    ;;
--clear)
    S "chattr -i $EFI/$VAR 2>/dev/null; rm -f $EFI/$VAR" >/dev/null 2>&1
    S "[ -f $EFI/$VAR ]" && die "没清掉" || echo "✓ oneshot 已清除"
    ;;
*)
    # ★ S11（统一启动入口设计稿 §4.15）：systemd-boot 的启动计数条目文件名是 <id>+LEFT[-DONE].conf，
    #   而 OneShot 匹配的是去掉计数之后的 <id>.conf ⇒ 传进来的名字先去掉计数，存在性检查同时认两种文件名。
    ENTRY=$(printf '%s' "$1" | sed -E 's/\+[0-9]+(-[0-9]+)?\.conf$/.conf/')
    [ "$ENTRY" != "$1" ] && echo "  （去掉启动计数：${1} → ${ENTRY}）"
    BASE=${ENTRY%.conf}
    S "mkdir -p $ESP; mount -t vfat /dev/block/by-name/esp $ESP 2>/dev/null
       R=1; for f in $ESP/loader/entries/$BASE.conf $ESP/loader/entries/$BASE+*.conf; do [ -f \"\$f\" ] && R=0; done
       umount $ESP 2>/dev/null; rmdir $ESP 2>/dev/null; exit \$R" \
        || die "ESP 上没有条目 '${ENTRY}'（也没有带计数的 ${BASE}+*.conf）—— 先用 --list 看确切文件名"
    # 属性 0x07 小端 + UTF-16LE + 双 NUL
    S "chattr -i $EFI/$VAR 2>/dev/null
       printf '\\x07\\x00\\x00\\x00' > /data/local/tmp/.oneshot.bin
       printf '%s' '$ENTRY' | iconv -f UTF-8 -t UTF-16LE >> /data/local/tmp/.oneshot.bin 2>/dev/null \
         || python3 -c \"import sys;sys.stdout.buffer.write('$ENTRY'.encode('utf-16-le'))\" >> /data/local/tmp/.oneshot.bin
       printf '\\x00\\x00' >> /data/local/tmp/.oneshot.bin
       cat /data/local/tmp/.oneshot.bin > $EFI/$VAR
       rm -f /data/local/tmp/.oneshot.bin" >/dev/null 2>&1
    GOT=$(S "dd if=$EFI/$VAR bs=1 skip=4 2>/dev/null | tr -d '\\0'" | tr -d '\r')
    # ★ 回读校验：写成功不等于内容对（#42 的三个坑都在这一步暴露）
    [ "$GOT" = "$ENTRY" ] || die "回读是 '$GOT'，期望 '$ENTRY' —— oneshot 没写对，别重启"
    echo "✓ oneshot = ${ENTRY}（回读一致）"
    echo "  下一次启动会走它，之后自动回到 default。重启请征得用户同意。"
    ;;
esac
