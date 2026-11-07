# gk3boot 的 UEFI 构建与 QEMU 夹具环境（见 tools/gk3boot/README.md）。
# 由 scripts/gk3boot/test-probe.sh 调用。
# ★ 与 scripts/live/live-build.Dockerfile 同一个 Debian 13 基础镜像（arm64，Mac 上原生，不用 qemu-user）。
# ★ 版本钉死：systemd-boot-efi 必须与设备同版本（257.13，scripts/live/packages-live.lock:333）；
#   gnu-efi / QEMU / AAVMF 钉住是为了"昨天绿今天红"时能把变量压到只剩代码。
# ★ fastboot（主机端，android-platform-tools）：S7c 的执行端端到端场景用它对 QEMU 里的 gk3-fastbootd 发命令
#   （与 fastbootd-test.Dockerfile 同样不钉版本）。
FROM debian@sha256:9cc080028c43b27d2074d63a5f9caf7166d731494965616c1a6d2827a004585c
ARG GK3_DEBIAN_MIRROR=
RUN if [ -n "$GK3_DEBIAN_MIRROR" ]; then \
      sed -i "s|http://deb.debian.org/debian|$GK3_DEBIAN_MIRROR|g" /etc/apt/sources.list.d/debian.sources; fi \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      gnu-efi=3.0.18-1+deb13u1 \
      systemd-boot-efi=257.13-1~deb13u1 \
      qemu-system-arm=1:10.0.13+ds-0+deb13u1 \
      qemu-efi-aarch64=2025.02-8+deb13u1 \
      gcc binutils make python3 python3-virt-firmware mtools dosfstools file \
      fastboot \
 && rm -rf /var/lib/apt/lists/*
