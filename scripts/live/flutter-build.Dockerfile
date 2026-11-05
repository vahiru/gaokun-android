# 在 Mac（Apple Silicon）上构建 gaokun3 图形安装器的 Linux arm64 版本。
# 由 scripts/live/build-flutter.sh 构建和调用。
#
# 为什么在这里而不是构建机：构建机是 x86_64，Flutter 的 Linux 桌面构建不做交叉编译；
#   Mac 是 arm64，arm64 容器里就是原生构建，不需要 qemu（见 live/installer-flutter/README.md）。
# ★ 钉死：Debian 按摘要，Flutter 按 tag（与 live/installer-flutter/pubspec.yaml 的
#   environment.flutter 一致）。换版本改这里的两行。
FROM debian@sha256:9cc080028c43b27d2074d63a5f9caf7166d731494965616c1a6d2827a004585c

# Flutter Linux 桌面构建要的工具链与 GTK3 开发文件；git/curl/unzip/xz 是 Flutter SDK 自己要的
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      git curl unzip xz-utils ca-certificates \
      clang cmake ninja-build pkg-config libgtk-3-dev liblzma-dev libstdc++-14-dev \
 && rm -rf /var/lib/apt/lists/*

ARG FLUTTER_TAG=3.47.2
RUN git clone --depth 1 --branch "$FLUTTER_TAG" https://github.com/flutter/flutter.git /opt/flutter
ENV PATH=/opt/flutter/bin:$PATH
# ⚠️ 引擎与 Dart SDK 的预取【不在这里做】，由 build-flutter.sh 在 docker run 里做完再 commit。
#    2026-09-25 实测：colima 上 docker build 里下载 Dart SDK 两次都是 curl 35（SSL 连接错误），
#    而同一镜像、同一条 curl 命令在 docker run 里完整下完 236 MB。两者的构建网络不同，
#    没有深究 —— 输入相同、产物相同，换一条证实能通的路。
