#!/usr/bin/env bash
# 造 LiveCD 的根文件系统（Debian 13 trixie arm64）：图形安装器（Flutter + cage）与它的后端 gk3l → squashfs。
#
#   bash installer/tools/build/build-live.sh                       # 在 arm64 容器里跑本脚本（推荐）
#   bash build-rootfs.sh --out /build/out --installer <Flutter bundle 目录> --firmware <GPU 固件目录>
#
# ⚠️ 必须 root（mmdebstrap 的 root 模式、chroot）、必须在 arm64 上跑（arm64 的主机或容器里是原生的，不需要 qemu）。
# ⚠️ 根文件系统造在容器自己的文件系统里，不要造在挂进来的目录上：
#    virtiofs 之类不保证属主、setuid 位和设备节点。只有最终产物拷出去。
#
# 2026-09-25 从 Alpine 换成 Debian（Flutter 引擎只有 glibc 版）。Alpine 版的每一条断言
# 都有一次上机事故在后面，这里全部保留、换成 Debian 的查法 —— 见"体检"一节。
# 设计取舍见 installer/tools/README.md 与 installer/live/README.md。
set -euo pipefail

# ---- 钉死的上游 ----------------------------------------------------------
SUITE=trixie
# ★ 可复现：GK3_SNAPSHOT=20260925T000000Z 用 snapshot.debian.org 上那一刻的归档
#   （慢，但"昨天好的今天坏了"时能把变量压到只剩代码）。默认走 deb.debian.org，
#   每次构建都把装了哪些包的哪个版本写进 packages-live.lock —— 两次构建之间 diff 它。
if [ -n "${GK3_SNAPSHOT:-}" ]; then MIRROR=http://snapshot.debian.org/archive/debian/$GK3_SNAPSHOT
else MIRROR=${GK3_DEBIAN_MIRROR:-http://deb.debian.org/debian}; fi

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
LIVE=$REPO/installer/tools/build

OUT= ; INSTALLER= ; KEEP= ; FIRMWARE=
# GPU（Adreno 690）要的三个固件。zap shader 的名字取自 dtb（gpu@3d00000/zap-shader/firmware-name）。
# ★ 为什么要单独给：zap shader 是华为专有的（HUAWEI/gaokun3/ 路径），不在 Debian 的固件包里；
#   Android 那边也是从设备树的 firmware/ 目录装进 vendor 的（device/huawei/gaokun3/device.mk 的"固件双路安装"）。
# ⚠️ M0 第一轮（2026-09-25）镜像里没有它们：a660_sqe.fw 加载 -2 → freedreno 建不了 pipe → cage 的 EGL 起不来
#    —— C 版安装器画 dumb buffer、从不碰 GPU，所以 live 以前从没缺过这几个。
# ★ 再分发：zap shader 是华为专有固件。【仓库里】照旧不收（.gitignore 的固件一节、
#    device/huawei/gaokun3/firmware/README.md）；【镜像里】带着它公开发布（用户 2026-09-27 定 B23 ①：随镜像发，与 ROM 同待遇 —— 已发布的 ROM 的 vendor 里本来就带着它）。
#    所以必须给 --firmware：固件从设备的 /vendor/firmware 取（CI 从 buildbot 的固定提交取、按 sha256 钉死）。
GPU_FW="qcom/a660_sqe.fw qcom/a660_gmu.bin qcom/sc8280xp/HUAWEI/gaokun3/qcdxkmsuc8280.mbn"
die() { echo "!! $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }
ok()  { echo "   ✓ $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --out)       OUT=$2; shift 2 ;;
        --installer) INSTALLER=$2; shift 2 ;;
        --firmware)  FIRMWARE=$2; shift 2 ;;   # 形如 /vendor/firmware 的目录（adb pull /vendor/firmware <目录>）
        --keep)      KEEP=1; shift ;;
        *) die "不认识的参数：$1" ;;
    esac
done
[ -n "$OUT" ] || die "要 --out <目录>"
[ "$(id -u)" = 0 ] || die "要 root（mmdebstrap --mode=root + chroot）"
[ "$(uname -m)" = aarch64 ] || die "要在 arm64 上跑（Mac 上用 build-live.sh，它起 arm64 容器）"
for t in mmdebstrap mksquashfs chroot; do
    command -v "$t" >/dev/null || die "缺工具：${t}（apt install mmdebstrap squashfs-tools）"
done
INSTALLER=${INSTALLER:-$REPO/out/installer-flutter-linux-arm64}
[ -x "$INSTALLER/gk3_installer" ] || die "要图形安装器：$INSTALLER/gk3_installer 不在（先跑 installer/tools/build/build-flutter.sh）"
# 先查、后花二十分钟构建：没有 GPU 固件的镜像开机就是黑屏
for f in $GPU_FW; do [ -f "$FIRMWARE/$f" ] || die "要 GPU 固件：--firmware <目录> 里缺 ${f}（从设备取：adb pull /vendor/firmware <目录>）"; done

WORK=${GK3_WORK:-/build/work-live}
ROOTFS=$WORK/rootfs
mkdir -p "$OUT"; rm -rf "$WORK"; mkdir -p "$WORK"

# ---- 1. mmdebstrap ----------------------------------------------------------
pkglist() { grep -vE '^\s*(#|$)' "$1" | tr '\n' ',' | sed 's/,$//'; }
PKGS=$(pkglist "$LIVE/pkgs.txt")
say "1. Debian $SUITE arm64（${MIRROR}）· $(echo "$PKGS" | tr ',' '\n' | wc -l | tr -d ' ') 个指定的包"
# ⚠️ 不装推荐包：要的都写在清单里。
# ⚠️ 文档、手册、翻译不进镜像（体积）；版权文件保留（再分发要带）。
mmdebstrap --mode=root --variant=minbase --arch=arm64 \
    --components="main,non-free-firmware" \
    --aptopt='APT::Install-Recommends "false"' \
    --dpkgopt='path-exclude=/usr/share/man/*' \
    --dpkgopt='path-exclude=/usr/share/info/*' \
    --dpkgopt='path-exclude=/usr/share/doc/*' \
    --dpkgopt='path-include=/usr/share/doc/*/copyright' \
    --dpkgopt='path-exclude=/usr/share/locale/*' \
    --dpkgopt='path-exclude=/usr/share/lintian/*' \
    --include="$PKGS" \
    "$SUITE" "$ROOTFS" "$MIRROR"
ok "根文件系统 $(du -sh "$ROOTFS" | cut -f1)"

mount -t proc  none "$ROOTFS/proc"
mount --rbind /sys "$ROOTFS/sys"; mount --make-rslave "$ROOTFS/sys"
mount --rbind /dev "$ROOTFS/dev"; mount --make-rslave "$ROOTFS/dev"
cleanup() {
    umount -lR "$ROOTFS/dev"  2>/dev/null || true
    umount -lR "$ROOTFS/sys"  2>/dev/null || true
    umount -l  "$ROOTFS/proc" 2>/dev/null || true
}
trap cleanup EXIT
ch() { chroot "$ROOTFS" "$@"; }

# ---- 2. 配置 ---------------------------------------------------------------
say "2. 配置"
cp -a "$LIVE/overlay"/. "$ROOTFS"/; ok "铺了 overlay"
for f in $GPU_FW; do install -Dm644 "$FIRMWARE/$f" "$ROOTFS/usr/lib/firmware/$f"; done
ok "GPU 固件（来自 ${FIRMWARE}）"
# 版本（build-live.sh 从 pubspec 读、从宿主取 git 提交后传进来）：gk3-installer-session 把它打进会话日志的开头
printf 'GK3_INSTALLER_VERSION=%s\nGK3_GIT=%s\nGK3_BUILT=%s\n' \
    "${GK3_VERSION:-unknown}" "${GK3_GIT:-unknown}" "${GK3_BUILT:-unknown}" > "$ROOTFS/etc/gaokun3-release"
ok "/etc/gaokun3-release：${GK3_VERSION:-unknown}（git ${GK3_GIT:-unknown}）"
HOST=gaokun3-live
echo "$HOST" > "$ROOTFS/etc/hostname"
printf '127.0.0.1\tlocalhost\n127.0.1.1\t%s\n::1\t\tlocalhost ip6-localhost ip6-loopback\n' "$HOST" > "$ROOTFS/etc/hosts"
ok "hostname ${HOST}"

# ★ 开机自启：显式 enable 我们要的，并且【不假设】包的 postinst 替我们 enable 了什么。
#   这里 enable 完，体检那一节再用 is-enabled 逐个核一遍。
UNITS="gk3-diag.timer gk3-installer.service seatd.service getty@tty2.service"
# shellcheck disable=SC2086
ch systemctl enable $UNITS >/dev/null 2>&1 || true
# 在一个全内存、每次开机都是新的系统上，apt 的定时任务只会白白吃 CPU。
# nvme-cli 带的 NVMe over Fabrics 自动连接：我们用不到，开机冒烟测试里它每次都失败，
# 污染"失败的单元"与 gk3-diag 的报告（installer/tools/test/test-boot-container.sh）
ch systemctl mask apt-daily.timer apt-daily-upgrade.timer e2scrub_all.timer \
    nvmf-autoconnect.service nvmefc-boot-connections.service >/dev/null 2>&1 || true
# 不许睡：live里没有Android那套usbrole守卫，挂起尚未经实测；initramfs把port0切到host失败时，a600000.usb停在role=device，
#   这种状态下挂起会整板复位（见docs/dev/deep-sleep.md），赶上写super就是一块写了一半的盘。logind的按键与合盖已经在
#   overlay的logind.conf.d里关了；这里把"睡"这条路本身堵上，谁来要都不行。体检逐个核对
SLEEP_TARGETS="sleep.target suspend.target hibernate.target hybrid-sleep.target suspend-then-hibernate.target"
# shellcheck disable=SC2086
ch systemctl mask $SLEEP_TARGETS >/dev/null 2>&1 || true
ok "enable：$UNITS"
ok "mask：$SLEEP_TARGETS"

# root 不设密码：图形安装器的"退出到终端"切到 tty2 的 getty，那里要能直接登录排障。镜像里没有任何网络服务
ch passwd -d root >/dev/null
ok "root 不设密码（只有本地控制台）"

# gk3l：图形安装器的后端（installer/backend，与 Windows 安装器的 gk3w 同源）。编法只在 installer/backend/linux/build.sh 一处
# （静态链接）；构建容器里没有 make，直接调它。版本号取图形安装器的版本
[ -f "$REPO/installer/backend/linux/build.sh" ] || die "没有 $REPO/installer/backend/linux/build.sh（build-live.sh 要把 installer/backend 一起拷进容器）"
GK3W_VERSION=${GK3_VERSION:-dev} sh "$REPO/installer/backend/linux/build.sh" "$WORK/gk3l" gcc || die "gk3l 编不过"
strip "$WORK/gk3l"
install -Dm755 "$WORK/gk3l" "$ROOTFS/usr/lib/gaokun3/gk3l"
ln -sf ../lib/gaokun3/gk3l "$ROOTFS/usr/bin/gk3l"
sha256sum "$WORK/gk3l" | cut -d' ' -f1 > "$OUT/gk3l.sha256"
ok "gk3l（静态，sha256 $(cut -c1-16 "$OUT/gk3l.sha256")…）→ /usr/lib/gaokun3/gk3l"

mkdir -p "$ROOTFS/usr/lib/gaokun3/installer"
cp -a "$INSTALLER"/. "$ROOTFS/usr/lib/gaokun3/installer/"
printf '#!/bin/sh\nexec /usr/lib/gaokun3/installer/gk3_installer "$@"\n' > "$ROOTFS/usr/bin/gk3-installer"
chmod 755 "$ROOTFS/usr/bin/gk3-installer"
ok "图形安装器（Flutter，$(du -sh "$INSTALLER" | cut -f1)）→ /usr/lib/gaokun3/installer/"

# ---- 3. 体检（在做成 squashfs 之前，别把坏镜像做出来）--------------------
say "3. 体检"
# ⚠️★ 必须在 chroot 【里面】查（Alpine 版第一次在外面查绝对符号链接，一个原因造出 5 个假失败）；
#    查【命令】而不是【路径】。
BAD=0
in_ch() { chroot "$ROOTFS" /bin/sh -c "$1" >/dev/null 2>&1; }
need_cmd()  { if in_ch "command -v $1"; then ok "命令 $1"; else echo "   ✗ 缺命令 $1"; BAD=1; fi; }
need_path() { if in_ch "[ -e '$1' ]"; then ok "$1"; else echo "   ✗ 缺 $1"; BAD=1; fi; }
# ⚠️ 多个候选要【逐个】试（Alpine 版把它们塞进同一个 ls，一个不匹配就整体判失败）
need_glob() { for pat in "$@"; do if in_ch "ls $pat"; then ok "$pat"; return; fi; done; echo "   ✗ 这些都没有匹配：$*"; BAD=1; }
need_enabled() { if in_ch "systemctl is-enabled $1 | grep -qx enabled"; then ok "enabled $1"; else echo "   ✗ 没 enable：$1"; BAD=1; fi; }
need_masked()  { if in_ch "systemctl is-enabled $1 | grep -qx masked"; then ok "masked $1"; else echo "   ✗ 没 mask：$1"; BAD=1; fi; }

if in_ch 'readlink -f /sbin/init | grep -q systemd'; then ok "/sbin/init → systemd"; else echo "   ✗ /sbin/init 不是 systemd"; BAD=1; fi
# gk3l 调用的外部工具（installer/backend/src/linux/）与终端排障用的
for c in blkid lsblk findmnt dmesg journalctl systemd-run mkfs.ext4 e2fsck resize2fs mkfs.vfat udevadm sgdisk; do need_cmd "$c"; done
need_path /usr/lib/systemd/boot/efi/systemd-bootaa64.efi   # gk3l 往目标机 ESP 上装的就是它
need_path /usr/bin/busybox                                 # initramfs 用的静态 busybox
need_path /usr/lib/gaokun3/gk3l
# gk3l 在【目标】上真的能跑：版本号与一次纯计算的分区方案（静态链接，不看镜像的 libc）
if in_ch '/usr/bin/gk3l --version | grep -q "^gk3l " && /usr/bin/gk3l plan --sector 512 --region-start 2048 --region-end 100000000 | grep -q "^PLANSUM mode=alongside "'; then
    ok "gk3l 在镜像里能跑（--version、plan）"
else echo "   ✗ gk3l 在镜像里跑不起来"; BAD=1; fi
need_enabled gk3-diag.timer
# ★ 写盘期间不许关机 / 挂起：几个睡眠 target 都 mask 了、logind 不管电源键与合盖、
#   安装器要套的 systemd-inhibit 在。判据锚定行首的实际指令（别 grep 到注释）
for u in $SLEEP_TARGETS; do need_masked "$u"; done
for k in HandlePowerKey HandleSuspendKey HandleHibernateKey HandleLidSwitch HandleLidSwitchExternalPower HandleLidSwitchDocked; do
    if in_ch "grep -qx '$k=ignore' /etc/systemd/logind.conf.d/gaokun3.conf"; then ok "logind：$k=ignore"
    else echo "   ✗ logind.conf.d/gaokun3.conf 里没有 $k=ignore —— 写盘时按电源键 / 合盖会关机或挂起"; BAD=1; fi
done
need_cmd systemd-inhibit
if in_ch 'grep -q "^root::" /etc/shadow'; then ok "root 不设密码（tty2 能直接登录）"; else echo "   ✗ root 有密码或被锁定 —— 退出到终端之后登录不了"; BAD=1; fi
# "退出到终端"要 chvt（它在 kbd 包里、没装 —— 按钮点了什么都不发生）。overlay 里链到 busybox
need_cmd chvt
if in_ch '[ -x /usr/lib/gaokun3/gk3-diag ]'; then ok "/usr/lib/gaokun3/gk3-diag 可执行"; else echo "   ✗ /usr/lib/gaokun3/gk3-diag 不可执行"; BAD=1; fi
# 触摸跟随画面旋转（cage不会替我们转，见gk3-touch-matrix）：规则能解析，程序在默认旋转270下给出矩阵
if in_ch 'udevadm verify /etc/udev/rules.d/70-gk3-touchscreen.rules'; then ok "udev规则70-gk3-touchscreen.rules"
else echo "   ✗ 缺少udev规则70-gk3-touchscreen.rules或无法解析，触摸会与画面错开90°"; BAD=1; fi
if in_ch '/usr/lib/gaokun3/gk3-touch-matrix /dev/null | grep -qx "LIBINPUT_CALIBRATION_MATRIX=0 1 0 -1 0 1"'; then ok "gk3-touch-matrix（默认旋转270）"
else echo "   ✗ gk3-touch-matrix不可执行，或没有给出旋转270的矩阵，触摸会与画面错开90°"; BAD=1; fi
need_path /usr/bin/gk3-installer
need_path /usr/bin/gk3-installer-session
for f in $GPU_FW; do need_path "/usr/lib/firmware/$f"; done
need_cmd cage; need_cmd wlr-randr; need_cmd seatd; need_cmd grim
# ★ 那根线：二进制在镜像里不等于它会跑（Alpine 版第一次停在 login 提示符）
for u in gk3-installer.service seatd.service getty@tty2.service; do need_enabled "$u"; done
# ★ 安装器的每一个动态库都要能解析 —— 缺一个就是开机一块黑屏
if in_ch 'ldd /usr/lib/gaokun3/installer/gk3_installer /usr/lib/gaokun3/installer/lib/*.so | grep -q "not found"'; then
    echo "   ✗ 安装器有解析不了的动态库："; ch sh -c 'ldd /usr/lib/gaokun3/installer/gk3_installer /usr/lib/gaokun3/installer/lib/*.so | grep "not found"' | sort -u | sed 's/^/       /'; BAD=1
else ok "安装器的动态库全部可解析（ldd 无 not found）"; fi
# 中文字体：主题里是【按名字】回退的（installer/common/lib/ui/theme.dart 的 kFontFallback）——
#   Flutter 在 Linux 上不按字符回退系统字体（容器里实测中文全是方块，见 installer/live/README.md）
if in_ch 'fc-list : family | grep -q "WenQuanYi Micro Hei"'; then ok "字体 WenQuanYi Micro Hei（主题按这个名字回退）"
else echo "   ✗ 没有 WenQuanYi Micro Hei —— 界面上的中文会全是方块"; BAD=1; fi
# ★ freedreno：至少要保证驱动在镜像里
need_path /usr/lib/aarch64-linux-gnu/dri/msm_dri.so
if in_ch 'grep -qa freedreno /usr/lib/aarch64-linux-gnu/libgallium-*.so'; then ok "libgallium 里编进了 freedreno"
else echo "   ✗ libgallium 里找不到 freedreno"; BAD=1; fi
[ $BAD -eq 0 ] || die "体检没过 —— 不出镜像。"

# ---- 4. 记账：装了什么、谁占地方 ---------------------------------------------
say "4. 记账"
ch dpkg-query -W -f='${Package}=${Version}\n' | sort > "$OUT/packages-live.lock"
ok "packages-live.lock：$(wc -l < "$OUT/packages-live.lock" | tr -d ' ') 个包"
echo "   ── 占地最多的 15 个包（KiB）"
ch dpkg-query -W -f='${Installed-Size}\t${Package}\n' | sort -rn | head -15 | sed 's/^/     /'

# ---- 5. 打包 ---------------------------------------------------------------
say "5. 打包"
cleanup; trap - EXIT
rm -rf "$ROOTFS"/var/lib/apt/lists/* "$ROOTFS"/var/cache/apt/* "$ROOTFS"/var/log/* "$ROOTFS"/tmp/*
# ★ machine-id 留空：systemd 每次开机现生成。镜像里带一个固定的，所有装机就共用一个
#   （gk3_apply 用它给 ESP 上的目录命名）
: > "$ROOTFS/etc/machine-id"; rm -f "$ROOTFS/var/lib/dbus/machine-id"
: > "$ROOTFS/etc/resolv.conf"

# initramfs 要的静态 busybox、U 盘要的 systemd-boot：在删 rootfs 之前先取出来
cp "$ROOTFS/usr/bin/busybox" "$OUT/busybox.static"
cp "$ROOTFS/usr/lib/systemd/boot/efi/systemd-bootaa64.efi" "$OUT/systemd-bootaa64.efi"
ok "busybox.static / systemd-bootaa64.efi"

SQUASH=$OUT/gaokun3-live.squashfs
rm -f "$SQUASH"
mksquashfs "$ROOTFS" "$SQUASH" -comp zstd -Xcompression-level 19 -noappend -no-progress -quiet
ok "$SQUASH  $(du -h "$SQUASH" | cut -f1)（根文件系统展开 $(du -sh "$ROOTFS" | cut -f1)）"
[ -n "$KEEP" ] || rm -rf "$WORK"
echo
echo "下一步：bash installer/tools/build/build-initramfs.sh --busybox $OUT/busybox.static --out $OUT"
