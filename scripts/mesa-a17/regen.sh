#!/usr/bin/env bash
# A17（mesa 26.1.0-devel）上重新生成 turnip 的 Android.bp：meson2hermetic 管线一键版。
# 在【构建机】跑；不调用 m / soong，只改 <mesa3d 目录> 里的文件。全流程约 20 秒。
#
#   bash scripts/mesa-a17/regen.sh <mesa3d 目录> [选项]
#     --generator DIR       用现成的 meson2hermetic 检出（默认按下面钉死的 SHA 拉进缓存）
#     --allow-crdroid-tree  允许直接改 ~/crdroid/external/mesa3d（默认拒绝，先在副本上跑）
#     --export FILE.tar.gz  生成后把全部 Android.bp + 补丁动过的源文件打包（备份进 R2 用）
#     --tree ROOT           交给 check-output.py：在整棵 crDroid 树里核对外部模块名（慢，约 1–3 分钟）
#     --log FILE            生成器日志写到这里（默认 ${TMPDIR:-/tmp}/mesa-a17-regen.<pid>.log）
#   退出码：0 = 生成 + 自检全过；1 = 某一步失败（看输出最后一行）；2 = 参数 / 环境不对。
#
# 步骤（详见 docs/dev/mesa-a17.md）：
#   1. 前提检查：VERSION 必须是 26.1.*；python3 >= 3.11（生成器要 tomllib）
#   2. 准备生成器：gurchetansingh/mesonbuild 的 meson2hermetic 分支，钉 SHA（分支会强推）
#   3. 核对 gfxstream 手写兼容块：树里根 Android.bp 末尾那段必须与
#      device/huawei/gaokun3/mesa/hermetic/android/gfxstream_workaround.txt 一致
#      （生成器原样把这个文件贴回去；上游改了它而我们没跟，会悄悄回退上游的改动）
#   4. 打源码补丁（幂等）：scripts/mesa-a17/backports/*.patch（上游 mesa 的 9 个提交）
#      + patches/0004（turnip ANB 延迟绑定 v3）+ patches/0130（turnip P010，上游 MR !45721）。
#      ⚠️ 不打 patches/0005（已证伪）。
#   5. 删掉旧的生成 Android.bp（文件头带 "The Magma GPU Project" 的那些），重新生成
#   6. postprocess.py：vulkan.freedreno 补 compile_multilib: "64"
#   7. check-output.py：模块名闭合、turnip 用 msm 不用 kgsl、SP-HAL 依赖白名单、无绝对路径
set -euo pipefail

M2H_URL=https://github.com/gurchetansingh/mesonbuild.git
M2H_BRANCH=meson2hermetic
# 2026-10-08 的分支头（"Consolidate custom default select instances per value"）。
# 换 SHA 前先在副本上跑一遍、看 check-output 与 git diff。
M2H_SHA=1217e6125d76e87912c71b15d517473c46d7b13f

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
CFG=$REPO/device/huawei/gaokun3/mesa/hermetic/android
BACKPORTS=$HERE/backports
PATCH0004=$REPO/patches/0004-mesa-turnip-handle-anb-rebind-with-null-memory.patch
PATCH0130=$REPO/patches/0130-mesa-turnip-support-p010.patch

die() { echo "✗ $*" >&2; exit 1; }
usage() { sed -n '2,12p' "$0" >&2; exit 2; }

MESA= GEN= ALLOW_CRDROID=0 EXPORT= TREE= LOG=
while [ $# -gt 0 ]; do
  case "$1" in
    --generator) GEN=$2; shift 2 ;;
    --allow-crdroid-tree) ALLOW_CRDROID=1; shift ;;
    --export) EXPORT=$2; shift 2 ;;
    --tree) TREE=$2; shift 2 ;;
    --log) LOG=$2; shift 2 ;;
    -h|--help) usage ;;
    -*) echo "未知选项 $1" >&2; usage ;;
    *) [ -z "$MESA" ] || usage; MESA=$1; shift ;;
  esac
done
[ -n "$MESA" ] || usage
[ -d "$MESA" ] || { echo "✗ 不是目录：$MESA" >&2; exit 2; }
MESA=$(cd "$MESA" && pwd -P)
LOG=${LOG:-${TMPDIR:-/tmp}/mesa-a17-regen.$$.log}

# ── 1. 前提 ──────────────────────────────────────────────────────────────
ver=$(cat "$MESA/VERSION" 2>/dev/null || true)
case "$ver" in
  26.1.*) ;;
  *) echo "✗ $MESA/VERSION = '$ver'，本管线只对 A17 的 mesa 26.1.x 验过" >&2; exit 2 ;;
esac
if [ -d "$HOME/crdroid/external/mesa3d" ] &&
   [ "$MESA" = "$(cd "$HOME/crdroid/external/mesa3d" && pwd -P)" ] && [ $ALLOW_CRDROID = 0 ]; then
  echo "✗ 目标是 ~/crdroid/external/mesa3d：先在副本上跑（cp -a），确认无误再加 --allow-crdroid-tree" >&2
  exit 2
fi
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' ||
  { echo "✗ 需要 python3 >= 3.11（meson convert 用 tomllib）" >&2; exit 2; }
for f in aosp_mesa3d.toml platforms.toml dependencies.toml gfxstream_workaround.txt; do
  [ -f "$CFG/$f" ] || { echo "✗ 缺配置 $CFG/$f" >&2; exit 2; }
done
# git apply 在"别人的仓库的子目录"里会按那个仓库的根解析路径 —— 要么 mesa 自己是仓库根，要么不在任何仓库里
top=$(git -C "$MESA" rev-parse --show-toplevel 2>/dev/null || true)
if [ -n "$top" ] && [ "$(cd "$top" && pwd -P)" != "$MESA" ]; then
  die "$MESA 位于另一个 git 仓库（$top）的子目录里，git apply 会解析错路径"
fi
echo "▶ mesa $ver @ $MESA"

# ── 2. 生成器 ────────────────────────────────────────────────────────────
if [ -z "$GEN" ]; then
  GEN=${XDG_CACHE_HOME:-$HOME/.cache}/gaokun-mesa/mesonbuild-$M2H_SHA
  if [ ! -f "$GEN/meson.py" ]; then
    echo "▶ 拉取 meson2hermetic $M2H_SHA → $GEN"
    rm -rf "$GEN.tmp"; mkdir -p "$GEN.tmp"
    git -C "$GEN.tmp" init -q
    git -C "$GEN.tmp" remote add origin "$M2H_URL"
    # GitHub 允许按 SHA 浅取；不行就退回取分支（SHA 已不在分支上时会在下面的校验里失败）
    git -C "$GEN.tmp" fetch -q --depth 1 origin "$M2H_SHA" ||
      git -C "$GEN.tmp" fetch -q --depth 200 origin "$M2H_BRANCH"
    git -C "$GEN.tmp" checkout -q "$M2H_SHA" || die "生成器 SHA $M2H_SHA 取不到（分支被强推？）"
    mv "$GEN.tmp" "$GEN"
  fi
fi
[ -f "$GEN/meson.py" ] || die "生成器目录不对：$GEN/meson.py 不存在"
gsha=$(git -C "$GEN" rev-parse HEAD 2>/dev/null || echo '?')
[ "$gsha" = "$M2H_SHA" ] || echo "⚠ 生成器 HEAD = $gsha，不是钉死的 $M2H_SHA（--generator 指定的，后果自负）"
echo "▶ 生成器 $GEN（$gsha）"

# ── 3. gfxstream 手写兼容块 ─────────────────────────────────────────────
# 根 Android.bp 末尾从 "This is a special handwritten template" 那段注释开始到文件尾。
# 生成过之后它就是我们贴回去的那份，所以重跑时同样相等（幂等）。
if ! python3 - "$MESA/Android.bp" "$CFG/gfxstream_workaround.txt" <<'EOF'
import sys
bp, want = (open(p).read() for p in sys.argv[1:])
i = bp.find('/*\n * This is a special handwritten template')
sys.exit(0 if i >= 0 and bp[i:].rstrip('\n') == want.rstrip('\n') else 1)
EOF
then
  echo "✗ $MESA/Android.bp 末尾的 gfxstream 手写块与 $CFG/gfxstream_workaround.txt 不一致。" >&2
  echo "  上游可能改过它：用树里那段（从 '/*' + 'This is a special handwritten template' 到文件尾）" >&2
  echo "  更新 gfxstream_workaround.txt，确认差异后再跑。" >&2
  exit 1
fi
echo "▶ gfxstream 手写块一致"

# ── 4. 源码补丁（幂等）───────────────────────────────────────────────────
for p in "$BACKPORTS"/*.patch "$PATCH0004" "$PATCH0130"; do
  n=$(basename "$p")
  if (cd "$MESA" && git apply --check -R "$p" 2>/dev/null); then
    echo "  = $n（已打）"
  elif (cd "$MESA" && git apply --check "$p" 2>/dev/null); then
    (cd "$MESA" && git apply "$p") || die "$n 打补丁失败"
    echo "  + $n"
  else
    (cd "$MESA" && git apply --check "$p") || true
    die "$n 既不能正向打也不能反向撤 —— 树不是预期的 26.1 快照，或被别的改动碰过"
  fi
done

# ── 5. 重新生成 ─────────────────────────────────────────────────────────
before=$(mktemp)
python3 "$HERE/check-output.py" "$MESA" --dump-names > "$before"
old_bps=$(grep -rl --include=Android.bp 'The Magma GPU Project' "$MESA" | grep -v '/\.git/' || true)
echo "▶ 删除旧的生成文件：$(printf '%s\n' "$old_bps" | grep -c . || true) 个 Android.bp"
printf '%s\n' "$old_bps" | while read -r f; do [ -n "$f" ] && rm -f "$f"; done

echo "▶ meson convert（日志 $LOG）"
if ! python3 "$GEN/meson.py" convert \
      --config="$CFG/aosp_mesa3d.toml" \
      --platforms="$CFG/platforms.toml" \
      --dependencies="$CFG/dependencies.toml" \
      --project-dir="$MESA" > "$LOG" 2>&1; then
  tail -20 "$LOG" >&2
  die "meson convert 失败（旧 Android.bp 已删；修好后重跑即可，或 git checkout 恢复）"
fi
grep -E 'ERROR|Traceback' "$LOG" && die "生成日志里有 ERROR / Traceback"
grep 'WARNING' "$LOG" | sort | uniq -c | sed 's/^/  /' || true
new_bps=$(grep -rl --include=Android.bp 'The Magma GPU Project' "$MESA" | grep -v '/\.git/' || true)
gone=$(comm -23 <(printf '%s\n' "$old_bps" | sort) <(printf '%s\n' "$new_bps" | sort) | grep . || true)
added=$(comm -13 <(printf '%s\n' "$old_bps" | sort) <(printf '%s\n' "$new_bps" | sort) | grep . || true)
echo "▶ 生成 $(printf '%s\n' "$new_bps" | grep -c .) 个 Android.bp（新增 $(printf '%s' "$added" | grep -c . || true)，消失 $(printf '%s' "$gone" | grep -c . || true)）"
[ -z "$gone" ] || printf '  消失：%s\n' $gone

# ── 6–7. 后处理 + 自检 ──────────────────────────────────────────────────
python3 "$HERE/postprocess.py" "$MESA"
# Blueprint 语法检查：prebuilts 里的 bpfmt 只解析不编译（-l 只列格式不同的文件，不改文件）
BPFMT=${BPFMT:-}
for c in "${TREE:+$TREE/prebuilts/build-tools/linux-x86/bin/bpfmt}" \
         "$MESA/../../prebuilts/build-tools/linux-x86/bin/bpfmt" \
         "$HOME/crdroid/prebuilts/build-tools/linux-x86/bin/bpfmt"; do
  [ -z "$BPFMT" ] && [ -n "$c" ] && [ -x "$c" ] && BPFMT=$c
done
if [ -n "$BPFMT" ]; then
  bperr=0
  while read -r f; do
    out=$("$BPFMT" -l "$f" 2>&1) || { echo "  ✗ $out" >&2; bperr=1; continue; }
    [ -z "$out" ] || echo "  ⚠ bpfmt 格式不同（不影响构建）：$out"
  done < <(find "$MESA" -name Android.bp -not -path '*/.git/*')
  [ $bperr = 0 ] || die "bpfmt 解析失败（见上）"
  echo "▶ bpfmt：全部 Android.bp 可解析"
else
  echo "⚠ 没找到 bpfmt（prebuilts/build-tools/linux-x86/bin/bpfmt），跳过语法检查"
fi
python3 "$HERE/check-output.py" "$MESA" --before-names "$before" ${TREE:+--tree "$TREE"} ||
  die "check-output.py 不通过"
rm -f "$before"

if [ -n "$EXPORT" ]; then
  # 归档：全部 Android.bp + 补丁动过的源文件（相对 mesa 根），解到干净的 26.1 树上即可复现
  list=$(mktemp)
  (cd "$MESA" && find . -name Android.bp -not -path './.git/*' | sed 's|^\./||') > "$list"
  cat "$BACKPORTS"/*.patch "$PATCH0004" "$PATCH0130" | sed -n 's|^+++ b/||p' | sort -u >> "$list"
  tar -C "$MESA" -czf "$EXPORT" -T "$list"
  rm -f "$list"
  echo "▶ 已打包 $EXPORT（$(wc -c < "$EXPORT") 字节）"
fi
echo "✓ 完成。下一步（构建机）：m vulkan.freedreno —— 见 docs/dev/mesa-a17.md「构建验证」"
