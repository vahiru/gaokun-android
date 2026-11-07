#!/usr/bin/env bash
# 造 gaokun3 救援/LiveCD 的根文件系统（Debian 13 trixie arm64）→ squashfs。
#
#   bash scripts/live/build-live.sh                       # Mac 上：在 arm64 容器里跑本脚本（推荐）
#   bash build-rootfs.sh --profile live --out /build/out \
#        [--ssh-key ~/.ssh/ed25519.pub] [--wifi-conf wpa.conf] [--installer <Flutter bundle 目录>]
#
# profile：
#   rescue  无图形，只有 ssh + 分区/文件系统工具。装在内置盘上。
#   live    rescue + 图形安装器（Flutter + cage）。做成 U 盘。
#
# ⚠️ 必须 root（mmdebstrap 的 root 模式、chroot）、必须在 arm64 上跑 —— Mac（Apple Silicon）上
#    的 arm64 容器里就是原生的，不需要 qemu（见 scripts/live/README.md）。
# ⚠️ 根文件系统造在容器自己的文件系统里，不要造在从 macOS 挂进来的目录上：
#    virtiofs 不保证属主、setuid 位和设备节点。只有最终产物拷出去。
#
# 2026-09-25 从 Alpine 换成 Debian（Flutter 引擎只有 glibc 版）。Alpine 版的每一条断言
# 都有一次上机事故在后面，这里全部保留、换成 Debian 的查法 —— 见"体检"一节。
# 设计取舍见 scripts/live/README.md 与 live/installer-flutter/README.md。
set -euo pipefail

# ---- 钉死的上游 ----------------------------------------------------------
SUITE=trixie
# ★ 可复现：GK3_SNAPSHOT=20260925T000000Z 用 snapshot.debian.org 上那一刻的归档
#   （慢，但"昨天好的今天坏了"时能把变量压到只剩代码）。默认走 deb.debian.org，
#   每次构建都把装了哪些包的哪个版本写进 packages-<profile>.lock —— 两次构建之间 diff 它。
if [ -n "${GK3_SNAPSHOT:-}" ]; then MIRROR=http://snapshot.debian.org/archive/debian/$GK3_SNAPSHOT
else MIRROR=${GK3_DEBIAN_MIRROR:-http://deb.debian.org/debian}; fi

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
LIVE=$REPO/scripts/live

PROFILE=rescue
OUT= ; SSH_KEY= ; WIFI_CONF= ; INSTALLER= ; KEEP= ; FIRMWARE=
# GPU（Adreno 690）要的三个固件。zap shader 的名字取自 dtb（gpu@3d00000/zap-shader/firmware-name）。
# ★ 为什么要单独给：zap shader 是华为专有的（HUAWEI/gaokun3/ 路径），不在 Debian 的固件包里；
#   Android 那边也是从设备树的 firmware/ 目录装进 vendor 的（device/huawei/gaokun3/device.mk 的"固件双路安装"）。
# ⚠️ M0 第一轮（2026-09-25）镜像里没有它们：a660_sqe.fw 加载 -2 → freedreno 建不了 pipe → cage 的 EGL 起不来
#    —— C 版安装器画 dumb buffer、从不碰 GPU，所以 live 以前从没缺过这几个。
# ★ 再分发：zap shader 是华为专有固件。【仓库里】照旧不收（.gitignore 的固件一节、
#    device/huawei/gaokun3/firmware/README.md）；【镜像里】带着它公开发布（用户 2026-09-27 定 B23 ①：随镜像发，与 ROM 同待遇 —— 已发布的 ROM 的 vendor 里本来就带着它）。
#    所以 live 必须给 --firmware：固件从设备的 /vendor/firmware 取，不从网上找。
GPU_FW="qcom/a660_sqe.fw qcom/a660_gmu.bin qcom/sc8280xp/HUAWEI/gaokun3/qcdxkmsuc8280.mbn"
die() { echo "!! $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }
ok()  { echo "   ✓ $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --profile)   PROFILE=$2; shift 2 ;;
        --out)       OUT=$2; shift 2 ;;
        --ssh-key)   SSH_KEY=$2; shift 2 ;;
        --wifi-conf) WIFI_CONF=$2; shift 2 ;;
        --installer) INSTALLER=$2; shift 2 ;;
        --firmware)  FIRMWARE=$2; shift 2 ;;   # 形如 /vendor/firmware 的目录（adb pull /vendor/firmware <目录>）
        --keep)      KEEP=1; shift ;;
        *) die "不认识的参数：$1" ;;
    esac
done
[ -n "$OUT" ] || die "要 --out <目录>"
case "$PROFILE" in rescue|live) ;; *) die "--profile 只能是 rescue 或 live" ;; esac
[ "$(id -u)" = 0 ] || die "要 root（mmdebstrap --mode=root + chroot）"
[ "$(uname -m)" = aarch64 ] || die "要在 arm64 上跑（Mac 上用 build-live.sh，它起 arm64 容器）"
for t in mmdebstrap mksquashfs chroot; do
    command -v "$t" >/dev/null || die "缺工具：${t}（apt install mmdebstrap squashfs-tools）"
done
if [ "$PROFILE" = live ]; then
    INSTALLER=${INSTALLER:-$REPO/out/installer-flutter-linux-arm64}
    [ -x "$INSTALLER/gk3_installer" ] || die "live 要图形安装器：$INSTALLER/gk3_installer 不在（先跑 scripts/live/build-flutter.sh）"
    # 先查、后花二十分钟构建：没有 GPU 固件的 live 镜像开机就是黑屏
    for f in $GPU_FW; do [ -f "$FIRMWARE/$f" ] || die "live 要 GPU 固件：--firmware <目录> 里缺 ${f}（从设备取：adb pull /vendor/firmware <目录>）"; done
fi

WORK=${GK3_WORK:-/build/work-$PROFILE}
ROOTFS=$WORK/rootfs
mkdir -p "$OUT"; rm -rf "$WORK"; mkdir -p "$WORK"

# ---- 1. mmdebstrap ----------------------------------------------------------
pkglist() { grep -vE '^\s*(#|$)' "$1" | tr '\n' ',' | sed 's/,$//'; }
PKGS=$(pkglist "$LIVE/pkgs-common.txt")
[ "$PROFILE" = live ] && PKGS="$PKGS,$(pkglist "$LIVE/pkgs-live.txt")"
say "1. Debian $SUITE arm64（${MIRROR}）· $(echo "$PKGS" | tr ',' '\n' | wc -l | tr -d ' ') 个指定的包"
# ⚠️ 不装推荐包：要的都写在清单里（openssh-sftp-server 就是这么被发现要显式写的）。
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
cp -a "$LIVE/overlay-common"/. "$ROOTFS"/; ok "铺了 overlay-common"
[ "$PROFILE" = live ] && { cp -a "$LIVE/overlay-live"/. "$ROOTFS"/; ok "铺了 overlay-live"; }
if [ -n "$FIRMWARE" ]; then
    for f in $GPU_FW; do [ -f "$FIRMWARE/$f" ] && install -Dm644 "$FIRMWARE/$f" "$ROOTFS/usr/lib/firmware/$f"; done
    ok "GPU 固件（来自 ${FIRMWARE}）"
fi
# 版本（build-live.sh 从 pubspec 读、从宿主取 git 提交后传进来）：gk3-installer-session 把它打进会话日志的开头
printf 'GK3_INSTALLER_VERSION=%s\nGK3_GIT=%s\nGK3_BUILT=%s\nGK3_PROFILE=%s\n' \
    "${GK3_VERSION:-unknown}" "${GK3_GIT:-unknown}" "${GK3_BUILT:-unknown}" "$PROFILE" > "$ROOTFS/etc/gaokun3-release"
ok "/etc/gaokun3-release：${GK3_VERSION:-unknown}（git ${GK3_GIT:-unknown}）"
HOST=gaokun3-$PROFILE
echo "$HOST" > "$ROOTFS/etc/hostname"
printf '127.0.0.1\tlocalhost\n127.0.1.1\t%s\n::1\t\tlocalhost ip6-localhost ip6-loopback\n' "$HOST" > "$ROOTFS/etc/hosts"
ok "hostname ${HOST}（scripts/find-device.sh --ssh 按它认身份）"

# ★ 开机自启：显式 enable 我们要的，并且【不假设】包的 postinst 替我们 enable 了什么 ——
#   Alpine 版 2026-08-23 吃过亏：只加了自己的服务，localmount 之类根本不在 runlevel 里。
#   这里 enable 完，体检那一节再用 is-enabled 逐个核一遍。
UNITS="gk3-wifi.service gk3-diag.timer ssh.service avahi-daemon.service"
[ "$PROFILE" = live ] && UNITS="$UNITS gk3-installer.service seatd.service getty@tty2.service"
# shellcheck disable=SC2086
ch systemctl enable $UNITS >/dev/null 2>&1 || true
# 在一个全内存、每次开机都是新的系统上，apt 的定时任务只会白白吃 CPU 和网络。
# nvme-cli 带的 NVMe over Fabrics 自动连接：我们用不到，开机冒烟测试里它每次都失败，
# 污染"失败的单元"与 gk3-diag 的报告（scripts/live/test-boot-container.sh）
ch systemctl mask apt-daily.timer apt-daily-upgrade.timer e2scrub_all.timer \
    nvmf-autoconnect.service nvmefc-boot-connections.service >/dev/null 2>&1 || true
# 不许睡：live 里 a600000.usb 停在 role=device、没有 usbrole 守卫，挂起会整板复位
#   （见 docs/dev/deep-sleep.md）—— 赶上缩 NTFS 或写 super 就是一块坏盘。logind 的按键与合盖已经在
#   overlay-common 的 logind.conf.d 里关了；这里把"睡"这条路本身堵上，谁来要都不行。体检逐个核对
SLEEP_TARGETS="sleep.target suspend.target hibernate.target hybrid-sleep.target suspend-then-hibernate.target"
# shellcheck disable=SC2086
ch systemctl mask $SLEEP_TARGETS >/dev/null 2>&1 || true
ok "enable：$UNITS"
ok "mask：$SLEEP_TARGETS"

# ★ 解锁 root：⚠️ 不是"顺手加的" —— Alpine 版 2026-08-23 上机踩出来：root 的密码字段是 `*`
#   时，配上 `UsePAM no`，sshd 在【检查公钥之前】就拒绝登录，钥匙、权限位、配置全都对也照样
#   Permission denied；而且它为了不泄露账户是否存在会通告全部认证方式，看着像 sshd_config 没生效。
#   清空密码字段的安全性：网络侧仍然只认公钥（PasswordAuthentication no、PermitEmptyPasswords no）；
#   控制台可以直接登录 —— 对救援/安装系统这是【需要的】：网络起不来时本地控制台是最后一条路。
ch passwd -d root >/dev/null
ok "root 账户已解锁（网络侧仍然只认公钥）"

# ssh 公钥：⚠️ 公开发布的 live 镜像【不能】带任何人的公钥
if [ -n "$SSH_KEY" ]; then
    [ -f "$SSH_KEY" ] || die "--ssh-key 指的文件不在：$SSH_KEY"
    case "$(head -c 4 "$SSH_KEY")" in ssh-|ecds) ;; *) die "$SSH_KEY 看着不像公钥 —— ⚠️ 别把私钥装进镜像" ;; esac
    install -d -m700 "$ROOTFS/root/.ssh"
    install -m600 "$SSH_KEY" "$ROOTFS/root/.ssh/authorized_keys"
    ok "已装入 ssh 公钥：$(cut -d' ' -f3 "$SSH_KEY" 2>/dev/null || echo '(无注释)')"
else
    [ "$PROFILE" = rescue ] && echo "   ⓘ 没给 --ssh-key：镜像本身不带公钥。要远程登录，把公钥放到介质的 gaokun3/authorized_keys（装机时安装器会带进救援分区）"
fi
# ssh 主机密钥：rescue 是给一个人用的私有镜像，保留 postinst 生成的密钥 → 每次开机指纹不变。
# ⚠️ live 是公开发布的：所有人共用一把私钥等于没有加密 → 删掉，开机现生成（ssh.service.d/gaokun3.conf）
if [ "$PROFILE" = live ]; then rm -f "$ROOTFS"/etc/ssh/ssh_host_*; ok "live 镜像不带主机密钥（开机现生成）"
else ok "保留 $(ls "$ROOTFS"/etc/ssh/ssh_host_*_key 2>/dev/null | wc -l) 把主机密钥（指纹跨重启不变）"; fi

# WiFi 凭据（可选）：⚠️ 本仓的规矩是凭据不入库；首选让它留在启动介质上（见 gk3-wifi）
if [ -n "$WIFI_CONF" ]; then
    [ -f "$WIFI_CONF" ] || die "--wifi-conf 指的文件不在：$WIFI_CONF"
    grep -q 'network=' "$WIFI_CONF" || die "$WIFI_CONF 里没有 network={...}，不像 wpa_supplicant 配置"
    install -Dm600 "$WIFI_CONF" "$ROOTFS/etc/wpa_supplicant/wpa_supplicant.conf"
    ok "已装入 WiFi 配置（$(grep -c 'network=' "$WIFI_CONF") 个网络）"
fi

# 安装器后端（两个前端共用；图形前端默认从这里加载，见 shell_backend.dart 的 locate()）
install -Dm644 "$LIVE/installer-lib.sh" "$ROOTFS/usr/share/gaokun3/installer-lib.sh"
for f in gk3-unsparse.py gk3-bootimg.py gk3-wpa-scan.py; do install -Dm755 "$LIVE/$f" "$ROOTFS/usr/share/gaokun3/$f"; done
install -Dm755 "$REPO/scripts/install-gaokun3.sh" "$ROOTFS/usr/share/gaokun3/install-gaokun3.sh"
# gk3-misc（S10）：gk3_apply 清零 misc 之后用它的 init 写初始的 A/B 状态与 GK3 记录（installer-lib.sh 的 gk3__misc_tool 先找这里）。
# 在构建容器里静态编（live-build.Dockerfile 装了 gcc / libc6-dev）：编法照 tools/gk3boot/Makefile 的 $(B)/gk3-misc.static ——
#   构建容器里没有 make，改那边要改这里。静态链接：不依赖镜像里装没装哪个 libc 的哪一版
G3=$REPO/tools/gk3boot
[ -f "$G3/misc/gk3-misc.c" ] || die "没有 $G3/misc/gk3-misc.c（build-live.sh 要把 tools/gk3boot 一起拷进容器）"
gcc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I"$G3/core/include" -static \
    -o "$WORK/gk3-misc" "$G3/misc/gk3-misc.c" "$G3"/core/src/*.c || die "gk3-misc 编不过"
strip "$WORK/gk3-misc"
install -Dm755 "$WORK/gk3-misc" "$ROOTFS/usr/share/gaokun3/gk3-misc"
sha256sum "$WORK/gk3-misc" | cut -d' ' -f1 > "$OUT/gk3-misc.sha256"
ok "安装器后端 + 命令行安装器 + gk3-misc（静态，sha256 $(cut -c1-16 "$OUT/gk3-misc.sha256")…）（/usr/share/gaokun3/）"

if [ "$PROFILE" = live ]; then
    mkdir -p "$ROOTFS/usr/lib/gaokun3/installer"
    cp -a "$INSTALLER"/. "$ROOTFS/usr/lib/gaokun3/installer/"
    printf '#!/bin/sh\nexec /usr/lib/gaokun3/installer/gk3_installer "$@"\n' > "$ROOTFS/usr/bin/gk3-installer"
    chmod 755 "$ROOTFS/usr/bin/gk3-installer"
    ok "图形安装器（Flutter，$(du -sh "$INSTALLER" | cut -f1)）→ /usr/lib/gaokun3/installer/"
fi

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
for c in sshd sgdisk parted partprobe resize2fs e2fsck mkfs.ext4 mkfs.vfat mkfs.f2fs ntfsresize blkid lsblk findmnt \
         udevadm wpa_supplicant wpa_cli dhcpcd iw python3 zstd curl cmp sha256sum mkntfs; do need_cmd "$c"; done
need_path /usr/lib/systemd/boot/efi/systemd-bootaa64.efi   # gk3_apply 往目标机 ESP 上装的就是它
need_path /usr/bin/busybox                                 # initramfs 用的静态 busybox
for f in installer-lib.sh gk3-unsparse.py gk3-bootimg.py gk3-wpa-scan.py install-gaokun3.sh gk3-misc; do need_path "/usr/share/gaokun3/$f"; done
# gk3-misc 在【目标】上真的能跑：在 chroot 里对一个 64 KiB 的空文件 init 一次、dump 读回来（静态链接，不看镜像的 libc）
if in_ch 'f=$(mktemp) && head -c 65536 /dev/zero > $f && /usr/share/gaokun3/gk3-misc init $f --slot a | grep -q "^MISCINIT slot=a default=none " && /usr/share/gaokun3/gk3-misc dump $f | grep -q "_a priority=15 tries=6 successful=0"; r=$?; rm -f $f; exit $r'; then
    ok "gk3-misc init 在镜像里能跑（64 KiB 空文件 → _a 15/6，读回一致）"
else echo "   ✗ gk3-misc 在镜像里跑不起来 / init 结果不对"; BAD=1; fi
# 辅助脚本在【目标】的 python 上真的能跑（不是只看文件在不在）
if in_ch 'python3 /usr/share/gaokun3/gk3-wpa-scan.py < /dev/null'; then ok "gk3-wpa-scan.py 在镜像的 python3 上能跑"; else echo "   ✗ 辅助脚本在镜像的 python3 上跑不起来"; BAD=1; fi
for u in gk3-wifi.service ssh.service gk3-diag.timer avahi-daemon.service; do need_enabled "$u"; done
# ⚠️ 判据锚定行首的实际指令（Alpine 版第一次 grep 到了自己注释里的"不能用 need"，当场自我误报）
if in_ch 'grep -qE "^(Requires|BindsTo|Requisite)=" /etc/systemd/system/gk3-wifi.service'; then
    echo "   ✗ gk3-wifi.service 里有硬依赖"; BAD=1; else ok "gk3-wifi 只做排序依赖，没有硬依赖"; fi
# ★ 写盘期间不许关机 / 挂起（v1.0 计划 B5）：几个睡眠 target 都 mask 了、logind 不管电源键与合盖、
#   安装器要套的 systemd-inhibit 在。判据锚定行首的实际指令（同上：别 grep 到注释）
for u in $SLEEP_TARGETS; do need_masked "$u"; done
for k in HandlePowerKey HandleSuspendKey HandleHibernateKey HandleLidSwitch HandleLidSwitchExternalPower HandleLidSwitchDocked; do
    if in_ch "grep -qx '$k=ignore' /etc/systemd/logind.conf.d/gaokun3.conf"; then ok "logind：$k=ignore"
    else echo "   ✗ logind.conf.d/gaokun3.conf 里没有 $k=ignore —— 写盘时按电源键 / 合盖会关机或挂起"; BAD=1; fi
done
need_cmd systemd-inhibit
if in_ch 'grep -q "^root::" /etc/shadow'; then ok "root 账户未锁定"; else echo "   ✗ root 账户是锁定的 —— ssh 公钥登录会被直接拒绝"; BAD=1; fi
if [ -n "$SSH_KEY" ]; then need_path /root/.ssh/authorized_keys; else ok "没装公钥（live 镜像本该如此）"; fi
# 网卡名回到 wlan0：屏蔽 systemd 的可预测命名（M0 第一轮实测被改成 wlP6p1s0，gk3-wifi 因此判"没有网卡"）。
# 查的是文档里的那种写法（指向 /dev/null 的链接）；overlay 里它是个符号链接，铺的时候要原样保留
if in_ch '[ "$(readlink /etc/systemd/network/99-default.link)" = /dev/null ]'; then ok "可预测网卡命名已屏蔽（99-default.link → /dev/null）"
else echo "   ✗ /etc/systemd/network/99-default.link 不是指向 /dev/null 的链接 —— 无线网卡会被改名"; BAD=1; fi
# "退出到终端"要 chvt（M0 实测：它在 kbd 包里、没装 —— 按钮点了什么都不发生）。overlay 里链到 busybox
need_cmd chvt
# 单元里 Exec* 指向的自家脚本必须可执行 —— ssh 的 ExecStartPre 失败 = sshd 起不来 = 一台连不上的机器
for f in gk3-ssh-keys gk3-wifi gk3-diag; do
    if in_ch "[ -x /usr/lib/gaokun3/$f ]"; then ok "/usr/lib/gaokun3/$f 可执行"; else echo "   ✗ /usr/lib/gaokun3/$f 不可执行"; BAD=1; fi
done
# 救援里"进 Android 的某个槽"的助手（v1.0 计划 INST-12；INSTALL 的 About the rescue system 让人用它）。
# 它写 EFI 变量要 chattr（e2fsprogs）；镜像里没有 bootctl，它不靠 bootctl
if in_ch '[ -x /usr/bin/gk3-boot-android ]'; then ok "/usr/bin/gk3-boot-android 可执行"; else echo "   ✗ /usr/bin/gk3-boot-android 不在或不可执行"; BAD=1; fi
need_cmd chattr
if [ "$PROFILE" = live ] && in_ch 'ls /etc/ssh/ssh_host_*_key'; then echo "   ✗ live 镜像里有主机私钥"; BAD=1; fi
# ★ ath11k 固件：没有它 wlan0 根本不出现，而"没网"在这台机器上等于"救援失效"
for f in amss.bin board-2.bin m3.bin; do need_glob "/usr/lib/firmware/ath11k/WCN6855/hw2.0/$f*"; need_glob "/usr/lib/firmware/ath11k/WCN6855/hw2.1/$f*"; done
if [ "$PROFILE" = live ]; then
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
    # 中文字体：主题里是【按名字】回退的（lib/ui/theme.dart 的 fontFamilyFallback）——
    #   Flutter 在 Linux 上不按字符回退系统字体（容器里实测中文全是方块，见 live/installer-flutter/README.md）
    if in_ch 'fc-list : family | grep -q "WenQuanYi Micro Hei"'; then ok "字体 WenQuanYi Micro Hei（主题按这个名字回退）"
    else echo "   ✗ 没有 WenQuanYi Micro Hei —— 界面上的中文会全是方块"; BAD=1; fi
    # ★ freedreno：M0 的头号风险。至少要保证驱动在镜像里
    need_path /usr/lib/aarch64-linux-gnu/dri/msm_dri.so
    if in_ch 'grep -qa freedreno /usr/lib/aarch64-linux-gnu/libgallium-*.so'; then ok "libgallium 里编进了 freedreno"
    else echo "   ✗ libgallium 里找不到 freedreno"; BAD=1; fi
fi
[ $BAD -eq 0 ] || die "体检没过 —— 不出镜像。"

# ---- 4. 记账：装了什么、谁占地方 ---------------------------------------------
say "4. 记账"
ch dpkg-query -W -f='${Package}=${Version}\n' | sort > "$OUT/packages-$PROFILE.lock"
ok "packages-$PROFILE.lock：$(wc -l < "$OUT/packages-$PROFILE.lock" | tr -d ' ') 个包"
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

# initramfs 要的静态 busybox 与 WiFi 固件、U 盘要的 systemd-boot：在删 rootfs 之前先抠出来
cp "$ROOTFS/usr/bin/busybox" "$OUT/busybox.static"
cp "$ROOTFS/usr/lib/systemd/boot/efi/systemd-bootaa64.efi" "$OUT/systemd-bootaa64.efi"
# WiFi 固件进 initramfs：内建 ath11k 在 initramfs 阶段（t≈1.19s，远早于 switch_root）就 probe，
#   那时 squashfs 里的固件还够不着（见 scripts/live/README.md）。
#   只带本机那颗芯片（WCN6855：hw2.0 与 hw2.1 都带，驱动按 board id 选）。
FW_CHIPS=${FW_CHIPS:-ath11k/WCN6855}
rm -rf "$OUT/fw"; mkdir -p "$OUT/fw/lib/firmware"
for chip in $FW_CHIPS; do
    src=$ROOTFS/usr/lib/firmware/$chip
    [ -d "$src" ] || die "rootfs 里没有 /usr/lib/firmware/$chip —— initramfs 会造出一个没网的系统"
    mkdir -p "$OUT/fw/lib/firmware/$(dirname "$chip")"
    cp -a "$src" "$OUT/fw/lib/firmware/$(dirname "$chip")/"
done
ok "busybox.static / systemd-bootaa64.efi / 固件（${FW_CHIPS}，$(du -sh "$OUT/fw" | cut -f1)）"

SQUASH=$OUT/gaokun3-$PROFILE.squashfs
rm -f "$SQUASH"
mksquashfs "$ROOTFS" "$SQUASH" -comp zstd -Xcompression-level 19 -noappend -no-progress -quiet
ok "$SQUASH  $(du -h "$SQUASH" | cut -f1)（根文件系统展开 $(du -sh "$ROOTFS" | cut -f1)）"
[ -n "$KEEP" ] || rm -rf "$WORK"
echo
echo "下一步：bash scripts/live/build-initramfs.sh --busybox $OUT/busybox.static --firmware $OUT/fw --out $OUT"
