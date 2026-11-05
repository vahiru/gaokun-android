#!/usr/bin/env bash
# ============================================================================
# data-snapshot-live.sh — 在内置 live（gaokun3 installer，Debian）里给 /data 做【离线】快照 / 回滚。
#
#   bash scripts/data-snapshot-live.sh <live 的 IP> status
#   bash scripts/data-snapshot-live.sh <live 的 IP> backup  [标签]     # 默认标签 a16
#   bash scripts/data-snapshot-live.sh <live 的 IP> restore <快照名> --yes
#
# ★ 10-26 起首选在 Android 里备份（scripts/data-snapshot-android.sh，不用重启、不靠 live 连 Wi-Fi），
#   本脚本主要负责 restore。restore 的 tar 带 --selinux：toybox 把标签存成 PAX 的 RHT.security.selinux，
#   GNU tar 只有 --selinux 才认（--xattrs 认的是 SCHILY.xattr.*，本脚本自己 backup 的归档用那个）。
#
# 为什么要有它（2026-10-26，A17 首次上机前）：
#   · 大版本升级会把 /data 上的东西升上去（包管理、数据库、ART、apex 数据）。A17 起不来时回落到 A16 槽，
#     A16 能不能在被 A17 动过的 /data 上开机没验证过；/data 又没有 checkpoint（fstab 没写 checkpoint=）。
#   · /data 是不加密的 ext4（fstab.gaokun3 没有 fileencryption=）⇒ 文件级快照就是完整的，不涉及密钥。
#   · 必须离线做：Android 跑着时数据库正在写，而且设备上的 toybox / busybox tar 都不保留 xattr ——
#     丢了 security.selinux，App 数据的 MLS 类别（per-app categories）恢复不回来。live 里是 GNU tar。
#   · 快照放在 userdata 自己的空余空间里（用了 83 GB、剩 287 GB），不经网络传。
#
# 范围：
#   · userdata：除 media/（用户文件，跨版本不受影响，42 GB）、ota_package/（OTA 载荷）、lost+found、快照目录本身之外的全部，
#     tar --xattrs --acls --numeric-owner，zstd 压缩。
#   · metadata（p10，ext4，1 MB）：整个文件系统 tar 一份。
#   · misc（p4，BCB，2014 扇区）：dd 原样一份。
#   分区按 PARTUUID 认（2026-10-26 在 rc2e 上从 /sys/class/block/*/uevent 读到），挂载后再核目录结构。
#
# 快照位置：userdata 根下的 gk3-snap/<名字>/（0700 root）。A17 开机的 restorecon 只会给目录换标签，
#   tar.zst 里的 xattr 不受影响。
#
# ⚠️ restore 会删掉 userdata 根下除 media / ota_package / gk3-snap / lost+found 之外的一切再解包。
# 挂载点用私有名字，避免与其他脚本冲突。
# ============================================================================
set -euo pipefail

IP="${1:?用法见文件头}"; CMD="${2:?用法见文件头}"; shift 2

case "$CMD" in
status|backup|restore) ;;
*) echo "✗ 未知命令 $CMD" >&2; exit 2 ;;
esac
ARG1="${1:-}"; ARG2="${2:-}"
if [ "$CMD" = restore ] && { [ -z "$ARG1" ] || [ "$ARG2" != --yes ]; }; then
    echo "✗ restore 要写快照名并加 --yes" >&2; exit 2
fi

ssh -o ConnectTimeout=15 -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/dev/null \
    root@"$IP" bash -s -- "$CMD" "$ARG1" <<'REMOTE'
set -euo pipefail
CMD="$1"; ARG="${2:-}"
U_PARTUUID=4ba36282-f11a-4b1e-b3a4-37940c2319b4      # userdata  nvme0n1p2
M_PARTUUID=2660e57f-f5f6-4ece-b818-2f25b075394c      # metadata  nvme0n1p10
B_PARTUUID=53912ab2-33ac-49d2-a099-94fed8664a26      # misc      nvme0n1p4
MNT=/run/gk3-datasnap; MMNT=/run/gk3-datasnap-meta
die() { echo "✗ $*" >&2; exit 1; }
ok()  { echo "✓ $*"; }

# 每个 PARTUUID 必须恰好对上一个设备：插着克隆出来的盘（同 PARTUUID）时宁可停下，也不猜
dev_of() { local d; d=$(blkid -t PARTUUID="$1" -o device || true)
    [ "$(printf '%s\n' "$d" | grep -c .)" = 1 ] || { echo "✗ PARTUUID $1 对上了 $(printf '%s\n' "$d" | grep -c .) 个设备：$(echo $d)" >&2; exit 1; }
    echo "$d"; }
UD=$(dev_of $U_PARTUUID) && MD=$(dev_of $M_PARTUUID) && BD=$(dev_of $B_PARTUUID) || exit 1
[ -b "$UD" ] && [ -b "$MD" ] && [ -b "$BD" ] || die "按 PARTUUID 找不全分区：userdata=$UD metadata=$MD misc=$BD"
[ "$(blockdev --getsz "$BD")" = 2014 ] || die "misc 的扇区数不是 2014（$BD）"
for d in "$UD" "$MD"; do
    grep -q "^$d " /proc/mounts && die "$d 已经被挂在别处了（$(grep "^$d " /proc/mounts | cut -d' ' -f2)）—— 先卸掉，别两边同时写"
done

cleanup() { umount "$MMNT" 2>/dev/null || true; umount "$MNT" 2>/dev/null || true; rmdir "$MNT" "$MMNT" 2>/dev/null || true; }
trap cleanup EXIT
mkdir -p "$MNT" "$MMNT"
mount -t ext4 "$UD" "$MNT"
mount -t ext4 "$MD" "$MMNT"
for d in app data media system misc user_de; do [ -d "$MNT/$d" ] || die "$UD 根下没有 $d/ —— 不像 Android 的 userdata"; done
SNAP="$MNT/gk3-snap"

case "$CMD" in
status)
    df -h "$MNT" | tail -1
    ok "userdata=$UD metadata=$MD misc=$BD"
    if [ -d "$SNAP" ]; then for s in "$SNAP"/*/; do [ -d "$s" ] || continue
        echo "── $(basename "$s")"; cat "$s/INFO" 2>/dev/null; ls -la "$s" | tail -n +2; done
    else echo "（还没有快照）"; fi ;;

backup)
    NAME="${ARG:-a16}-$(date -u +%Y%m%d-%H%M%S)"
    OUT="$SNAP/$NAME"
    need=$(du -sk --exclude=./media --exclude=./gk3-snap --exclude=./ota_package -x "$MNT" | cut -f1)
    avail=$(df -k --output=avail "$MNT" | tail -1)
    margin=${GK3_SNAP_MARGIN_KIB:-$((10*1024*1024))}     # 留给 A17 首次开机写 /data 的余量；容器测试里调小
    [ "$avail" -gt $((need + margin)) ] || die "空间不够：要约 $((need/1024)) MiB（未压缩上限）+ $((margin/1024)) MiB 余量，只剩 $((avail/1024)) MiB"
    install -d -m 700 "$SNAP" "$OUT"
    echo "── userdata → $OUT/userdata.tar.zst（源约 $((need/1024)) MiB）"
    t0=$(date +%s)
    ( cd "$MNT" && tar --xattrs --xattrs-include='*' --acls --numeric-owner --sparse --one-file-system \
          --exclude=./media --exclude=./gk3-snap --exclude=./ota_package --exclude=./lost+found \
          -cpf - . ) | zstd -T0 -3 -q -o "$OUT/userdata.tar.zst"
    ok "userdata 打包完，$(( $(date +%s) - t0 )) 秒"
    ( cd "$MMNT" && tar --xattrs --xattrs-include='*' --acls --numeric-owner --exclude=./lost+found -cpf - . ) \
        | zstd -T0 -3 -q -o "$OUT/metadata.tar.zst"
    dd if="$BD" of="$OUT/misc.img" bs=512 status=none
    ok "metadata / misc 也备好了"
    echo "── 校验"
    zstd -tq "$OUT/userdata.tar.zst" "$OUT/metadata.tar.zst"
    n_src=$(cd "$MNT" && find . -xdev \( -path ./media -o -path ./gk3-snap -o -path ./ota_package -o -path ./lost+found \) -prune -o -print | wc -l)
    n_tar=$(zstd -dcq "$OUT/userdata.tar.zst" | tar -tf - | wc -l)
    # 两边都含根目录本身（find 的 "."、tar 的 "./"）
    [ "$n_tar" -eq "$n_src" ] || die "条目数对不上：源 $n_src vs 归档 $n_tar"
    ok "条目数一致：$n_tar"
    # 抽查 SELinux 标签确实进了归档：只解出 ./system 这个目录本身（目录不会是硬链接，单独解不会失败），
    # 解到 userdata 上（ext4 能存 security.*），读 xattr 与原件比
    P="$OUT/.probe"; install -d -m 700 "$P"
    zstd -dcq "$OUT/userdata.tar.zst" | ( cd "$P" && tar --xattrs --xattrs-include='*' --no-recursion -xpf - ./system )
    lbl() { python3 -c 'import os,sys; print(os.getxattr(sys.argv[1], "security.selinux").rstrip(b"\0").decode())' "$1" 2>/dev/null || true; }
    a=$(lbl "$MNT/system"); b=$(lbl "$P/system"); rm -rf "$P"
    [ -n "$a" ] && [ "$a" = "$b" ] || die "归档里的 SELinux 标签不对：原件 '$a' vs 归档 '$b'"
    ok "SELinux 标签随归档保存（system/：$b）"
    ( cd "$OUT" && sha256sum userdata.tar.zst metadata.tar.zst misc.img > SHA256SUMS )
    { echo "created_utc=$(date -u +%FT%TZ)"; echo "entries=$n_tar"; echo "src_kib=$need"
      echo "packages_xml_sha256=$(sha256sum "$MNT/system/packages.xml" 2>/dev/null | cut -d' ' -f1)"; } > "$OUT/INFO"
    chmod 600 "$OUT"/*
    ls -la "$OUT"; df -h "$MNT" | tail -1
    ok "快照 $NAME 完成" ;;

restore)
    OUT="$SNAP/$ARG"
    [ -f "$OUT/userdata.tar.zst" ] && [ -f "$OUT/SHA256SUMS" ] || die "没有快照 $ARG"
    echo "── 先核快照完整性"
    ( cd "$OUT" && sha256sum -c --quiet SHA256SUMS ) || die "SHA256SUMS 对不上，不动 /data"
    echo "── 清空 userdata（保留 media / ota_package / gk3-snap / lost+found —— 都不在快照里，原样留着）"
    find "$MNT" -mindepth 1 -maxdepth 1 ! -name media ! -name ota_package ! -name gk3-snap ! -name lost+found -exec rm -rf {} +
    echo "── 解包"
    zstd -dcq "$OUT/userdata.tar.zst" | ( cd "$MNT" && tar --selinux --xattrs --xattrs-include='*' --acls --numeric-owner -xpf - )
    echo "── metadata"
    find "$MMNT" -mindepth 1 -maxdepth 1 ! -name lost+found -exec rm -rf {} +
    zstd -dcq "$OUT/metadata.tar.zst" | ( cd "$MMNT" && tar --selinux --xattrs --xattrs-include='*' --acls --numeric-owner -xpf - )
    echo "── misc"
    dd if="$OUT/misc.img" of="$BD" bs=512 conv=fsync status=none
    sync
    want=$(sed -n 's/^packages_xml_sha256=//p' "$OUT/INFO")
    got=$(sha256sum "$MNT/system/packages.xml" | cut -d' ' -f1)
    [ -z "$want" ] || [ "$want" = "$got" ] || die "恢复后 packages.xml 的 sha256 与快照时不一致"
    ok "已回滚到 $ARG" ;;
esac
REMOTE
