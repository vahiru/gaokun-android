#!/usr/bin/env bash
# 发安装器（预览）：随 ROM 的发布附带（用户 2026-09-27）。在 Mac 上跑 —— live 镜像只能在 arm64 容器里造，
# 构建机上的 scripts/release.sh 做不了这一半。
#
#   bash scripts/live/release-installer.sh --boot-img <要发布的那一版的 boot.img> --rom v0.6.3-alpha \
#        --firmware out/vendor-firmware \
#        --artifacts <那一版的 install-artifacts.sha256>      # 只造、不传 → out/release-installer/<安装器版本>/
#   （带 --payload <发布目录> 时 --artifacts 默认取 <发布目录>/install-artifacts.sha256）
#   … --upload v0.6.3-alpha                                    # 再传到那个 GitHub release（⚠️ 发布要用户点头）
#   … --r2                                                     # 再传 R2 的 installer/<安装器版本>/（国内下得到；可与 --upload 一起）
# 附件：<B>-usb.img.xz、<B>-windows.zip、<B>-release.txt、<B>-SHA256SUMS，以及命令行安装要的 rescue.squashfs + initramfs.img（INST-6）
#
# ★ --boot-img 必须是【要发布的那一版】的 boot.img（与 release.sh --no-build 同一个道理：发的就是验过的那一份）。
#   live 与 Android 共用这个内核；安装时写进 ESP 的内核来自安装载荷，不是这个。
# ★ G11 / GUI-15（2026-10-11）：拿 release.sh 为那一版生成的 install-artifacts.sha256 核对 --boot-img ——
#   v0.7.0 附带的安装器用的是旧内核（GUI-15），原因就是这里只核"镜像内核来自给的文件"、不核"给的文件就是发布的那一版"。
#   清单里 boot.img 那一行的 sha256 必须等于 --boot-img，否则在构建前就停。
# ★ 不带 --with-rescue：装进救援分区的就是 live 镜像本身 —— 它在真机上起过十几次（M0、M4b），还能重新安装；
#   单独的 rescue profile 在真机上一次没起过（Stage 7 M4.5 的事）。
# ★ 不带 --m0：M0 的变体启动项是开发用的。
# ⓘ 镜像里带华为的 GPU zap shader —— 与 ROM 同待遇（TODO B23，用户 2026-09-27 定）。
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
die() { echo "✗ $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }
ok()  { echo "   ✓ $*"; }

BOOTIMG= ROM= FW= UPLOAD= PAYLOAD= R2= ARTS=
while [ $# -gt 0 ]; do
    case "$1" in
        --boot-img) BOOTIMG=$2; shift 2 ;;
        --rom)      ROM=$2; shift 2 ;;
        --firmware) FW=$2; shift 2 ;;
        --artifacts) ARTS=$2; shift 2 ;;        # 那一版的 install-artifacts.sha256（G11）
        --payload)  PAYLOAD=$2; shift 2 ;;      # 可选：把一整个发布目录放进 U 盘（离线装），镜像会大 1.3 GB
        --upload)   UPLOAD=$2; shift 2 ;;
        --r2)       R2=1; shift ;;            # 同时传 R2 的 installer/<版本>/（凭据从环境变量读，同 scripts/release.sh）
        *) die "不认识的参数：$1" ;;
    esac
done
[ -f "$BOOTIMG" ] || die "要 --boot-img <要发布的那一版的 boot.img>"
[ -n "$ROM" ] || die "要 --rom <这个安装器随哪个 ROM 版本发，例如 v0.6.3-alpha>"
[ -d "$FW" ] || die "要 --firmware <GPU 固件目录>（adb pull /vendor/firmware out/vendor-firmware）"
[ -n "$ARTS" ] || { [ -n "$PAYLOAD" ] && ARTS=$PAYLOAD/install-artifacts.sha256; }
[ -f "$ARTS" ] || die "要 --artifacts <那一版的 install-artifacts.sha256>（release.sh 生成；R2 install/<VER>/ 或 staging/<VER>/ 里有）—— G11：安装器的内核必须就是发布的那个 boot.img"
WANT=$(awk '$2=="boot.img"{print $1}' "$ARTS")
[[ "$WANT" =~ ^[0-9a-f]{64}$ ]] || die "$ARTS 里没有 boot.img 那一行"
[ "$(shasum -a 256 "$BOOTIMG" | cut -d' ' -f1)" = "$WANT" ] \
    || die "--boot-img 与 $ARTS 里发布的 boot.img 不是同一个（G11 / GUI-15）"
# 发出去的东西必须对应一个提交：工作区有改动就停（镜像里的 GK3_GIT 会带 -dirty，发布说明对不上号）
[ -z "$(git -C "$REPO" status --porcelain)" ] || die "工作区有没提交的改动 —— 先提交（发出去的镜像要对应一个提交）"

VER=$(sed -n 's/^version:[[:space:]]*\([^+[:space:]]*\).*/\1/p' "$REPO/installer/live/pubspec.yaml")
[ -n "$VER" ] || die "读不到安装器版本（installer/live/pubspec.yaml）"
OUT=$REPO/out/release-installer/$VER
say "安装器 ${VER}（随 ROM ${ROM}）· git $(git -C "$REPO" rev-parse --short=12 HEAD)"

say "1. 构建"
bash "$REPO/scripts/live/build-flutter.sh" >/dev/null || die "build-flutter.sh 失败"
ok "图形安装器"
bash "$REPO/scripts/live/build-live.sh" --boot-img "$BOOTIMG" --firmware "$FW" ${PAYLOAD:+--release "$PAYLOAD" --payload} \
    > "$REPO/out/release-installer-build.log" 2>&1 || { tail -20 "$REPO/out/release-installer-build.log"; die "build-live.sh 失败（全文 out/release-installer-build.log）"; }
ok "live 镜像 + Windows 安装包（日志 out/release-installer-build.log）"

say "2. 核对"
L=$REPO/out/live
grep -q "^GK3_INSTALLER_VERSION=$VER$" "$L/release.txt" || die "release.txt 里的版本不是 $VER"
! grep -q '^GK3_GIT=.*-dirty' "$L/release.txt" || die "release.txt 说工作区有改动（-dirty）"
grep -q "^GK3_BOOTIMG_SHA256=$(shasum -a 256 "$BOOTIMG" | cut -d' ' -f1)$" "$L/release.txt" || die "镜像的内核不是来自给的 boot.img"
# S10：安装器初始化 misc 要 /usr/share/gaokun3/gk3-misc（build-rootfs.sh 编进去、体检在镜像里跑过 init）。没有它 gk3_apply 在动盘前就拒绝
grep -qE '^GK3_MISC_SHA256=[0-9a-f]{64}$' "$L/release.txt" || die "release.txt 里没有 GK3_MISC_SHA256 —— 镜像里没编进 gk3-misc（安装会在动盘前失败）"
[ -f "$L/gaokun3-windows/release.txt" ] && cmp -s "$L/release.txt" "$L/gaokun3-windows/release.txt" || die "Windows 安装包里的 release.txt 不对"
# 开发用的东西不许混进发布版
! grep -rqs 'gaokun3 M0' "$L/.entries" || die "带着 M0 的变体启动项（.entries 不空）"
ok "版本、提交、内核来源都对（= 发布清单里的 boot.img ${WANT:0:16}，G11）；带着 gk3-misc；没有 M0 变体"

say "3. 打包 → $OUT"
rm -rf "$OUT"; mkdir -p "$OUT"
B=gaokun3-installer-$VER
cp "$L/release.txt" "$OUT/$B-release.txt"
xz -T0 -6 -c "$L/gaokun3-live.img" > "$OUT/$B-usb.img.xz"
xz -t "$OUT/$B-usb.img.xz" || die "xz 自检没过"
cp "$L/gaokun3-windows.zip" "$OUT/$B-windows.zip"
# ★ v1.0 计划 INST-6：命令行安装要救援系统，得在发布目录里放 rescue.squashfs + initramfs.img —— 原先哪个 Release 都不带。
#   发的就是 U 盘上那一份（live 镜像本身，不带 --with-rescue，理由见文件头）。名字不带版本前缀：installer-lib 只认这两个名字
#   （gk3__find_rescue_squashfs），下载下来直接放进发布目录就行；与 ROM 的附件（boot.img、super.img.zst…）也不撞名。
cp "$L/gaokun3-live.squashfs" "$OUT/rescue.squashfs"
cp "$L/initramfs.img" "$OUT/initramfs.img"
# dd 之后要核对的是【解压后】的镜像，所以两个都列
IMG_SHA=$(shasum -a 256 "$L/gaokun3-live.img" | cut -d' ' -f1)
# ⚠️ GitHub 上附件名就是文件名（"文件#标签" 的 # 后面只是显示标签）—— 所以直接叫带版本的名字，不和 ROM 的清单撞
( cd "$OUT" && shasum -a 256 "$B-usb.img.xz" "$B-windows.zip" "$B-release.txt" rescue.squashfs initramfs.img > "$B-SHA256SUMS"
  echo "$IMG_SHA  $B-usb.img" >> "$B-SHA256SUMS" )
ls -la "$OUT"; cat "$OUT/$B-SHA256SUMS"
FILES=("$B-usb.img.xz" "$B-windows.zip" "$B-release.txt" rescue.squashfs initramfs.img "$B-SHA256SUMS")

# R2（国内下得到；GitHub 的下载服务器在国内常常连不上）：installer/<安装器版本>/<文件>，与网络安装读的
# installer/variants.txt 同一个前缀（installer-lib.sh 的 GK3_MANIFEST_URL）。域名与桶与 scripts/release.sh 相同。
R2_BUCKET=${BUCKET:-gaokun-android}
R2_HOST=${HOST:-https://ota.072172.xyz}
r2_links() { local f; for f in "${FILES[@]}"; do echo "  $R2_HOST/installer/$VER/$f"; done; }

if [ -z "$UPLOAD" ] && [ -z "$R2" ]; then
    cat <<EOF

造好了，没有上传（⚠️ 发布要用户点头）。
  传到 GitHub release ${ROM}：bash scripts/live/release-installer.sh --boot-img $BOOTIMG --rom $ROM --firmware $FW --upload $ROM
  再加 --r2（要 R2_ENDPOINT / R2_ACCESS_KEY_ID / R2_SECRET_ACCESS_KEY）同时传 R2，传完的链接会是：
$(r2_links)
EOF
    exit 0
fi

if [ -n "$R2" ]; then
    say "4a. 上传到 R2（桶 ${R2_BUCKET}，installer/$VER/）"
    for v in R2_ENDPOINT R2_ACCESS_KEY_ID R2_SECRET_ACCESS_KEY; do
        [ -n "${!v:-}" ] || die "环境变量 $v 未设置（凭据只从环境变量读）"
    done
    U=$REPO/scripts/r2-upload.py
    # REL-13 同一条规矩：已有同名对象且字节不同 ⇒ 一个都不传（已经贴出去的链接不许悄悄换内容）
    for f in "${FILES[@]}"; do
        rc=0; python3 "$U" --check "$R2_BUCKET" "$OUT/$f" "installer/$VER/$f" || rc=$?
        [ "$rc" = 0 ] || [ "${GK3_R2_OVERWRITE:-}" = 1 ] || die "R2 上 installer/$VER/$f 已有、内容不同（--check 退出码 ${rc}）—— 别覆盖已发布的链接；确要覆盖设 GK3_R2_OVERWRITE=1"
    done
    for f in "${FILES[@]}"; do
        case "$f" in *.txt|*SHA256SUMS) ct="text/plain; charset=utf-8" ;; *.zip) ct=application/zip ;; *) ct=application/octet-stream ;; esac
        python3 "$U" "$R2_BUCKET" "$OUT/$f" "installer/$VER/$f" "$ct" || die "传 $f 到 R2 失败"
    done
    # 判据看服务器上的东西，不看上传命令的输出。不能拿 --check 当判据：对象不存在它也返回 0。
    #   经公开域名 HEAD 一次，Content-Length 要等于本地字节数（与 release.sh 发完让人用 curl -sI 验是同一个意思）
    for f in "${FILES[@]}"; do
        want=$(wc -c < "$OUT/$f" | tr -d ' ')
        got=$(curl -fsI --max-time 30 "$R2_HOST/installer/$VER/$f" | tr -d '\r' | awk 'tolower($1) == "content-length:" {print $2}' | tail -1)
        [ "$got" = "$want" ] && ok "${f}：$R2_HOST 上 $got 字节" || die "${f}：$R2_HOST 上是 ${got:-取不到}，本地 $want"
    done
    echo "R2 链接（贴进 INSTALL 的 Downloads 表与发版说明的 Files 表）："
    r2_links
fi
[ -n "$UPLOAD" ] || exit 0

say "4. 上传到 GitHub release $UPLOAD"
gh release view "$UPLOAD" >/dev/null || die "GitHub 上没有 release ${UPLOAD}（先发 ROM：release.sh --no-build）"
gh release upload "$UPLOAD" --clobber "${FILES[@]/#/$OUT/}" || die "gh release upload 失败"
# 判据看服务器上的字节数，不看 gh 的输出（gh release upload 报过 uploaded 而什么都没传）
for f in "${FILES[@]}"; do
    want=$(wc -c < "$OUT/$f" | tr -d ' ')
    got=$(gh release view "$UPLOAD" --json assets --jq ".assets[] | select(.name == \"$f\") | .size")
    [ "$got" = "$want" ] && ok "${f}：服务器上 $got 字节" || die "${f}：服务器上是 ${got:-没有}，本地 $want"
done
echo "GitHub 链接：https://github.com/vahiru/gaokun-android/releases/tag/$UPLOAD"
[ -z "$R2" ] || { echo "R2 链接："; r2_links; }
