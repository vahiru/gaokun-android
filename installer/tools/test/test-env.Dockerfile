# 安装器后端等的测试环境：Debian trixie + 分区 / 文件系统 / libsparse 工具。
# 由 installer/tools/test/test-in-container.sh 构建和调用，不单独用。
#
# ★ 钉死到摘要，不写 debian:trixie —— tag 会动，测试环境一动，
#   "昨天绿今天红"就分不清是代码变了还是环境变了。
#   换版本：docker pull debian:trixie，再 docker image inspect 取 RepoDigests。
#   （2026-09-24 取：Debian 13.7）
FROM debian@sha256:9cc080028c43b27d2074d63a5f9caf7166d731494965616c1a6d2827a004585c

# 每一个都是被测代码或测试本身真的会调用的工具：
#   gdisk(sgdisk) fdisk(sfdisk) parted(partprobe)  —— 分区表
#   e2fsprogs dosfstools ntfs-3g        —— mkfs / dumpe2fs / 造一个假 Windows 分区（容器的内核不一定有 ntfs3）
#   android-sdk-libsparse-utils zstd    —— 造发版格式的 super.img.zst（img2simg）
#   python3 mtools udev(udevadm)        —— 测试里的检查脚本 / FAT / 等分区节点
#   dmsetup                             —— 造读盘出错的盘（dm-error，installer/backend/test/linux/test-probe.sh）
#   gcc libc6-dev                       —— 测试里现编 gk3l（installer/backend/linux/build.sh）
#   xorriso                             —— 造 ISO、对照 ISO 读取（installer/backend/test/test-media.sh）
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gdisk fdisk parted e2fsprogs dosfstools ntfs-3g \
      android-sdk-libsparse-utils zstd python3 mtools udev util-linux gcc libc6-dev dmsetup xorriso \
 && rm -rf /var/lib/apt/lists/*
