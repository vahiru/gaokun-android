#!/usr/bin/env bash
# 在 Ego 的 Ubuntu 上跑：从构建机拉产物 → 刷 super → 换内核/DTB/ramdisk → 回 Android
#
# 用法（在 Ego 的 Ubuntu 里）：
#   bash deploy-from-ubuntu.sh [all|super|boot|rollback]
#     all      （默认）super + 内核 + DTB + ramdisk 全刷
#     super    只刷 super.img
#     boot     只换内核/DTB/ramdisk（不动 super）
#     rollback 刷回 Stage 5 那套能用的 AOSP 16（构建机 ~/keep/aosp16-images/）
#              —— turnip + 声音 + 蓝牙都在里面，十来分钟回到换轨前状态
#
# 源码树用环境变量切：SRC_TREE=~/aosp 可回到旧的 AOSP 树。
#   SRC_TREE=~/aosp bash deploy-from-ubuntu.sh super
#
# 前提：Ego 已有直连构建机的 ssh 钥匙，且设了 BUILD_VM 环境变量，例如
#         export BUILD_VM=user@buildhost.example.com
#       ESP 挂在 /boot/efi（★现在是【内置盘】nvme0n1p1，不再是 U 盘 sda1），
#       Android 目录是 <machine-id>/android/。
#
# ★ 2026-08-20 全内置化之后的现状：
#     - 救援系统 = 内置盘 nvme0n1p3 上的 Ubuntu（hostname gaokun3-rescue），
#       不需要 U 盘；默认启动项就是它，Android 挂死拍电源键即自动回落。
#     - Android 分区：super=nvme0n1p8，userdata=nvme0n1p2（376G），
#       metadata=nvme0n1p10；nvme0n1p9 是迁移前 userdata 的备份（userdata-old）。
set -euo pipefail

# 构建机地址。★不要把真实主机写死在这里 —— 仓库是公开的，
# 硬编码一台开着 SSH 的机器的地址等于请人来扫。
VM=${BUILD_VM:?请先 export BUILD_VM=<user>@<构建机地址>}
# Stage 6 起默认从 crDroid 树取产物（旧 AOSP 树已删，归档在构建机 ~/keep/）
#
# ⚠️ 这两个路径里的 ~ 必须保持【字面量】，不能让本地 shell 展开：
#    它们是【构建机】上的路径（/home/vahiru），而脚本跑在 Ego（/home/user）。
#    展开了就会去 scp vahiru@vm:/home/user/... —— 必然找不到。
#    pull() 把路径整个交给 scp，由远端 shell 展开 ~。
SRC_TREE=${SRC_TREE:-'~/crdroid'}
OUT=$SRC_TREE/out/target/product/gaokun3
KEEP='~/keep/aosp16-images'
MID=8a29534fa802480d9fbb71aa18c01d7b
ESP=/boot/efi/$MID/android
SUPER_PART=/dev/nvme0n1p8
MODE=${1:-all}
# ENTRY 提前定义：flash_boot 要靠它找到条目实际引用的文件名
# ★ 2026-08-20 起默认条目名带 int- 前缀：引导链已搬进【内置 ESP】(nvme0n1p1)，
# 条目是 $MID-int-crdroid.conf / $MID-int-ubuntu.conf。
# U 盘上那套老条目（$MID-crdroid.conf 等）还在，但删掉 Windows 分区之后固件
# 改为优先内置 ESP，所以跑这个脚本的救援系统 /boot/efi = 内置 ESP。
# 要针对 U 盘那套操作就显式传 ENTRY=$MID-crdroid.conf。
#
# 2026-08-20 改默认为 android-a.conf，**int-crdroid.conf 已不可用**：
#   A/B 化之后 fstab.gaokun3 的四条 logical 行都带了 slotselect，于是
#   first-stage mount 必须靠 cmdline 的 androidboot.slot_suffix 才能把
#   "system" 解析成 "system_a"。两个条目的 kernel/dtb/initrd 完全相同，
#   唯一差别就是 android-a.conf 带 slot_suffix=_a 而 int-crdroid.conf 没有。
#   少了它时：init 主动 reboot() 而非 panic → pstore 全空、
#   屏幕一闪而过，**唯一症状是"进 Android 就重启"**（见 docs/dev/android-bringup.md）。
#   要切另一个槽就传 ENTRY=$MID-android-b.conf。
ENTRY=${ENTRY:-$MID-android-a.conf}

pull() {  # pull <远端文件> <本地目标>
  echo "拉取 $(basename "$1") …"
  scp -o BatchMode=yes -q "$VM:$1" "$2"
}

flash_super() {
  pull "$OUT/super.img" /tmp/super.img
  echo "super sha1: $(sha1sum /tmp/super.img | cut -c1-12)"
  # ⚠️ super.img 是 Android sparse 格式，必须 simg2img 展开，不能 dd
  sudo -n simg2img /tmp/super.img "$SUPER_PART"
  sync
  echo "super 已写入 $SUPER_PART"
}

flash_boot() {
  # ⚠️⚠️ 目标文件名【必须从 BLS 条目里读】，不能写死（2026-08-20 M5 发现）。
  #
  # 这个函数原先硬编码写 $ESP/Image 与 $ESP/ramdisk.img —— 那是【旧 AOSP】
  # 条目 android.conf 用的文件名。crDroid 的 crdroid.conf 用的是
  # Image-kb23 与 ramdisk-crdroid.img。于是 `deploy-from-ubuntu.sh all`
  # 会把新内核/新 ramdisk 写到【没人读的文件】上，然后打印"已更新"。
  # 结果就是 M3 那个坑的镜像版：一切显示成功，实机跑的却是旧内核。
  # 改成从条目文件里解析 linux/initrd/devicetree 三行，永远不会漂。
  local conf="/boot/efi/loader/entries/$ENTRY"
  [ -f "$conf" ] || { echo "找不到启动项 $conf"; exit 1; }
  local k_rel i_rel d_rel
  k_rel=$(awk '$1=="linux"      {print $2}' "$conf")
  i_rel=$(awk '$1=="initrd"     {print $2}' "$conf")
  d_rel=$(awk '$1=="devicetree" {print $2}' "$conf")
  local K="/boot/efi$k_rel" I="/boot/efi$i_rel" D="/boot/efi$d_rel"
  echo "条目 $ENTRY 实际读取的文件："
  echo "  内核   $K"
  echo "  ramdisk $I"
  echo "  DTB    $D"

  # ★ 用 vmlinuz.efi（EFI_ZBOOT 自解压 PE，13 MB）而不是 Image（37 MB）：
  #   ESP 只有 300 MiB，而 A/B 两个槽各存一份。
  pull "~/gaokun/kernel-out/arch/arm64/boot/vmlinuz.efi" /tmp/vmlinuz.efi
  pull "~/gaokun/kernel-out/arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dtb" /tmp/gk3.dtb
  pull "$OUT/ramdisk.img" /tmp/ramdisk.img

  # ⚠️★ 不再留 .bak-prev 备份。原先每次部署都把三个文件各备份一份，
  #   把 296 MiB 的 ESP 吃到 95%（63 MB 全是历史备份），下一次部署就会写出
  #   被截断的内核。现在【另一个槽位就是备份】—— 这正是 A/B 的意义：
  #   要回退就 bootctl set-oneshot 到另一个槽的条目。
  sudo -n cp /tmp/vmlinuz.efi "$K"
  sudo -n cp /tmp/ramdisk.img "$I"
  sudo -n cp /tmp/gk3.dtb     "$D"
  sync
  echo "内核 / DTB / ramdisk 已写入【条目实际引用的路径】"

  # ★ 同时更新该槽的 boot 分区，否则 ESP 上的派生文件与 boot 分区会不一致：
  #   下一次 OTA 的 postinstall 是从 boot 分区解包的，不同步就会把机器悄悄
  #   退回到旧内核。boot 分区是唯一真相源，ESP 只是派生物。
  local slot
  slot=$(echo "$ENTRY" | sed -n 's/.*-android-\([ab]\)\.conf/\1/p')
  if [ -n "$slot" ] && [ -b "/dev/disk/by-partlabel/boot_$slot" ]; then
    if pull "$OUT/boot.img" /tmp/boot.img; then
      sudo -n dd if=/tmp/boot.img of="/dev/disk/by-partlabel/boot_$slot"                  bs=4M conv=fsync status=none
      sync
      echo "boot.img 也已写入 boot_${slot}（与 ESP 上的派生文件保持一致）"
    fi
  else
    echo "  ⚠️ 没能从条目名推出槽位或找不到 boot_$slot 分区 —— boot 分区未更新。"
    echo "     下次 OTA 的 postinstall 会从 boot 分区解包，两边不一致会退回旧内核。"
  fi
  echo "  ⚠️ 起来后必须核对：adb shell uname -a 的编译时间要与本次内核一致"
}

flash_rollback() {
  # 回到换轨前那套 AOSP 16（归档于构建机，sha256 校验过）。
  #
  # ⚠️★ 2026-08-20 起这个函数【已过时，不要直接用】：它往 $ESP/ramdisk.img
  #   这个扁平路径写，而 ESP 已改成每槽独立目录（slot_a/ slot_b/），
  #   那个文件now没人读 —— 会"成功"却毫无作用，正是本仓反复踩过的那类坑。
  #   而且归档的 AOSP 16 用的是未压缩 Image + 旧 cmdline（无 slot_suffix），
  #   在 A/B 化之后根本起不来。
  #   真要回退到 Stage 5，得先手工把 BLS 条目改回扁平路径。
  #   ★ 日常回退请用 A/B：oneshot 到另一个槽的条目即可。
  echo "⚠️ rollback 已过时（ESP 已改为每槽目录），请先读本函数的注释" >&2
  # 内核/DTB 不用换：crDroid 与 AOSP 用的是同一个 kb21 内核。
  echo "════ 回退到 Stage 5 的 AOSP 16 ════"
  pull "$KEEP/super.img"   /tmp/super-rollback.img
  pull "$KEEP/ramdisk.img" /tmp/ramdisk-rollback.img
  sudo -n simg2img /tmp/super-rollback.img "$SUPER_PART"
  sudo -n cp /tmp/ramdisk-rollback.img "$ESP/ramdisk.img"
  [ -f "$ESP/ramdisk-debug.img" ] && sudo -n cp /tmp/ramdisk-rollback.img "$ESP/ramdisk-debug.img"
  sync
  echo "已回退。注意 /data 里是 crDroid 写的数据，AOSP 起不来就清 userdata："
  echo "  sudo mkfs.ext4 -F /dev/disk/by-partlabel/userdata"
  echo "  （置备已编进镜像，不再需要刷机后脚本；只需手动连一次 WiFi）"
}

case "$MODE" in
  all)   flash_boot; flash_super ;;
  rollback) flash_rollback ;;
  super) flash_super ;;
  boot)  flash_boot ;;
  *) echo "未知模式: ${MODE}（all|super|boot|rollback）"; exit 2 ;;
esac

# ⚠️⚠️ 这里【不再】改默认启动项，而是设 oneshot（2026-08-19 M3 血泪）。
#
# 旧写法把默认改成 $MID-android.conf，两个后果：
#   1. ★ 条目就错了 —— android.conf 指向的是【旧 AOSP 的】Image(kb18) +
#      ramdisk.img。crDroid 有自己的条目 $MID-crdroid.conf
#      （Image-kb23 + ramdisk-crdroid.img）。用错条目的表现极具迷惑性：
#      能起来、adb 能连、getprop 都对，但内核是不带 patches/kernel/core/0002 的旧核 →
#      bpffs 标签问题回归 → system_server 崩溃循环、永远到不了 boot_completed。
#      查的时候会一路怀疑刚换的 turnip，其实跟 GPU 毫无关系。
#      判据：`adb shell uname -a` 的编译时间必须与本次内核一致。
#   2. 破坏"默认留 Ubuntu"这条运维约定 —— 默认是 Ubuntu 时，Android 挂死
#      拍一下电源键就自动回落到能远程接入的系统；默认改成 Android 就没有
#      这个安全网了，而本机没有串口。
#
# 补救手段（万一又踩到）：Android 起来但卡住时，adb 里两步拿 root——
#   adb shell setprop service.adb.root 1 && adb root
# 然后 `mount -t vfat -o rw /dev/block/sda1 /mnt/usb` 直接改 ESP 上的
# loader.conf（引导 ESP 是 U 盘 = sda1，不是内置盘的 nvme0n1p1）。
# ⚠️ 这里过去写成 `bootctl set-oneshot ... && echo 成功`，失败时【一声不吭】。
# 而所有 BLS 条目的 cmdline 都带 efi=noruntime —— 理论上 EFI 变量不可写，
# bootctl 会失败。实战里这一步是成功过的，说明 Ubuntu 侧仍有 efivarfs；
# 但不能靠"应该能行"，失败必须炸出来，否则会重启进 Ubuntu 然后一脸茫然。
if sudo -n bootctl set-oneshot "$ENTRY"; then
  echo "下次启动 → ${ENTRY}（一次性，默认仍是 Ubuntu）"
else
  echo "!!! bootctl set-oneshot 失败（多半是 efi=noruntime 导致 EFI 变量不可写）"
  echo "!!! 手动改默认项（记得事后改回 Ubuntu，那是唯一的自动回落安全网）："
  echo "      sudo cp /boot/efi/loader/loader.conf /boot/efi/loader/loader.conf.bak-manual"
  echo "      sudo sed -i \"s|^default .*|default $ENTRY|\" /boot/efi/loader/loader.conf"
  echo "!!! 或者重启后在 systemd-boot 菜单里手选（timeout 15 秒）。"
  exit 3
fi
echo
echo "现在重启：sudo systemctl reboot"
echo "起来后建议验证："
echo "  adb shell 'dmesg | grep -iE \"zap|adreno\" | tail -5'          # zap shader 应无报错"
echo "  adb shell 'getprop ro.hardware.vulkan; ls /vendor/lib64/hw/'  # 应为 freedreno"
echo "  adb shell 'cat /proc/asound/cards'                            # 应能看到声卡"
echo "  （无需刷机后脚本 —— 置备已编进镜像，M7 起）"
