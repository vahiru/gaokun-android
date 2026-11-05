#!/usr/bin/env bash
# 执行端 initramfs（fastboot.img）一键：打镜像 → QEMU 场景测试 → 判 PASS/FAIL。
# 说明见 tools/gk3boot/README.md。
#
#   bash scripts/gk3boot/test-initramfs.sh                    全部场景
#   bash scripts/gk3boot/test-initramfs.sh fastboot idle      只跑列出的场景
#   GK3_DEBIAN_MIRROR=http://mirrors.ustc.edu.cn/debian bash scripts/gk3boot/test-initramfs.sh   换源
#
# 测的是 tools/gk3boot/build/fastboot/fastboot.img 本身（被测的 /init、gk3-fbi、busybox 原样不动），
# 测试 overlay 另拼在 initrd 后面（模块、test-hook、假 gk3-fastbootd）。
# 要 docker（本机 colima）。镜像 / 容器名带 fbi- 前缀。全程不碰设备、不碰构建机。
# 串口日志与截图：tools/gk3boot/build/fbi-test/。
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
bash "$ROOT/scripts/gk3boot/build-fastboot-img.sh" || exit 1
docker run --rm --name "${GK3_DOCKER_PREFIX:-}fbi-test-$$" -v "$ROOT:/src" ${GK3_DEBIAN_MIRROR:+-e GK3_DEBIAN_MIRROR=$GK3_DEBIAN_MIRROR} \
    -w /src "${GK3_DOCKER_PREFIX:-}fbi-gk3boot-build" bash tools/gk3boot/initramfs/test/run-tests.sh "$@"
