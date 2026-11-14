# 造 Debian 根文件系统 / initramfs / 混合 ISO 的构建环境。由 installer/tools/build/build-live.sh 调用。
# ★ arm64：在 arm64 的主机上原生运行，mmdebstrap 造 arm64 根文件系统不需要 qemu。
# gcc libc6-dev：build-rootfs.sh 把安装器后端 gk3l 静态编进镜像；python3：gk3-bootimg.py 拆 boot.img
FROM debian@sha256:9cc080028c43b27d2074d63a5f9caf7166d731494965616c1a6d2827a004585c
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      mmdebstrap squashfs-tools cpio file mtools xorriso dosfstools python3 ca-certificates util-linux gcc libc6-dev \
 && rm -rf /var/lib/apt/lists/*
