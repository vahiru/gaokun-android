# 执行端 initramfs（fastboot.img，见 tools/gk3boot/README.md）的构建与 QEMU 测试环境。
# 由 scripts/gk3boot/build-fastboot-img.sh、scripts/gk3boot/test-initramfs.sh 调用；镜像名 fbi-gk3boot-build。
# ★ 与 gk3boot-build.Dockerfile 同一个 Debian 13 基础镜像（arm64，Mac 上原生，不用 qemu-user）。
# ★ 进 fastboot.img 的东西版本钉死（busybox-static / musl / console-setup 的字体 / cpio），这样同一份源码
#   打出来的 fastboot.img 逐字节相同（build.sh 里固定了 mtime、属主、排序、gzip -n）。
#   qemu-system-arm 与 gk3boot-build 钉同一版。测试内核不钉（Debian 只留当前版本，见 tools/gk3boot/qemu/fetch-test-kernel.sh）。
FROM debian@sha256:9cc080028c43b27d2074d63a5f9caf7166d731494965616c1a6d2827a004585c
ARG GK3_DEBIAN_MIRROR=
RUN if [ -n "$GK3_DEBIAN_MIRROR" ]; then \
      sed -i "s|http://deb.debian.org/debian|$GK3_DEBIAN_MIRROR|g" /etc/apt/sources.list.d/debian.sources; fi \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      busybox-static=1:1.37.0-6+b9 \
      musl-tools=1.2.5-3.1~deb13u1 \
      cpio=2.15+dfsg-2 \
      console-setup-linux=1.242~deb13u1 \
      linux-libc-dev=6.12.111-1 \
      qemu-system-arm=1:10.0.13+ds-0+deb13u1 \
      gcc make python3 file xz-utils kmod ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /opt/kh \
 && ln -s /usr/include/linux /opt/kh/linux \
 && ln -s /usr/include/asm-generic /opt/kh/asm-generic \
 && ln -s /usr/include/aarch64-linux-gnu/asm /opt/kh/asm
# /opt/kh：musl-gcc 的 specs 带 -nostdinc，内核 UAPI 头（linux/input.h、linux/usb/functionfs.h）要另给路径。
# UAPI 头与 libc 无关，借 Debian 的 linux-libc-dev 即可。
