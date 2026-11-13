#!/usr/bin/env bash
# 开机冒烟：把 LiveCD 的 squashfs 解开当容器根，以 systemd 为 PID 1 启动，看各单元起不起得来。
#
#   bash scripts/live/test-boot-container.sh [squashfs] [秒数]     # 默认 out/live/gaokun3-live.squashfs、70 秒
#   GK3_TEST_OVERLAY=1 bash scripts/live/test-boot-container.sh    # 先把【工作区】的 overlay 叠上去再开机
#     —— 只改了 overlay、又没法重建时用（2026-09-25：换了网络，deb.debian.org 不通）。
#     ⚠️ 这样验的是"已构建的根 + 工作区的 overlay"，【不是】产物本身；上机前仍要重建。
#
# ★ 为什么值得：这台机器没有串口，开机前的错误在真机上就是一块黑屏。单元文件写错一个键、
#   路径不对、顺序依赖打环、哪个服务一起来就挂 —— 这些在容器里 70 秒就能看到。
# ⚠️ 验不了：内核、硬件、initramfs（容器用的是 colima 虚拟机的内核）。
#    下面这几个在容器里【注定】起不来，不算问题：
#      gk3-installer   cage 拿不到 DRM（没有 /dev/dri），会按 Restart=always 每 2 秒重试
#      seatd           可能拿不到 VT
#      getty@tty2      容器里没有 /dev/tty2（ConditionPathExists 跳过）
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SQ=${1:-$REPO/out/live/gaokun3-live.squashfs}
SECS=${2:-70}
die() { echo "✗ $*" >&2; exit 1; }
[ -f "$SQ" ] || die "没有 ${SQ}（先跑 scripts/live/build-live.sh）"
H=$(shasum -a 256 "$REPO/scripts/live/live-build.Dockerfile" 2>/dev/null || sha256sum "$REPO/scripts/live/live-build.Dockerfile")
BUILD="gk3-live-build:${H:0:12}"
docker image inspect "$BUILD" >/dev/null 2>&1 || die "没有构建环境 ${BUILD}（先跑一次 build-live.sh）"
NAME=gk3-bootsmoke
echo "══ squashfs → 容器镜像"
docker run --rm -v "$(cd "$(dirname "$SQ")" && pwd):/in:ro" "$BUILD" \
    sh -c "unsquashfs -q -n -d /r /in/$(basename "$SQ") >/dev/null && tar -C /r -cf - ." \
    | docker import - "$NAME:latest" >/dev/null
if [ -n "${GK3_TEST_OVERLAY:-}" ]; then
    echo "══ ⚠️ 叠上工作区的 overlay（验的不是产物本身）"
    c=$(docker create "$NAME:latest" /bin/true)
    ods=("$REPO/scripts/live/overlay-common")
    case "$(basename "$SQ")" in *live*) ods+=("$REPO/scripts/live/overlay-live") ;; esac
    for od in "${ods[@]}"; do docker cp "$od/." "$c:/"; done
    docker commit "$c" "$NAME:latest" >/dev/null; docker rm "$c" >/dev/null
    # docker cp 带进来的是 Mac 上的属主（uid 501）；产物里是 root —— 改回来，别让测试与产物在这一点上不同
    paths=$(for od in "${ods[@]}"; do (cd "$od" && find . -mindepth 1 | sed 's#^\.##'); done)
    docker run --name "$NAME-own" -i "$NAME:latest" sh -c 'xargs chown -h 0:0' <<< "$paths"
    docker commit "$NAME-own" "$NAME:latest" >/dev/null; docker rm "$NAME-own" >/dev/null
fi
docker rm -f "$NAME" >/dev/null 2>&1 || true
# 假的启动介质，挂到 /media/gk3（gk3-diag 往它的 gaokun3/diag/ 写开机报告）
VOL=gk3-bootsmoke-media
docker volume rm -f "$VOL" >/dev/null 2>&1 || true
docker volume create "$VOL" >/dev/null
docker run --rm -v "$VOL:/m" "$NAME:latest" sh -c 'mkdir -p /m/gaokun3'
echo "══ 以 systemd 为 PID 1 启动，等 ${SECS} 秒"
docker run -d --name "$NAME" --privileged --cgroupns=host -v /sys/fs/cgroup:/sys/fs/cgroup:rw \
    -v "$VOL:/media/gk3" \
    --tmpfs /run --tmpfs /run/lock --tmpfs /tmp -e container=docker "$NAME:latest" /sbin/init >/dev/null
sleep "$SECS"
X() { docker exec "$NAME" "$@" 2>&1; }
echo "── 系统状态：$(X systemctl is-system-running)"
echo "── 失败的单元"; X systemctl --failed --no-legend --plain | sed 's/^/   /'
echo "── 我们的单元"
for u in gk3-diag.timer gk3-diag.service seatd.service gk3-installer.service getty@tty2.service; do
    printf '   %-22s %-10s %s\n' "$u" "$(X systemctl is-enabled "$u")" "$(X systemctl show -p ActiveState,SubState,Result --value "$u" | tr '\n' ' ')"
done
echo "── 单元文件的告警（键写错、值不合法、依赖打环）"
X journalctl -b --no-pager | grep -iE "unknown (key|section)|invalid|ordering cycle|failed to parse|bad unit|not executable|No such file" | sed 's/^/   /' | head -30 || true
echo "── 没有任何网络服务在听"
X sh -c 'ss -ltnu 2>/dev/null | tail -n +2 | grep -v "127.0.0" || echo "   （没有）"' | sed 's/^/   /'
echo "── gk3-diag 的输出"; X journalctl -b -u gk3-diag --no-pager -o cat | tail -5 | sed 's/^/   /'
echo "── gk3-installer 最近几次尝试"; X journalctl -b -u gk3-installer --no-pager -o cat | tail -6 | sed 's/^/   /'
docker rm -f "$NAME" >/dev/null
docker volume rm -f "$VOL" >/dev/null
