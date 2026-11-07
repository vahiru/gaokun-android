#!/usr/bin/env bash
# Clone upstream reference trees into refs/.
#
# Shallow, single-branch: these are read-only references to grep against,
# not trees we develop in. Total ~2.3 GB (+ ~0.5 GB for the boot-entry set).
#
#     bash scripts/clone-refs.sh            # 全部
#     bash scripts/clone-refs.sh gbl edk2   # 只克隆列出的目录名
#
# Re-running is safe — existing clones are skipped（钉了提交的会核对 HEAD，不符只告警不动）。

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)/refs"
mkdir -p "$ROOT"
cd "$ROOT" || exit 1

# dir|url|branch    (branches verified against the remotes 2026-08-13)
#
# 可选的第 4、5 列（2026-10-05 起，统一启动入口 tools/gk3boot/README.md 引用的上游）：
#   第 4 列 = 钉住的提交（文中的 `文件:行号` 都是对这个提交说的）；
#             分支头移动了就按提交单独 fetch，取不到只告警
#   第 5 列 = 稀疏检出的目录（空格分隔），只要这几个子目录
REPOS="
linux-gaokun|https://github.com/right-0903/linux-gaokun|main
matebook-e-go-linux|https://github.com/whitelewi1-ctrl/matebook-e-go-linux|master
boot-works|https://github.com/matalama80td3l/matebook-e-go-boot-works|main
aospm-device-sdm845|https://github.com/aospm/android_device_generic_sdm845|main
aospm-manifests|https://github.com/aospm/android_local_manifests|main
aospm-system-core|https://github.com/aospm/platform_system_core|master
aospm-tinyhal|https://github.com/aospm/tinyhal|master
jhovold-linux|https://github.com/jhovold/linux|wip/sc8280xp-6.16
egotouchrev-linux|https://github.com/chiyuki0325/EGoTouchRev-Linux|main
gaokun-buildbot|https://github.com/KawaiiHachimi/linux-gaokun-buildbot|main
egotouchrev-rebuild|https://github.com/awarson2233/EGoTouchRev-rebuild|main
libcamera|https://gitlab.freedesktop.org/camera/libcamera.git|master
lineage-sepolicy|https://github.com/LineageOS/android_system_sepolicy|lineage-23.0
lineage-bootable-recovery|https://github.com/LineageOS/android_bootable_recovery|lineage-23.0
systemd-v257|https://github.com/systemd/systemd.git|v257.13|70b5d110be7702afc4dbce012f60d49506d513da|src/boot src/fundamental docs man
aosp-hardware-interfaces|https://android.googlesource.com/platform/hardware/interfaces|main|1a56e38edc2f2f6189ef405ee1edce554e15cbc0|boot
gbl|https://android.googlesource.com/platform/bootable/libbootloader|gbl-mainline|e8577449164625a3167efdd25dab4e6c3144fd0f|
clo-abl-5.0|https://git.codelinaro.org/clo/la/abl/tianocore/edk2.git|uefi.lnx.5.0.r53-rel|9dd1d0b8f913b0a1bdac06f32efb0ced8a721b35|
clo-abl-6.0|https://git.codelinaro.org/clo/la/abl/tianocore/edk2.git|uefi.lnx.6.0.r49-rel|72e4842ebac6b6b77077cf6908908db0285c0275|
edk2|https://github.com/tianocore/edk2.git|master|999fd0f12a27709eee04b93e46bd867e6b0163a5|ArmPkg EmbeddedPkg MdeModulePkg MdePkg NetworkPkg SecurityPkg ShellPkg
"

WANT=" $* "

echo "=== clone start ==="

echo "$REPOS" | while IFS='|' read -r dir url branch commit sparse; do
    [ -z "$dir" ] && continue
    if [ $# -gt 0 ] && [ "${WANT#* $dir }" = "$WANT" ]; then
        continue
    fi

    if [ -d "$dir/.git" ]; then
        if [ -n "${commit:-}" ]; then
            have=$(git -C "$dir" rev-parse HEAD 2>/dev/null)
            if [ "$have" != "$commit" ]; then
                echo "WARN   $dir HEAD=$have，期望钉住的 $commit（行号引用以钉住的为准）"
                continue
            fi
        fi
        echo "SKIP   $dir (already present)"
        continue
    fi

    echo "CLONE  $dir  <- $url @ $branch${commit:+ ($commit)}${sparse:+ [sparse: $sparse]}"
    # autocrlf=false: CRLF conversion would corrupt kernel patches and shell scripts.
    # longpaths=true: the Linux tree has paths past the 260-char Win32 limit.
    # symlinks=false: avoids needing Developer Mode / admin on Windows.
    extra=""
    [ -n "${sparse:-}" ] && extra="--filter=blob:none --sparse"
    # shellcheck disable=SC2086
    git -c core.autocrlf=false \
        -c core.longpaths=true \
        -c core.symlinks=false \
        clone --depth 1 --single-branch --branch "$branch" $extra \
        "$url" "$dir" 2>&1 | tail -5

    if [ ! -d "$dir/.git" ]; then
        echo "FAIL   $dir"
        continue
    fi

    if [ -n "${sparse:-}" ]; then
        # shellcheck disable=SC2086
        git -C "$dir" sparse-checkout set $sparse 2>&1 | tail -3
    fi

    if [ -n "${commit:-}" ]; then
        have=$(git -C "$dir" rev-parse HEAD)
        if [ "$have" != "$commit" ]; then
            echo "PIN    $dir：$branch 已移到 $have，按提交取 $commit"
            if git -C "$dir" fetch --depth 1 origin "$commit" 2>&1 | tail -3 &&
               git -C "$dir" -c advice.detachedHead=false checkout -q "$commit"; then
                :
            else
                echo "WARN   $dir 取不到 $commit，留在 $have —— 设计稿里的行号可能对不上"
            fi
        fi
        have=$(git -C "$dir" rev-parse HEAD)
        [ "$have" = "$commit" ] && echo "OK     $dir @ $commit" || echo "WARN   $dir @ $have"
    else
        echo "OK     $dir"
    fi
done

echo "=== clone done ==="
du -sh "$ROOT"/* 2>/dev/null
