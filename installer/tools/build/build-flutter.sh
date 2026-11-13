#!/usr/bin/env bash
# 构建图形安装器的 Linux arm64 版本（在 Mac 上的 arm64 Debian 容器里，原生构建）。
#
#   bash installer/tools/build/build-flutter.sh            → out/installer-flutter-linux-arm64/
#
# ⚠️ 工程是【拷进】容器再编的，不在挂载目录里编：容器里跑一次 pub get 会把
#    .dart_tool/package_config.json 里的路径改成 Linux 的，回到 Mac 上又改回来，
#    两边来回打架。产物拷回 out/（.gitignore 里）。
# ★ installer/ 是 pub 工作区（installer/pubspec.yaml）：应用依赖同一工作区里的 gk3_common，
#   锁文件在 installer/pubspec.lock。所以拷的是整个 installer/（C 后端 installer/backend 除外），在 live/ 里编。
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=$REPO/out/installer-flutter-linux-arm64
die() { echo "✗ $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }

command -v docker >/dev/null && docker info >/dev/null 2>&1 || die "docker 没在跑（macOS 上：colima start）"
[ "$(docker info --format '{{.Architecture}}')" = aarch64 ] || die "docker 不是 arm64 —— 这个脚本就是为了原生构建才存在的"

H=$(shasum -a 256 "$REPO/installer/tools/build/flutter-build.Dockerfile" 2>/dev/null || sha256sum "$REPO/installer/tools/build/flutter-build.Dockerfile")
BASE="gk3-flutter-base:${H:0:12}"
TAG="gk3-flutter-build:${H:0:12}"
if ! docker image inspect "$TAG" >/dev/null 2>&1; then
    say "构建 Flutter 构建环境 ${TAG}（第一次要下 Flutter SDK 与引擎，约 1–2 GB）"
    docker image inspect "$BASE" >/dev/null 2>&1 \
        || docker build -t "$BASE" -f "$REPO/installer/tools/build/flutter-build.Dockerfile" "$REPO/installer/tools/build"
    # 预取在 docker run 里做（理由见 Dockerfile 末尾），做完 commit 成构建镜像
    docker rm -f gk3-flutter-prep >/dev/null 2>&1 || true
    docker run --name gk3-flutter-prep "$BASE" bash -euo pipefail -c '
        flutter config --no-analytics >/dev/null
        flutter config --enable-linux-desktop >/dev/null
        flutter precache --linux --no-android --no-ios --no-web --no-macos --no-windows --no-fuchsia
        flutter --version'
    docker commit gk3-flutter-prep "$TAG" >/dev/null
    docker rm gk3-flutter-prep >/dev/null
fi

# MD3 的两个字体打包进应用（在 gk3_common 里声明，installer/common/lib/ui/theme.dart）；文件不入库，先取
for f in Roboto-VF.ttf NotoSansSC-VF.ttf; do
    [ -f "$REPO/installer/common/assets/fonts/$f" ] \
        || die "缺字体 installer/common/assets/fonts/${f}（python3 installer/common/tool/fetch-fonts.py —— 从 google/fonts 下载，或 --from <目录>）"
done
say "flutter build linux --release"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm -v "$REPO/installer:/src:ro" -v "$OUT:/out" "$TAG" bash -euo pipefail -c '
    mkdir /build && cd /src && tar --exclude=./backend --exclude=build --exclude=.dart_tool --exclude=./live/test/shots --exclude=./live/test/fonts -cf - . | tar -C /build -xf -
    cd /build/live
    flutter pub get >/dev/null
    flutter build linux --release
    cp -a build/linux/arm64/release/bundle/. /out/
'
# 判据看产物，不看退出码
[ -x "$OUT/gk3_installer" ] || die "没有产物 $OUT/gk3_installer"
# 字体真的进了产物（中文方块那次的教训：界面用哪个字体，要在产物里看得见）
# 包里声明的资源在产物里放在 packages/<包名>/ 下
for f in Roboto-VF.ttf NotoSansSC-VF.ttf; do
    [ -s "$OUT/data/flutter_assets/packages/gk3_common/assets/fonts/$f" ] || die "产物里没有字体 $f"
done
file "$OUT/gk3_installer" 2>/dev/null | grep -q 'ARM aarch64' || echo "  ⚠️ 没法确认是 aarch64（没有 file 命令？）"
say "产物"
du -sh "$OUT"; ( cd "$OUT" && find . -maxdepth 2 -type f -size +1M -exec du -h {} + | sort -rh )
