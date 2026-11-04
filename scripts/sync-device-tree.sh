#!/usr/bin/env bash
# 把本仓的 device/huawei/gaokun3/ 同步到构建机的 crdroid 树，并断言构建机上
# 【不在版本库里、但构建必需】的输入仍然在。
#
#   bash scripts/sync-device-tree.sh <构建机 IP 或主机>
#
# ★ 这个脚本存在的理由（2026-09-16，B0 第 6 咬，#116 §15）：
#   `rsync -a --delete device/huawei/gaokun3/ vm:~/crdroid/device/huawei/gaokun3/`
#   看起来是"让构建机的树就是本仓 checkout"的正确做法 —— 而它把构建机上
#   **四样被 .gitignore 挡在公开仓之外、却是构建必需**的东西全删了：
#       adb_keys                 开发机 adb 公钥（个人密钥）          → soong panic，构建 42 秒就死
#       firmware/**              华为专有 .mbn + linux-firmware 那 18 个 → 显式 COPY_FILES，构建会报错
#       hexagonrpcd-root/**      SLPI 传感器 VFS 根（34 个）           → 一半是 wildcard，缺了【静默消失】
#       prebuilt-boot/**         内核与 DTB                             → 本机会产出，可同步
#   更糟的是事后那道"127 个文件逐字节一致"的 md5 核对**通过了** —— 因为两边一样地缺，
#   我拿了一个不完整的本机树当参照。**参照物必须是"构建需要什么"，不是"本机有什么"。**
#   恢复靠的是设备上 /vendor 里实际装进去的那一套（README 里写着的路）+ 上次构建的 out/。
#
# ★ 2026-09-26 第五样：effects/prebuilt/**（扬声器增强试验功能的 Histen 引擎，华为专有，PR #7）。
#   device.mk 对它用 wildcard —— 缺了构建【照样通过】，ROM 里只是悄悄没了 Histen，
#   正是上面那种"静默消失"。所以同样排除在 --delete 之外、单独同步、按 sha256 断言。
#   确实不想带它构建时（例如别人的构建机）：GK3_ALLOW_NO_HISTEN=1。
#
# ★ 2026-11-03 第六样：micarray/prebuilt/**（内置麦克风阵列的系数文件，华为Windows驱动里的数据）。
#   与Histen引擎同一套处理：排除在 --delete 之外、单独同步、按sha256断言。缺了构建照样通过，
#   ROM里的阵列只是不接管录音。确实不想带它构建时：GK3_ALLOW_NO_MICARRAY=1。
#   阵列还要构建机上的拓扑改成允许四路（firmware/不同步，第3b1b步核对它的sha256，同一个开关跳过）。
#
# ★ 2026-11-03 第七样：denoise/prebuilt/**（麦克风降噪服务要的QNN运行库、头文件和CDSP外壳，高通的东西）。
#   不入库、也不转手，同步前由scripts/fetch-denoise-blobs.py从公开来源取（第0b步），排除在 --delete 之外、
#   单独同步、按sha256断言。【没有跳过的开关】：ROM里一定要有降噪服务（用户11-03定），缺了就停。
#
# ★ 2026-10-04（B1）：adb_keys 只有开发构建（GAOKUN3_DEV_BUILD=1）才用 —— 发布构建不设
#   PRODUCT_ADB_KEYS（见 lineage_gaokun3.mk），缺了它照样能编。所以第 3 步只在本机环境里
#   GAOKUN3_DEV_BUILD=1 时才要求它在；平时只报大小。它仍排除在 --delete 之外（留给开发构建）。
set -euo pipefail
HOST=${1:?用法: $0 <构建机 IP>}
SSH="ssh -o StrictHostKeyChecking=no -o BatchMode=yes"
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SRC=$REPO/device/huawei/gaokun3
# ⚠️ 不能写 …:~/crdroid：bash 对赋值里 ":" 后面的 "~" 做波浪号展开（PATH 风格），
#   远端会拿到【本机】的 home 路径。用相对远端 home 的路径。
DST=vahiru@$HOST:crdroid/device/huawei/gaokun3
die() { echo "✗ $*" >&2; exit 1; }
ok()  { echo "✓ $*"; }

echo "═══ 0. 同步之前：设备树里每个 .xml 都必须能解析 ═══"
# ★ 2026-09-24：PR #6 的 features xml 少了一个注释结尾，整包构建到 systemfeatures-gen-tool
#   才报错（构建开始后约 2 分钟，而且只有整包构建会碰它 —— 单编 HAL 发现不了）。
#   在这里 1 秒内拦下；坏的 XML 同步过去只会浪费一次构建。
python3 - "$SRC" <<'PY' || die "设备树里有解析不了的 XML（见上），先修再同步"
import sys, pathlib, xml.etree.ElementTree as ET
bad = 0
for p in sorted(pathlib.Path(sys.argv[1]).rglob("*.xml")):
    if any(part in ("firmware", "hexagonrpcd-root", "prebuilt-boot") for part in p.parts):
        continue
    try:
        ET.parse(p)
    except ET.ParseError as e:
        print("  ✗ %s: %s" % (p.relative_to(sys.argv[1]), e))
        bad += 1
sys.exit(1 if bad else 0)
PY
ok "设备树 XML 全部可解析"

echo "═══ 0b. 麦克风降噪服务要的高通文件（denoise/prebuilt）：不全就从公开来源取 ═══"
# 见denoise/prebuilt/README.md。取CDSP外壳要cabextract（brew install cabextract）。
python3 -I "$REPO/scripts/fetch-denoise-blobs.py" --check >/dev/null ||
    python3 -I "$REPO/scripts/fetch-denoise-blobs.py" || die "降噪服务要的高通文件取不全（见上）"
ok "denoise/prebuilt 齐全"

echo "═══ 1. 同步受版本控制的部分（--delete 但排除四样不入库的构建输入）═══"
# ★ 三个目录里的 README.md 受版本控制（第 4 步要逐字节对照），必须先 --include 放行 ——
#   rsync 取第一条匹配的规则。2026-09-26 踩到：firmware/README.md 09-24 改过，而
#   'firmware/**' 把它也挡住了，第 4 步于是永远报差异、同步本身又永远修不好它。
rsync -a --delete \
      --exclude '._*' --exclude '.DS_Store' \
      --include 'firmware/README.md' --include 'hexagonrpcd-root/README.md' --include 'prebuilt-boot/README.md' \
      --include 'prebuilt-gk3boot/README.md' \
      --exclude 'adb_keys' --exclude 'firmware/**' --exclude 'hexagonrpcd-root/**' --exclude 'prebuilt-boot/**' \
      --exclude 'prebuilt-gk3boot/**' \
      --exclude 'effects/prebuilt/**' \
      --exclude 'micarray/prebuilt/**' \
      --exclude 'denoise/prebuilt/**' \
      --exclude '/gk3core/' \
      --exclude 'prebuilt-apps/fcitx5/*.apk' \
      -e "$SSH" "$SRC/" "$DST/"
ok "device/ 已同步（不动 adb_keys / firmware / hexagonrpcd-root / prebuilt-boot / prebuilt-gk3boot / effects/prebuilt / micarray/prebuilt / denoise/prebuilt / gk3core / fcitx5 APK）"
# fcitx5 APK 不入库（.gitignore）。2026-10-25 踩到：从一个新 worktree 同步时，本机没有这个 APK，
#   上面的 --delete 把构建机上那份删了（第 3b2 步才发现）。所以它也排除在 --delete 之外，本机有就单独补过去。
if [ -f "$SRC/prebuilt-apps/fcitx5/fcitx5-android-arm64-v8a.apk" ]; then
    rsync -a -e "$SSH" "$SRC/prebuilt-apps/fcitx5/fcitx5-android-arm64-v8a.apk" "$DST/prebuilt-apps/fcitx5/"
fi

echo "═══ 2. prebuilt-boot 单独同步，【不带 --delete】═══"
if [ -f "$SRC/prebuilt-boot/vmlinuz.efi" ]; then
    rsync -a --exclude '._*' --exclude '.DS_Store' -e "$SSH" "$SRC/prebuilt-boot/" "$DST/prebuilt-boot/"
    ok "prebuilt-boot 已同步（$(shasum -a 256 "$SRC/prebuilt-boot/vmlinuz.efi" | cut -c1-16)）"
else
    echo "· 本机没有 prebuilt-boot/vmlinuz.efi，保留构建机上的那份"
fi

# Fingerprint candidates require three ignored inputs: the TA, the Goodix
# configuration template and the fingerprint kernel. This branch is only
# reached when explicitly enabled; no device connection is made by this script.
if [ "${GAOKUN3_FINGERPRINT:-false}" = true ]; then
    FP_TA="$SRC/firmware/fingerpr.mbn"
    FP_CFG="$SRC/firmware/gaokun3-goodix-cfg.bin"
    [ -f "$FP_TA" ] || die "指纹 TA 缺失：先运行 scripts/fingerprint/prepare-crdroid.py"
    [ "$(shasum -a 256 "$FP_TA" | cut -d' ' -f1)" = b081543c7b6ae4360969007836747601e8c48ec48a8335a5479e45ec0e34355a ] || die "指纹 TA 哈希错误"
    [ -f "$FP_CFG" ] || die "Goodix 配置模板缺失：先运行 scripts/fingerprint/prepare-crdroid.py --gfspi"
    [ "$(shasum -a 256 "$FP_CFG" | cut -d' ' -f1)" = 273537378e759cc19b829c6c5a3b5226da603b55f96be6bcdf9a2e66c6c26725 ] || die "Goodix 配置模板哈希错误"
    (cd "$SRC/prebuilt-boot" && shasum -a 256 -c fingerprint-kernel.sha256) || die "指纹候选内核缺失或已变化"
    rsync -a -e "$SSH" "$FP_TA" "$FP_CFG" "$DST/firmware/"
    $SSH "vahiru@$HOST" 'cd ~/crdroid/device/huawei/gaokun3 &&
      test "$(sha256sum firmware/fingerpr.mbn | cut -d" " -f1)" = b081543c7b6ae4360969007836747601e8c48ec48a8335a5479e45ec0e34355a &&
      test "$(sha256sum firmware/gaokun3-goodix-cfg.bin | cut -d" " -f1)" = 273537378e759cc19b829c6c5a3b5226da603b55f96be6bcdf9a2e66c6c26725 &&
      cd prebuilt-boot && sha256sum -c fingerprint-kernel.sha256' || die "构建机指纹输入校验失败"
fi

echo "═══ 2b. effects/prebuilt（Histen 引擎）单独同步，【不带 --delete】═══"
# README.md 也在这个目录里、且受版本控制 —— 第 1 步把整个目录排除了，所以它也靠这一步过去。
rsync -a --exclude '._*' --exclude '.DS_Store' -e "$SSH" "$SRC/effects/prebuilt/" "$DST/effects/prebuilt/"
if [ -f "$SRC/effects/prebuilt/lib64/soundfx/libhw_histen_processing.so" ]; then
    ok "effects/prebuilt 已同步（含 Histen 引擎）"
else
    echo "· 本机没有 Histen 引擎，保留构建机上的那份（若有）"
fi

echo "═══ 2b1. micarray/prebuilt（麦克风阵列的系数文件）单独同步，【不带 --delete】═══"
# 与2b同理：README.md受版本控制，靠这一步过去。
rsync -a --exclude '._*' --exclude '.DS_Store' -e "$SSH" "$SRC/micarray/prebuilt/" "$DST/micarray/prebuilt/"
if [ -f "$SRC/micarray/prebuilt/hdrec_bf_data.bin" ]; then
    ok "micarray/prebuilt 已同步（含系数文件）"
else
    echo "· 本机没有麦克风阵列的系数文件，保留构建机上的那份（若有）"
fi

echo "═══ 2b2. denoise/prebuilt（麦克风降噪服务要的高通文件）单独同步 ═══"
# 第0b步核对过本机这份，构建机上的只是它的镜像：--delete 让换版本后旧文件不留下，--checksum 不信大小和时间。
rsync -a --delete --checksum --exclude '._*' --exclude '.DS_Store' -e "$SSH" "$SRC/denoise/prebuilt/" "$DST/denoise/prebuilt/"
ok "denoise/prebuilt 已同步"

echo "═══ 2c. libgk3core（tools/gk3boot/core）→ 构建机的 device/huawei/gaokun3/gk3core ═══"
# ★ 2026-10-05（统一启动入口 S9）：boot_control HAL 链接 libgk3core（读写 misc 的 GK3 记录），而它的源码与
#   Android.bp 在本仓的 tools/gk3boot/core/ —— 构建机的 crDroid 树里没有 tools/，Soong 看不见。
#   所以整目录拷进设备树的 gk3core/（本仓 checkout 里【没有】这个目录，第 1 步也把它排除在 --delete 之外）。
#   这里用 --delete：gk3core/ 在构建机上只是 tools/gk3boot/core 的镜像，删掉的源文件也要跟着删。
CORE=$REPO/tools/gk3boot/core
[ -f "$CORE/Android.bp" ] && [ -f "$CORE/include/gk3core.h" ] || die "$CORE 不全（缺 Android.bp 或 include/gk3core.h）"
rsync -a --delete --exclude '._*' --exclude '.DS_Store' -e "$SSH" "$CORE/" "$DST/gk3core/"
CL=$(cd "$CORE" && find . -type f ! -name '._*' ! -name .DS_Store | LC_ALL=C sort | xargs md5 -r | awk '{print $1"  "$2}')
CR=$($SSH "vahiru@$HOST" 'cd ~/crdroid/device/huawei/gaokun3/gk3core && find . -type f | LC_ALL=C sort | xargs md5sum')
[ "$CL" = "$CR" ] || { echo "本机："; echo "$CL"; echo "构建机："; echo "$CR"; die "构建机的 gk3core/ ≠ tools/gk3boot/core/"; }
ok "gk3core/ 与 tools/gk3boot/core/ 逐字节一致（$(echo "$CL" | wc -l | tr -d ' ') 个文件）"

echo "═══ 2d. prebuilt-gk3boot（统一启动入口 gk3boot.efi + version [+ 执行端 fastboot.img]）单独同步，【不带 --delete】═══"
# ★ 2026-10-05（S9）：与 prebuilt-boot 同一个规矩 —— 二进制不入库，本机有就带过去，没有就保留构建机上的那份；
#   device.mk 对它用 wildcard（缺了构建照样通过、ROM 里只是没有入口 = 又一种"静默消失"）⇒ 第 3c 步断言。
#   执行端 fastboot.img 在同一个目录里，整目录 rsync 一并带过去（不带 --delete：本机这次没放它时，构建机上
#   旧的那份会留着 —— 第 3c 步把它的 sha256 打出来，与 version 对不上的旧执行端要人看一眼）。
if [ -f "$SRC/prebuilt-gk3boot/gk3boot.efi" ]; then
    rsync -a --exclude '._*' --exclude '.DS_Store' -e "$SSH" "$SRC/prebuilt-gk3boot/" "$DST/prebuilt-gk3boot/"
    ok "prebuilt-gk3boot 已同步（版本 $(cat "$SRC/prebuilt-gk3boot/version" 2>/dev/null || echo '<缺 version>')，sha256 $(shasum -a 256 "$SRC/prebuilt-gk3boot/gk3boot.efi" | cut -c1-16)，执行端 $( [ -f "$SRC/prebuilt-gk3boot/fastboot.img" ] && shasum -a 256 "$SRC/prebuilt-gk3boot/fastboot.img" | cut -c1-16 || echo '本机没有')）"
else
    echo "· 本机没有 prebuilt-gk3boot/gk3boot.efi，保留构建机上的那份（若有）"
fi

echo "═══ 3. 断言：构建机上不入库的构建输入都在 ═══"
# adb_keys 的下限：开发构建要 >500 字节（一把 RSA 公钥约 720），发布构建不要求（0）。
if [ "${GAOKUN3_DEV_BUILD:-}" = 1 ]; then AK_MIN=500; AK_WHAT="adb_keys（开发构建）"
else AK_MIN=-1; AK_WHAT="adb_keys 不要求（发布构建；开发构建设 GAOKUN3_DEV_BUILD=1 再跑）"; fi
$SSH "vahiru@$HOST" "AK_MIN=$AK_MIN; "'cd ~/crdroid/device/huawei/gaokun3
  fw=$(find firmware -type f ! -name README.md ! -name fingerpr.mbn ! -name gaokun3-goodix-cfg.bin 2>/dev/null | wc -l)
  hx=$(find hexagonrpcd-root -type f ! -name README.md 2>/dev/null | wc -l)
  ak=$( (wc -c < adb_keys) 2>/dev/null || echo 0)
  dtb=$(ls prebuilt-boot/dtb/*.dtb 2>/dev/null | wc -l)
  echo "firmware=$fw hexagonrpcd=$hx adb_keys=${ak}B dtb=$dtb"
  [ "$fw" -eq 18 ] && [ "$hx" -eq 34 ] && [ "$ak" -gt "$AK_MIN" ] && [ "$dtb" -eq 1 ] && [ -f prebuilt-boot/vmlinuz.efi ]' \
  | tee /dev/stderr | tail -1 >/dev/null || die "构建机上缺构建必需的输入 —— 见上一行；恢复方法在 firmware/README.md 与 hexagonrpcd-root/README.md"
ok "18 个固件 · 34 个 hexagonrpcd 文件 · 1 个 dtb · vmlinuz.efi 都在 · $AK_WHAT"

echo "═══ 3b. 断言：Histen 引擎在、且是核对过的那一份 ═══"
# 期望值与来源见 effects/prebuilt/README.md。
HISTEN_SHA=338b774e70feeccd7718f9d2f25c5bfddf8254ddd2623fa25a778ffcaaf38a2e
got=$($SSH "vahiru@$HOST" 'sha256sum ~/crdroid/device/huawei/gaokun3/effects/prebuilt/lib64/soundfx/libhw_histen_processing.so 2>/dev/null | cut -d" " -f1' || true)
if [ "$got" = "$HISTEN_SHA" ]; then
    ok "Histen 引擎在（${HISTEN_SHA:0:16}）"
elif [ "${GK3_ALLOW_NO_HISTEN:-0}" = 1 ]; then
    echo "· Histen 引擎不在或不符（${got:-缺失}），GK3_ALLOW_NO_HISTEN=1 ⇒ 这一版的扬声器增强只有扬声器链"
else
    if [ -n "$got" ]; then why="sha256 不符（${got}）"; else why="缺失"; fi
    die "构建机上的 Histen 引擎$why —— 见 effects/prebuilt/README.md；确实不要它就设 GK3_ALLOW_NO_HISTEN=1"
fi

echo "═══ 3b1. 断言：麦克风阵列的系数文件在、且是核对过的那一份 ═══"
# 期望值与来源见 micarray/prebuilt/README.md。
MICARRAY_SHA=34a5fb58b694703c1db7b70f01b2cc56fc5283109ca9d3be4b8d6207a77e1654
got=$($SSH "vahiru@$HOST" 'sha256sum ~/crdroid/device/huawei/gaokun3/micarray/prebuilt/hdrec_bf_data.bin 2>/dev/null | cut -d" " -f1' || true)
if [ "$got" = "$MICARRAY_SHA" ]; then
    ok "麦克风阵列的系数文件在（${MICARRAY_SHA:0:16}）"
elif [ "${GK3_ALLOW_NO_MICARRAY:-0}" = 1 ]; then
    echo "· 麦克风阵列的系数文件不在或不符（${got:-缺失}），GK3_ALLOW_NO_MICARRAY=1 ⇒ 这一版的内置麦按原来的两路录音"
else
    if [ -n "$got" ]; then why="sha256 不符（${got}）"; else why="缺失"; fi
    die "构建机上的麦克风阵列系数文件$why —— 见 micarray/prebuilt/README.md；确实不要它就设 GK3_ALLOW_NO_MICARRAY=1"
fi

echo "═══ 3b1b. 断言：拓扑允许四路采集（麦克风阵列要它） ═══"
# firmware/ 不同步，拓扑要在构建机的树里用 micarray/tplg-four-channel-capture.py 改过（micarray/README.md 第六节）。
# 没改时构建照样通过，ROM 里的阵列、通话的回声消除和降噪却每次录音都悄悄不接管。
TPLG_FOUR_SHA=8f5f040b181eab482576b8c48c2bd5d55d8c9b4dfc364bc7f067fc07e3e10765
TPLG_TWO_SHA=ee911c748deadc0191af50074b26ad177c8a2e78ff00e9578504b71d5d27b08a
TPLG=device/huawei/gaokun3/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin
got=$($SSH "vahiru@$HOST" "sha256sum ~/crdroid/$TPLG 2>/dev/null | cut -d' ' -f1" || true)
if [ "$got" = "$TPLG_FOUR_SHA" ]; then
    ok "拓扑允许四路采集（${TPLG_FOUR_SHA:0:16}）"
elif [ "${GK3_ALLOW_NO_MICARRAY:-0}" = 1 ]; then
    echo "· 拓扑不是四路那份（${got:-缺失}），GK3_ALLOW_NO_MICARRAY=1 ⇒ 这一版的内置麦按原来的两路录音"
elif [ "$got" = "$TPLG_TWO_SHA" ]; then
    die "构建机上的拓扑还是原来的两路版 —— 在构建机的 ~/crdroid 里跑一次 python3 device/huawei/gaokun3/micarray/tplg-four-channel-capture.py $TPLG（micarray/README.md 第六节）；确实不要阵列就设 GK3_ALLOW_NO_MICARRAY=1"
else
    if [ -n "$got" ]; then why="是没见过的版本（${got}）"; else why="缺失"; fi
    die "构建机上的拓扑$why —— 见 firmware/README.md 与 micarray/README.md 第六节；确实不要阵列就设 GK3_ALLOW_NO_MICARRAY=1"
fi

echo "═══ 3b1a. 断言：降噪服务要的高通文件在构建机上、且是钉住的那几份 ═══"
# 本机这份第0b步核对过（值钉在fetch-denoise-blobs.py里）。两边逐个文件算sha256比整张清单：头文件、FastRPC源码、
# 多出来的文件都算在内。
[ -f "$SRC/denoise/prebuilt/SHA256SUMS" ] || die "本机没有 denoise/prebuilt/SHA256SUMS —— 先跑 python3 -I scripts/fetch-denoise-blobs.py"
want=$(cd "$SRC/denoise/prebuilt" && python3 -I -c '
import hashlib, os
paths = sorted(os.path.join(r, f) for r, _, fs in os.walk(".") for f in fs
               if not f.startswith("._") and f != ".DS_Store")
for p in paths:
    with open(p, "rb") as f:
        print(hashlib.sha256(f.read()).hexdigest() + "  " + p)')
got=$($SSH "vahiru@$HOST" "cd ~/crdroid/device/huawei/gaokun3/denoise/prebuilt && find . -type f ! -name '._*' ! -name .DS_Store | LC_ALL=C sort | xargs sha256sum" 2>/dev/null || true)
[ "$want" = "$got" ] || { diff <(echo "$want") <(echo "$got") | head -20 || true; die "构建机上的降噪服务文件和本机核对过的不一致（上面是差别，< 本机，> 构建机）—— 见denoise/prebuilt/README.md"; }
ok "降噪服务要的高通文件都在（$(echo "$want" | wc -l | tr -d ' ') 个）"

echo "═══ 3b2. 断言：中文输入法 fcitx5-android 的 APK 在、且是钉住的那一份 ═══"
# 来源与版本见 prebuilt-apps/fcitx5/README.md（用户 2026-10-06 定：GitHub release）。期望值入库在 .sha256 里。
# APK 走第 1 步的 device/ 同步（它没被排除；.gitignore 只是不入库）。不在时 device.mk 的 wildcard 会静默不带 ——
# 这里大声说出来：发版构建要带它（DISP-3），确实不要就设 GK3_ALLOW_NO_FCITX5=1（同时构建时设 GAOKUN3_WITH_FCITX5=false）。
FCITX_SHA=$(cut -d' ' -f1 "$SRC/prebuilt-apps/fcitx5/fcitx5-android-arm64-v8a.apk.sha256")
got=$($SSH "vahiru@$HOST" 'sha256sum ~/crdroid/device/huawei/gaokun3/prebuilt-apps/fcitx5/fcitx5-android-arm64-v8a.apk 2>/dev/null | cut -d" " -f1' || true)
if [ "$got" = "$FCITX_SHA" ]; then
    ok "fcitx5-android APK 在（${FCITX_SHA:0:16}）"
elif [ "${GK3_ALLOW_NO_FCITX5:-0}" = 1 ]; then
    echo "· fcitx5-android APK 不在或不符（${got:-缺失}），GK3_ALLOW_NO_FCITX5=1 ⇒ 这一版不带中文输入法"
else
    if [ -n "$got" ]; then why="sha256 不符（${got}）"; else why="缺失"; fi
    die "构建机上的 fcitx5-android APK $why —— 取法见 prebuilt-apps/fcitx5/README.md；确实不要它就设 GK3_ALLOW_NO_FCITX5=1"
fi

echo "═══ 3c. 断言：统一启动入口 gk3boot.efi 在、version 与二进制里嵌的版本串一致 ═══"
# 规矩与理由见 prebuilt-gk3boot/README.md：version 决定 ESP 上的目录名 EFI/gk3boot/<version>/，
#   与二进制里的 androidboot.bootloader=gk3boot-<串> 对不上就是"换了二进制没换版本串"。
#   确实不想带入口构建时（例如别人的构建机）：GK3_ALLOW_NO_GK3BOOT=1。
GK=$($SSH "vahiru@$HOST" 'cd ~/crdroid/device/huawei/gaokun3/prebuilt-gk3boot 2>/dev/null || exit 0
  [ -f gk3boot.efi ] && [ -f version ] || exit 0
  v=$(head -1 version | tr -d "\r\n")
  e=$(LC_ALL=C grep -ao "gk3boot-[A-Za-z0-9._+-]*" gk3boot.efi | sort -u | tr "\n" " ")
  echo "$v|$e|$(sha256sum gk3boot.efi | cut -d" " -f1)"' || true)
if [ -z "$GK" ]; then
    if [ "${GK3_ALLOW_NO_GK3BOOT:-0}" = 1 ]; then
        echo "· 构建机上没有 prebuilt-gk3boot/{gk3boot.efi,version}，GK3_ALLOW_NO_GK3BOOT=1 ⇒ 这一版 vendor 不带入口"
    else
        die "构建机上没有 prebuilt-gk3boot/{gk3boot.efi,version} —— 见 prebuilt-gk3boot/README.md；确实不要入口就设 GK3_ALLOW_NO_GK3BOOT=1"
    fi
else
    GV=${GK%%|*}; rest=${GK#*|}; GE=${rest%%|*}; GS=${rest#*|}
    case "$GV" in ''|*[!A-Za-z0-9._+-]*|log|LOG|.|..) die "prebuilt-gk3boot/version 不合法：'$GV'（只准 [A-Za-z0-9._+-]）" ;; esac
    [ "${#GV}" -le 64 ] || die "prebuilt-gk3boot/version 太长（${#GV} > 64）"
    case "$GV" in *.dirty*) die "prebuilt-gk3boot/version 带 .dirty（${GV}）—— 那一版对不上任何提交，别发" ;; esac
    [ "$GE" = "gk3boot-$GV " ] || die "prebuilt-gk3boot：version='$GV'，二进制里嵌的是 '${GE% }' —— 换了二进制没换版本串？"
    ok "gk3boot.efi 在：版本 ${GV}（与二进制一致），sha256 $GS"
    # 执行端 fastboot.img（可选）：在就必须是 gzip、≤ 4 MiB（设计稿 §4.1 的预算）、gzip -t 通过；不在只打一行。
    #   它没有自己的版本串 —— 与 gk3boot.efi 共用 version，换了它也要换 version（prebuilt-gk3boot/README.md）。
    FB=$($SSH "vahiru@$HOST" 'cd ~/crdroid/device/huawei/gaokun3/prebuilt-gk3boot 2>/dev/null && [ -f fastboot.img ] || exit 0
      m=$(head -c 2 fastboot.img | od -An -tx1 | tr -d " \n")
      t=bad; gzip -t fastboot.img 2>/dev/null && t=ok
      echo "$(stat -c %s fastboot.img)|$m|$t|$(sha256sum fastboot.img | cut -d" " -f1)"' || true)
    if [ -z "$FB" ]; then
        echo "· prebuilt-gk3boot 里没有 fastboot.img —— 这一版不带执行端（gk3boot 照常启动 Android，菜单里没有 fastboot 项）"
    else
        FBS=${FB%%|*}; rest=${FB#*|}; FBM=${rest%%|*}; rest=${rest#*|}; FBT=${rest%%|*}; FBH=${rest#*|}
        [ "$FBM" = 1f8b ] || die "prebuilt-gk3boot/fastboot.img 不是 gzip（开头 ${FBM}）—— 放错文件了？见 prebuilt-gk3boot/README.md"
        [ "$FBS" -le 4194304 ] || die "prebuilt-gk3boot/fastboot.img ${FBS} 字节，超过 4 MiB 预算（ESP 上要容得下三版，设计稿 §4.1）"
        [ "$FBT" = ok ] || die "prebuilt-gk3boot/fastboot.img 没过 gzip -t —— 截断了？（传输完要核字节数与 sha256）"
        ok "fastboot.img 在：${FBS} 字节，gzip -t 通过，sha256 ${FBH}（与 gk3boot.efi 同属版本 ${GV}）"
    fi
fi

echo "═══ 4. 受版本控制的文件逐一 md5 ═══"
L=$(mktemp); R=$(mktemp)
(cd "$SRC" && git ls-files -z . | xargs -0 md5 -r 2>/dev/null | awk '{print $1"  "$2}' | sort) > "$L"
(cd "$SRC" && git ls-files . ) | $SSH "vahiru@$HOST" 'cd ~/crdroid/device/huawei/gaokun3 && xargs md5sum 2>/dev/null | sort' > "$R"
if diff -q "$L" "$R" >/dev/null; then ok "$(wc -l < "$L" | tr -d ' ') 个受版本控制的文件逐字节一致"
else echo "✗ 有差异："; diff "$L" "$R" | head -10; rm -f "$L" "$R"; die "构建机的树 ≠ 本仓"; fi
rm -f "$L" "$R"
