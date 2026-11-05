#!/system/bin/sh
#
# A/B OTA 的 postinstall 钩子：把刚刷好的那个槽的内核（与 recovery）搬到 ESP 上。
#
# 背景：本机按 Android 分区规范有 boot_a / boot_b，update_engine 会像刷别的
# 分区一样把标准 Android boot 镜像刷进去。但引导链是 UEFI + systemd-boot，
# 它【读不了】Android boot 镜像 —— 只会从 ESP 按 BLS 条目加载文件。
# 所以过渡期在这里补一步：从 boot_<目标槽> 解出 kernel/ramdisk/dtb，
# 放到 ESP 上该槽专属的目录。
#   boot 分区 = 唯一真相源；ESP 上的文件 = 派生物。
# 两个槽的 BLS 条目【永久】指向各自的 slot_a/ slot_b，所以这里只放文件、
# 不改条目，也就绝不会碰到正在运行的那个槽 —— 回滚天然安全。
#
# recovery 走同一条路，但它【没有自己的分区】（安装器把剩余空间全给了
# userdata，已装机器没有余地再切），所以它的 ramdisk 作为文件随 vendor 走
# payload，在这里铺到 ESP 并派生出一个 BLS 条目。
# ★ 于是已装的机器一次普通 OTA 就能拿到 recovery，不必重装。
#
# 自研的 EFI 加载器（读 misc 选槽 + 解析 boot 镜像 + 装 initrd/DTB 协议）
# 就位之后，本脚本连同 ESP 上那些派生文件一起退役。
#
# ★ 参数是 update_engine 给的，不是猜的：
#   system/update_engine/payload_consumer/postinstall_runner_action.cc:355-357
#     argv[1] = target_slot（整数，0=_a 1=_b）  argv[2] = 状态 fd
#
# 退出码非 0 会让整个 OTA 失败。这是【故意的】：宁可更新失败，也不能让某个槽
# 位上出现"新 system + 旧内核"的组合。
set -u

TARGET_SLOT="${1:-}"
case "$TARGET_SLOT" in
    0) SUFFIX=a ;;
    1) SUFFIX=b ;;
    *) echo "postinstall: 目标槽位参数无效: '$TARGET_SLOT'"; exit 1 ;;
esac

# 本脚本随【新的】vendor 分区被挂到 /postinstall，所以同目录下的解包器
# 也是新的那一份 —— 与新 boot 镜像的格式必然匹配。
HERE="$(dirname "$0")"
EXTRACT="$HERE/gaokun3-bootimg-extract"
BOOT_DEV="/dev/block/by-name/boot_$SUFFIX"
ESP_DEV=/dev/block/by-name/esp
MNT=/mnt/gaokun3_ota_esp

log()  { echo "postinstall: $*"; }
fail() { log "失败: $*"; [ -n "${MOUNTED:-}" ] && { sync; umount "$MNT" 2>/dev/null; }; exit 1; }

log "目标槽位 = _$SUFFIX"

[ -x "$EXTRACT" ] || fail "$EXTRACT 不存在或不可执行"
[ -e "$BOOT_DEV" ] || fail "$BOOT_DEV 不存在（boot_a/boot_b 分区建了吗？）"
# ★ 2026-09-14（用户反馈 #1：v0.6.0 四次 OTA 全部死在这里）：by-name/esp 这个链接只在 GPT 分区名
#   （PARTLABEL）正好是 esp 时才有。手工分区 / 双系统的人往往只打了 vfat 卷标（比如 GAOKUN3ESP），
#   链接不存在，OTA 就在最后一步失败。所以不再依赖名字：找不到链接时【按内容】探测 ——
#   扫所有 vfat 分区，只读挂上，看谁有 loader/entries/*-android-*.conf。多个 ESP（比如 Windows 的）
#   也能分开：只有我们的那个有 android 条目。
find_esp() {
    # ⚠️ 这个函数的 stdout 就是返回值（ESP_DEV=$(find_esp)）⇒ 里面的 log 一律走 stderr。
    #   2026-09-29 之前"按内容找到 ESP"那一行打在 stdout 上，会和设备路径一起被捕获。
    # ★ 2026-09-29：双系统复用 Windows 的 ESP，PARTLABEL 是 "EFI system partition"，
    #   ueventd 规整成 by-name/EFI_system_partition。两个名字 sepolicy/file_contexts 都标成 ESP 类型。
    #   ⚠️ 名字只是候选：盘上可能同时有 Windows 的 ESP（就叫这个名字）和另起名字的 Android ESP
    #   （用户反馈 #1 那种手工分区）⇒ 候选也要过内容检查。全都不过才扫全盘；
    #   扫全盘在 enforcing 下走不通（那些节点是通用 block_device，domain.te:705）。
    # ⓘ 这里的 *-android-*.conf 只用来认"是不是我们的 ESP"，gk3boot / gk3prev 条目也是我们写的，所以不必像
    #   下面选直连条目那样排除它们（gk3boot 条目不会脱离直连条目单独存在：它的 fail-open 要指向直连条目）。
    PROBE=/mnt/gaokun3_esp_probe; mkdir -p "$PROBE" || return 1
    NAMED=""
    for n in esp EFI_system_partition; do
        d=/dev/block/by-name/$n
        [ -e "$d" ] || continue
        [ -n "$NAMED" ] || NAMED=$d
        mount -o ro -t vfat "$d" "$PROBE" 2>/dev/null || continue
        if ls "$PROBE"/loader/entries/*-android-*.conf >/dev/null 2>&1; then
            umount "$PROBE"; echo "$d"; return 0
        fi
        umount "$PROBE"
    done
    log "by-name 候选（esp / EFI_system_partition）里没有带 android 启动项的，改为按内容探测全部 vfat 分区（enforcing 下会失败）" >&2
    for d in /dev/block/nvme*n*p* /dev/block/sd*[0-9] /dev/block/mmcblk*p*; do
        [ -b "$d" ] || continue
        toybox blkid "$d" 2>/dev/null | grep -q 'TYPE="vfat"' || continue
        mount -o ro -t vfat "$d" "$PROBE" 2>/dev/null || continue
        if ls "$PROBE"/loader/entries/*-android-*.conf >/dev/null 2>&1; then
            umount "$PROBE"; log "按内容找到 ESP = $d" >&2; echo "$d"; return 0
        fi
        umount "$PROBE"
    done
    # 最后一招（2026-09-14 之前的行为）：信名字。by-name/esp 是整盘安装建的，
    #   它的 loader/entries 为空更像是 ESP 坏了、而不是找错了。
    [ -n "$NAMED" ] && { log "按内容没找到，退回用 $NAMED" >&2; echo "$NAMED"; return 0; }
    return 1
}
ESP_DEV=$(find_esp) || fail "找不到 ESP：没有 /dev/block/by-name/esp（PARTLABEL=esp），扫描 vfat 分区也没有含 loader/entries/*-android-*.conf 的那个"

mkdir -p "$MNT" || fail "mkdir $MNT"
mount -t vfat "$ESP_DEV" "$MNT" || fail "挂载 ESP"
MOUNTED=1

# systemd-boot 的布局是 <ESP>/<machine-id>/…，machine-id 不固定。
# ★ v1.0 OTA-9（2026-10-05）：目录【从该槽的启动项反推】，不再按名字猜。
#   原来是 `ls | grep -E '^[0-9a-f]{32}$' | head -1`（第一个 32 位十六进制目录）。ESP 与另一个用
#   systemd-boot / kernel-install 的 Linux 共用、而它的 machine-id 排在我们前面时，内核写进了别人的目录，
#   我们的启动项照旧指着旧内核，cmdline 同步也只打一行警告 —— OTA 报成功，重启后是"新 system + 旧内核"，
#   正是文件开头说绝不能出现的组合（新 vendor_dlkm 配旧内核，模块版本对不上）。
#   现在：启动项 loader/entries/*-android-<槽>.conf（.conf.disabled 不算，glob 本来就匹配不到）必须【恰好一个】，
#   它的 linux 行必须是 /<目录>/android/slot_<槽>/Image —— 内核写进那个目录、改的也是那个条目。
#   找不到 / 多于一个 / linux 行对不上 ⇒ 让 OTA 失败：没有启动项的槽本来就起不来（boot_control 切 default 时
#   按同一个通配找条目），失败了用户留在当前能用的槽上，比"报成功、重启进一个起不来的槽"好。
#   安装器写的条目正是这个形状（scripts/live/installer-lib.sh:919-926：$mid-android-$slot.conf，
#   linux /$mid/android/slot_$slot/Image），所以装好的机器两边选出的是同一个目录。
#   ★ 统一启动入口（2026-10-05，S9）：只认【直连条目】<32 位小写十六进制 machine-id>-android-<槽>.conf。
#     gk3boot 的条目被祝福（去掉计数）之后叫 gk3boot-android-<槽>.conf、上一版入口叫 gk3prev-android-<槽>.conf，
#     都匹配 *-android-<槽>.conf；不排除的话这里会数出 2–3 个而让 OTA 失败。它们是 efi 条目、没有 linux 行，
#     内核目录只能从直连条目反推。（loader.conf 的 default *-android-<槽>.conf 不用改：gk3boot 条目 sort-key
#     0gk3 排在直连条目 zandroid<槽> 前面，照样先命中，设计稿 §4.2。）
#     非默认条目 gk3boot-tools.conf 名字里没有 -android-，这里的 glob、default 通配、find_esp 的认盘通配都匹配不到它。
# $1 = 条目文件名（不带目录），$2 = 槽字母；是直连条目时返回 0。规则与 gk3boot.efi 找 fail-open 目标
#   （tools/gk3boot/efi/boot/gk3boot.c 的 direct_cb）、安装器的 gk3__esp_pick_mid 一致。
is_direct_entry() {
    _id=${1%-android-"$2".conf}
    [ "$_id" != "$1" ] && [ "${#_id}" -eq 32 ] || return 1
    case "$_id" in *[!0-9a-f]*) return 1 ;; esac
    return 0
}
ENTS=""; NENT=0
for e in "$MNT"/loader/entries/*-android-"$SUFFIX".conf; do
    [ -f "$e" ] || continue
    is_direct_entry "${e##*/}" "$SUFFIX" || continue
    ENTS="$ENTS ${e##*/}"; NENT=$((NENT + 1)); ENT=$e
done
[ "$NENT" = 1 ] || fail "ESP 上 <machine-id>-android-$SUFFIX.conf 直连启动项有 $NENT 个（${ENTS:- 无}）—— 要恰好一个，才知道内核该写进哪个目录。多出来的那个请用安装器 live 清理（或改名成 .conf.disabled）"
KPATH=$(sed -n 's/^linux[[:space:]][[:space:]]*//p' "$ENT" | head -1 | tr -d '\r' | sed 's/[[:space:]]*$//')
# ↑ 行尾空白 systemd-boot 容忍，这里也去掉（审查建议修 2）
case "$KPATH" in
    /*/android/slot_"$SUFFIX"/Image) ;;
    *) fail "启动项 ${ENT##*/} 的 linux 行是 '$KPATH'，不是 /<目录>/android/slot_$SUFFIX/Image —— 不知道该往哪写" ;;
esac
MID=${KPATH#/}; MID=${MID%%/*}
# 嵌套路径（/x/y/android/slot_a/Image）能过上面的 case，却会算出 MID=x、把内核写到别处而 OTA 报成功 ⇒ 反核（审查建议修 1）
[ "$KPATH" = "/$MID/android/slot_$SUFFIX/Image" ] || fail "启动项 ${ENT##*/} 的 linux 行 '$KPATH' 不是 /<目录>/android/slot_$SUFFIX/Image 这一层结构 —— 不知道该往哪写"
[ -n "$MID" ] && [ -d "$MNT/$MID" ] || fail "启动项 ${ENT##*/} 指向的目录 /$MID 在 ESP 上不存在"
DEST="$MNT/$MID/android/slot_$SUFFIX"
mkdir -p "$DEST" || fail "mkdir $DEST"
log "启动项 = ${ENT##*/} → 目标目录 = $DEST"

# ★ v1.0 OTA-8（2026-10-05）：recovery ramdisk 只在 recovery 启动项开着时才铺。
#   原来只要 vendor 里有 recovery-ramdisk.img（实机 14974339 字节）就每次 OTA 往 ESP 写一份，而启动项默认根本不建
#   （见下面 recovery 一节），两个槽合计白占约 30 MB —— 开发机 ESP 只剩约 46 MB。
#   关着时顺手删掉两个槽里已有的那份，但【只删没有任何启动项引用的】（grep 整个 loader/entries）——
#   当前在跑的槽目录里也只动这一个文件，Image / ramdisk / dtb 一概不碰，回滚不受影响。
REC_ON=0
[ "$(getprop persist.vendor.gaokun3.recovery_entry 2>/dev/null)" = "1" ] && REC_ON=1
if [ "$REC_ON" = 0 ]; then
    # 目标槽自己的 recovery 条目（调试时 recovery_entry=1 建过的）先删：这个槽正要被换掉、不在跑，
    # 删了它下面那份 ramdisk 才没人引用，空间检查前就能腾出来。只删我们起的这个名字。
    REC_ENT_T="$MNT/loader/entries/$MID-recovery-$SUFFIX.conf"
    [ -f "$REC_ENT_T" ] && rm -f "$REC_ENT_T" && log "recovery 启动项没开 ⇒ 删掉旧的 ${REC_ENT_T##*/}"
    for sl in a b; do
        r="$MNT/$MID/android/slot_$sl/recovery-ramdisk.img"
        [ -f "$r" ] || continue
        if grep -qs "slot_$sl/recovery-ramdisk.img" "$MNT"/loader/entries/*.conf; then
            log "slot_$sl 的 recovery-ramdisk.img 还被启动项引用，保留"
        else
            rm -f "$r" && log "recovery 启动项没开 ⇒ 删掉 slot_$sl 里用不上的 recovery-ramdisk.img"
        fi
    done
fi

# ★ 先看空间：ESP 只有 300 MiB，还要和固件自己那个 73 MiB 的
#   Persisted_Capsules.bin 共处。空间不够必须【当场失败】，
#   而不是写出一个被截断的内核 —— 那会变成一台不开机的机器。
#   目标目录里的旧文件会被覆盖，所以它们占的空间算作可用。
avail_kb=$(df -k "$MNT" | tail -1 | awk '{print $4}')
# recovery-ramdisk.img 只在这次要重写它时（REC_ON=1）才算"将被覆盖"；没开时它要么已被上面删掉，
# 要么还被别的启动项引用、不会动 —— 都不能算进可用空间。
OVERWRITE="Image ramdisk.img gaokun3.dtb"
[ "$REC_ON" = 1 ] && OVERWRITE="$OVERWRITE recovery-ramdisk.img"
for f in $OVERWRITE; do
    [ -f "$DEST/$f" ] && avail_kb=$((avail_kb + $(stat -c%s "$DEST/$f") / 1024))
done
log "可用（含将被覆盖的旧文件）约 ${avail_kb} KB"
# zboot 内核 13 + ramdisk 13 + dtb 0.2 + recovery ramdisk 15 ≈ 42 MB，留 56 MB 余量。
# 不铺 recovery 时少 15 MB（14974339 字节 ≈ 14.3 MiB），门槛同减 15 MiB，余量不变。
need_kb=57344
[ "$REC_ON" = 0 ] && need_kb=$((need_kb - 15360))
# 统一启动入口（见下面 gk3boot 一节）：要部署时再加上它的二进制（约 100 KB）+ 执行端 fastboot.img（2–4 MiB，
#   这一版带了才算，按实际大小）+ 条目的余量（两个现役 / .staged + gk3boot-tools.conf，16 KB 绰绰有余）。
#   宁可多算：已经是同一份就不会真写，但这里不为省这几 MB 去先比对。
#   只算【这次要写的一版】：ESP 上已有的版本目录（现役 + gk3prev）早已算在 df 的"已用"里。常态最多两版共存；
#   这次铺 .staged 之后、新槽开机完成之前是三版（现役 + gk3prev + staged），HAL 激活时只写条目、不新增文件，
#   并回收没人引用的最老那版 —— 所以峰值就是"已有的 + 这一版"，正是这里检查的。
GK3_PROP=persist.vendor.gaokun3.gk3boot
GK3_MODE=$(getprop "$GK3_PROP" 2>/dev/null); [ -n "$GK3_MODE" ] || GK3_MODE=off
GK3_SRC="$HERE/../boot/gk3boot"
case "$GK3_MODE" in
    observe|action)
        if [ -f "$GK3_SRC/gk3boot.efi" ]; then
            need_kb=$((need_kb + $(stat -c%s "$GK3_SRC/gk3boot.efi") / 1024 + 16))
            [ -f "$GK3_SRC/fastboot.img" ] && need_kb=$((need_kb + $(stat -c%s "$GK3_SRC/fastboot.img") / 1024 + 1))
        fi ;;
esac
[ "$avail_kb" -gt "$need_kb" ] || \
    fail "ESP 空间不足（需约 $((need_kb / 1024)) MB）。清掉 <ESP>/$MID/android/ 下的 *.bak-* 再试"

# 解包器自己会写临时文件再改名，并逐段核对长度
"$EXTRACT" "$BOOT_DEV" "$DEST" || fail "从 $BOOT_DEV 解包失败"

# ── 把 boot.img 里的 cmdline 同步进该槽的启动项 ──────────────────────────────
# ★ 2026-09-16（#116 §17）：此前这里只换 Image/ramdisk/dtb，启动项的 options 行
#   是装机当天写死的、以后永远不动。于是 BOARD_KERNEL_CMDLINE 的任何改动都会进
#   boot.img（解出来的 cmdline.txt 里有），却【永远到不了】实际启动用的 .conf ——
#   v0.6.2 的 himax_hx83121a_spi.disable_pressure=0 就是这么静默丢掉的。
#   现在 options = cmdline.txt 的内容 + slot_suffix，与 boot.img 永远一致。
#   写法与 recovery 条目一样：临时文件再改名；cmdline.txt 缺失或为空则保留旧 options。
# ENT 是上面 OTA-9 那段选出来的启动项（不再按 $MID-android-$SUFFIX.conf 拼名字）。
if [ -s "$DEST/cmdline.txt" ] && [ -f "$ENT" ]; then
    NEWCMD=$(tr -d '\r\n' < "$DEST/cmdline.txt")
    case " $NEWCMD " in
        *" androidboot.slot_suffix="*) ;;                       # 万一 cmdline 已带，不重复
        *) NEWCMD="$NEWCMD androidboot.slot_suffix=_$SUFFIX" ;;
    esac
    if awk -v cmd="$NEWCMD" '/^options[[:space:]]/ { print "options    " cmd; next } { print }'            "$ENT" > "$ENT.new" && grep -q "^options " "$ENT.new"; then
        mv -f "$ENT.new" "$ENT"
        log "启动项 options 已同步为 boot.img 的 cmdline（$(wc -c < "$DEST/cmdline.txt") 字节）"
    else
        rm -f "$ENT.new"
        log "⚠️ 同步 options 失败，保留旧启动项（内核仍能起，但 cmdline 改动不会生效）"
    fi
else
    log "⚠️ 没有 cmdline.txt 或找不到 $ENT，启动项 options 未更新"
fi

# ── 救援条目：借这个槽内核的那条，options 也跟着 boot.img 走（v1.0 计划 OTA-10 / GUI-10 / INST-17）────────
# 安装器装了救援系统时写两条（scripts/live/installer-lib.sh 的 gk3_apply）：<MID>-rescue.conf 借 slot_a 的内核与 dtb、
#   <MID>-rescue-b.conf 借 slot_b 的，initramfs 共用 <MID>/rescue/initramfs.img。上面刚把这个槽的内核换了 ——
#   借它的那条救援条目的 options 原先是装机当天写死的，与新内核的 cmdline 会漂（与直连条目 #116 §17 是同一种漂移）。
# 规则与安装器的 gk3__rescue_cmdline【逐字相同】（改一边就要改另一边；tools/gk3boot/test/postinstall/run.sh 的 R 组交叉核对）：
#   去掉 androidboot.* / init= / firmware_class.path=，末尾加 loglevel=3 panic=10 gk3.squash=/gaokun3/rescue.squashfs。
# 只动安装器写的那种：文件名 <MID>-rescue*.conf、linux 行正好是 /<MID>/android/slot_<槽>/Image、options 里有
#   gk3.squash=/gaokun3/rescue.squashfs。别的（手写的、指向别处的）一概不碰。
# v1.0 之前装的机器只有借 slot_a 的那一条：目标是 b 时从它派生借 slot_b 的那条（只多一个 .conf，不占 ESP 空间）；
#   原标题 "(runs from RAM)" 不准（squashfs 是只读挂的，只有写入层在内存），顺手改成与安装器一致的 "(slot a kernel)"。
# ★ 这一节任何失败都只记日志、不让 OTA 失败：救援条目不影响 Android 启动。
rescue_cmdline() {   # $1 = Android 的 cmdline
    _rk=""
    set -f
    for _t in $1; do
        case "$_t" in androidboot.*|init=*|firmware_class.path=*) ;; *) _rk="$_rk$_t " ;; esac
    done
    set +f
    echo "${_rk}loglevel=3 panic=10 gk3.squash=/gaokun3/rescue.squashfs"
}
rescue_ours() {   # $1=条目 $2=槽字母：是安装器写的、借这个槽内核的救援条目时返回 0
    _rl=$(sed -n 's/^linux[[:space:]][[:space:]]*//p' "$1" | head -1 | tr -d '\r' | sed 's/[[:space:]]*$//')
    [ "$_rl" = "/$MID/android/slot_$2/Image" ] || return 1
    grep -qE '^options[[:space:]].*gk3\.squash=/gaokun3/rescue\.squashfs([[:space:]]|$)' "$1"
}
RESC_OLD_TITLE="gaokun3 rescue (runs from RAM)"
RA="$MNT/loader/entries/$MID-rescue.conf"; RB="$MNT/loader/entries/$MID-rescue-b.conf"
if [ -s "$DEST/cmdline.txt" ] && [ -f "$RA" ]; then
    RCMD=$(rescue_cmdline "$(tr -d '\r\n' < "$DEST/cmdline.txt")")
    if [ "$SUFFIX" = b ] && [ ! -e "$RB" ] && rescue_ours "$RA" a; then
        if sed -e "s|^title .*|title      gaokun3 rescue (slot b kernel)|" -e "s|^version .*|version    gaokun3-rescue-b|" \
               -e "s|^sort-key .*|sort-key   linux2|" -e "s|^\(linux[[:space:]].*\)/android/slot_a/|\1/android/slot_b/|" \
               -e "s|^\(devicetree[[:space:]].*\)/android/slot_a/|\1/android/slot_b/|" "$RA" > "$RB.new" &&
           grep -q "^linux .*/android/slot_b/Image" "$RB.new" && mv -f "$RB.new" "$RB"; then
            log "救援条目：从 ${RA##*/} 派生了借 slot_b 内核的 ${RB##*/}（INST-17）"
        else
            rm -f "$RB.new"; log "⚠️ 派生 ${RB##*/} 失败（救援系统仍可从 ${RA##*/} 进）"
        fi
    fi
    for e in "$MNT/loader/entries/$MID"-rescue*.conf; do
        [ -f "$e" ] || continue
        rescue_ours "$e" "$SUFFIX" || continue
        if awk -v cmd="$RCMD" -v old="$RESC_OLD_TITLE" -v sl="$SUFFIX" '
               /^options[[:space:]]/ { print "options    " cmd; next }
               /^title[[:space:]]/ { t = $0; sub(/^title[[:space:]]+/, "", t); if (t == old) { print "title      gaokun3 rescue (slot " sl " kernel)"; next } }
               { print }' "$e" > "$e.new" && grep -q "^options " "$e.new"; then
            if cmp -s "$e.new" "$e"; then rm -f "$e.new"
            elif mv -f "$e.new" "$e"; then log "救援条目 ${e##*/}：options 已同步为 boot.img 的 cmdline（按安装器的规则派生）"
            else rm -f "$e.new"; log "⚠️ 救援条目 ${e##*/} 的 options 没同步上（救援系统用旧 cmdline 起，仍可用）"; fi
        else
            rm -f "$e.new"; log "⚠️ 救援条目 ${e##*/} 的 options 没同步上（救援系统用旧 cmdline 起，仍可用）"
        fi
    done
fi

# ── 统一启动入口 gk3boot.efi（tools/gk3boot/README.md）──────────
# 开关 persist.vendor.gaokun3.gk3boot（缺省 off，1.0 发版时再定默认值）：
#   off            删掉 ESP 上全部 gk3boot-android-* / gk3prev-android-* 条目（含 .staged）与 gk3boot-tools.conf
#                  ⇒ 新槽走直连条目。
#                  EFI/gk3boot/<ver>/ 目录留给新槽开机完成时的 boot_control HAL 回收：删目录要 vfat:dir rmdir，
#                  而 postinstall 跑在【旧槽】的策略下（sepolicy/postinstall.te 顶上的设计约束），这里不新增权限。
#   observe/action 从【新】vendor 的 boot/gk3boot/{gk3boot.efi,version[,fastboot.img]} 取入口：
#                  · 先把二进制写到 EFI/gk3boot/<ver>/（已逐字节相同就不写；.new → sync → cmp → rename）；
#                  · 执行端 fastboot.img（这一版带了才有；与 gk3boot.efi 同版本、同目录、一起轮换）同一条规则写到
#                    EFI/gk3boot/<ver>/fastboot.img。它写失败【不挡】入口部署、只记日志：没有执行端时 gk3boot 照常
#                    启动 Android。（vendor 不带它而 ESP 同版本目录里有一份时，这里不删 —— 留给新槽的 HAL 对齐。）
#                  · ESP 上还没有现役入口（0.7.x/1.0-dev → 第一次部署）⇒ 直接写 gk3boot-android-{a,b}+3.conf，
#                    重启就经入口启动（§4.8 第 2 步）；连续 3 次没走到开机完成，systemd-boot 自己改走直连条目；
#                  · 已有现役入口、且就是这一版这个模式 ⇒ 不动；
#                  · 已有别的版本 / 别的模式 ⇒ 只写 gk3boot-android-{a,b}.conf.staged（不以 .conf 结尾，systemd-boot 不读），
#                    由新槽开机完成时的 HAL 激活：旧版（祝福过的）改名 gk3prev、新版 +3（§4.11"一次只换一样"）。
#                    OTA 回滚到旧槽时，旧槽的 HAL 看到 .staged 不是自己那一版，会删掉它。
#                  · 非默认条目 gk3boot-tools.conf（菜单里直接进执行端，设计稿 §4.1、§4.3.5）总是指向【现役】那一版：
#                    只在 action 且这一版的 fastboot.img 已在 ESP 上时写；第一次部署 / ESP 上已是这一版时按这条对齐
#                    （observe、不带执行端、执行端没写上 ⇒ 删掉）；铺 .staged 时【不动】它，等新槽的 HAL 激活时再换。
# 条目正文与 boot_control/Gk3Boot.cpp 的 EntryText / ToolsText 逐字节一致（改一边要改另一边）。
# ★ 这一节的任何失败都【不】让 OTA 失败：直连条目上面已经写好，入口没部署上 = 今天的启动路径。
#   部署到一半失败时撤掉这次写的条目（二进制留着，下次 / HAL 会复用或回收）。
gk3_entry_text() {   # $1=active|prev $2=槽 $3=版本 $4=observe（0|1）
    if [ "$1" = prev ]; then _gt="Android (previous loader)"; _gk=0gk3prev
    elif [ "$4" = 1 ]; then _gt="Android (gk3boot observe)"; _gk=0gk3
    else _gt="Android"; _gk=0gk3; fi
    # 动作模式顺带 gk3.dispatch=1（1.0，与 boot_control/Gk3Boot.cpp 的 EntryText 逐字相同）
    if [ "$4" = 1 ]; then _gd=""; else _gd=" gk3.dispatch=1"; fi
    printf 'title      %s\nversion    gk3boot-%s\nsort-key   %s\nefi        /EFI/gk3boot/%s/gk3boot.efi\noptions    gk3.observe=%s gk3.hint=%s%s\n' \
        "$_gt" "$3" "$_gk" "$3" "$4" "$2" "$_gd"
}
gk3_put() {   # $1=文件 $2…=gk3_entry_text 的参数；写 .new 再改名
    gk3_entry_text "$2" "$3" "$4" "$5" > "$1.new" && mv -f "$1.new" "$1" && return 0
    rm -f "$1.new"; return 1
}
gk3_tools_text() {   # $1=版本；gk3boot-tools.conf 的正文（不带计数、不带 gk3.hint / gk3.observe）
    printf 'title      Android fastboot / boot menu\nversion    gk3boot-%s\nsort-key   0gk3tools\nefi        /EFI/gk3boot/%s/gk3boot.efi\noptions    gk3.action=fastboot\n' \
        "$1" "$1"
}
gk3_tools() {   # 对齐 gk3boot-tools.conf：action 且 _gfb=1 ⇒ 指向 $GV（已一样就不写）；否则删掉。失败只记日志
    _gtf="$GK3_ENT/gk3boot-tools.conf"
    if [ "$GK3_MODE" = action ] && [ "$_gfb" = 1 ]; then
        gk3_tools_text "$GV" > "$_gtf.new" || { rm -f "$_gtf.new"; log "⚠️ 统一启动入口：写 gk3boot-tools.conf 失败（菜单里暂时没有 fastboot 项，入口不受影响）"; return 0; }
        if cmp -s "$_gtf.new" "$_gtf"; then
            rm -f "$_gtf.new"
        elif mv -f "$_gtf.new" "$_gtf"; then
            log "统一启动入口：gk3boot-tools.conf → ${GV}（菜单里的 Android fastboot / boot menu）"
        else
            rm -f "$_gtf.new"; log "⚠️ 统一启动入口：写 gk3boot-tools.conf 失败（菜单里暂时没有 fastboot 项，入口不受影响）"
        fi
    elif [ -f "$_gtf" ]; then
        rm -f "$_gtf" && log "统一启动入口：模式 ${GK3_MODE}、执行端就位 = ${_gfb} ⇒ 删掉 gk3boot-tools.conf"
    fi
    return 0
}
gk3_deploy() {
    GK3_ENT="$MNT/loader/entries"
    case "$GK3_MODE" in
        off)
            _gn=0
            for _ge in "$GK3_ENT"/gk3boot-android-* "$GK3_ENT"/gk3prev-android-* "$GK3_ENT"/gk3boot-tools.conf; do
                [ -f "$_ge" ] || continue
                rm -f "$_ge" && _gn=$((_gn + 1))
            done
            [ "$_gn" = 0 ] || log "统一启动入口：$GK3_PROP=off ⇒ 删掉 $_gn 个入口条目（EFI/gk3boot/ 下的目录由新槽开机完成时回收）"
            return 0 ;;
        observe) _gobs=1 ;;
        action)  _gobs=0 ;;
        *) log "⚠️ 统一启动入口：$GK3_PROP='$GK3_MODE' 不是 off|observe|action，不动 ESP 上的入口"; return 0 ;;
    esac
    if [ ! -f "$GK3_SRC/gk3boot.efi" ] || [ ! -f "$GK3_SRC/version" ]; then
        log "统一启动入口：新 vendor 里没有 boot/gk3boot/（这一版不带入口），不动 ESP 上的入口"
        return 0
    fi
    GV=$(head -n 1 "$GK3_SRC/version" | tr -d '\r')
    case "$GV" in
        ''|*[!A-Za-z0-9._+-]*|log|LOG|.|..) log "⚠️ 统一启动入口：vendor 里的版本串 '$GV' 不合法，不部署"; return 0 ;;
    esac
    [ "${#GV}" -le 64 ] || { log "⚠️ 统一启动入口：版本串太长，不部署"; return 0; }

    # 现役条目（gk3boot-android-<x>[+N[-M]].conf；.staged 不以 .conf 结尾、匹配不到）
    _ghave=0; _gsame=1
    for _ge in "$GK3_ENT"/gk3boot-android-*.conf; do
        [ -f "$_ge" ] || continue
        _ghave=1
        grep -qF "/EFI/gk3boot/$GV/gk3boot.efi" "$_ge" || _gsame=0
        if grep -qE '^options[[:space:]].*gk3\.observe=1([[:space:]]|$)' "$_ge"; then _go=1; else _go=0; fi
        [ "$_go" = "$_gobs" ] || _gsame=0
    done

    _gd="$MNT/EFI/gk3boot/$GV"
    if ! cmp -s "$GK3_SRC/gk3boot.efi" "$_gd/gk3boot.efi" 2>/dev/null; then
        if mkdir -p "$_gd" && cp "$GK3_SRC/gk3boot.efi" "$_gd/gk3boot.efi.new" && sync &&
           cmp -s "$GK3_SRC/gk3boot.efi" "$_gd/gk3boot.efi.new" && mv -f "$_gd/gk3boot.efi.new" "$_gd/gk3boot.efi"; then
            log "统一启动入口：写好 EFI/gk3boot/$GV/gk3boot.efi（$(stat -c%s "$_gd/gk3boot.efi") 字节，读回一致）"
        else
            rm -f "$_gd/gk3boot.efi.new"
            log "⚠️ 统一启动入口：写 EFI/gk3boot/$GV/gk3boot.efi 失败（ESP 满了？）—— 这次不部署，直连条目照常可用"
            return 0
        fi
    fi

    # 执行端 fastboot.img：同一条规则；失败只记日志、不 return（没有执行端时 gk3boot 照常启动 Android）。
    # _gfb=1 = ESP 上 <ver>/fastboot.img 与 vendor 逐字节相同 —— gk3boot-tools.conf 只在这时才写。
    _gfb=0
    if [ ! -f "$GK3_SRC/fastboot.img" ]; then
        log "统一启动入口：这一版 vendor 不带执行端 fastboot.img（入口照常部署，菜单里没有 fastboot 项）"
    elif cmp -s "$GK3_SRC/fastboot.img" "$_gd/fastboot.img" 2>/dev/null; then
        _gfb=1
    elif cp "$GK3_SRC/fastboot.img" "$_gd/fastboot.img.new" && sync &&
         cmp -s "$GK3_SRC/fastboot.img" "$_gd/fastboot.img.new" && mv -f "$_gd/fastboot.img.new" "$_gd/fastboot.img"; then
        _gfb=1
        log "统一启动入口：写好 EFI/gk3boot/$GV/fastboot.img（$(stat -c%s "$_gd/fastboot.img") 字节，读回一致）"
    else
        rm -f "$_gd/fastboot.img.new"
        log "⚠️ 统一启动入口：写 EFI/gk3boot/$GV/fastboot.img 失败（ESP 满了？）—— 入口照常部署，只是没有执行端"
    fi

    if [ "$_ghave" = 0 ]; then
        for _gx in a b; do
            if ! gk3_put "$GK3_ENT/gk3boot-android-$_gx+3.conf" active "$_gx" "$GV" "$_gobs"; then
                rm -f "$GK3_ENT/gk3boot-android-a+3.conf" "$GK3_ENT/gk3boot-android-b+3.conf"
                log "⚠️ 统一启动入口：写条目失败，撤掉这次写的条目 —— 直连条目照常可用"
                return 0
            fi
        done
        rm -f "$GK3_ENT"/gk3boot-android-*.conf.staged
        log "统一启动入口：第一次部署 ${GV}（${GK3_MODE}）：gk3boot-android-{a,b}+3.conf —— 重启后经入口启动；连续 3 次没开机完成会自动改走直连条目"
        gk3_tools
    elif [ "$_gsame" = 1 ]; then
        rm -f "$GK3_ENT"/gk3boot-android-*.conf.staged
        log "统一启动入口：ESP 上已是 ${GV}（${GK3_MODE}），不动"
        gk3_tools   # 现役就是这一版：tools 按这一版对齐不会碰到别的版本（例如上次执行端没写上、这次补上）
    else
        # gk3boot-tools.conf 不动：它跟着现役走，现役要等新槽的 HAL 激活 .staged 时才换

        for _gx in a b; do
            if ! gk3_put "$GK3_ENT/gk3boot-android-$_gx.conf.staged" active "$_gx" "$GV" "$_gobs"; then
                rm -f "$GK3_ENT"/gk3boot-android-*.conf.staged
                log "⚠️ 统一启动入口：写 .staged 失败 —— 新槽开机完成时 HAL 仍会从它自己的 vendor 部署这一版"
                return 0
            fi
        done
        log "统一启动入口：已有别的入口，新版 ${GV}（${GK3_MODE}）只铺目录 + gk3boot-android-{a,b}.conf.staged；新槽开机完成时由 boot_control HAL 激活（旧版留作 gk3prev）"
    fi
    return 0
}
gk3_deploy

# ── loader.conf 的菜单等待（tools/gk3boot/README.md）──────────
# 开机菜单（音量上/下与电源键即可无键盘操作）统一显示 5 秒。新装时安装器直接写 5
# （scripts/live/installer-lib.sh 的 GK3_LOADER_TIMEOUT）；之前装的机器上是旧安装器写的 15 —— 只改【正好是 timeout 15】
# 那一行，用户改过的（别的数、menu-hidden、多了空格）不动。
# 改 loader.conf 只进 PCR5，不会让 BitLocker 要密钥。写法同条目：.new → 核对 → 改名；失败只记日志、不让 OTA 失败。
# 权限：只用到 vfat:file create/write/rename（sepolicy/postinstall.te 已有），不新增规则。
LC="$MNT/loader/loader.conf"
if [ -f "$LC" ] && grep -qx 'timeout 15' "$LC"; then
    if sed 's/^timeout 15$/timeout 5/' "$LC" > "$LC.new" && grep -qx 'timeout 5' "$LC.new" &&
       [ "$(grep -vx 'timeout 5' "$LC.new")" = "$(grep -vx 'timeout 15' "$LC")" ] && mv -f "$LC.new" "$LC"; then
        log "loader.conf 的 timeout 从安装器原来写的 15 改成 5"
    else
        rm -f "$LC.new"; log "⚠️ 没能把 loader.conf 的 timeout 改成 5（菜单仍等 15 秒，不影响启动）"
    fi
fi

# ── recovery ────────────────────────────────────────────────────────────────
# ★ recovery 与系统【共用同一个内核和 dtb】（实测 recovery.img 里的 kernel 与
#   boot.img 里的 sha256 完全相同），所以条目直接复用该槽刚解出来的
#   Image 与 gaokun3.dtb，ESP 上只多一个 ramdisk。
REC_SRC="$HERE/../boot/recovery-ramdisk.img"
DST_ENT="$MNT/loader/entries/$MID-recovery-$SUFFIX.conf"
if [ "$REC_ON" = 0 ]; then
    # OTA-8：启动项没开就不铺 ramdisk（旧文件与本槽的旧条目在空间检查之前已处理）。
    log "recovery 按默认跳过（persist.vendor.gaokun3.recovery_entry 不是 1；未验证，会复位循环）"
elif [ -f "$REC_SRC" ]; then
    log "铺设 recovery ramdisk"
    if cp "$REC_SRC" "$DEST/.recovery-ramdisk.new" &&
       mv -f "$DEST/.recovery-ramdisk.new" "$DEST/recovery-ramdisk.img"; then
        # ★ 条目【从该槽的 android 条目派生】，只替换 initrd/title/version/sort-key。
        #   这样 cmdline（含 slot_suffix）永远与主条目一致，不会漂 —— 本仓已被
        #   BOARD_KERNEL_CMDLINE 与 BLS 条目漂移各教育过一次。
        #   recovery 不需要特殊 cmdline：实测它内嵌的 cmdline 与 boot 的完全相同，
        #   是 ramdisk 决定它是 recovery。
        SRC_ENT="$ENT"
        # ⚠️★ 默认【不】创建 recovery 启动项 —— 2026-08-20 实测这个 ramdisk 在本机
        #   会进复位循环（Android 一次都没进，启动原因历史里没有新条目），
        #   而且不留 panic 记录（本机 init 的服务级失败是主动 reboot() 而不是
        #   panic，所以 init_fatal_panic + efi_pstore 抓不到）。
        #   条目一旦存在，用户在 15 秒菜单里误选一次就要跑到机器旁按电源键 ——
        #   在验证通过之前不能把这个坑发出去。
        #   要调试就设 persist.vendor.gaokun3.recovery_entry=1 再触发一次 OTA/部署。
        #   （REC_ON=1 才会走到这里，OTA-8。）
        if [ -f "$SRC_ENT" ]; then
            sed -e "s|^initrd .*|initrd     /$MID/android/slot_$SUFFIX/recovery-ramdisk.img|" \
                -e "s|^title .*|title      Recovery (gaokun3) — slot _$SUFFIX|" \
                -e "s|^version .*|version    gaokun3-recovery-$SUFFIX|" \
                -e "s|^sort-key .*|sort-key   zzrecovery$SUFFIX|" \
                "$SRC_ENT" > "$DST_ENT" &&
                log "recovery 条目已写: $MID-recovery-$SUFFIX.conf"
        else
            log "警告: 找不到 ${SRC_ENT}，跳过 recovery 条目"
        fi
    else
        log "警告: recovery ramdisk 写入失败，recovery 条目不会更新"
    fi
else
    log "vendor 里没有 recovery-ramdisk.img，跳过 recovery（旧 vendor 会这样）"
fi

sync
umount "$MNT" || log "警告: umount 失败（数据已 sync）"
log "完成：_$SUFFIX 槽的内核已就位"
exit 0
