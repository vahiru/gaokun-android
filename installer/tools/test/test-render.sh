#!/usr/bin/env bash
# 在容器里用 headless 的 cage 真跑 Linux arm64 版的图形安装器：截图 + 记 RSS。
#
#   bash installer/tools/build/build-flutter.sh      # 先构建
#   bash installer/tools/test/test-render.sh [秒数]  → out/render-test/{*.png, rss.txt, app.log, log.txt}
#   SOAK=1 RENDERER=skia bash installer/tools/test/test-render.sh 60
#     SOAK=1          进浸泡页（持续出帧；app.log 里每 5 秒一行 SOAK fps= raster_p95_ms=）
#     RENDERER=skia   关 Impeller（3.47.2 在 Linux 上默认是 Impeller）。走 runner 认的 GK3_RENDERER
#                     （linux/runner/my_application.cc）—— release 版不读 FLUTTER_ENGINE_SWITCHES
#
# ★ 验得了：Linux 版能不能启动；runner 是不是真的无标题栏全屏；fontconfig 找不找得到中文字体；
#   GTK embedder 在 wlroots 合成器下能不能出帧；wlr-randr 能不能设缩放（真机上靠它把
#   2560×1600 缩成 1280×800 逻辑像素，旋转也靠它 —— cage 0.2.0 没有旋转参数）。
# ⚠️ 验不了：freedreno。这里是 mesa 的软件渲染（llvmpipe）+ pixman 合成，与真机的
#   GPU 路径不同 —— flutter/flutter#192603 那个逐帧泄漏说的恰恰是 ARM GLES 驱动。
#   这里的 RSS 曲线只是一条【软件渲染下的基线】，真机 M0 要另量。
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
APP=$REPO/out/installer-flutter-linux-arm64
OUT=$REPO/out/render-test${RENDERER:+-$RENDERER}${SOAK:+-soak}
SECS=${1:-60}
die() { echo "✗ $*" >&2; exit 1; }
[ -x "$APP/gk3_installer" ] || die "先跑 installer/tools/build/build-flutter.sh"
H=$(shasum -a 256 "$REPO/installer/tools/test/render-test.Dockerfile" 2>/dev/null || sha256sum "$REPO/installer/tools/test/render-test.Dockerfile")
TAG="gk3-render-test:${H:0:12}"
docker image inspect "$TAG" >/dev/null 2>&1 || docker build -q -t "$TAG" -f "$REPO/installer/tools/test/render-test.Dockerfile" "$REPO/installer/tools/test" >/dev/null
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm -e SECS="$SECS" -e SOAK="${SOAK:-}" -e RENDERER="${RENDERER:-}" -v "$APP:/app:ro" -v "$OUT:/out" "$TAG" bash -c '
    export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p -m 700 $XDG_RUNTIME_DIR
    export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 LIBGL_ALWAYS_SOFTWARE=1
    export GK3_FIXTURE=windows-free
    [ "$SOAK" = 1 ] && export GK3_SOAK=1
    # release 版不读 FLUTTER_ENGINE_SWITCHES（engine_switches.cc:16-18）；runner 自己认 GK3_RENDERER
    [ "$RENDERER" = skia ] && export GK3_RENDERER=skia
    # 真机的面板横用是 2560×1600；-s 允许切 VT（cage 默认禁止 —— 不加它命令行逃生口必然失效）
    # 应用自己的输出单独落一个文件（cage 以截断方式打开它的输出，往同一个文件追加会被覆盖）
    cage -s -- sh -c "exec /app/gk3_installer > /out/app.log 2>&1" > /out/log.txt 2>&1 &
    for i in $(seq 1 50); do [ -S $XDG_RUNTIME_DIR/wayland-0 ] && break; sleep 0.2; done
    export WAYLAND_DISPLAY=wayland-0
    wlr-randr > /out/outputs-before.txt 2>&1 || true
    OUTNAME=$(wlr-randr 2>/dev/null | awk "NR==1{print \$1}")
    wlr-randr --output "$OUTNAME" --custom-mode 2560x1600 --scale 2 >> /out/log.txt 2>&1 && echo "wlr-randr：2560x1600 scale 2 ✓" >> /out/log.txt \
        || echo "wlr-randr 设不了模式/缩放" >> /out/log.txt
    wlr-randr > /out/outputs-after.txt 2>&1 || true
    sleep 6
    grim /out/01-first.png 2>>/out/log.txt || echo "grim 失败" >> /out/log.txt
    : > /out/rss.txt
    for t in $(seq 0 5 $SECS); do
        pid=$(pidof gk3_installer || true)
        [ -n "$pid" ] || { echo "t=$t 进程没了" >> /out/rss.txt; break; }
        echo "t=$t rss_kib=$(ps -o rss= -p $pid | tr -d " ")" >> /out/rss.txt
        sleep 5
    done
    grim /out/02-last.png 2>>/out/log.txt || true
    kill %1 2>/dev/null; wait 2>/dev/null; true
'
echo "══ 结果：$OUT"; ls -l "$OUT"
echo "── 渲染后端"; grep -h -o -E "Using the Impeller rendering backend[^.]*|Skia|impeller=false" "$OUT/app.log" | sort | uniq -c || echo "  （日志里没提渲染后端 —— Skia 时引擎不打这一行）"
echo "── app.log 里的 SOAK 行"; grep '^SOAK' "$OUT/app.log" || true
echo "── rss.txt"; cat "$OUT/rss.txt"
