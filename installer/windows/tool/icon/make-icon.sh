#!/usr/bin/env bash
# 由 crdroid-eye.svg 生成安装器的图标（要 ImageMagick 7 的 magick）：
#   windows/runner/resources/app_icon.ico   Windows 的任务栏、Alt+Tab、exe 文件的图标（16–256 多尺寸）
#   web/favicon.png、web/icons/*.png         浏览器预览
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
SVG=$HERE/crdroid-eye.svg
command -v magick >/dev/null || { echo "missing ImageMagick (magick)" >&2; exit 1; }
# 先按很高的密度栅格化，再缩到各个尺寸：小尺寸不糊
render() { magick -density 1536 -background none "$SVG" -resize "$1x$1" "$2"; }
render 256 "$APP/windows/runner/resources/icon-256.png"
magick "$APP/windows/runner/resources/icon-256.png" -define icon:auto-resize=256,128,96,64,48,40,32,24,20,16 \
    "$APP/windows/runner/resources/app_icon.ico"
rm -f "$APP/windows/runner/resources/icon-256.png"
render 32 "$APP/web/favicon.png"
render 192 "$APP/web/icons/Icon-192.png"
render 512 "$APP/web/icons/Icon-512.png"
# maskable：系统会按自己的形状裁掉四周，图形缩到中间 60% 的安全区里，底色用界面的深色背景
for s in 192 512; do
    magick -size "${s}x${s}" xc:'#111418' \( -density 1536 -background none "$SVG" -resize "$((s * 3 / 4))x$((s * 3 / 4))" \) \
        -gravity center -composite "$APP/web/icons/Icon-maskable-$s.png"
done
echo "OK: app_icon.ico, favicon.png, icons/*.png"
