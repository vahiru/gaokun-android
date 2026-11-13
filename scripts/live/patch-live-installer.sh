#!/usr/bin/env bash
# 不联网换掉 live 镜像里【我们自己的东西】：解开现成的 squashfs，换图形安装器（/usr/lib/gaokun3/installer/）、
# 安装器后端（/usr/share/gaokun3/，与 build-rootfs.sh 同样的装法），再把工作区的 overlay-common / overlay-live
# 原样叠上去，按原参数重新压。Debian 的包一个都不动。
# ⚠️★ 后端必须一起换：界面与 installer-lib.sh 是配套的（2026-09-25 第一版只换了界面 —— 新界面调
#    --mode reinstall，旧后端根本不认识，差点就这么上了机）。
#
#   bash scripts/live/build-flutter.sh                 # 先构建新的安装器（不要网：pub 依赖都在构建镜像里）
#   bash scripts/live/patch-live-installer.sh          # → out/live/gaokun3-live-patched.squashfs
#   GK3_M0_SQUASHFS=out/live/gaokun3-live-patched.squashfs bash scripts/live/m0-internal.sh prepare
#
# ★ 为什么有它：重建镜像要 mmdebstrap 装包，换到慢网络（2026-09-25 手机热点，镜像源一分钟 400 KB）就做不了；
#   而只改了界面的时候，根文件系统里别的东西一个字节都不用变。
# ⚠️ overlay 里【新加的】单元不会被 enable（正式构建在 chroot 里 systemctl enable），只适合改已有文件。
# ⚠️ 这【不是】正式构建：没有重跑 build-rootfs.sh 的体检（只重做了"安装器的动态库都能解析"那一条）、
#   packages-live.lock 也不更新。镜像里写一份 /etc/gaokun3-patched 说明它是怎么来的（底子的 sha256 +
#   换进去的安装器的 sha256），免得和正式构建混淆。发版一律用 build-live.sh 正式构建。
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
die() { echo "✗ $*" >&2; exit 1; }
ok()  { echo "   ✓ $*"; }
BASE=${1:-$REPO/out/live/gaokun3-live.squashfs}
APP=$REPO/out/installer-flutter-linux-arm64
OUT=$REPO/out/live/gaokun3-live-patched.squashfs
[ -f "$BASE" ] || die "没有底子 ${BASE}（先 build-live.sh 正式构建一次）"
[ -x "$APP/gk3_installer" ] || die "没有新的安装器 $APP/gk3_installer（先 build-flutter.sh）"
H=$(shasum -a 256 "$REPO/scripts/live/live-build.Dockerfile" | cut -c1-12)
TAG="gk3-live-build:$H"
docker image inspect "$TAG" >/dev/null 2>&1 || die "没有构建环境 ${TAG}（先跑一次 build-live.sh）"
BASE_SHA=$(shasum -a 256 "$BASE" | cut -d' ' -f1)
APP_SHA=$( (cd "$APP" && find . -type f | LC_ALL=C sort | xargs shasum -a 256) | shasum -a 256 | cut -d' ' -f1)
echo "══ 底子 $(basename "$BASE")（${BASE_SHA:0:16}…）+ 安装器（${APP_SHA:0:16}…）"
OV_SHA=$( (cd "$REPO/scripts/live" && find overlay-common overlay-live \( -type f -o -type l \) | LC_ALL=C sort | while read -r f; do
    [ -L "$f" ] && echo "$f -> $(readlink "$f")" || echo "$f $(shasum -a 256 < "$f" | cut -d' ' -f1)"; done) | shasum -a 256 | cut -d' ' -f1)
LIB_SHA=$(cat "$REPO/scripts/live/installer-lib.sh" "$REPO"/scripts/live/gk3-{unsparse,bootimg,wpa-scan}.py "$REPO/scripts/install-gaokun3.sh" "$REPO/tools/gk3boot/misc/gk3-misc.c" "$REPO"/tools/gk3boot/core/src/*.c | shasum -a 256 | cut -d' ' -f1)
docker run --rm -v "$(dirname "$BASE"):/base:ro" -v "$APP:/app:ro" -v "$REPO/out/live:/out" -v "$REPO/scripts/live:/live:ro" \
    -v "$REPO/scripts/install-gaokun3.sh:/cli.sh:ro" -v "$REPO/tools/gk3boot:/gk3boot:ro" -e OV_SHA="$OV_SHA" -e LIB_SHA="$LIB_SHA" \
    -e BASE_NAME="$(basename "$BASE")" -e BASE_SHA="$BASE_SHA" -e APP_SHA="$APP_SHA" "$TAG" bash -euo pipefail -c '
    R=/w/root; mkdir -p /w
    unsquashfs -q -n -d $R /base/$BASE_NAME >/dev/null
    [ -d $R/usr/lib/gaokun3/installer ] || { echo "✗ 底子里没有 /usr/lib/gaokun3/installer —— 不是 live profile？"; exit 1; }
    rm -rf $R/usr/lib/gaokun3/installer && mkdir -p $R/usr/lib/gaokun3/installer
    cp -a /app/. $R/usr/lib/gaokun3/installer/ && chown -R 0:0 $R/usr/lib/gaokun3/installer
    # 后端：与 build-rootfs.sh 同样的装法（权限一样）
    install -Dm644 /live/installer-lib.sh $R/usr/share/gaokun3/installer-lib.sh
    for f in gk3-unsparse.py gk3-bootimg.py; do install -Dm755 /live/$f $R/usr/share/gaokun3/$f; done
    install -Dm755 /cli.sh $R/usr/share/gaokun3/install-gaokun3.sh
    # gk3-misc（S10）：编法同 build-rootfs.sh（静态；构建环境 live-build.Dockerfile 带 gcc）
    gcc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I/gk3boot/core/include -static -o /w/gk3-misc \
        /gk3boot/misc/gk3-misc.c /gk3boot/core/src/*.c && strip /w/gk3-misc
    install -Dm755 /w/gk3-misc $R/usr/share/gaokun3/gk3-misc
    f=$(chroot $R mktemp) && head -c 65536 /dev/zero > $R$f && chroot $R /usr/share/gaokun3/gk3-misc init $f | grep -q "^MISCINIT slot=a " \
        || { echo "✗ 换进去的 gk3-misc 在镜像里跑不起来"; exit 1; }
    rm -f $R$f
    chroot $R bash -n /usr/share/gaokun3/installer-lib.sh || { echo "✗ 换进去的 installer-lib.sh 语法不对"; exit 1; }
    for od in overlay-common overlay-live; do
        cp -a /live/$od/. $R/ && (cd /live/$od && find . -mindepth 1 | sed "s#^\.##") | while read -r p; do chown -h 0:0 "$R$p"; done
    done
    chroot $R sh -c "command -v chvt" >/dev/null || { echo "✗ 叠完 overlay 还是没有 chvt"; exit 1; }
    # 体检里与安装器有关的那一条：每个动态库都解析得了（新版要是多依赖了一个库，这里就会露出来）
    if chroot $R sh -c "ldd /usr/lib/gaokun3/installer/gk3_installer /usr/lib/gaokun3/installer/lib/*.so" | grep -q "not found"; then
        echo "✗ 安装器有解析不了的动态库："; chroot $R sh -c "ldd /usr/lib/gaokun3/installer/gk3_installer /usr/lib/gaokun3/installer/lib/*.so" | grep "not found" | sort -u; exit 1
    fi
    printf "%s\n" "这不是正式构建：scripts/live/patch-live-installer.sh 在现成的镜像上换了图形安装器" \
        "底子      $BASE_NAME sha256=$BASE_SHA" "安装器    out/installer-flutter-linux-arm64 sha256(清单)=$APP_SHA" "overlay   scripts/live/overlay-{common,live} sha256(清单)=$OV_SHA" "后端      installer-lib.sh + gk3-*.py + install-gaokun3.sh + gk3-misc 源码 sha256=$LIB_SHA" \
        "时间      $(date -u +%FT%TZ)" > $R/etc/gaokun3-patched
    mksquashfs $R /out/gaokun3-live-patched.squashfs -comp zstd -Xcompression-level 19 -noappend -no-progress -quiet
    echo "   ✓ $(du -h /out/gaokun3-live-patched.squashfs | cut -f1)（底子 $(du -h /base/$BASE_NAME | cut -f1)）"'
[ -s "$OUT" ] || die "没有产物 $OUT"
ok "→ $OUT"
echo "上机（不重启）：GK3_M0_SQUASHFS=$OUT bash scripts/live/m0-internal.sh prepare"
