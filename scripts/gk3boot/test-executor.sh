#!/usr/bin/env bash
# 一键：gk3boot 拉起执行端的 QEMU 端到端（说明见 tools/gk3boot/README.md §15）。
#
#   bash scripts/gk3boot/test-executor.sh              exec-* 四个端到端场景 + 分派相关的三个（bcb-dispatch migrate exec-missing）
#   bash scripts/gk3boot/test-executor.sh all          test-boot.sh 的全部场景（含 exec-*）
#   bash scripts/gk3boot/test-executor.sh exec-wipe    只跑列出的场景
#
# 步骤：① 打 fastboot.img（真 gk3-fastbootd 用 musl 编进去，scripts/gk3boot/build-fastboot-img.sh）；
#       ② fbi 容器里取 Debian 测试内核 + 模块进 build/cache-fbi/（tools/gk3boot/initramfs/test/run-tests.sh --prep）；
#       ③ gk3boot 容器里编 gk3boot.efi、造夹具盘（ESP 上放 fastboot.img + 测试 overlay）、QEMU + AAVMF + systemd-boot，
#          宿主侧（容器里）用真 fastboot 经 TCP（gk3.fbtcp=1）对执行端发命令（scripts/gk3boot/test-boot.sh）。
# 要 docker（本机 colima）。与别的会话共用 colima 时：GK3_DOCKER_PREFIX=s7c- 给镜像 / 容器名加前缀。
# 不碰设备、不碰构建机。
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PFX=${GK3_DOCKER_PREFIX:-}
docker info >/dev/null 2>&1 || { echo "✗ docker 不通 —— 本机先 colima start" >&2; exit 2; }

bash "$ROOT/scripts/gk3boot/build-fastboot-img.sh" || exit 1
echo "▶ 测试内核 + 模块（build/cache-fbi/）"
docker run --rm --name "${PFX}fbi-prep-$$" -v "$ROOT:/src" ${GK3_DEBIAN_MIRROR:+-e GK3_DEBIAN_MIRROR=$GK3_DEBIAN_MIRROR} \
    -w /src "${PFX}fbi-gk3boot-build" bash tools/gk3boot/initramfs/test/run-tests.sh --prep || exit 1
if [ "${1:-}" = all ]; then
    shift
    exec bash "$ROOT/scripts/gk3boot/test-boot.sh" "$@"
fi
[ $# -gt 0 ] || set -- bcb-dispatch migrate exec-missing exec-bootloader exec-wipe exec-bootloop exec-tools
exec bash "$ROOT/scripts/gk3boot/test-boot.sh" "$@"
