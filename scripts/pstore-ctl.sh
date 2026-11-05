#!/usr/bin/env bash
#
# efi_pstore 操作封装 —— MateBook E Go (gaokun3)
#
#   sudo bash pstore-ctl.sh enable      # 注册 efi_pstore 后端（每次开机都要跑）
#   sudo bash pstore-ctl.sh read        # 读崩溃日志
#   sudo bash pstore-ctl.sh save <dir>  # 归档到目录
#   sudo bash pstore-ctl.sh clear       # 清 NVRAM（读完必须做）
#   sudo bash pstore-ctl.sh status      # 当前状态
#
# ⚠️★★★ 2026-10-30 起发布构建默认关 efi_pstore（cmdline efi_pstore.pstore_disable=1 + 内核
#   EFI_VARS_PSTORE_DEFAULT_DISABLE）：用户报告没人清的 dump-type* 撑满 NVRAM，固件建不了新启动项、
#   U 盘启动不了（BoardConfig.mk 里那一项的注释）。`enable` 现在就是开发时临时打开的开关 ——
#   开了就必须在用完后 `clear`，不要在长期使用的机器上一直开着。
#
# 为什么需要这个脚本 —— 本机 efi_pstore 有两个反直觉的地方：
#
# 1. 开机不会自动注册。efi-pstore 编进内核后在 device_initcall 阶段跑，
#    而提供 EFI 变量读写的高通 uefisecapp 驱动要到 ~0.76s 才注册 efivars，
#    此时 efivar_supports_writes() 还是 false，efi-pstore 静默返回且永不重试。
#    绕法：切换 pstore_disable 参数，其 setter 会重新调用 efivars_pstore_init()。
#
# 2. 从 /sys/fs/pstore/ 删文件【不会】真的删掉 EFI 变量。实测 rm 之后
#    NVRAM 计数不变，必须直接删 /sys/firmware/efi/efivars/dump-type* 。
#    不清的话每次 panic 吃掉约 11 个变量（10 KB），迟早撑爆 NVRAM。
#
# 背景：ramoops 在这台机器上不可用 —— 固件每次复位都重新初始化 DRAM，
# 低位(0xae900000)和高位(0x865d38000)地址都试过，内容一律不存活。
# 详见 docs/dev/hardware.md。

set -u

PARAM=/sys/module/efi_pstore/parameters/pstore_disable
EFIVARS=/sys/firmware/efi/efivars
PSTORE=/sys/fs/pstore

[ "$(id -u)" -ne 0 ] && { echo "需要 root"; exit 1; }

is_registered() { dmesg | grep -q 'Registered efi_pstore'; }

do_enable() {
    if [ ! -e "$PARAM" ]; then
        echo "错误: $PARAM 不存在。内核缺 CONFIG_EFI_VARS_PSTORE？"
        exit 1
    fi
    mountpoint -q "$PSTORE" || mount -t pstore pstore "$PSTORE" 2>/dev/null

    # 切一下参数，触发 efivars_pstore_init() 重跑
    echo 1 > "$PARAM"; echo 0 > "$PARAM"
    sleep 1

    if dmesg | tail -20 | grep -q 'Registered efi_pstore'; then
        echo "✅ efi_pstore 已注册"
    elif is_registered; then
        echo "✅ efi_pstore 已注册（本次启动早些时候）"
    else
        echo "❌ 注册失败。检查 dmesg 里 'efivars: Registered efivars operations' 是否出现过。"
        exit 1
    fi
}

do_status() {
    echo "efi_pstore 已注册 : $(is_registered && echo 是 || echo 否)"
    echo "pstore 已挂载     : $(mountpoint -q $PSTORE && echo 是 || echo 否)"
    echo "pstore 条目数     : $(ls -A $PSTORE 2>/dev/null | wc -l)"
    echo "NVRAM 变量总数    : $(ls $EFIVARS 2>/dev/null | wc -l)"
    echo "其中 dump-type*   : $(ls $EFIVARS 2>/dev/null | grep -c '^dump-type')"
    echo
    echo "ramoops 状态      : 本机不可用（固件复位重初始化 DRAM）"
}

do_read() {
    local n
    n=$(ls -A "$PSTORE" 2>/dev/null | wc -l)
    if [ "$n" -eq 0 ]; then
        echo "pstore 为空。"
        is_registered || echo "（后端尚未注册 —— 先跑 '$0 enable'）"
        return
    fi
    echo "共 $n 条记录："
    echo
    # 分片文件名形如 dmesg-efi_pstore-<时间戳><序号>，按名字排序即按 Part 顺序
    for f in $(ls "$PSTORE" | sort); do
        echo "════════ $f  ($(stat -c%s "$PSTORE/$f") 字节)"
        cat "$PSTORE/$f"
        echo
    done
}

do_save() {
    local dir="${1:-/var/log/pstore-archive/$(date +%Y%m%d-%H%M%S)}"
    local n
    n=$(ls -A "$PSTORE" 2>/dev/null | wc -l)
    [ "$n" -eq 0 ] && { echo "pstore 为空，无需归档。"; return; }

    mkdir -p "$dir"
    cp "$PSTORE"/* "$dir"/ 2>/dev/null
    # 分片重新拼成一份可读的完整日志
    { for f in $(ls "$dir" | sort); do cat "$dir/$f"; echo; done; } > "$dir/combined.txt"
    echo "已归档 $n 条到 $dir"
    echo "合并版: $dir/combined.txt"
}

do_clear() {
    local before after
    before=$(ls "$EFIVARS" 2>/dev/null | wc -l)

    # 关键：rm /sys/fs/pstore/* 不会真的删 EFI 变量，必须直接操作 efivarfs
    for f in "$EFIVARS"/dump-type*; do
        [ -e "$f" ] || continue
        chattr -i "$f" 2>/dev/null
        rm -f "$f" 2>/dev/null
    done
    rm -f "$PSTORE"/* 2>/dev/null

    after=$(ls "$EFIVARS" 2>/dev/null | wc -l)
    echo "NVRAM 变量: $before → $after"
    local left
    left=$(ls "$EFIVARS" 2>/dev/null | grep -c '^dump-type')
    [ "$left" -eq 0 ] && echo "✅ dump 变量已清空" || echo "⚠️ 仍剩 $left 个"
}

case "${1:-status}" in
    enable) do_enable ;;
    read)   do_read ;;
    save)   do_save "${2:-}" ;;
    clear)  do_clear ;;
    status) do_status ;;
    *) sed -n '2,12p' "$0"; exit 1 ;;
esac
