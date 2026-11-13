# 安装器后端的测试环境：Debian trixie arm64 + 分区 / 文件系统 / libsparse 工具。
# 由 scripts/live/test-in-container.sh 构建和调用，不单独用。
#
# ★ 钉死到摘要，不写 debian:trixie —— tag 会动，测试环境一动，
#   "昨天绿今天红"就分不清是代码变了还是环境变了。
#   换版本：docker pull debian:trixie，再 docker image inspect 取 RepoDigests。
#   （2026-09-24 取：Debian 13.7）
FROM debian@sha256:9cc080028c43b27d2074d63a5f9caf7166d731494965616c1a6d2827a004585c

# 每一个都是被测代码真的会调用的工具，与 live 镜像的 pkgs-common.txt 对应：
#   gdisk(sgdisk) fdisk(sfdisk) parted  —— 分区表
#   e2fsprogs dosfstools ntfs-3g        —— mkfs / resize / 造一个假 Windows 分区
#   android-sdk-libsparse-utils         —— 真 simg2img/img2simg，拿来交叉比对
#   zstd python3 mtools udev(udevadm)   —— 发版格式 / 解包器 / FAT / 等分区节点
#   curl                                —— 网络安装（gk3_net_fetch / gk3_net_release）
#   dmsetup                             —— 造读盘出错的盘（dm-error，installer/backend/test/linux/test-probe.sh）
#   gcc libc6-dev                       —— 现编 gk3-misc（installer-lib.sh 的 gk3__misc_tool：仓库 checkout 里没有现成的就用 cc 编；
#                                          live 镜像里是 build-rootfs.sh 静态编好放进 /usr/share/gaokun3/ 的）
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gdisk fdisk parted e2fsprogs dosfstools ntfs-3g \
      android-sdk-libsparse-utils zstd python3 mtools udev util-linux curl ca-certificates gcc libc6-dev dmsetup \
 && rm -rf /var/lib/apt/lists/*
