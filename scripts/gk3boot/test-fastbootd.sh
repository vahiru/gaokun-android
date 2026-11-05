#!/usr/bin/env bash
# gk3-fastbootd（统一启动入口的 fastboot 执行端）一键离线测试：
#   arm64 容器里静态编译 → 造 loop 盘（出厂 / 双系统 / 重名 / 缺分区 / 两块好盘 / 克隆盘）→ 起 TCP 模式的守护进程 →
#   用 Debian 包里的真 fastboot 主机工具跑测试命令 → 判 PASS/FAIL。
# 说明见 tools/gk3boot/README.md §13。
#
#   bash scripts/gk3boot/test-fastbootd.sh
#   GK3_DEBIAN_MIRROR=http://mirrors.ustc.edu.cn/debian bash scripts/gk3boot/test-fastbootd.sh   换源构建镜像
#
# 要 docker（本机是 colima：`colima start`）。容器要 --privileged（loop 设备、挂 vfat）。
# ⚠️ 与别的会话共用 colima：镜像 / 容器名都带 fbd- 前缀；用完不要随手 colima stop，先 docker ps 看有没有别人的容器。
# 全程不碰设备、不碰构建机。产物：tools/gk3boot/build/fbd/gk3-fastbootd.static（与上机那份同一编法）。
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMG=${GK3_DOCKER_PREFIX:-}fbd-gk3-fastbootd-test   # GK3_DOCKER_PREFIX：与别的会话共用 colima 时加前缀
NAME=${GK3_DOCKER_PREFIX:-}fbd-test-$$

docker info >/dev/null 2>&1 || { echo "✗ docker 不通 —— 本机先 colima start" >&2; exit 2; }

echo "▶ 构建环境镜像 ${IMG}（scripts/gk3boot/fastbootd-test.Dockerfile）"
docker build -q -t "$IMG" ${GK3_DEBIAN_MIRROR:+--build-arg GK3_DEBIAN_MIRROR=$GK3_DEBIAN_MIRROR} \
    -f "$ROOT/scripts/gk3boot/fastbootd-test.Dockerfile" "$ROOT/scripts/gk3boot" >/dev/null \
    || { echo "✗ 镜像构建失败（网络？试 GK3_DEBIAN_MIRROR=…）" >&2; exit 1; }

VER="$(git -C "$ROOT" describe --always --dirty --abbrev=12 2>/dev/null || echo unknown)"
# 工作区放容器自己的 /tmp（不放 virtiofs 共享目录：loop 盘和大量小写在共享目录上慢且语义不同）；
# 结束时把二进制与日志拷回 tools/gk3boot/build/fbd/。
docker run --rm --name "$NAME" --privileged -v "$ROOT:/src" -e FBD_VER="$VER" -e FBD_WORK=/tmp/fbd -w /src "$IMG" \
    bash -c 'mkdir -p tools/gk3boot/build/fbd
             bash tools/gk3boot/test/fbd/run.sh; rc=$?
             cp /tmp/fbd/build/gk3-fastbootd.static /tmp/fbd/daemon-all.log /tmp/fbd/fastboot.log tools/gk3boot/build/fbd/ 2>/dev/null
             echo; echo "▶ 第二遍：同一套用例跑 ASan + UBSan 版"
             FBD_ASAN=1 FBD_WORK=/tmp/fbd-asan bash tools/gk3boot/test/fbd/run.sh; rc2=$?
             cp /tmp/fbd-asan/daemon.stderr tools/gk3boot/build/fbd/asan-daemon.stderr 2>/dev/null
             echo; echo "════════ 两遍合计：静态版 rc=$rc，ASan 版 rc=$rc2"
             [ $rc = 0 ] && [ $rc2 = 0 ]'
rc=$?
f=$ROOT/tools/gk3boot/build/fbd/gk3-fastbootd.static
[ -f "$f" ] && echo "   宿主路径：$f  $(stat -f%z "$f" 2>/dev/null || stat -c%s "$f") 字节  sha256 $(shasum -a 256 "$f" | cut -d' ' -f1)"
exit $rc
