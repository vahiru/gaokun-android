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
#   GK3_VERSION=<版本号> [GK3_ACCEPT_REPORT=<report.txt>] \
#     scripts/release.sh [--dry-run] [--stage-only] [--no-build]
#   （两个 GK3_* 变量见下面 REL-5 / REL-6 两段）
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
#
#   ★ 构建出来默认就是【发布构建】（B1，2026-10-04）：adb 要授权、ro.debuggable=0、
#     不开 TCP adb、镜像里没有开发者公钥。环境里 GAOKUN3_DEV_BUILD=1 才是开发构建
#     （见 device/huawei/gaokun3/lineage_gaokun3.mk），它只能 --stage-only：
#         GAOKUN3_DEV_BUILD=1 scripts/release.sh --dry-run --stage-only
#     第 2 步对产物逐条断言（--stage-only 只报不拦，与 allow_suspend 那条同一个规矩）。
#   每版还附一份内核的"对应源码清单"（GPL-2.0 §3，见 gen_kernel_sources）。它要读构建内核的那棵树：
#     GK3_KTREE  内核树，默认 ~/gk3-kernel-72y（编发布内核的那棵，v7.2.9 基线，SEC-9 2026-10-05 起；
#                v7.2-rc2 的旧树 ~/gk3-kernel-iris 只对得上本仓 c1062f2 及以前的配方）
#     GK3_REPO   本仓 checkout，默认本脚本所在的仓库（要它的 patches/ 与 kernel-*.sh）
#
#   ★ REL-5 版本属性（2026-10-05）：
#     GK3_VERSION  项目版本号（如 0.8.0-alpha）。device.mk 把它写成 vendor 属性
#                  ro.vendor.gaokun3.version（PRODUCT_VENDOR_PROPERTIES；ro.vendor. 前缀在 vendor 允许
#                  清单里，refs/lineage-sepolicy/build/soong/selinux_contexts.go:377）。
#                  第 2 步断言 vendor/build.prop 里那一行 = 这个值（--stage-only 只警告）。
#                  非 --stage-only（含 --dry-run）必须设：属性是【构建时】烤进去的，
#                  所以构建候选版时和用 --no-build 发它时要设【同一个】GK3_VERSION。
#                  不动 gaokun3.json 的 version 字段（Updater 的兼容判断可能用到它）。
#                  ⚠️ 环境变量能否传进 Kati 的产品配置【待构建机核实】—— 与 GAOKUN3_DEV_BUILD 是同一个问题
#                  （见 docs/dev/release-checklist.md）。
#
#   ★ REL-6 / G11 验收闸门（2026-10-05）：正式发版前必须有【这个戳】的 A 档验收报告且全过。
#     GK3_ACCEPT_REPORT  scripts/accept.sh 写的 report.txt 的路径。
#                  ⚠️ 为什么要手动 scp：accept.sh 在【维护者的 Mac】上跑（它走 adb 连设备），报告写在那边的
#                  out/accept/<戳>-<时间>/report.txt（accept.sh:49-51）；本脚本在【构建机】上跑。两台机器，
#                  报告不会自己过来。做法：scp out/accept/<戳>-<时间>/report.txt 构建机:… 再设这个变量。
#                  没设时退而找 $GK3_REPO/out/accept/<本版戳>-*/report.txt 里最新的一份（目录名按时间排序）。
#     判据全按 accept.sh 的实际输出：① 第一行含 profile=release 且不含"只读"（:75）；
#     ② 有一行恰好是 "  [PASS] A1 ro.build.date.utc = <本版戳>"（:73、:81；没给 --stamp 跑出来的那一行
#     后面带"（没给 --stamp，只记录）"，不算）；③ 最后一条"═══ 汇总："行含"FAIL 0 "、不含"中断（adb 掉线）"（:65、:340）。
#     拦不拦：只有真要往 ota/ 发的那一次（不带 --dry-run、不带 --stage-only）拦；
#     --dry-run 与 --stage-only 只警告 —— 候选版就是用 --dry-run 构建出来的，那时它的戳刚生成、
#     不可能已经有验收报告；验完再用 --no-build（可先加 --dry-run 彩排一遍看警告）发。
#
#   ★ R2 滚动清理（2026-10-11）：上传完自动跑 scripts/r2-prune.py --go（--stage-only 只滚 staging）。
#     正式版留当前 + 前两版、staging 留最新 2 组、installer 留最新 2 个，最新发布页引用的另外保留。GK3_R2_PRUNE=0 跳过。
#
#   ★ REL-13 同名覆盖闸门（2026-10-05）：正式上传前，builds/<zip> 与 install/<ver>/ 的三个载荷若在 R2 上
#     已存在且字节不同（同一天的另一版已经发过 —— OTA 包名只含日期）就停下、一个都不传。
#     GK3_R2_OVERWRITE=1 才照样覆盖（撤回坏包之类，对外可见，先想清楚）。用的是 scripts/r2-upload.py --check。

set -euo pipefail

# 要在 cd 到 ANDROID_BUILD_TOP 之前算：BASH_SOURCE 可能是相对路径
REPO=${GK3_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
KTREE=${GK3_KTREE:-$HOME/gk3-kernel-72y}

BUCKET=${BUCKET:-gaokun-android}
HOST=${HOST:-https://ota.072172.xyz}
# ★ REL-13（2026-10-05）：默认改用本仓的 scripts/r2-upload.py（第 4 步前要用它新加的 --check）。
#   原来默认是构建机上的 ~/r2-upload.py —— 一份不入库的拷贝，没有 --check：拿旧拷贝跑 --check 会把
#   "--check" 当成桶名去 PUT，报错退出（不会误传，但会拦下发版）。要用别的拷贝就显式设 UPLOAD。
UPLOAD=${UPLOAD:-$REPO/scripts/r2-upload.py}
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

# 开发构建发不出去：构建前就拦（否则要白等一整次构建，第 2 步才拦下）。--no-build 时看产物。
if [ "$NO_BUILD" = 0 ] && [ "${GAOKUN3_DEV_BUILD:-}" = 1 ] && [ "$STAGE_ONLY" = 0 ]; then
    die "GAOKUN3_DEV_BUILD=1 是开发构建（adb 免授权 / TCP 5555 / 开发者公钥），只能 --stage-only"
fi
# REL-5：版本号要在构建时就进 vendor/build.prop —— 构建前拦，免得白等一次构建（第 2 步还会对产物再断言）
if [ "$NO_BUILD" = 0 ] && [ "$STAGE_ONLY" = 0 ] && [ -z "${GK3_VERSION:-}" ]; then
    die "没设 GK3_VERSION —— 它在构建时写进 ro.vendor.gaokun3.version；构建时和发版时要设同一个 GK3_VERSION"
fi
# ═══ 内核的"对应源码清单"（GPL-2.0 §3），每版一份 ═══
# boot.img 里是 GPL-2.0 的内核二进制，而内核在 AOSP 树外编（prebuilt-boot/），配方分在四处：
#   上游 tag · 构建树上 git am 进来的 gaokun-buildbot 补丁（提交）· 本仓 patches/（只活在构建树
#   【工作区】里）· ReSukiSU（钉住的提交 + 本仓补丁）。
# 以前一样都没随发版留下，外人照仓库重建不出同一个内核；v0.7.1 是事后从实机与 boot.img 补录的
# （docs/relnotes/v0.7.1-alpha-sources.md）。产出放在 $S，随 install/$VER/ 与 GitHub release 一起发：
#   kernel-source.txt           清单本体：二进制 sha256、基底、补丁序列与 sha256、ReSukiSU、配置、重建步骤
#   kernel-config.txt           从【发布的 boot.img】里抽的 .config（scripts/extract-kconfig.py）——
#                               不取构建树 O= 目录里那份，那份可能已经被下一轮实验改过
#   kernel-base-patches.tar.gz  构建树上 $LINUX_BASE_TAG..HEAD 的提交（git format-patch），即 buildbot 那一层。
#                               直接带补丁本身，就不依赖"当时用的是 buildbot 哪个提交"这个从没记过的信息
#   crdroid-manifest.xml        repo manifest -r（ROM 侧各仓库的确切提交）；拿不到就在清单里写命令
# ⚠️ 不写进 install-artifacts.sha256：那份文件安装器在读（scripts/live/m0-internal.sh:221-225），
#    只认三个载荷；这几份的 sha256 记在 kernel-source.txt 里。
# 缺了任何一样就是这一版给不出对应源码 ⇒ 发版（含 --dry-run）拦，--stage-only 只警告。
# ★ SEC-9（2026-10-05）：基线从 v7.2-rc2 追到 v7.2.9 stable。v7.2-rc2 时代的值（tag 对象 4c45e14df2f4…、
#   提交 8cdeaa50eae8…、torvalds 仓库）见本仓 c1062f2 —— 发 #16 及以前的内核要用那时的脚本。
LINUX_BASE_TAG=v7.2.9
LINUX_BASE_URL=https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git
# tag 对象 bd50482a9c7a6eb91468f1064a346a75e677e692 解引用后的提交
# （2026-10-05 `git ls-remote git.kernel.org/…/stable/linux.git 'refs/tags/v7.2.9*'`）
LINUX_BASE=5fce161649b4d779d1b76d9fcd52dc77779774b8
GPL_FILES=()
gpl_bad() {
    [ "$STAGE_ONLY" = 1 ] || die "源码清单：$*"
    echo "⚠️ 源码清单：$*（--stage-only 不拦）" >&2
}
gen_kernel_sources() {
    local s=$1 txt=$1/kernel-source.txt cfg=$1/kernel-config.txt
    local AP=$REPO/scripts/kernel-apply-patches.sh RS=$REPO/scripts/kernel-setup-resukisu.sh
    [ -f "$AP" ] && [ -f "$RS" ] && [ -d "$REPO/patches" ] \
        || die "源码清单：$REPO 不像本仓 checkout（缺 patches/ 或 kernel-*.sh）—— 设 GK3_REPO"
    [ -f "$OUT/boot.img" ] || { gpl_bad "没有 boot.img，无从对应"; return 0; }

    # 补丁顺序只有一个出处：kernel-apply-patches.sh 的 KPATCHES 数组。不在这里另抄一份 —— 抄的那份一定会漂。
    local KP PIN KURL p
    KP=$(sed -n '/^KPATCHES=(/,/^)/p' "$AP" | sed -n 's/^[[:space:]]\{1,\}\(kernel\/[a-z-]*\/[0-9]\{4\}-[^[:space:]]*\).*/\1/p')
    [ -n "$KP" ] || die "源码清单：从 $AP 解析不出 KPATCHES"
    PIN=$(sed -n 's/^RESUKISU_PIN=\([0-9a-f]\{40\}\).*/\1/p' "$RS")
    KURL=$(sed -n 's/^RESUKISU_URL=\([^[:space:]]*\).*/\1/p' "$RS")
    [ -n "$PIN" ] || die "源码清单：从 $RS 解析不出 RESUKISU_PIN"

    # ---- 发布的二进制本身：配置、内核与 dtb 的 sha256、版本横幅 ----
    python3 "$REPO/scripts/extract-kconfig.py" "$OUT/boot.img" > "$cfg" \
        || die "源码清单：从 boot.img 抽不出 .config（CONFIG_IKCONFIG 关了？）"
    GPL_FILES=(kernel-source.txt kernel-config.txt)
    local BIN
    BIN=$(python3 - "$OUT/boot.img" <<'PY'
import gzip, hashlib, struct, sys
d = open(sys.argv[1], "rb").read()
ks, rs, ss, page, hv = (struct.unpack_from("<I", d, o)[0] for o in (8, 16, 24, 36, 40))
k = d[page:page + ks]
print("vmlinuz.efi sha256  %s  (%d bytes)" % (hashlib.sha256(k).hexdigest(), ks))
if hv >= 2:                                   # 偏移算法与第 2 步数 FDT 的那段相同
    pad = lambda n: (n + page - 1) // page * page
    off = pad(1) + pad(ks) + pad(rs) + pad(ss) + pad(struct.unpack_from("<I", d, 1632)[0])
    n = struct.unpack_from("<I", d, 1648)[0]
    print("dtb         sha256  %s  (%d bytes)" % (hashlib.sha256(d[off:off + n]).hexdigest(), n))
if k[:2] == b"MZ" and k[4:8] == b"zimg":     # EFI zboot，载荷 gzip（同 extract-kconfig.py）
    o, n = struct.unpack_from("<II", k, 8)
    img = gzip.decompress(k[o:o + n])
    i = img.find(b"Linux version ")
    if i >= 0:
        print("banner              %s" % img[i:img.find(b"\n", i)].decode(errors="replace").replace("\0", " ").strip())
PY
) || die "源码清单：解析 boot.img 失败"

    # ---- 构建树：基底、buildbot 层、本仓补丁的落地状态、ReSukiSU ----
    local HEAD="" DESC="" NB=0 KV="" VERIFY="" EXTRA="" KH="" KCNT="" KFIX=""
    if git -C "$KTREE" rev-parse -q --verify HEAD >/dev/null 2>&1; then
        HEAD=$(git -C "$KTREE" rev-parse HEAD)
        DESC=$(git -C "$KTREE" describe --tags --always HEAD 2>/dev/null || echo "$HEAD")
        KV=$(make -s -C "$KTREE" kernelversion 2>/dev/null || true)
        grep -qF "Linux version ${KV:-<无>}" <<<"$BIN" \
            || gpl_bad "构建树 kernelversion=${KV:-<无>} 与 boot.img 横幅对不上 —— GK3_KTREE 指错了树？"
        if git -C "$KTREE" merge-base --is-ancestor "$LINUX_BASE" HEAD 2>/dev/null; then
            NB=$(git -C "$KTREE" rev-list --count "$LINUX_BASE..HEAD")
            local fp; fp=$(mktemp -d)
            if git -C "$KTREE" format-patch -q -o "$fp/kernel-base-patches" "$LINUX_BASE..HEAD" \
               && tar -C "$fp" --sort=name --mtime="@$UTC" --owner=0 --group=0 --numeric-owner \
                      -cf - kernel-base-patches | gzip -n -9 > "$s/kernel-base-patches.tar.gz"; then
                GPL_FILES+=(kernel-base-patches.tar.gz)
            else
                gpl_bad "git format-patch $LINUX_BASE..HEAD 失败"
            fi
            rm -rf "$fp"
        else
            gpl_bad "$KTREE 的 HEAD 不在 ${LINUX_BASE_TAG}（${LINUX_BASE:0:12}）之上 —— 基底换了就先改本脚本的 LINUX_BASE"
        fi
        # 本仓补丁：--verify 在干净 worktree 上重放整条链、逐文件比对真实树（B0 唯一可靠的探测器）
        local vlog; vlog=$(mktemp)
        if bash "$AP" "$KTREE" --verify >"$vlog" 2>&1; then
            VERIFY="OK, identical to a clean replay — $(grep '^重放' "$vlog" | tail -1)"
        else
            VERIFY="MISMATCH — $(grep -E '^(✗|重放：)' "$vlog" | head -8 | tr '\n' ' ')"
            gpl_bad "构建树与本仓配方不一致（kernel-apply-patches.sh --verify）：$VERIFY"
        fi
        rm -f "$vlog"
        # 配方之外的工作区改动：--verify 只看补丁碰过的文件，看不见别处的手改。
        # ReSukiSU 的接线（KernelSU/、drivers/kernelsu、drivers/Makefile、drivers/Kconfig）是配方内的。
        EXTRA=$(git -C "$KTREE" status --porcelain --untracked-files=all \
                | awk 'FNR == NR { keep[$0] = 1; next }
                       { f = substr($0, 4); sub(/.* -> /, "", f) }
                       f ~ /^KernelSU\// || f ~ /^drivers\/(kernelsu|Makefile|Kconfig)$/ || f ~ /\.(orig|rej)$/ { next }
                       !(f in keep)' \
                      <(for p in $KP; do sed -n 's|^+++ b/||p' "$REPO/patches/$p" 2>/dev/null; done) - \
                || true)
        if grep -q '^[^?]' <<<"$EXTRA"; then
            gpl_bad "构建树有配方之外的已跟踪改动：$(grep '^[^?]' <<<"$EXTRA" | head -5 | tr '\n' ' ')"
        fi
        # ReSukiSU：只在内核真的开了 KSU 时才算数
        if grep -q '^CONFIG_KSU=y' "$cfg"; then
            KH=$(git -C "$KTREE/KernelSU" rev-parse HEAD 2>/dev/null || true)
            [ "$KH" = "$PIN" ] || gpl_bad "构建树的 KernelSU 在 ${KH:-<无>}，kernel-setup-resukisu.sh 钉的是 $PIN"
            KCNT=$(git -C "$KTREE/KernelSU" rev-list --count HEAD 2>/dev/null || true)
            for p in "$REPO"/patches/resukisu/*.patch; do
                [ -f "$p" ] || continue
                if git -C "$KTREE/KernelSU" apply --reverse --check "$p" >/dev/null 2>&1; then
                    KFIX+="$(sha256sum "$p" | cut -c1-64)  patches/resukisu/$(basename "$p")  (applied)"$'\n'
                else
                    KFIX+="$(sha256sum "$p" | cut -c1-64)  patches/resukisu/$(basename "$p")  (NOT applied in the build tree)"$'\n'
                    gpl_bad "ReSukiSU 补丁 $(basename "$p") 不在构建树里"
                fi
            done
        fi
    else
        gpl_bad "$KTREE 不是 git 内核树（设 GK3_KTREE 指向编发布内核的那棵）"
    fi

    # ---- ROM 侧：repo manifest -r（当前目录就是 ANDROID_BUILD_TOP）----
    local MAN="crdroid-manifest.xml was not generated; in the crDroid tree run: repo manifest -r -o crdroid-manifest.xml"
    # 构建机上 repo 装在 ~/bin，非交互 ssh 的 PATH 里没有它（V2，2026-10-18 查明这就是以前"没跑成"的原因）
    local REPO_BIN; REPO_BIN=$(command -v repo 2>/dev/null || { [ -x "$HOME/bin/repo" ] && echo "$HOME/bin/repo"; })
    if [ -n "$REPO_BIN" ] && "$REPO_BIN" manifest -r -o "$s/crdroid-manifest.xml" >/dev/null 2>&1; then
        GPL_FILES+=(crdroid-manifest.xml)
        MAN="crdroid-manifest.xml  sha256 $(sha256sum "$s/crdroid-manifest.xml" | cut -c1-64)"
    else
        echo "⚠️ repo manifest -r 没跑成，清单里只写命令" >&2
    fi

    local RC RDIRTY
    RC=$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo "<not a git checkout>")
    RDIRTY=$(git -C "$REPO" status --porcelain -- patches scripts/kernel-apply-patches.sh \
             scripts/kernel-setup-resukisu.sh scripts/kernel-config-android.sh 2>/dev/null | wc -l | tr -d " " || true)
    # ↑ `|| true`：$REPO 不是 git checkout（如构建机上 git archive 出来的副本）时 git 退 128，pipefail + set -e
    #   会让整个 release.sh 在打包这一步静默退出（2026-10-05 构建 1791138567 实测）。RC 那行已经标了 <not a git checkout>。

    # 清单本体用英文写：它是 GitHub release（英文发版说明）的附件，读者是外人
    {
        echo "# gaokun3 kernel: corresponding source (GPL-2.0 section 3) for $VER"
        echo "# Build stamp $UTC (ro.build.date.utc). Generated by scripts/release.sh; the sha256 values are authoritative."
        echo
        echo "## 1. The shipped binary (kernel and DTB inside boot.img)"
        echo "boot.img    sha256  $(sha256sum "$OUT/boot.img" | cut -c1-64)"
        echo "$BIN"
        echo "kernel-config.txt sha256 $(sha256sum "$cfg" | cut -c1-64) (the embedded .config, = /proc/config.gz, extracted from boot.img)"
        echo
        echo "## 2. Kernel base"
        echo "upstream    $LINUX_BASE_URL  $LINUX_BASE_TAG = $LINUX_BASE"
        echo "build tree  HEAD ${HEAD:-<unknown>} (git describe: ${DESC:-?})"
        echo "$NB commits on top of $LINUX_BASE_TAG = the linux-gaokun-buildbot patches, applied with git am"
        echo "            (https://github.com/KawaiiHachimi/linux-gaokun-buildbot)"
        if [ -f "$s/kernel-base-patches.tar.gz" ]; then
            echo "  full text: kernel-base-patches.tar.gz  sha256 $(sha256sum "$s/kernel-base-patches.tar.gz" | cut -c1-64)"
            git -C "$KTREE" log --reverse --format='  %h %s' "$LINUX_BASE..HEAD"
        fi
        echo
        echo "## 3. This project's kernel patches (scripts/kernel-apply-patches.sh KPATCHES order, applied on top of section 2)"
        echo "repository  https://github.com/vahiru/gaokun-android  commit $RC"
        echo "            (uncommitted changes to the patch files at release time: $RDIRTY; the sha256 below are of the files actually used)"
        local i=0
        for p in $KP; do
            i=$((i + 1))
            if [ -f "$REPO/patches/$p" ]; then
                printf '%2d  %s  patches/%s\n' "$i" "$(sha256sum "$REPO/patches/$p" | cut -c1-64)" "$p"
            else
                printf '%2d  %-64s  patches/%s\n' "$i" "<missing>" "$p"
            fi
        done
        echo "build tree vs. this list (kernel-apply-patches.sh --verify): ${VERIFY:-<not run>}"
        [ -z "$EXTRA" ] || { echo "build tree entries outside the recipe (?? = untracked):"; sed 's/^/  /' <<<"$EXTRA"; }
        echo
        echo "## 4. ReSukiSU (KernelSU fork; its in-kernel part, kernel/, is GPL-2.0)"
        echo "upstream    $KURL @ $PIN (pinned in scripts/kernel-setup-resukisu.sh)"
        echo "build tree  KernelSU/ HEAD ${KH:-<unknown, or kernel built without KSU>}, rev-list --count ${KCNT:-?} (Kbuild derives the version from it)"
        [ -z "$KFIX" ] || printf '%s' "$KFIX"
        echo
        echo "## 5. ROM sources (crDroid 16.0)"
        echo "$MAN"
        echo "This project's changes to the AOSP / crDroid tree (device/huawei/gaokun3/, scripts/crdroid-tree-fixes.py,"
        echo "the non-kernel files in patches/) are all in the repository commit above."
        echo
        echo "## 6. Rebuilding the kernel"
        echo "  git clone --branch $LINUX_BASE_TAG $LINUX_BASE_URL && cd linux"
        echo "  tar xzf kernel-base-patches.tar.gz && git am kernel-base-patches/*.patch"
        echo "  git -C <gaokun-android> checkout $RC"
        echo "  bash <gaokun-android>/scripts/kernel-apply-patches.sh .     # section 3, in order"
        echo "  bash <gaokun-android>/scripts/kernel-setup-resukisu.sh .    # ReSukiSU @ ${PIN:0:12} + patches/resukisu/"
        echo "  mkdir -p ../kout && cp kernel-config.txt ../kout/.config"
        echo "  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=../kout olddefconfig vmlinuz.efi dtbs"
        echo "  (compiler: see the banner in section 1; where the outputs go: device/huawei/gaokun3/prebuilt-boot/README.md)"
    } > "$txt"
    ok "源码清单：${GPL_FILES[*]}"
}

[ -n "${ANDROID_BUILD_TOP:-}" ] || die "先 source build/envsetup.sh && lunch"
cd "$ANDROID_BUILD_TOP"
OUT=${OUT:-$ANDROID_BUILD_TOP/out/target/product/gaokun3}

if [ "$NO_BUILD" = 1 ]; then
    echo "═══ 1. --no-build：用 out/ 里现成的产物（发的就是验过的那一版）═══"
else
    echo "═══ 1. 一次构建（bacon 与 superimage 必须同一次调用）═══"
    # ★ BUILD_NUMBER 决定【指纹里的 incremental】。不设的话 AOSP 回落成
    #   `eng.$(BUILD_USERNAME 前 6 个字符)`，而 Lineage 把 BUILD_USERNAME 匿名成
    #   `android-build` ⇒ 指纹里是 `eng.androi`（2026-09-14 在 v0.6.1 上实测），
    #   与 `ro.build.version.incremental`（构建戳）**对不上** —— 一个构建里两个
    #   互相矛盾的 incremental，崩溃归并、缺陷报告、任何解析指纹的东西都会拿到没用的值。
    #   见 docs/dev/android-bringup.md。
    export BUILD_NUMBER=${BUILD_NUMBER:-$(date -u +%Y%m%d%H%M%S)}
    echo "  BUILD_NUMBER=${BUILD_NUMBER}（进指纹的 incremental）"
    # vendor/build.prop 是 Make 生成的（crDroid 16 build/make/core/sysprop.mk:216），规则只依赖
    #   属性文件（同文件 :127），日期是命令运行时才 `cat` 的（config.mk:874 BUILD_DATETIME_FROM_FILE）
    #   ⇒ 增量构建里 vendor 属性没变就不重生成，日期 / 指纹 / incremental 停在老构建
    #   （v0.7.0–v0.7.1 的 vendor 一直是 09-28 的 1790597477）。删掉输出逼它重跑。
    #   （行号取自 refs/aosp-build，crDroid 16 的 build/make 待构建机核实；第 2 步有断言把关。）
    rm -f "$OUT/vendor/build.prop"
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

# ★ 统一启动入口（2026-10-05，S9）：vendor 里那份 gk3boot.efi / version 必须就是设备树 prebuilt-gk3boot/ 里那份
#   （设计稿 §4.11：sha256 清单随 vendor 下发，release.sh 断言一致）。device.mk 对它用 wildcard ——
#   prebuilt 在而 vendor 里没有 / 不同，说明 out/ 是另一次构建或增量构建没重拷，发出去的入口不是验过的那个。
#   prebuilt 不在时只报 vendor 里有没有（发不带入口的版本是允许的，属性缺省 off 也不会部署）。
GKP=device/huawei/gaokun3/prebuilt-gk3boot
GKV=$OUT/vendor/boot/gk3boot
if [ -f "$GKP/gk3boot.efi" ]; then
    for f in gk3boot.efi version; do
        [ -f "$GKV/$f" ] || die "prebuilt-gk3boot/$f 在，而 \$OUT/vendor/boot/gk3boot/$f 不在 —— vendor 镜像没带上入口（device.mk 的 wildcard？两样要同时在）"
        [ "$(sha256sum "$GKP/$f" | cut -d' ' -f1)" = "$(sha256sum "$GKV/$f" | cut -d' ' -f1)" ] \
            || die "vendor 里的 gk3boot/$f 与 prebuilt-gk3boot/$f 不同 —— out/ 不是用这份入口编的"
    done
    ok "vendor 里的统一启动入口 = prebuilt-gk3boot（版本 $(head -1 "$GKV/version")，sha256 $(sha256sum "$GKV/gk3boot.efi" | cut -c1-16)）"
    # 执行端 fastboot.img（可选，与 gk3boot.efi 同一个 version）：同一个规矩 —— prebuilt 有就必须逐字节进了 vendor；
    #   prebuilt 没有而 vendor 有，说明 out/ 里留着更早那次构建的执行端（PRODUCT_COPY_FILES 删了一项不会删 out/ 里的旧文件）。
    if [ -f "$GKP/fastboot.img" ]; then
        [ -f "$GKV/fastboot.img" ] || die "prebuilt-gk3boot/fastboot.img 在，而 \$OUT/vendor/boot/gk3boot/fastboot.img 不在 —— vendor 镜像没带上执行端（device.mk 的 wildcard？）"
        [ "$(sha256sum "$GKP/fastboot.img" | cut -d' ' -f1)" = "$(sha256sum "$GKV/fastboot.img" | cut -d' ' -f1)" ] \
            || die "vendor 里的 gk3boot/fastboot.img 与 prebuilt-gk3boot/fastboot.img 不同 —— out/ 不是用这份执行端编的"
        ok "vendor 里的执行端 fastboot.img = prebuilt-gk3boot（$(stat -c %s "$GKV/fastboot.img") 字节，sha256 $(sha256sum "$GKV/fastboot.img" | cut -c1-16)）"
    elif [ -f "$GKV/fastboot.img" ]; then
        echo "· prebuilt-gk3boot 里没有 fastboot.img，但 vendor 里有一份 —— 来自更早的构建？核对后再发（它会以版本 $(head -1 "$GKV/version") 的执行端身份部署）"
    else
        echo "· 这一版 vendor 不带执行端 fastboot.img（入口照常；菜单里没有 fastboot 项）"
    fi
elif [ -f "$GKV/gk3boot.efi" ]; then
    echo "· prebuilt-gk3boot 不在，但 vendor 里有 gk3boot $(head -1 "$GKV/version" 2>/dev/null) —— 来自更早的构建？核对后再发"
else
    echo "· 这一版 vendor 不带统一启动入口（prebuilt-gk3boot 不在）"
fi

# ★ TODO S1（2026-09-24）：发给用户的版本，待机默认必须是开的。开发期（2026-09-18～09-24）
#   这个默认值是 0；而 v0.6.2 的用户多半从没设过这个属性 ⇒ 发出默认 0 的版本，所有人 OTA 后
#   都会失去 s2idle。--stage-only（只给自己验）不拦。
AS=$(sed -n 's/^persist\.vendor\.gaokun3\.allow_suspend=//p' "$OUT/vendor/build.prop" | tail -1)
if [ "$STAGE_ONLY" = 1 ]; then
    echo "· 待机默认 allow_suspend=${AS:-<无>}（--stage-only 不拦）"
else
    [ "$AS" = 1 ] || die "vendor/build.prop 里 persist.vendor.gaokun3.allow_suspend=${AS:-<无>} —— 发版必须是 1（TODO S1）"
    ok "待机默认开（persist.vendor.gaokun3.allow_suspend=1）"
fi

# ★ B1（2026-10-04）：发给用户的版本不能带开发期的 adb 便利。v0.7.1 及以前的公开镜像全带着：
#   ro.adb.secure=0 + ro.debuggable=1 + persist.adb.tcp.port=5555（所有网卡、含热点口）
#   + 维护者的 adb 公钥 ⇒ 同一网段的任何人 adb connect 进来不弹授权框，再 adb root 就是 root。
#   开关在 lineage_gaokun3.mk（GAOKUN3_DEV_BUILD）。判据只看产物，--no-build 也能查：
#   · 每一份 build.prop 里都不许出现 ro.adb.secure≠1、ro.debuggable≠0、persist.adb.tcp.port、
#     含 adb 的 persist.sys.usb.config —— init 按 system → system_ext → vendor → odm → product
#     加载、后者覆盖前者（lineage_gaokun3.mk 的机制 2），所以哪一份都不能有；
#   · 且至少有一处 ro.adb.secure=1 —— adbd 读它时缺省按 false（lineage_gaokun3.mk 引的
#     adb/daemon/main.cpp:223-226），"哪儿都没写"等于不要授权；
#   · product/etc/security/adb_keys 不存在或为空（/adb_keys 是指向它的符号链接）。
#     ⚠️ 开发构建之后接着编发布构建，out/ 里可能残留上一次装进去的 adb_keys，
#     而镜像是从 out/ 的目录打的 —— 这里照拦，先 `m installclean` 再编。
ADB_BAD=$(python3 - "$OUT" <<'PY'
import os, sys
out = sys.argv[1]
props = ["system/build.prop", "system_ext/etc/build.prop", "vendor/build.prop",
         "odm/etc/build.prop", "product/etc/build.prop"]
bad, secure1 = [], False
for rel in props:
    p = os.path.join(out, rel)
    if not os.path.isfile(p):
        continue
    for n, line in enumerate(open(p, encoding="utf-8", errors="replace"), 1):
        line = line.strip()
        if line.startswith("#") or "=" not in line:
            continue
        k, v = (x.strip() for x in line.split("=", 1))
        where = "%s:%d %s" % (rel, n, line)
        if k == "ro.adb.secure":
            if v == "1":
                secure1 = True
            else:
                bad.append(where)
        elif k == "ro.debuggable" and v != "0":
            bad.append(where)
        elif k == "persist.adb.tcp.port":
            bad.append(where)
        elif k == "persist.sys.usb.config" and "adb" in v.split(","):
            bad.append(where)
if not secure1:
    bad.append("没有任何一份 build.prop 写 ro.adb.secure=1")
k = os.path.join(out, "product/etc/security/adb_keys")
if os.path.isfile(k) and os.path.getsize(k) > 0:
    bad.append("product/etc/security/adb_keys 在（%d 字节）" % os.path.getsize(k))
print("\n".join(bad))
PY
) || die "adb 断言脚本本身失败了（python3 退出码非 0）"
if [ -z "$ADB_BAD" ]; then
    ok "发布构建：adb 要授权、ro.debuggable=0、无 TCP adb、无开发者公钥"
elif [ "$STAGE_ONLY" = 1 ]; then
    echo "· 这是开发构建（--stage-only 不拦）："; echo "$ADB_BAD" | sed 's/^/    /'
else
    echo "$ADB_BAD" | sed 's/^/    /' >&2
    die "产物带着开发期的 adb 便利（见上）—— 发版必须是发布构建（不设 GAOKUN3_DEV_BUILD，B1）"
fi
# ★ OTA-11（2026-10-04）：每个分区 build.prop 的构建日期都必须等于 system 的。
#   v0.7.0–v0.7.1 的 vendor 停在 09-28（成因见第 1 步的 rm），指纹与 incremental 也随之对不上 ——
#   缺陷报告里同一台机器报出两个版本。用 --no-build 发的是别处构建的 out/，第 1 步那条 rm
#   未必跑过，所以这里兜底。odm 在没有独立分区时落在 vendor/odm 下，两处都认。
PD_BAD=0
while read -r part c1 c2; do
    f=""
    for c in $c1 $c2; do [ -f "$OUT/$c" ] && { f=$OUT/$c; break; }; done
    [ -n "$f" ] || continue
    PU=$(sed -n "s/^ro\.${part}\.build\.date\.utc=//p" "$f" | head -1)
    if [ "$PU" = "$UTC" ]; then continue; fi
    MSG="$part 的 ro.${part}.build.date.utc=${PU:-<无>} ≠ system 的 $UTC —— 增量构建没重生成它；删掉 ${f#"$OUT"/} 再构建"
    [ "$STAGE_ONLY" = 1 ] || die "$MSG"
    echo "⚠️ ${MSG}（--stage-only 不拦）" >&2; PD_BAD=1
done <<'EOF'
vendor      vendor/build.prop
odm         odm/etc/build.prop          vendor/odm/etc/build.prop
product     product/etc/build.prop
system_ext  system_ext/etc/build.prop
vendor_dlkm vendor_dlkm/etc/build.prop
EOF
[ "$PD_BAD" = 1 ] || ok "各分区的构建日期与 system 一致（${UTC}）"

# ★ REL-5（2026-10-05）：项目版本号 ro.vendor.gaokun3.version 必须就是这次要发的 GK3_VERSION。
#   --no-build 发的是之前构建的 out/，构建当时必须已经设了同一个值 —— 发版时设的这个只用来核对。
#   用 -F（固定串）：版本号里的 "." 不能当正则。
GV=$(sed -n 's/^ro\.vendor\.gaokun3\.version=//p' "$OUT/vendor/build.prop" | tail -1)
if [ -n "${GK3_VERSION:-}" ]; then
    if grep -Fqx "ro.vendor.gaokun3.version=$GK3_VERSION" "$OUT/vendor/build.prop"; then
        ok "版本属性 ro.vendor.gaokun3.version=$GK3_VERSION"
    else
        MSG="vendor/build.prop 里 ro.vendor.gaokun3.version=${GV:-<无>} ≠ GK3_VERSION=$GK3_VERSION —— 构建时设的不是这个值（或没设 / 没传进 Kati）"
        [ "$STAGE_ONLY" = 1 ] || die "$MSG"
        echo "⚠️ ${MSG}（--stage-only 不拦）" >&2
    fi
elif [ "$STAGE_ONLY" = 1 ]; then
    echo "· 版本属性 ro.vendor.gaokun3.version=${GV:-<无>}（没设 GK3_VERSION，--stage-only 不核对）"
else
    die "没设 GK3_VERSION（产物里是 ${GV:-<无>}）—— 构建时和发版时要设同一个 GK3_VERSION"
fi

# ★ REL-6 / G11（2026-10-05）：正式发版前必须有这个戳的 A 档验收报告且全过（判据与来由见头注释）。
#   accept.sh 在维护者的 Mac 上跑、报告要 scp 过来（GK3_ACCEPT_REPORT）；没设就在 $REPO/out/accept/ 下找。
ACC=${GK3_ACCEPT_REPORT:-}
if [ -z "$ACC" ]; then
    for f in "$REPO"/out/accept/"$UTC"-*/report.txt; do   # glob 按名字排序，目录名 <戳>-<YYYYmmdd-HHMMSS> ⇒ 最后一个最新
        [ -f "$f" ] && ACC=$f
    done
fi
ACC_BAD=()
if [ -z "$ACC" ]; then
    ACC_BAD+=("没有验收报告：GK3_ACCEPT_REPORT 没设，$REPO/out/accept/$UTC-*/report.txt 也没有 —— 在 Mac 上跑 SER=gaokun3 bash scripts/accept.sh --stamp ${UTC}，再把 report.txt scp 过来")
elif [ ! -f "$ACC" ]; then
    ACC_BAD+=("GK3_ACCEPT_REPORT=$ACC 不是文件")
else
    L1=$(head -1 "$ACC")
    case $L1 in *profile=release*) ;; *) ACC_BAD+=("第一行没有 profile=release（是 --profile dev 跑的？）：$L1") ;; esac
    case $L1 in *只读*) ACC_BAD+=("这是 --readonly 跑的（相机 / 麦克风 / 真解视频都没测）：$L1") ;; esac
    grep -Fqx "  [PASS] A1 ro.build.date.utc = $UTC" "$ACC" \
        || ACC_BAD+=("没有 \"[PASS] A1 ro.build.date.utc = $UTC\" —— 报告不是这个戳的，或跑的时候没给 --stamp $UTC")
    SUM=$(grep '^═══ 汇总：' "$ACC" | tail -1 || true)
    if [ -z "$SUM" ]; then
        ACC_BAD+=("没有汇总行 —— accept.sh 没跑完（中途被杀？）")
    else
        case $SUM in *"FAIL 0 "*) ;; *) ACC_BAD+=("汇总不是 FAIL 0：$SUM") ;; esac
        case $SUM in *"中断（adb 掉线）"*) ACC_BAD+=("验收中途 adb 掉线，后面的检查没跑：$SUM") ;; esac
    fi
fi
if [ ${#ACC_BAD[@]} = 0 ]; then
    ok "验收报告：$ACC —— $SUM"
else
    printf '    %s\n' "${ACC_BAD[@]}" >&2
    if [ "$DRY" = 1 ] || [ "$STAGE_ONLY" = 1 ]; then
        echo "⚠️ 验收报告不过关（见上）—— --dry-run / --stage-only 不拦，正式发版时这里会停（REL-6）" >&2
    else
        die "这个戳（${UTC}）没有过关的 A 档验收报告（见上）—— 发版前先装机跑 scripts/accept.sh（REL-6 / G11）"
    fi
fi

VER=$(basename "$ZIP" .zip)
echo "═══ 3. 打包安装产物 ═══"
S=$(mktemp -d); trap 'rm -rf "$S"' EXIT
cp "$ZIP" "$OUT/gaokun3.json" "$S/"
[ -f "$OUT/boot.img" ] && cp "$OUT/boot.img" "$S/"
zstd -T0 -19 --long -f "$OUT/super.img" -o "$S/super.img.zst"
( cd "$S" && sha256sum boot.img super.img.zst "$(basename "$ZIP")" > install-artifacts.sha256 )
gen_kernel_sources "$S"
ls -la "$S"

# ═══ REL-4：发版说明 / 安装指南放到 R2 同域名下 ═══
# 系统更新（Updater）里的「更新日志」「下载」两个链接指向 $HOST/relnotes/latest.md，「无法更新（版本不受支持）」
# 的说明链接指向 $HOST/relnotes/INSTALL.md（device/huawei/gaokun3/overlay/packages/apps/Updater/…/strings.xml）——
# 原来都指向 GitHub，国内多半打不开（TODO B12 / v1.0-plan REL-4）。这里每版正式发布时把它们传上去。
#   docs/relnotes/v<GK3_VERSION>.md 必须有（正式发版拦；--dry-run / --stage-only 只警告 —— 候选版构建时说明多半还没写完）；
#   有 v<GK3_VERSION>.zh-CN.md 就一起传，并且 latest.md 用中文版（主要用户在国内）；INSTALL 同理（有 INSTALL.zh-CN.md 就用它）。
#   Content-Type 用 text/plain; charset=utf-8：Markdown 原文在手机浏览器里直接可读、中文不乱码；不额外依赖 Markdown 转换器。
RN_EN=$REPO/docs/relnotes/v${GK3_VERSION:-}.md
RN_ZH=$REPO/docs/relnotes/v${GK3_VERSION:-}.zh-CN.md
INSTALL_DOC=$REPO/docs/INSTALL.md
[ -f "$REPO/docs/INSTALL.zh-CN.md" ] && INSTALL_DOC=$REPO/docs/INSTALL.zh-CN.md
if [ -z "${GK3_VERSION:-}" ] || [ ! -f "$RN_EN" ]; then
    if [ "$DRY" = 1 ] || [ "$STAGE_ONLY" = 1 ]; then
        echo "⚠️ 没有发版说明 ${RN_EN}（--dry-run / --stage-only 不拦；正式发版时这里会停，REL-4）" >&2
    else
        die "没有发版说明 $RN_EN —— Updater 的「更新日志」链接（$HOST/relnotes/latest.md）要它（REL-4）"
    fi
else
    ok "发版说明 ${RN_EN#$REPO/}$( [ -f "$RN_ZH" ] && echo "（另有中文版 ${RN_ZH#$REPO/}，latest.md 用它）")"
fi
[ -f "$INSTALL_DOC" ] || die "没有 $INSTALL_DOC（Updater 的 blocked_update_info_url 指向它的 R2 副本，REL-4）"
DOC_CT="text/plain; charset=utf-8"

# GPL 附件的 Content-Type（r2-upload.py 的第 4 个参数）
gpl_ctype() { case "$1" in *.txt) echo "text/plain; charset=utf-8" ;; *.tar.gz) echo application/gzip ;;
                           *.xml) echo application/xml ;; *) echo application/octet-stream ;; esac; }

# dry-run 结束时 $S 会被删，清单打出来给人过目（发版说明要链它）
[ "$DRY" = 1 ] && { echo "── kernel-source.txt ──"; cat "$S/kernel-source.txt" 2>/dev/null || true
                    ok "--dry-run：到此为止，未上传"; exit 0; }

for v in R2_ENDPOINT R2_ACCESS_KEY_ID R2_SECRET_ACCESS_KEY; do
    [ -n "${!v:-}" ] || die "环境变量 $v 未设置（凭据只从环境变量读）"
done

# ★ R2 滚动清理（2026-10-11 用户定）：正式版留 OTA 清单指向的当前版 + 前两版；staging 留最新 2 组；
#   installer 留最新 2 个；另外都保留最新正式 release / 最新 pre-release 的 GitHub 发布页链接到的分组。
#   规则与失败即停的条件见 scripts/r2-prune.py 头注释。它删的是对外可见的下载链接，撤不回 ——
#   这一版只想传、不想删：GK3_R2_PRUNE=0。清理失败只告警：产物与清单已经传完，发版本身是成功的。
r2_prune() {
    if [ "${GK3_R2_PRUNE:-1}" = 0 ]; then
        echo "· GK3_R2_PRUNE=0：跳过 R2 滚动清理（计划：python3 $REPO/scripts/r2-prune.py $*）"; return 0
    fi
    echo "═══ 5. R2 滚动清理 $* ═══"
    BUCKET="$BUCKET" python3 "$REPO/scripts/r2-prune.py" --go "$@" \
        || echo "⚠️ R2 滚动清理没做完（见上）—— 不影响本次发版；修好后手工跑 scripts/r2-prune.py --go $*" >&2
}

if [ "$STAGE_ONLY" = 1 ]; then
    echo "═══ 4. 传到 staging/$VER/（不更新清单，没有用户会收到）═══"
    for f in boot.img super.img.zst install-artifacts.sha256 gaokun3.json "$(basename "$ZIP")"; do
        python3 "$UPLOAD" "$BUCKET" "$S/$f" "staging/$VER/$f" application/octet-stream
    done
    for f in ${GPL_FILES[@]+"${GPL_FILES[@]}"}; do
        python3 "$UPLOAD" "$BUCKET" "$S/$f" "staging/$VER/$f" "$(gpl_ctype "$f")"
    done
    ok "已 staging。要发布，重跑本脚本不带 --stage-only"
    echo "staging 里的对象（桶 ${BUCKET}；⚠️ 只给自己验，不给用户 —— 别贴进发版说明，正式发版会重传到 builds/ 与 install/）："
    for f in boot.img super.img.zst install-artifacts.sha256 gaokun3.json "$(basename "$ZIP")" \
             ${GPL_FILES[@]+"${GPL_FILES[@]}"}; do
        echo "  staging/$VER/$f"
    done
    echo "  （经 $HOST 取时 URL 是 $HOST/staging/$VER/<文件名>，前提是自定义域对整个桶开放 —— 本脚本不核实）"
    r2_prune --only staging
    exit 0
fi

# ═══ REL-13：同名对象不许悄悄换内容 ═══
# OTA 包名只含日期（crDroidAndroid-16.0-<日期>-gaokun3-….zip），VER 就是它去掉 .zip。同一天出两个版本，
# builds/<zip> 与 install/$VER/* 的键完全相同 —— 后传的会悄悄盖掉先发的，已经贴出去的链接（发版说明 /
# GitHub 发布页里的 R2 链接）就换了内容。所以：这几个键在 R2 上已存在、且字节与本地不同 ⇒ 一个都不传、停下。
#   字节相同（重跑同一版，例如上次传到一半断了）照常放行。判据是 ETag == MD5（r2-upload.py check() 的注释）。
#   ota/gaokun3.json 本来就是每版覆盖的，不查；staging/ 按约定可以覆盖（--stage-only 在上面已经 exit），也不查。
#   GPL 附件（kernel-source.txt 等）每次 --no-build 重跑会重新生成，内容可能带时间，不查 —— 载荷一致就说明是同一版。
#   ⚠️ 真要替换已发布的同名对象（例如撤回一版坏包）：GK3_R2_OVERWRITE=1 —— 这是对外可见的改动，先想清楚。
echo "═══ 4a. 上传前核对：已有的同名载荷必须字节相同（REL-13）═══"
R2_CONFLICT=()
r2_check() {   # $1=本地文件 $2=键
    local rc=0
    python3 "$UPLOAD" --check "$BUCKET" "$1" "$2" || rc=$?
    [ "$rc" = 0 ] || R2_CONFLICT+=("$2（--check 退出码 $rc）")
}
r2_check "$S/$(basename "$ZIP")" "builds/$(basename "$ZIP")"
for f in install-artifacts.sha256 boot.img super.img.zst; do
    r2_check "$S/$f" "install/$VER/$f"
done
if [ ${#R2_CONFLICT[@]} != 0 ]; then
    printf '    %s\n' "${R2_CONFLICT[@]}" >&2
    if [ "${GK3_R2_OVERWRITE:-}" = 1 ]; then
        echo "⚠️ GK3_R2_OVERWRITE=1：照样覆盖上面这些已发布的对象（REL-13）" >&2
    else
        die "R2 上已有同名、但内容不同的载荷（见上）—— 多半是同一天的另一版已经发过。别覆盖已发布的链接；确要覆盖设 GK3_R2_OVERWRITE=1（REL-13）"
    fi
fi
ok "R2 同名对象核对通过"

echo "═══ 4. 上传：★产物先传，清单【最后】传 ═══"
python3 "$UPLOAD" "$BUCKET" "$S/$(basename "$ZIP")" "builds/$(basename "$ZIP")" application/zip
python3 "$UPLOAD" "$BUCKET" "$S/install-artifacts.sha256" "install/$VER/install-artifacts.sha256" text/plain
for f in boot.img super.img.zst; do
    python3 "$UPLOAD" "$BUCKET" "$S/$f" "install/$VER/$f" application/octet-stream
done
for f in ${GPL_FILES[@]+"${GPL_FILES[@]}"}; do       # 对应源码清单（REL-7）—— 也是 GitHub release 的附件
    python3 "$UPLOAD" "$BUCKET" "$S/$f" "install/$VER/$f" "$(gpl_ctype "$f")"
done
ok "产物已就位"

# REL-4：发版说明与安装指南（Updater 里的链接指向这几个键；比清单先传，用户一看到更新就点得开）
python3 "$UPLOAD" "$BUCKET" "$RN_EN" "relnotes/v$GK3_VERSION.md" "$DOC_CT"
RN_LATEST=$RN_EN
if [ -f "$RN_ZH" ]; then
    python3 "$UPLOAD" "$BUCKET" "$RN_ZH" "relnotes/v$GK3_VERSION.zh-CN.md" "$DOC_CT"
    RN_LATEST=$RN_ZH
fi
python3 "$UPLOAD" "$BUCKET" "$RN_LATEST" "relnotes/latest.md" "$DOC_CT"
python3 "$UPLOAD" "$BUCKET" "$INSTALL_DOC" "relnotes/INSTALL.md" "$DOC_CT"
ok "发版说明已就位：$HOST/relnotes/latest.md（= ${RN_LATEST#$REPO/}）"

# 清单最后传，且 download 指向刚上传的那个 zip
# ★ GK3_OS_SDK_LEVEL（2026-10-30，SEC-1 让 A16 用户直接升 A17）：覆盖清单的 os_sdk_level。
#   Updater 遇到 os_sdk_level > 本机 SDK_INT 且没设 crdroid.updater.allow_major_upgrades 就判 VERSION_UNSUPPORTED
#   （packages/apps/Updater InstallUtils.kt:30-31）；写成 36 后 A16（36）照常能装。代价：SDK 37 的机器把它当成
#   "更旧的 Android" 滤掉（UpdatesRepository.kt:131）—— 只有 A16 用户还要从这份清单升级时才设。
python3 - "$S/gaokun3.json" "$(basename "$ZIP")" "$HOST" "${GK3_OS_SDK_LEVEL:-}" <<'PY'
import json, sys
path, zipname, host, sdk = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
d = json.load(open(path))
r = d["response"][0]
r["download"] = "%s/builds/%s" % (host, zipname)
if sdk:
    print("  os_sdk_level %s -> %s" % (r.get("os_sdk_level"), sdk))
    r["os_sdk_level"] = int(sdk)
json.dump(d, open(path, "w"), indent=2)
print("  download -> %s" % r["download"])
PY
python3 "$UPLOAD" "$BUCKET" "$S/gaokun3.json" "ota/gaokun3.json" application/json
ok "清单已发布 —— 设备端「系统更新」现在能看到 $VER"
r2_prune                                          # 必须在清单之后：它按清单认"当前版"

# INST-5：本版全部 R2 链接，贴进发版说明 Files 表的 R2 列（docs/relnotes/TEMPLATE.md）。
#   与上面的上传路径一一对应：OTA 包在 builds/，其余在 install/$VER/。
R2_LINKS="  $HOST/builds/$(basename "$ZIP")"
R2_LINKS+=$'\n'"  $HOST/relnotes/v$GK3_VERSION.md（发版说明本身；Updater 的「更新日志」打开的是 $HOST/relnotes/latest.md）"
for f in boot.img super.img.zst install-artifacts.sha256 ${GPL_FILES[@]+"${GPL_FILES[@]}"}; do
    R2_LINKS+=$'\n'"  $HOST/install/$VER/$f"
done

cat <<EOF

发布完成。本版的 R2 链接（贴进发版说明 Files 表的 R2 列，国内用户靠它下载）：
$R2_LINKS

剩下要人做的：
  * 用【设备】而不是构建机去验一次抓取（沙箱会挡出站 HTTP，那边的结论不可信）：
      curl -sI $HOST/ota/gaokun3.json | head -3      # 必须 200，不能是 3xx
  * GitHub Release 另发（gh release create），把 install/$VER/ 那几个文件带上 ——
    ★ 包括内核的对应源码清单：${GPL_FILES[*]:-<没生成>}（GPL-2.0 §3，REL-7），发版说明的 Files 一节要链到 kernel-source.txt。
    ⚠️ 两个 2026-09-14 踩过的坑：① gh 要在仓库目录里跑、或加 -R vahiru/gaokun-android，
       在别的目录里它报 "not a git repository" 就什么都没传；② 别写 gh ... | tail -1 ——
       管道的退出码是 tail 的，上传失败照样印"uploaded"（与 az/make 那两次是同一个坑）。
    ③ 附件名就是文件名，"file#label" 里的 # 后面只是显示标签；要叫 install-artifacts.sha256 就先把文件改成这个名。
    ④ 2026-09-16：gh release create 一次带 5 个附件，1.28 GB 那个上传收到 HTTP 400（本机上行不稳），
       gh 随即把刚建的 release 整个删掉 —— 所以别一把全传。稳妥顺序：--draft 建草稿 + 小附件 →
       大附件逐个 gh release upload --clobber、每个都用 gh release view --json assets 核对字节数、失败重试 →
       全对再 gh release edit --draft=false --latest。草稿期间外面看不见，半途失败也没有半发布状态。
EOF
