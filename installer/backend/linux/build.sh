#!/bin/sh
# gk3l（LiveCD 的安装器后端）的唯一编法：Makefile 的 linux 目标、installer/tools/build/build-rootfs.sh（构建容器里没有 make）
# 与 test/linux/ 的容器测试都调这里。静态链接：不依赖镜像里装的是哪一版 libc。
#
#   sh installer/backend/linux/build.sh <输出文件> [编译器，默认 cc]
#   GK3W_VERSION=0.2.0 sh …          版本号（gk3l --version、PREFLIGHT 的 gk3l=），默认 dev
#   GK3L_CFLAGS="-O1 -g -fsanitize=…" 换掉 -O2 -static（测试用）
set -eu
out=$1
cc=${2:-cc}
here=$(cd "$(dirname "$0")/.." && pwd)
core=$here/../../tools/gk3boot/core
flags=${GK3L_CFLAGS:--O2 -static}
mkdir -p "$(dirname "$out")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# src/*.c 是与平台无关的部分（Makefile 的 PORTABLE），src/linux/*.c 是 Linux 侧。zstd 是第三方代码，不加 -Werror（与 Makefile 相同）
# shellcheck disable=SC2086
$cc $flags -w -I"$here/third_party/zstd/lib" -c -o "$tmp/zstd.o" "$here/third_party/zstd/zstd_dec.c"
# shellcheck disable=SC2086
$cc -std=gnu11 $flags -Wall -Wextra -Wshadow -Wstrict-prototypes -Wno-unused-parameter -Werror \
    -I"$core/include" -I"$here/third_party/zstd/lib" -DGK3W_VERSION="\"${GK3W_VERSION:-dev}\"" \
    -o "$out" \
    "$here"/src/*.c "$here"/src/linux/*.c "$core"/src/*.c "$tmp/zstd.o"
