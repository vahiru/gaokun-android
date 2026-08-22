#!/usr/bin/env bash
#
# 发版：一次构建 → 断言一致 → 产物先传、清单最后传。
#
# 这个脚本存在的理由是它包含的三条断言，每一条都对应一次真实事故：
#
#  1. ★ 一次 `m bacon superimage`，不是两次调用。
#     `bacon` 与 `superimage` 分开跑会得到两个 build stamp，于是 OTA 包与
#     安装用的 super 互不相认（对 Updater 甚至构成降级）。
#  2. ★ 清单的 `timestamp` 必须【等于】该构建的 `ro.build.date.utc`。
#     `createjson.sh` 有时填的是打包时刻；那会让装上此版本后 Updater
#     **永远显示"有更新"**，而那正是同一个版本。
#     判据在 `UpdatesRepository.kt:113-115`（`==` 视为当前、`<` 视为更旧）。
#  3. ★ 顺序：产物先传，清单最后传。否则中间状态的清单会指向不存在的文件。
#
# 还有两条不是断言但同样致命的，写在这里免得再犯：
#  * 抓清单的客户端设了 `.followRedirects(false)`，任何 3xx 直接失败。
#    别在 OTA 主机名上加 Redirect / Page Rule —— 报错只有
#    `Unexpected HTTP status: 301`，绝对想不到是那条规则。
#  * 凭据只从环境变量读，且**只送 S3 密钥上构建机**。账户级 API 令牌能改
#    DNS、删桶，绝不能出现在构建机上。
#
# 用法（在构建机上跑）：
#   R2_ENDPOINT=... R2_ACCESS_KEY_ID=... R2_SECRET_ACCESS_KEY=... \
#     scripts/release.sh [--dry-run] [--stage-only]
#
#   --dry-run     只构建与校验，不上传
#   --stage-only  产物传到 staging/<ver>/ 而【不更新】ota/gaokun3.json。
#                 用来在自己机器上验一版而不惊动任何用户 —— 发布是对外动作，
#                 应该是显式的一步。
#   --no-build    跳过构建，直接用 out/ 里现成的产物。
#                 ★ 这个选项是必需的，不是方便：build stamp 每次构建都会变，
#                 所以"重跑一次构建再发版"发出去的**不是**你在硬件上验过的那一版。
#                 流程应该是：构建一次 → 装到机器上验 → 用 --no-build 发那一版。
#                 （2026-08-21 亲自踩过：我在设备上验了 1787246871，
#                   然后跑 release.sh 又构建了一次，staging 里成了 1787247612。）

set -euo pipefail

BUCKET=${BUCKET:-gaokun-android}
HOST=${HOST:-https://ota.072172.xyz}
UPLOAD=${UPLOAD:-$HOME/r2-upload.py}
DRY=0; STAGE_ONLY=0; NO_BUILD=0
for a in "$@"; do
    case "$a" in
        --dry-run)    DRY=1 ;;
        --stage-only) STAGE_ONLY=1 ;;
        --no-build)   NO_BUILD=1 ;;
        *) echo "未知参数: $a" >&2; exit 2 ;;
    esac
done

die() { echo "✗ $*" >&2; exit 1; }
ok()  { echo "✓ $*"; }

[ -n "${ANDROID_BUILD_TOP:-}" ] || die "先 source build/envsetup.sh && lunch"
cd "$ANDROID_BUILD_TOP"
OUT=${OUT:-$ANDROID_BUILD_TOP/out/target/product/gaokun3}

if [ "$NO_BUILD" = 1 ]; then
    echo "═══ 1. --no-build：用 out/ 里现成的产物（发的就是验过的那一版）═══"
else
    echo "═══ 1. 一次构建（bacon 与 superimage 必须同一次调用）═══"
    m -j"$(nproc)" bacon superimage
fi

echo "═══ 2. 断言 ═══"
ZIP=$(ls -t "$OUT"/crDroidAndroid-*.zip 2>/dev/null | head -1)
[ -n "$ZIP" ] || die "找不到 OTA zip —— bacon 没产出？"
[ -f "$OUT/super.img" ] || die "找不到 super.img"
[ -f "$OUT/gaokun3.json" ] || die "找不到 gaokun3.json（createjson.sh 没跑）"

UTC=$(sed -n 's/^ro\.build\.date\.utc=//p' "$OUT/system/build.prop" | head -1)
[ -n "$UTC" ] || die "读不到 ro.build.date.utc"
JTS=$(sed -n 's/.*"timestamp"[^0-9]*\([0-9]\+\).*/\1/p' "$OUT/gaokun3.json" | head -1)
[ "$UTC" = "$JTS" ] \
    || die "清单 timestamp=$JTS ≠ ro.build.date.utc=$UTC —— 装上后 Updater 会永远显示有更新"
ok "构建戳一致：$UTC"

# super 与 zip 必须同期。两者不同源时（分两次调用构建）这里通常就能看出来。
SUPER_UTC=$(stat -c %Y "$OUT/super.img"); ZIP_UTC=$(stat -c %Y "$ZIP")
DIFF=$(( SUPER_UTC > ZIP_UTC ? SUPER_UTC - ZIP_UTC : ZIP_UTC - SUPER_UTC ))
[ "$DIFF" -lt 3600 ] || die "super.img 与 zip 相差 ${DIFF}s —— 多半不是同一次构建"
ok "super.img 与 OTA zip 同期（相差 ${DIFF}s）"

# boot.img 里的内核必须就是设备树里那个预编译内核
if [ -f "$OUT/boot.img" ] && [ -f device/huawei/gaokun3/prebuilt-boot/vmlinuz.efi ]; then
    K=$(python3 - "$OUT/boot.img" <<'PY'
import struct, sys, hashlib
f = open(sys.argv[1], "rb"); d = f.read(4096)
assert d[:8] == b"ANDROID!"
ks, ka, rs, ra, ss, sa, tags, page, hv = struct.unpack("<9I", d[8:44])
f.seek(page); print(hashlib.sha256(f.read(ks)).hexdigest())
PY
)
    V=$(sha256sum device/huawei/gaokun3/prebuilt-boot/vmlinuz.efi | cut -d' ' -f1)
    [ "$K" = "$V" ] || die "boot.img 里的 kernel 与 prebuilt-boot/vmlinuz.efi 不同"
    ok "boot.img 的 kernel 与 prebuilt 内核逐字节相同"

    # ★ boot.img 里的 DTB 必须【只有一个】FDT。
    #   BOARD_PREBUILT_DTBIMAGE_DIR 会把目录里【所有】*.dtb 拼接起来 ——
    #   目录里留一个陈旧文件，产出的就是两份 DTB 首尾相连，而构建全程不报一声。
    #   2026-08-22 真踩过：346052 字节 = 恰好 2 × 173026，靠"大小是整数倍"才看出来。
    #   后果取决于消费者读不读第二个，属于那种"这次没炸不代表下次不炸"的隐患。
    NF=$(python3 - "$OUT/boot.img" <<'PY'
import struct, sys
f = open(sys.argv[1], "rb"); d = f.read(4096)
ks, ka, rs, ra, ss, sa, tags, page, hv = struct.unpack("<9I", d[8:44])
if hv < 2:
    print(1); sys.exit()                      # header v0/v1 不带 dtb 段
dtb_size = struct.unpack("<I", d[1648:1652])[0]
def pad(n): return (n + page - 1) // page * page
off = pad(1) + pad(ks) + pad(rs) + pad(ss) + pad(struct.unpack("<I", d[1632:1636])[0])
f.seek(off)
print(f.read(dtb_size).count(bytes.fromhex("d00dfeed")))
PY
)
    [ "$NF" = 1 ] || die "boot.img 的 dtb 段里有 $NF 个 FDT —— prebuilt-boot/dtb/ 多半留了陈旧文件"
    ok "boot.img 的 dtb 段只含 1 个 FDT"
fi

VER=$(basename "$ZIP" .zip)
echo "═══ 3. 打包安装产物 ═══"
S=$(mktemp -d); trap 'rm -rf "$S"' EXIT
cp "$ZIP" "$OUT/gaokun3.json" "$S/"
[ -f "$OUT/boot.img" ] && cp "$OUT/boot.img" "$S/"
zstd -T0 -19 --long -f "$OUT/super.img" -o "$S/super.img.zst"
( cd "$S" && sha256sum boot.img super.img.zst "$(basename "$ZIP")" > install-artifacts.sha256 )
ls -la "$S"

[ "$DRY" = 1 ] && { ok "--dry-run：到此为止，未上传"; exit 0; }

for v in R2_ENDPOINT R2_ACCESS_KEY_ID R2_SECRET_ACCESS_KEY; do
    [ -n "${!v:-}" ] || die "环境变量 $v 未设置（凭据只从环境变量读）"
done

if [ "$STAGE_ONLY" = 1 ]; then
    echo "═══ 4. 传到 staging/$VER/（不更新清单，没有用户会收到）═══"
    for f in boot.img super.img.zst install-artifacts.sha256 gaokun3.json "$(basename "$ZIP")"; do
        python3 "$UPLOAD" "$BUCKET" "$S/$f" "staging/$VER/$f" application/octet-stream
    done
    ok "已 staging。要发布，重跑本脚本不带 --stage-only"
    exit 0
fi

echo "═══ 4. 上传：★产物先传，清单【最后】传 ═══"
python3 "$UPLOAD" "$BUCKET" "$S/$(basename "$ZIP")" "builds/$(basename "$ZIP")" application/zip
python3 "$UPLOAD" "$BUCKET" "$S/install-artifacts.sha256" "install/$VER/install-artifacts.sha256" text/plain
for f in boot.img super.img.zst; do
    python3 "$UPLOAD" "$BUCKET" "$S/$f" "install/$VER/$f" application/octet-stream
done
ok "产物已就位"

# 清单最后传，且 download 指向刚上传的那个 zip
python3 - "$S/gaokun3.json" "$(basename "$ZIP")" "$HOST" <<'PY'
import json, sys
path, zipname, host = sys.argv[1], sys.argv[2], sys.argv[3]
d = json.load(open(path))
r = d["response"][0]
r["download"] = "%s/builds/%s" % (host, zipname)
json.dump(d, open(path, "w"), indent=2)
print("  download -> %s" % r["download"])
PY
python3 "$UPLOAD" "$BUCKET" "$S/gaokun3.json" "ota/gaokun3.json" application/json
ok "清单已发布 —— 设备端「系统更新」现在能看到 $VER"

cat <<EOF

发布完成。剩下要人做的：
  * 用【设备】而不是构建机去验一次抓取（沙箱会挡出站 HTTP，那边的结论不可信）：
      curl -sI $HOST/ota/gaokun3.json | head -3      # 必须 200，不能是 3xx
  * GitHub Release 另发（gh release create），把 install/$VER/ 那几个文件带上。
EOF
