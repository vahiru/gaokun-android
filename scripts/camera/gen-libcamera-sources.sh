#!/usr/bin/env bash
# ============================================================================
# gen-libcamera-sources.sh — 产出 libcamera 的「生成文件」，供
#                            patches/libcamera/libcamera-Android.bp 消费。
#
#   ⚠️ 这个脚本在仓库里【原本被引用却不存在】：
#      patches/libcamera/libcamera-Android.bp:15 写着
#        "…见 generated/ 目录与 scripts/camera/gen-libcamera-sources.sh"
#      但全仓（含 origin/main）找不到它 ⇒ 构建 ROM 的这一步是"口口相传"的。
#      本脚本把它补上，并自带断言 —— 缺文件就报错退出，不让你带着半棵生成的树去编。
#
# 为什么需要生成：
#   Soong 不跑 libcamera 的 meson。这 5 个 .cpp 与那批生成头必须另外产出：
#     generated/src/control_ids.cpp      generated/src/property_ids.cpp
#     generated/src/version.cpp          generated/src/ipa_pub_key.cpp
#     generated/src/qcom-ife_ipa_proxy.cpp
#     generated/include/libcamera/**（control_ids.h / property_ids.h / formats.h /
#                                    version.h / ipa/*_ipa_interface.h …）
#
# 为什么走 meson 而不是手调各个 Python 生成器：
#   生成器本身只依赖树内 YAML/mojom，但 IPA 的代理 cpp 还要 mojo 的 bytecode
#   templates（utils/codegen/ipc/...）与一套 env；照 meson 拼参数极易拼错且不报错。
#   ⇒ 让 meson 把生成规则搭好，我们只 `ninja` 那几个【生成类】目标，不编 C++ 本体。
#
# ★ 重要前提：libcamera 必须是【上游源码】并已打上 patches/libcamera/*.patch
#   （qcom-ife 管线与 IPA 在本仓补丁里，src/ipa/qcom-ife 由补丁加入）。
#   AOSP 的 external/libcamera（platform/external/libcamera @ android-16.0.0_r4）
#   只有 ipu3/libipa/mtkisp7/rkisp1/rpi/vimc。
#   构建流程是：用上游 libcamera 换掉 external/libcamera，再打 patches/libcamera/*.patch。
#   见 patches/libcamera/README.md 与 docs/dev/camera.md。
#
# 用法：
#   bash scripts/camera/gen-libcamera-sources.sh <libcamera_tree>            # 干跑：只检查
#   bash scripts/camera/gen-libcamera-sources.sh <libcamera_tree> --apply    # 真生成
#   bash scripts/camera/gen-libcamera-sources.sh ~/crdroid/external/libcamera --apply
#
# 退出码：0 成功；非 0 表示生成不完整（绝不"部分成功"）。
# ============================================================================
set -uo pipefail

TREE="${1:-}"
APPLY=0
[ "${2:-}" = "--apply" ] && APPLY=1
[ -z "$TREE" ] && { sed -n '2,40p' "$0"; exit 2; }
TREE="$(cd "$TREE" 2>/dev/null && pwd)" || { echo "✗ 目录不存在: $1"; exit 1; }

BUILD="$TREE/build-gen"
GEN="$TREE/generated"

fail=0
say()  { echo "$*"; }
ok()   { echo "  ✔ $*"; }
bad()  { echo "  ✗ $*"; fail=1; }
warn() { echo "  ⚠ $*"; }

say "════ gen-libcamera-sources ════"
say "  树   : $TREE"
say "  模式 : $([ $APPLY = 1 ] && echo APPLY || echo 干跑)"

# ---------------------------------------------------------------- 0. 前置
say ""
say "── 0. 前置检查 ──"
for t in meson ninja python3 pkg-config; do
    command -v "$t" >/dev/null 2>&1 && ok "$t" || bad "缺 ${t}（apt install meson ninja-build pkg-config）"
done
python3 -c "import ply" 2>/dev/null && ok "python3-ply" \
    || bad "缺 python3-ply（mojo 生成器要它；缺了会在 meson configure 最后一步才炸）"
python3 -c "import yaml" 2>/dev/null && ok "python3-yaml" || bad "缺 python3-yaml"
pkg-config --exists yaml-0.1 2>/dev/null && ok "libyaml (yaml-0.1)" \
    || bad "缺 libyaml-dev（★ pkg-config 名是 yaml-0.1，不是 libyaml）"
[ -f "$TREE/meson.build" ] && ok "libcamera meson.build 在" || bad "不像 libcamera 源码树"
[ -d "$TREE/src/ipa/qcom-ife" ] && ok "src/ipa/qcom-ife 在（上游源码 + 本仓补丁）" \
    || bad "没有 src/ipa/qcom-ife —— 不是打过 patches/libcamera 的上游源码；生成不出 qcom-ife 代理"
[ -d "$TREE/utils/codegen" ] && ok "utils/codegen 在" || bad "没有 utils/codegen（生成器所在）"
[ "$fail" = 0 ] || { echo; echo "✗ 前置不满足，中止。"; exit 1; }

# ---------------------------------------------------------------- 1. meson 配置
if [ "$APPLY" = 0 ]; then
    say ""
    say "（干跑：前置都过了。加 --apply 才真正跑 meson/ninja 生成。）"
    say "  将执行：meson setup $BUILD && ninja（只编生成类目标）→ 收集到 $GEN"
    exit 0
fi

say ""
say "── 1. meson setup（只配置；用最省的选项集，避免拉无关依赖）──"
rm -rf "$BUILD"
# 这些选项只为"能配置成功 + 让 qcom-ife 的 mojom 参与生成"，不追求能编出库：
#   -Dtest=false -Ddocumentation=false  : 省掉一大堆测试/文档依赖
#   -Dv4l2=false -Dgstreamer=disabled -Dcam=disabled -Dqcam=disabled -Dpycamera=disabled
#   -Dpipelines=qcom-ife                 : 只用 qcom-ife 管线（IFE 硬件 ISP）
#   -Dipas=qcom-ife                      : ★ 必须，否则 qcom-ife 的代理不会被生成
#   -Dandroid=disabled                   : 不给它加 Android 专属的东西（那是 AOSP 版的行为）
# ⚠️ 选项名与【类型】都不能猜（README 第 3 条的老教训，我照旧踩了两次）：
#    - documentation / v4l2 / gstreamer / cam / qcam / pycamera ：feature → enabled|disabled|auto
#      写 =false 会报 'Value "false" (of type "string") … is not one of the choices'
#    - test ：是【boolean】→ 只能 true|false；写成 disabled 会报 'Value disabled is not boolean'
#    - pipelines / ipas ：是【array】，取值来自各自 choices
#    ⇒ 照 meson_options.txt 抄，别按直觉填。
if ! meson setup "$BUILD" "$TREE" \
        -Dtest=false -Ddocumentation=disabled \
        -Dv4l2=disabled -Dgstreamer=disabled \
        -Dcam=disabled -Dqcam=disabled -Dpycamera=disabled \
        -Dpipelines=qcom-ife -Dipas=qcom-ife \
        > "$BUILD.setup.log" 2>&1; then
    echo "✗ meson setup 失败。日志尾部："
    tail -25 "$BUILD.setup.log" | sed 's/^/    /'
    echo "    （完整日志：$BUILD.setup.log）"
    exit 1
fi
ok "configure 成功"

# ---------------------------------------------------------------- 2. 找出生成目标
say ""
say "── 2. 从 meson 的目标表里找出【生成类】目标 ──"
# 生成目标的清单在下面那段 python 里（want_exact / want_re）——
# 别在这里再维护第二份，两处一定会漂移。
TARGETS_JSON="$BUILD/targets.json"
meson introspect --targets "$BUILD" > "$TARGETS_JSON" 2>/dev/null || { bad "introspect 失败"; exit 1; }

# introspect 的 JSON 里每个 target 有 name / filename / type；生成类的是 custom。
# ★ 关键：自定义目标的【输出路径】才是 ninja 认的目标名 —— 用 meson 的 target name
#   去 ninja 会得到 "unknown target 'controls_ids_cpp'"（踩过）。
#   所以这里输出 "输出路径<TAB>meson 名"，ninja 用前者。
python3 - "$TARGETS_JSON" <<'PY' > "$BUILD/gen_targets.txt"
import json, re, sys
# 生成文件分两类，都要：
#   .cpp —— libcamera-Android.bp 的 srcs 直接引用这 5 个
#   .h   —— 那组 local_include_dirs 里的 generated/include/libcamera/**
#           （IPA 源码大量用【裸名】include，少一个就一串看不出关联的 unknown type name）
want_exact = {
    "control_ids.cpp", "property_ids.cpp", "version.cpp",
    "ipa_pub_key.cpp", "qcom-ife_ipa_proxy.cpp",
    "control_ids.h", "property_ids.h", "formats.h", "version.h",
    # ★ libcamera.h（伞头）也是生成的：include/libcamera/meson.build:127-135 的
    #   custom_target('gen-header')（utils/codegen/gen-header.sh）。PR #6 初版以为上游没有它、
    #   手写了一份放在 patches/libcamera/include/ —— 手写的只覆盖当时用到的头，会随 HAL 漂移。
    "libcamera.h",
    # ★ tracepoints.h 走的是 meson 的 custom_target('tp_header')，
    #   由 utils/tracepoints/gen-tracepoints.py 从 include/libcamera/internal/tracepoints/*.tp
    #   生成。漏了它 ⇒ request.cpp:24 找不到 "libcamera/internal/tracepoints.h"，
    #   而且是在【编到 90%】时才报（实测：17 分钟后）。
    "tracepoints.h",
}
want_re = re.compile(r".*(_ipa_interface|_ipa_serializer|_ipa_proxy)\.h$")
seen = {}
for t in json.load(open(sys.argv[1])):
    filenames = t.get("filename") or []
    if isinstance(filenames, str):
        filenames = [filenames]
    for f in filenames:
        base = f.split("/")[-1]
        if (base in want_exact or want_re.match(base)) and f not in seen:
            seen[f] = t["name"]
for f in sorted(seen):
    print("%s\t%s" % (f, seen[f]))
PY
if [ ! -s "$BUILD/gen_targets.txt" ]; then
    bad "introspect 里找不到生成目标 —— 检查 -Dipas=qcom-ife 是否生效"
    exit 1
fi
say "  发现 $(wc -l < "$BUILD/gen_targets.txt") 个生成目标"
sed 's/^/    /' "$BUILD/gen_targets.txt" | head -30

# ---------------------------------------------------------------- 3. ninja 生成
say ""
say "── 3. ninja 构建这些生成目标（不编 libcamera 本体）──"
# ★ ninja 的目标名要【相对 build 目录】，introspect 给的是绝对路径 —— 直接喂会得到
#   "unknown target '/home/.../control_ids.cpp'"（踩过）。也【不要】用 meson 的 target
#   name（"unknown target 'controls_ids_cpp'"）—— 那是 meson 侧的名字，ninja 不认。
REL_TARGETS=""
while IFS=$'\t' read -r outpath tname; do
    [ -z "${outpath:-}" ] && continue
    rel="${outpath#"$BUILD"/}"
    REL_TARGETS="$REL_TARGETS $rel"
done < "$BUILD/gen_targets.txt"
say "  目标（相对 build 目录）：$REL_TARGETS"

if ninja -C "$BUILD" $REL_TARGETS >> "$BUILD.ninja.log" 2>&1; then
    ok "生成目标全部构建成功"
else
    say "  ⚠ 指定目标失败，退回【整体 ninja】（会连 libcamera 本体一起编，慢但保证产出）："
    if ninja -C "$BUILD" >> "$BUILD.ninja.log" 2>&1; then
        ok "整体构建成功（已产出生成文件）"
    else
        bad "整体 ninja 也失败 —— 日志尾部："
        tail -20 "$BUILD.ninja.log" | sed 's/^/    /'
        echo; echo "✗ 生成失败，中止。"; exit 1
    fi
fi

# ---------------------------------------------------------------- 4. 收集
say ""
say "── 4. 收集产物到 generated/ ──"
rm -rf "$GEN"
mkdir -p "$GEN/src" "$GEN/include"

for f in control_ids.cpp property_ids.cpp version.cpp ipa_pub_key.cpp qcom-ife_ipa_proxy.cpp; do
    src="$(find "$BUILD" -name "$f" -type f 2>/dev/null | head -1)"
    if [ -n "$src" ]; then
        cp "$src" "$GEN/src/$f" && ok "generated/src/$f  ($(stat -c %s "$src") 字节)"
    else
        bad "找不到生成的 $f"
    fi
done

# 生成头：定位到 build 树里的 include/libcamera/，整棵拷过来。
# ★ 别用 `find -type d -path "*/include/libcamera" | head -1` —— 源码树里那份
#   手写的 include/libcamera 也会被匹配到，结果只拷到 1 个头（踩过）。
#   改用【锚文件】control_ids.h 反推，它只可能来自生成。
HDR_ANCHOR="$(find "$BUILD" -type f -path "*/include/libcamera/control_ids.h" 2>/dev/null | head -1)"
if [ -n "$HDR_ANCHOR" ]; then
    HDR_ROOT="${HDR_ANCHOR%/control_ids.h}"          # …/include/libcamera
    mkdir -p "$GEN/include"
    rm -rf "$GEN/include/libcamera"
    cp -r "$HDR_ROOT" "$GEN/include/libcamera"
    n_hdr="$(find "$GEN/include/libcamera" -name '*.h' | wc -l)"
    n_ipa="$(find "$GEN/include/libcamera" -name '*_ipa_interface.h' | wc -l)"
    ok "generated/include/libcamera/**  ($n_hdr 个 .h，其中 IPA 接口头 $n_ipa 个)"
else
    bad "找不到生成的 include/libcamera/control_ids.h"
fi

# ---------------------------------------------------------------- 5. 断言
say ""
say "── 5. 断言（缺一个就失败，绝不"部分成功"）──"
for f in control_ids.cpp property_ids.cpp version.cpp ipa_pub_key.cpp qcom-ife_ipa_proxy.cpp; do
    [ -s "$GEN/src/$f" ] && ok "src/$f 非空" || bad "src/$f 缺失或为空"
done
for h in control_ids.h property_ids.h formats.h version.h libcamera.h; do
    find "$GEN/include/libcamera" -name "$h" 2>/dev/null | grep -q . \
        && ok "include/libcamera/$h" || bad "include/libcamera/$h 缺失"
done
# qcom-ife 的 IPA 接口头（libcamera_ipa_qcom_ife_gk3 依赖它）
find "$GEN/include/libcamera" -name '*_ipa_interface.h' 2>/dev/null | grep -q . \
    && ok "include/libcamera/**/*_ipa_interface.h" || bad "IPA 接口头缺失"

# tracepoints.h 在 internal/ 下，单独断言一次（它在 bp 的 include 路径里是裸名 include）
find "$GEN/include/libcamera" -name 'tracepoints.h' 2>/dev/null | grep -q . \
    && ok "include/libcamera/internal/tracepoints.h" \
    || bad "include/libcamera/internal/tracepoints.h 缺失（meson 目标 tp_header）"

# ── 合同检查：libcamera-Android.bp 引用的每个 generated/** 路径都必须存在 ──
# 为什么要这道：生成产物不全时，失败点离原因很远（15 分钟的构建 + 一句
# "file not found"）。这里把"生成物是否齐"在 1 秒内判掉。
# 只检查 generated/ 开头的路径 —— 那些是我们【必须】产出的；bp 里还有 ipu3 等
# AOSP 专有路径，在【上游】libcamera 树上本来就不存在，检查它们会全是假阳性。
BP="$TREE/Android.bp"
if [ -f "$BP" ]; then
    miss=""
    while read -r rel; do
        [ -n "$rel" ] || continue
        [ -e "$TREE/$rel" ] || miss="$miss $rel"
    done < <(grep -oE '"generated/[A-Za-z0-9_./-]+"' "$BP" | tr -d '"' | sort -u)
    n_ref="$(grep -oE '"generated/[A-Za-z0-9_./-]+"' "$BP" | tr -d '"' | sort -u | wc -l)"
    if [ -z "$miss" ]; then
        ok "合同检查：bp 引用的 $n_ref 个 generated/** 路径全部存在"
    else
        bad "合同检查：bp 引用了不存在的生成物 ——$miss"
    fi
else
    warn "找不到 ${BP}，跳过合同检查"
fi

say ""
if [ "$fail" = 0 ]; then
    say "✔ 生成完成：$GEN"
    say "  下一步：确认 device.mk 引用的路径就是这里（libcamera-Android.bp 的"
    say "          local_include_dirs 里有 \"generated/include\"，srcs 里有"
    say "          \"generated/src/...\"，所以生成物必须落在 \$TREE/generated/ 下）。"
    say "  ⚠️ 这一步是【手动】的 —— 别指望 repo sync 会带来它。"
    exit 0
fi
say "✗ 生成不完整（见上面的 ✗）。别拿着半棵树去编 —— 失败会以"
say "  'unknown type name' 这种看不出关联的形式出现在很后面。"
exit 1
