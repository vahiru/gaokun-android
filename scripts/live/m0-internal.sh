#!/usr/bin/env bash
# M0 不用 U 盘：把 live 系统放到内置盘上【旧救援 Ubuntu 那个分区】（p3），ESP 上加【非默认】的启动项，
# 用一次性启动（LoaderEntryOneShot）进去。下一次重启自动回到 Android 的默认槽。
#
#   bash scripts/live/m0-internal.sh check      # 只读：找分区、量空间、核内核配置、看凭据在不在
#   bash scripts/live/m0-internal.sh prepare    # 放文件 + 写启动项（★ 不重启、不改 default）
#   bash scripts/boot-oneshot.sh gaokun3-live.conf && adb -s "$SER" reboot  ← ⚠️ 要用户同意
#   bash scripts/live/m0-internal.sh logs       # 回到 Android 之后：取回 p3:/gaokun3/diag/ → out/m0/diag/
#   bash scripts/live/m0-internal.sh payload <发布目录>   # 安装载荷 → p3:/gaokun3/payload/（live 里就是"U 盘里的镜像"）
#   bash scripts/live/m0-internal.sh remove     # 撤掉启动项、initramfs、live.squashfs 与 payload/
#
#   SER=192.168.10.239:5555 bash scripts/live/m0-internal.sh check      # 走 TCP adb 时
#
# 先例：Alpine 版 M0 就是这么做的 —— squashfs 放 p3、ESP 上一个非默认的 rescue-alpine.conf、
#   一条 LoaderEntryOneShot 进去（见 scripts/live/README.md）。
# 这条路比 U 盘还好的一点：p3 上有 WiFi 配置的话，live 系统一起来就连上网，放进去的公钥让
#   开发机能 ssh 进去量浸泡数据 —— 人只需要看屏幕、摸触摸。
# 为什么装不坏内置盘：live 系统是从 nvme0n1 上的分区启动的，gk3_probe 把整块内置盘标成
#   medium=yes，界面上禁用它；gk3_apply 的安全闸 1 也拒绝整盘清空介质所在的盘。
#
# 设备现在没有回落槽（_a 不可启动）—— 但这里【不碰】任何 Android 分区、不改 default：
#    live 起不来时 initramfs 60 秒后自己重启（或者长按电源键），回到 default 的 _b。
# 挂载点用私有的名字（/mnt/gaokun3_m0_*），不叫 /mnt/esp。
set -uo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SER=${SER:-gaokun3}
LIVE=$REPO/out/live
M0=$REPO/out/m0
ESPM=/mnt/gaokun3_m0_esp
P3M=/mnt/gaokun3_m0_p3
SQ_NAME=live.squashfs
# 默认上机的是正式构建的那份；GK3_M0_SQUASHFS=out/live/gaokun3-live-patched.squashfs 用不联网换了安装器的那份
# （scripts/live/patch-live-installer.sh）。设备上的文件名不变，启动项不用改
LIVE_SQ=${GK3_M0_SQUASHFS:-$LIVE/gaokun3-live.squashfs}
FW_MEDIA=/media/gk3/gaokun3/firmware        # live 里看到的路径；p3 上是 /gaokun3/firmware
# GPU 要的三个（zap shader 的名字取自 dtb：/proc/device-tree/soc@0/gpu@3d00000/zap-shader/firmware-name）
GPU_FW="qcom/a660_sqe.fw qcom/a660_gmu.bin qcom/sc8280xp/HUAWEI/gaokun3/qcdxkmsuc8280.mbn"
S() { adb -s "$SER" shell "$@" | tr -d '\r'; }
die() { echo "✗ $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }
ok()  { echo "   ✓ $*"; }
warn(){ echo "   ⚠️ $*"; }

S true >/dev/null 2>&1 || die "adb 连不上 ${SER}（走 TCP：SER=192.168.10.239:5555；找设备：bash scripts/find-device.sh）"
[ "$(S getprop ro.crdroid.device)" = gaokun3 ] || die "$SER 不是 gaokun3（局域网里那台小米手机也开着 5555）"
# 要 adb shell 本身就是 root：下面的命令都是多行脚本直接交给 S，不经 su -c 转发（套进 su -c '…' 要把里面的引号全改写）。
# 发布构建（ro.debuggable=0）上 adb root 走不通。开发机上 adb shell 经 KSU 直接就是 root；
#   不是的话在 ReSukiSU（KSU）管理器里给 Shell 授 root。
if [ "$(S id -u)" != 0 ]; then
    if [ "$(S getprop ro.debuggable)" = 1 ]; then
        die "adb shell 不是 root：开发构建上 adb -s $SER root（本机老办法：先 adb -s $SER shell setprop service.adb.root 1）"
    elif [ "$(S "su -c 'id -u'" 2>/dev/null)" = 0 ]; then
        die "adb shell 不是 root（发布构建没有 adb root），su -c 倒是能用 —— 本脚本不经 su 转发：在 KSU 管理器里给 Shell（com.android.shell）授 root，让 adb shell 本身就是 root，再重跑"
    else
        die "adb shell 不是 root，su 也不可用：发布构建（ro.debuggable=0）没有 adb root —— 装 ReSukiSU 管理器、给 Shell（com.android.shell）授 root 后重跑"
    fi
fi

umount_all() { S "umount $ESPM 2>/dev/null; umount $P3M 2>/dev/null; rmdir $ESPM $P3M 2>/dev/null; true" >/dev/null; }
trap umount_all EXIT

# 找放 live 的分区：按【内容】认 —— 上面有 /gaokun3/live.squashfs（或 Alpine M0 留下的 rescue.squashfs）的那个。
# 不写死分区号：出厂分区表之后又加过 boot_a/boot_b。
# 只看【没挂载】的 ext4 分区，只读挂载。GK3_M0_PART=/dev/block/nvme0n1pN 可以直接指定。
find_part() {
    if [ -n "${GK3_M0_PART:-}" ]; then echo "$GK3_M0_PART"; return; fi
    S "mkdir -p $P3M
       for d in /dev/block/nvme0n1p*; do
         grep -q \"^\$d \" /proc/mounts && continue
         n=\$(readlink -f \$d); grep -q \"^\$n \" /proc/mounts && continue
         blkid \$d 2>/dev/null | grep -q 'TYPE=\"ext4\"' || continue
         mount -t ext4 -o ro \$d $P3M 2>/dev/null || continue
         { [ -f $P3M/gaokun3/live.squashfs ] || [ -f $P3M/gaokun3/rescue.squashfs ]; } && { umount $P3M; echo \$d; break; }
         umount $P3M
       done; rmdir $P3M 2>/dev/null; true"
}

esp_info() {   # 挂 ESP（只读），打印 MID 与 slot_b 的内核、options、空闲
    S "mkdir -p $ESPM; mount -t vfat -o ro /dev/block/by-name/esp $ESPM 2>/dev/null || exit 1
       MID=\$(ls $ESPM | grep -E '^[0-9a-f]{32}\$' | head -1); echo MID=\$MID
       echo FREE_KB=\$(df -k $ESPM | tail -1 | awk '{print \$4}')
       echo DEFAULT=\$(sed -n 's/^default *//p' $ESPM/loader/loader.conf)
       for f in Image gaokun3.dtb; do [ -f $ESPM/\$MID/android/slot_b/\$f ] && echo HAVE_\$(echo \$f | tr -c 'A-Za-z0-9\n' _)=yes; done
       n=\$(ls $ESPM/loader/entries/ 2>/dev/null | grep -E '^[0-9a-f]{32}-android-b\.conf\$' | head -1); e=$ESPM/loader/entries/\$n; echo ENTRY_B=\$n
       echo OPTS_B=\$(sed -n 's/^options *//p' \"\$e\")
       echo ENTRIES=\$(ls $ESPM/loader/entries/ | tr '\n' ' ')
       umount $ESPM"
}

cmd=${1:-check}
case "$cmd" in
check|prepare)
    say "1. 放 live 的分区"
    PART=$(find_part | tail -1)
    [ -n "$PART" ] || die "没找到带 /gaokun3/live.squashfs 或 rescue.squashfs 的 ext4 分区。指定：GK3_M0_PART=/dev/block/nvme0n1pN"
    KNODE=$(S "readlink -f $PART"); KNAME=${KNODE##*/}
    ok "${PART}（内核节点 /dev/${KNAME}）"
    S "mkdir -p $P3M; mount -t ext4 -o ro $PART $P3M && { df -h $P3M | tail -1; ls -la $P3M/gaokun3/; umount $P3M; }" | sed 's/^/     /'
    P3_FREE_KB=$(S "mkdir -p $P3M; mount -t ext4 -o ro $PART $P3M && df -k $P3M | tail -1 | awk '{print \$4}'; umount $P3M" | tail -1)
    HAVE_WIFI=$(S "mkdir -p $P3M; mount -t ext4 -o ro $PART $P3M && { [ -f $P3M/gaokun3/wpa_supplicant.conf ] && echo yes || echo no; }; umount $P3M" | tail -1)
    [ "$HAVE_WIFI" = yes ] && ok "p3 上有 gaokun3/wpa_supplicant.conf —— live 起来会自己连 WiFi，开发机能 ssh 进去" \
        || warn "p3 上没有 WiFi 配置 —— live 起来连不上网，M0 只能看屏幕（数据回到 Android 后用 logs 取）"

    say "2. ESP"
    INFO=$(esp_info) || die "挂不上 ESP（/dev/block/by-name/esp）"
    printf '%s\n' "$INFO" | grep -v '^OPTS_B=' | sed 's/^/     /'
    eval "$(printf '%s\n' "$INFO" | grep -E '^(MID|FREE_KB|HAVE_Image|HAVE_gaokun3_dtb)=')"
    OPTS_B=$(printf '%s\n' "$INFO" | sed -n 's/^OPTS_B=//p')
    [ -n "${MID:-}" ] || die "ESP 上找不到 machine-id 目录"
    [ "${HAVE_Image:-}" = yes ] && [ "${HAVE_gaokun3_dtb:-}" = yes ] || die "ESP 上 $MID/android/slot_b/ 缺内核或 dtb"
    [ -n "$OPTS_B" ] || die "读不到 slot_b 启动项的 options"
    INIT_KB=$(( $(wc -c < "$LIVE/initramfs.img" 2>/dev/null || echo 0) / 1024 + 1 ))
    [ "${FREE_KB:-0}" -gt $(( INIT_KB + 512 )) ] && ok "ESP 空闲 ${FREE_KB} KiB，initramfs 要 ${INIT_KB} KiB" \
        || die "ESP 只剩 ${FREE_KB:-?} KiB，放不下 ${INIT_KB} KiB 的 initramfs"

    say "3. 内核：slot_b 的内核能不能跑 systemd（抽它自己的 .config）"
    mkdir -p "$M0"
    S "mkdir -p $ESPM; mount -t vfat -o ro /dev/block/by-name/esp $ESPM && cp $ESPM/$MID/android/slot_b/Image /data/local/tmp/gk3-m0-Image; umount $ESPM" >/dev/null
    adb -s "$SER" pull /data/local/tmp/gk3-m0-Image "$M0/slot_b-Image" >/dev/null && S "rm -f /data/local/tmp/gk3-m0-Image" >/dev/null
    if python3 "$REPO/scripts/extract-kconfig.py" "$M0/slot_b-Image" > "$M0/slot_b.config" 2>/dev/null; then
        miss=""
        for o in DEVTMPFS CGROUPS INOTIFY_USER SIGNALFD TIMERFD EPOLL UNIX SYSFS PROC_FS FHANDLE NET_NS USER_NS SQUASHFS OVERLAY_FS BLK_DEV_LOOP EXT4_FS; do
            grep -q "^CONFIG_$o=y" "$M0/slot_b.config" || miss="$miss $o"
        done
        [ -z "$miss" ] && ok "systemd 的硬性要求与 squashfs/overlay/loop/ext4 都是 =y（$(grep -m1 '^# Linux' "$M0/slot_b.config" | cut -c3-)）" \
            || die "slot_b 的内核缺：$miss"
    else warn "抽不出 slot_b 内核的配置（没开 IKCONFIG？）—— 按 v0.6.2 的审计结果继续"; fi

    say "4. 本地产物"
    for f in gaokun3-live.squashfs initramfs.img; do [ -f "$LIVE/$f" ] || die "没有 $LIVE/${f}（先跑 scripts/live/build-live.sh）"; done
    SQ_KB=$(( $(wc -c < "$LIVE_SQ") / 1024 ))
    ok "squashfs $((SQ_KB / 1024)) MiB，initramfs ${INIT_KB} KiB"
    [ "${P3_FREE_KB:-0}" -gt $(( SQ_KB + 65536 )) ] && ok "p3 空闲 $((P3_FREE_KB / 1024)) MiB，够" || die "p3 空间不够放 squashfs"

    # 启动项的内核参数：从 slot_b 启动项派生（它由 OTA postinstall 从 boot.img 同步），过滤规则与
    # 装机时的救援条目是同一个函数；再把 squashfs 指到 p3，并【只】让 initramfs 去碰这一个分区
    # （ext4 即使只读挂载也可能回放日志，扫描所有分区不如直接指定）
    . "$REPO/scripts/live/installer-lib.sh"
    BASE=$(gk3__rescue_cmdline "$OPTS_B" | sed "s#gk3.squash=[^ ]*#gk3.squash=/gaokun3/$SQ_NAME gk3.dev=/dev/$KNAME#")
    # ⚠️ M0 第一轮（2026-09-25 14:56）实测出的两处绕行 —— 镜像里的正式修法要重建，而这台 Mac 换网后连不上
    #    deb.debian.org；这两条在镜像修好之后是冗余但无害的：
    #  * firmware_class.path：镜像里【没有 GPU 固件】（a660_sqe.fw 加载 -2 → freedreno 建不了 pipe →
    #    cage 的 EGL 起不来）。C 版画 dumb buffer 从不碰 GPU，所以以前从没缺过。第 6 步把本机 Android
    #    正在用的那三个拷到 p3。GPU 是 cage【第一次打开】时才加载，那时 p3 早已挂在 /media/gk3。
    #  * net.ifnames=0：Debian 的 systemd-udevd 把 wlan0 改名成 wlP6p1s0，gk3-wifi 只认 wlan0。
    # ★ 2026-09-25 16:40 之后构建的镜像两处都已正式修好（GPU 固件进了镜像、99-default.link 屏蔽了改名），
    #   默认【不】再绕行 —— 验的就是镜像本身。拿旧镜像上机时 GK3_M0_WORKAROUNDS=1 打开。
    [ "${GK3_M0_WORKAROUNDS:-0}" = 1 ] && BASE="$BASE firmware_class.path=$FW_MEDIA net.ifnames=0"
    say "5. 启动项（非默认；标题 ASCII —— 开机菜单由固件字体画）"
    echo "     options  $BASE"
    # 文件名 | 标题 | sort-key | 附加参数。常驻的只有安装器这一个 —— 与 Windows 那条路装上的同名同标题
    # （scripts/windows/build-bundle.sh），装坏了从开机菜单进它再装一次。
    # M0 的四个变体（渲染后端 / 浸泡 / 反向旋转）验收完就撤了（用户 2026-09-27"启动项清理下"），要时 GK3_M0_VARIANTS=1
    ENTRIES=("gaokun3-live.conf|gaokun3 installer|gk3live|")
    [ "${GK3_M0_VARIANTS:-0}" = 1 ] && ENTRIES+=(
        "gaokun3-m0-skia.conf|gaokun3 M0: Skia (Impeller off)|zzm01|gk3.renderer=skia"
        "gaokun3-m0-soak.conf|gaokun3 M0: soak test, Impeller|zzm02|gk3.soak=1"
        "gaokun3-m0-soak-skia.conf|gaokun3 M0: soak test, Skia|zzm03|gk3.soak=1 gk3.renderer=skia"
        "gaokun3-m0-rot90.conf|gaokun3 M0: rotate the other way|zzm04|gk3.rotate=90"
    )
    for e in "${ENTRIES[@]}"; do
        f=${e%%|*}
        # ⚠️★ 文件名【不能】匹配 boot_control HAL 改写 default 用的 *-android-a.conf / *-android-b.conf
        #    （device/huawei/gaokun3/boot_control/EspSlot.cpp:42 写 default 的通配、:60 认 ESP 的通配）
        case "$f" in *-android-a.conf|*-android-b.conf) die "条目名 $f 会被 HAL 当成 Android 槽" ;; esac
        echo "     $f"
    done
    [ "$cmd" = check ] && { echo; echo "只读检查完毕。准备（不重启）：bash scripts/live/m0-internal.sh prepare"; exit 0; }

    say "6. 放文件（p3 读写挂载）"
    S "mkdir -p $P3M; mount -t ext4 $PART $P3M && mkdir -p $P3M/gaokun3" >/dev/null || die "p3 挂不成读写"
    adb -s "$SER" push "$LIVE_SQ" "$P3M/gaokun3/$SQ_NAME" >/dev/null || die "推 squashfs 失败"
    # 判据看产物：字节数 + sha256（scp 曾经退出码 0 而文件只有 77%）
    want=$(shasum -a 256 "$LIVE_SQ" | cut -d' ' -f1)
    got=$(S "sha256sum $P3M/gaokun3/$SQ_NAME" | cut -d' ' -f1)
    [ "$want" = "$got" ] && ok "live.squashfs sha256 一致（${want:0:16}…）" || die "live.squashfs 的 sha256 不对：$got"
    # GPU 固件（只在 GK3_M0_WORKAROUNDS=1 时有用）：从本机 Android 的 /vendor/firmware 原样拷（Android 上 freedreno/turnip 跑的就是这三个），设备上逐个比 sha256
    # ⚠️ 写成 if，别写成 [ … ] && … || die：开关关着时 [ ] 返回 1，会一路走到 die
    if [ "${GK3_M0_WORKAROUNDS:-0}" = 1 ]; then
        S "for f in $GPU_FW; do mkdir -p \$(dirname $P3M/gaokun3/firmware/\$f) && cp /vendor/firmware/\$f $P3M/gaokun3/firmware/\$f || exit 1
             [ \"\$(sha256sum < /vendor/firmware/\$f)\" = \"\$(sha256sum < $P3M/gaokun3/firmware/\$f)\" ] || exit 1; done" >/dev/null \
            && ok "GPU 固件 3 个 → p3:/gaokun3/firmware/（取自 /vendor/firmware，sha256 一致）" || die "拷 GPU 固件失败"
    fi
    # 公钥：开发机专用的一对（out/m0/，不动 ~/.ssh）；p3 上已有 authorized_keys 就【追加】不覆盖
    [ -f "$M0/ssh_ed25519" ] || ssh-keygen -q -t ed25519 -N '' -C "gaokun3-m0@$(hostname -s)" -f "$M0/ssh_ed25519"
    adb -s "$SER" push "$M0/ssh_ed25519.pub" /data/local/tmp/gk3-m0.pub >/dev/null
    # ⓘ p3 的根目录属 uid 1001、gaokun3/ 是 777 —— sshd 的 StrictModes 本来会因此拒绝这把钥匙。
    #   不在这里改 p3 的权限：镜像里的 gk3-ssh-keys 开机时把它并进 /root/.ssh，绕开了介质的属主问题。
    #   ⚠️ 那是 2026-09-25 下午才加的，这之前构建的 squashfs 里没有它（那时介质上的公钥登不进去）。
    S "f=$P3M/gaokun3/authorized_keys; touch \$f; grep -qF \"\$(cat /data/local/tmp/gk3-m0.pub)\" \$f || cat /data/local/tmp/gk3-m0.pub >> \$f
       chmod 600 \$f; chown 0:0 \$f; rm -f /data/local/tmp/gk3-m0.pub; sync; umount $P3M" >/dev/null
    ok "公钥 out/m0/ssh_ed25519.pub → p3:/gaokun3/authorized_keys（ssh -i out/m0/ssh_ed25519 root@<ip>）"

    say "7. ESP：initramfs + 启动项"
    S "mkdir -p $ESPM; mount -t vfat /dev/block/by-name/esp $ESPM && mkdir -p $ESPM/$MID/live" >/dev/null || die "ESP 挂不成读写"
    adb -s "$SER" push "$LIVE/initramfs.img" "$ESPM/$MID/live/initramfs.img" >/dev/null || die "推 initramfs 失败"
    want=$(shasum -a 256 "$LIVE/initramfs.img" | cut -d' ' -f1)
    [ "$(S "sha256sum $ESPM/$MID/live/initramfs.img" | cut -d' ' -f1)" = "$want" ] && ok "initramfs sha256 一致" || die "initramfs 的 sha256 不对"
    TMP=$(mktemp); i=0
    for e in "${ENTRIES[@]}"; do
        IFS='|' read -r f title skey extra <<< "$e"
        cat > "$TMP" <<EOF
title      $title
version    ${f%.conf}
sort-key   $skey
linux      /$MID/android/slot_b/Image
devicetree /$MID/android/slot_b/gaokun3.dtb
initrd     /$MID/live/initramfs.img
options    $BASE${extra:+ $extra}
EOF
        adb -s "$SER" push "$TMP" "$ESPM/loader/entries/$f" >/dev/null || die "写 $f 失败"
        i=$((i + 1))
    done
    rm -f "$TMP"
    S "sync; umount $ESPM" >/dev/null
    DEF=$(esp_info | sed -n 's/^DEFAULT=//p')
    ok "写了 $i 个启动项；default 仍是 ${DEF}（没动）"
    cat <<EOF

准备完毕，没有重启。进 M0（⚠️ 要用户在场并同意）：
  bash scripts/boot-oneshot.sh gaokun3-live.conf        # GK3_M0_VARIANTS=1 时还有 gaokun3-m0-soak.conf 等
  adb -s $SER reboot
起不来：initramfs 60 秒后自己重启，或长按电源键 → 回到 default（${DEF}）。
回到 Android 后取结果：bash scripts/live/m0-internal.sh logs
EOF
    ;;
payload)
    # M4b（2026-09-26）：在内置盘上的 live 里"重新安装"，载荷放在介质（p3）上 —— live 把 p3 挂在 /media/gk3，
    # gk3_release_info 找的正是 /media/gk3/gaokun3/payload（build-live.sh --payload 在 U 盘上放的同一个位置）。
    # 发布目录 = release.sh 的安装产物：boot.img · super.img.zst · install-artifacts.sha256
    REL=${2:?用法：$0 payload <发布目录>}
    FILES="boot.img super.img.zst install-artifacts.sha256"
    for f in $FILES; do [ -f "$REL/$f" ] || die "$REL 里没有 $f"; done
    # 推之前先在本机核一遍：清单里的 OTA zip 本机没有，只核装机要用的两个
    ( cd "$REL" && for f in boot.img super.img.zst; do
        want=$(awk -v f="$f" '{sub(/^\*/, "", $2)} $2 == f {print $1}' install-artifacts.sha256)
        [ -n "$want" ] && [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" = "$want" ] || { echo "✗ 本机的 $f 与清单不符" >&2; exit 1; }
      done ) || exit 1
    ok "本机：boot.img / super.img.zst 与 install-artifacts.sha256 一致"
    PART=$(find_part | tail -1); [ -n "$PART" ] || die "没找到放 live 的分区"
    NEED_KB=$(( $(cat "$REL"/boot.img "$REL"/super.img.zst | wc -c) / 1024 + 65536 ))
    S "mkdir -p $P3M; mount -t ext4 $PART $P3M && mkdir -p $P3M/gaokun3/payload" >/dev/null || die "p3 挂不成读写"
    FREE_KB=$(S "df -k $P3M | tail -1 | awk '{print \$4}'" | tail -1)
    # 已经有的同名文件不算占用（会被覆盖）
    HAVE_KB=$(S "du -sk $P3M/gaokun3/payload | cut -f1" | tail -1)
    [ $(( ${FREE_KB:-0} + ${HAVE_KB:-0} )) -gt "$NEED_KB" ] || die "p3 空闲 ${FREE_KB:-?} KiB，放不下（要 ${NEED_KB} KiB）"
    for f in $FILES; do
        want=$(shasum -a 256 "$REL/$f" | cut -d' ' -f1)
        if [ "$(S "sha256sum $P3M/gaokun3/payload/$f 2>/dev/null" | cut -d' ' -f1)" = "$want" ]; then
            ok "$f 已经在 p3 上且 sha256 一致，跳过"; continue
        fi
        adb -s "$SER" push "$REL/$f" "$P3M/gaokun3/payload/$f" >/dev/null || die "推 $f 失败"
        # 判据看产物
        [ "$(S "sha256sum $P3M/gaokun3/payload/$f" | cut -d' ' -f1)" = "$want" ] && ok "$f → p3:/gaokun3/payload/（sha256 一致）" \
            || die "$f 推上去的 sha256 不对"
    done
    S "sync; umount $P3M" >/dev/null
    ver=$(awk '{sub(/^\*/, "", $2)} $2 ~ /\.zip$/ {sub(/\.zip$/, "", $2); print $2; exit}' "$REL/install-artifacts.sha256")
    ok "载荷就位：${ver:-?}。live 里选「使用 U 盘里的镜像」就是它"
    ;;
logs)
    PART=$(find_part | tail -1); [ -n "$PART" ] || die "没找到放 live 的分区"
    mkdir -p "$M0/diag"
    S "mkdir -p $P3M; mount -t ext4 -o ro $PART $P3M && tar -C $P3M/gaokun3 -cf /data/local/tmp/gk3-m0-diag.tar diag 2>/dev/null; umount $P3M" >/dev/null
    adb -s "$SER" pull /data/local/tmp/gk3-m0-diag.tar "$M0/diag.tar" >/dev/null || die "p3 上没有 gaokun3/diag/（live 还没跑过？）"
    S "rm -f /data/local/tmp/gk3-m0-diag.tar" >/dev/null
    tar -C "$M0" -xf "$M0/diag.tar" && rm -f "$M0/diag.tar"
    ok "→ $M0/diag/"; ls -la "$M0/diag/"
    ;;
remove)
    INFO=$(esp_info) || die "挂不上 ESP"; eval "$(printf '%s\n' "$INFO" | grep '^MID=')"
    S "mkdir -p $ESPM; mount -t vfat /dev/block/by-name/esp $ESPM && rm -f $ESPM/loader/entries/gaokun3-m0*.conf $ESPM/loader/entries/gaokun3-live.conf && rm -rf $ESPM/$MID/live; sync; umount $ESPM" >/dev/null
    ok "ESP：删了 gaokun3-live.conf、gaokun3-m0*.conf 与 $MID/live/"
    PART=$(find_part | tail -1)
    [ -n "$PART" ] && S "mkdir -p $P3M; mount -t ext4 $PART $P3M && rm -f $P3M/gaokun3/$SQ_NAME; sync; umount $P3M" >/dev/null \
        && S "mkdir -p $P3M; mount -t ext4 $PART $P3M && rm -rf $P3M/gaokun3/firmware $P3M/gaokun3/payload $P3M/gaokun3/variants.txt; sync; umount $P3M" >/dev/null \
        && ok "p3：删了 gaokun3/${SQ_NAME}、firmware/、payload/ 与 variants.txt（diag/ 与 authorized_keys 留着）"
    ;;
*) die "用法：$0 check|prepare|payload <发布目录>|logs|remove" ;;
esac
