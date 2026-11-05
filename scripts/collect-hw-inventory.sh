#!/usr/bin/env bash
#
# Stage 0 hardware inventory collector — MateBook E Go (sc8280xp / gaokun)
#
# Run this ON THE EGO, booted into mainline Linux, as root:
#
#     sudo bash collect-hw-inventory.sh
#
# It writes everything to ./hw-inventory-<timestamp>/ and tars it up.
# Copy the tarball back to the project box and it becomes the source of truth
# for docs/dev/hardware.md.
#
# Nothing here modifies the system. Every command is read-only.
# Missing tools are recorded as MISSING rather than aborting the run — collect
# what we can on the first pass, install the gaps, run again.

set -u

TS="$(date +%Y%m%d-%H%M%S)"
OUT="${PWD}/hw-inventory-${TS}"
mkdir -p "$OUT" || { echo "cannot create $OUT"; exit 1; }

MISSING=""

# run <outfile> <description> <cmd...>
# Captures stdout+stderr. Records the exact command line at the top of the file
# so that months later we know what produced the output.
run() {
    local f="$OUT/$1"; shift
    local desc="$1"; shift
    {
        echo "### $desc"
        echo "### \$ $*"
        echo
    } >> "$f"
    if command -v "$1" >/dev/null 2>&1 || [ -x "$1" ]; then
        "$@" >> "$f" 2>&1
        echo "  [ok]      $desc"
    else
        echo "!!! MISSING TOOL: $1" >> "$f"
        MISSING="${MISSING} $1"
        echo "  [MISSING] $desc  (needs: $1)"
    fi
    echo >> "$f"
}

# grab <outfile> <path>...  — copy/cat sysfs or proc files
grab() {
    local f="$OUT/$1"; shift
    for p in "$@"; do
        if [ -r "$p" ]; then
            echo "### $p" >> "$f"
            cat "$p" >> "$f" 2>&1
            echo >> "$f"
        else
            echo "### $p  -- NOT READABLE / ABSENT" >> "$f"
        fi
    done
}

echo "=== Stage 0 inventory -> $OUT ==="
[ "$(id -u)" -ne 0 ] && echo "!!! not root: dmesg, debugfs and some sysfs reads will be incomplete"

# ---------------------------------------------------------------- 00 identity
echo "--- identity"
run 00-identity.txt   "kernel"          uname -a
run 00-identity.txt   "os release"      cat /etc/os-release
grab 00-identity.txt  /proc/cmdline
grab 00-identity.txt  /sys/class/dmi/id/sys_vendor \
                      /sys/class/dmi/id/product_name \
                      /sys/class/dmi/id/product_sku \
                      /sys/class/dmi/id/board_name \
                      /sys/class/dmi/id/bios_version \
                      /sys/class/dmi/id/bios_date
# Mainline sc8280xp boots device-tree, not ACPI. The model/compatible strings
# here are what the AOSP device tree must eventually match.
grab 00-identity.txt  /proc/device-tree/model /proc/device-tree/compatible

# ------------------------------------------------------------------ 01 config
# .config 中所有 QCOM / ath11k / hid 相关项
echo "--- kernel config"
KCONF=""
for c in /proc/config.gz "/boot/config-$(uname -r)" /boot/config; do
    [ -r "$c" ] && KCONF="$c" && break
done
if [ -n "$KCONF" ]; then
    echo "### source: $KCONF" > "$OUT/01-kconfig-full.txt"
    case "$KCONF" in
        *.gz) zcat "$KCONF" >> "$OUT/01-kconfig-full.txt" ;;
        *)    cat  "$KCONF" >> "$OUT/01-kconfig-full.txt" ;;
    esac
    # Case-insensitive so we catch CONFIG_QCOM_*, CONFIG_ARCH_QCOM, _qcom_, etc.
    grep -iE 'qcom|qrtr|rpmh|adreno|msm|ath11k|ath12k|hid|input_|dwc3|typec|ucsi|pstore|ramoops|binder|drm|panel|nvme|remoteproc|venus|iris|spmi|pmic|interconnect|llcc|cpufreq' \
        "$OUT/01-kconfig-full.txt" | sort > "$OUT/01-kconfig-relevant.txt"
    echo "  [ok]      kernel config from $KCONF"
    echo "            $(wc -l < "$OUT/01-kconfig-relevant.txt") relevant lines"
else
    echo "!!! no readable kernel config (need CONFIG_IKCONFIG_PROC=y or /boot/config-*)" \
        > "$OUT/01-kconfig-full.txt"
    echo "  [MISSING] kernel config"
fi

# ------------------------------------------------------------------ 02 dmesg
# The single most valuable artifact. Everything else is cross-checked against it.
echo "--- dmesg"
run 02-dmesg-full.txt      "full ring buffer"  dmesg -T
run 02-dmesg-errors.txt    "errors + warnings" dmesg -T --level=emerg,alert,crit,err,warn

# dmesg | grep -i firmware 的完整固件加载路径
if [ -s "$OUT/02-dmesg-full.txt" ]; then
    grep -iE 'firmware|fw |\.mbn|\.mdt|\.b0[0-9]|\.elf|\.tlv|\.bin|nvm|loading|direct-loading|failed to load' \
        "$OUT/02-dmesg-full.txt" > "$OUT/02-dmesg-firmware.txt"
    # Pull out bare firmware paths so Stage 2 knows exactly what to ship in vendor/.
    grep -oE '[a-zA-Z0-9_/.-]+\.(mbn|mdt|elf|bin|tlv|b[0-9]{2}|fw|jsn|xml)' \
        "$OUT/02-dmesg-full.txt" | sort -u > "$OUT/02-firmware-paths.txt"
    echo "  [ok]      firmware paths: $(wc -l < "$OUT/02-firmware-paths.txt") unique"
fi
grab 02-dmesg-firmware.txt /sys/module/firmware_class/parameters/path

# --------------------------------------------------------------- 03 DRM / KMS
# modetest 完整输出：connector 名、plane 数量、支持的 format 和 modifier
echo "--- DRM / KMS"
run 03-modetest.txt  "modetest, all resources"  modetest -c -p -e -a
run 03-modetest.txt  "connectors"               modetest -c
run 03-modetest.txt  "planes (formats+modifiers)" modetest -p
run 03-modetest.txt  "encoders"                  modetest -e
run 03-modetest.txt  "framebuffers"              modetest -F

echo "### /sys/class/drm topology" > "$OUT/03-drm-sysfs.txt"
for d in /sys/class/drm/*; do
    [ -e "$d" ] || continue
    echo "--- $d" >> "$OUT/03-drm-sysfs.txt"
    for a in status enabled modes dpms edid; do
        [ -r "$d/$a" ] || continue
        if [ "$a" = edid ]; then
            # EDID is binary; hexdump it and also try to decode.
            echo "  edid (hex):" >> "$OUT/03-drm-sysfs.txt"
            hexdump -C "$d/$a" >> "$OUT/03-drm-sysfs.txt" 2>&1
        else
            echo "  $a: $(cat "$d/$a" 2>/dev/null | tr '\n' ' ')" >> "$OUT/03-drm-sysfs.txt"
        fi
    done
done
run 03-drm-sysfs.txt "drm drivers in use" sh -c 'ls -l /sys/class/drm/*/device/driver 2>&1'

# Panel: Himax HX83121A / ppc357db11, shared with Galaxy Tab S7 FE.
# Confirm what the running kernel actually binds, and capture the backlight curve.
run 03-panel.txt   "panel + backlight drivers" \
    sh -c 'ls -l /sys/class/backlight/*/device/driver /sys/bus/mipi-dsi/devices 2>&1'
for b in /sys/class/backlight/*; do
    [ -e "$b" ] || continue
    grab 03-panel.txt "$b/max_brightness" "$b/brightness" "$b/type" "$b/scale"
done
grab 03-panel.txt /sys/kernel/debug/dri/0/state

# ------------------------------------------------------------------- 04 GPU
# Stage 0 acceptance: "vulkaninfo 认出 a690"
echo "--- GPU"
run 04-gpu-vulkan.txt  "vulkaninfo summary"  vulkaninfo --summary
run 04-gpu-vulkan.txt  "vulkan devices"      vulkaninfo
run 04-gpu-gl.txt      "GL renderer"         glxinfo -B
run 04-gpu-gl.txt      "GLES"                es2_info
run 04-gpu-gl.txt      "eglinfo"             eglinfo
run 04-gpu-gl.txt      "mesa version"        sh -c 'MESA_DEBUG=1 glxinfo 2>&1 | head -40'
grab 04-gpu-gl.txt /sys/class/devfreq/*/cur_freq \
                   /sys/class/devfreq/*/available_frequencies \
                   /sys/class/devfreq/*/governor
run 04-gpu-gl.txt "adreno / freedreno in dmesg" \
    sh -c 'dmesg | grep -iE "adreno|msm_dpu|msm_dsi|a6xx|gmu|zap" 2>&1'

# ----------------------------------------------------------------- 05 input
# 触摸屏/键盘/触控板的 evdev 名和 evtest 输出 + 触摸屏走 SPI 还是 I2C
echo "--- input"
# /proc/bus/input/devices gives name AND bus id in one shot.
# Bus ids that matter here: 0018=I2C, 001C=SPI, 0003=USB, 0019=HOST.
grab 05-input-devices.txt /proc/bus/input/devices
run  05-input-devices.txt "libinput device list" libinput list-devices
run  05-input-devices.txt "evtest device enumeration" sh -c 'echo | evtest 2>&1'

# Resolve every evdev node back to its physical bus. This is what answers the
# I2C-vs-SPI question definitively rather than by inference.
echo "### evdev -> physical bus" > "$OUT/05-input-bus.txt"
for ev in /sys/class/input/event*; do
    [ -e "$ev" ] || continue
    name="$(cat "$ev/device/name" 2>/dev/null)"
    # Walk up the sysfs chain to the real controller.
    real="$(readlink -f "$ev/device" 2>/dev/null)"
    {
        echo "--- $(basename "$ev")  name='${name}'"
        echo "    path: $real"
        case "$real" in
            *i2c*) echo "    BUS: I2C" ;;
            *spi*) echo "    BUS: SPI" ;;
            *usb*) echo "    BUS: USB" ;;
            *)     echo "    BUS: (other/platform)" ;;
        esac
        [ -r "$ev/device/id/bustype" ] && \
            echo "    bustype: 0x$(cat "$ev/device/id/bustype")  (0018=I2C 001c=SPI 0003=USB)"
    } >> "$OUT/05-input-bus.txt"
done

run 05-input-bus.txt "i2c devices"      sh -c 'ls -l /sys/bus/i2c/devices/ 2>&1'
run 05-input-bus.txt "i2c drivers bound" sh -c 'for d in /sys/bus/i2c/devices/*/driver; do [ -e "$d" ] && echo "$d -> $(readlink -f "$d")"; done 2>&1'
run 05-input-bus.txt "spi devices"      sh -c 'ls -l /sys/bus/spi/devices/ 2>&1'
run 05-input-bus.txt "spi drivers bound" sh -c 'for d in /sys/bus/spi/devices/*/driver; do [ -e "$d" ] && echo "$d -> $(readlink -f "$d")"; done 2>&1'
run 05-input-bus.txt "hid devices"      sh -c 'ls -l /sys/bus/hid/devices/ 2>&1'
run 05-input-bus.txt "touch/keyboard in dmesg" \
    sh -c 'dmesg | grep -iE "hid|touch|i2c_hid|input:|elan|synaptics|himax|goodix" 2>&1'

# ----------------------------------------------------------------- 06 audio
# ALSA UCM2 配置文件（用于翻译成 mixer_paths.xml）
echo "--- audio"
run 06-audio.txt "playback devices" aplay -l
run 06-audio.txt "capture devices"  arecord -l
run 06-audio.txt "pcm list"         aplay -L
grab 06-audio.txt /proc/asound/cards /proc/asound/modules /proc/asound/pcm
run 06-audio.txt "full mixer state" amixer -c 0 contents
run 06-audio.txt "alsactl store"    alsactl -f - store
run 06-audio.txt "sound cards in dmesg" \
    sh -c 'dmesg | grep -iE "snd|asoc|q6|lpass|wcd|wsa|va-macro|rx-macro|tx-macro" 2>&1'

# Copy the UCM2 trees wholesale — Stage 4 translates these into mixer_paths.xml.
mkdir -p "$OUT/06-ucm2"
UCM_FOUND=0
for u in /usr/share/alsa/ucm2 /usr/local/share/alsa/ucm2 /var/lib/alsa/ucm2; do
    if [ -d "$u" ]; then
        # Only the conf files, and only ones plausibly ours — the full ucm2 tree
        # is thousands of files for every board Linux supports.
        find "$u" -iname '*.conf' \
            \( -ipath '*[Ss][Cc]8280*' -o -ipath '*[Qq]ualcomm*' -o -ipath '*[Ss][Dd]m845*' \
               -o -ipath '*[Ll]enovo*' -o -ipath '*[Hh]uawei*' -o -ipath '*[Gg]aokun*' \) \
            -exec cp --parents {} "$OUT/06-ucm2/" \; 2>/dev/null
        echo "$u:" >> "$OUT/06-ucm2/INDEX.txt"
        find "$u" -maxdepth 2 -iname '*[Ss][Cc]8280*' -o -maxdepth 2 -iname '*[Qq]ualcomm*' \
            >> "$OUT/06-ucm2/INDEX.txt" 2>/dev/null
        UCM_FOUND=1
    fi
done
if [ "$UCM_FOUND" = 1 ]; then
    echo "  [ok]      UCM2 configs -> 06-ucm2/"
else
    echo "  [MISSING] no ALSA UCM2 tree found (install alsa-ucm-conf)"
fi

# ---------------------------------------------------------------- 07 pstore
# Verify whether ramoops/pstore is working.
echo "--- pstore / ramoops"
run 07-pstore.txt "pstore mount" sh -c 'mount | grep -i pstore 2>&1'
run 07-pstore.txt "pstore contents" sh -c 'ls -la /sys/fs/pstore/ 2>&1'
grab 07-pstore.txt /sys/module/ramoops/parameters/mem_address \
                   /sys/module/ramoops/parameters/mem_size \
                   /sys/module/ramoops/parameters/record_size \
                   /sys/module/ramoops/parameters/console_size \
                   /sys/module/ramoops/parameters/ecc
run 07-pstore.txt "reserved-memory nodes in DT" \
    sh -c 'ls /proc/device-tree/reserved-memory/ 2>&1'
run 07-pstore.txt "ramoops/pstore in dmesg" \
    sh -c 'dmesg | grep -iE "pstore|ramoops|persistent" 2>&1'
run 07-pstore.txt "iomem (find a safe reserved window)" cat /proc/iomem

# --------------------------------------------------------------- 08 storage
# fstab uses PARTLABEL — so PARTLABEL is what we must capture.
echo "--- storage"
run 08-storage.txt "block devices with PARTLABEL" \
    lsblk -o NAME,SIZE,TYPE,FSTYPE,PARTLABEL,PARTUUID,LABEL,UUID,MOUNTPOINT
run 08-storage.txt "blkid" blkid
run 08-storage.txt "partition table" sh -c 'for d in /dev/nvme*n1; do echo "== $d"; sgdisk -p "$d" 2>&1 || fdisk -l "$d" 2>&1; done'
run 08-storage.txt "nvme list" nvme list
grab 08-storage.txt /proc/partitions /proc/mounts

# ---------------------------------------------------------- 09 EC / power
echo "--- EC / power / thermal"
run 09-ec-power.txt "EC + UCSI in dmesg" \
    sh -c 'dmesg | grep -iE "gaokun|huawei|ucsi|typec|embedded controller|\bec\b" 2>&1'
run 09-ec-power.txt "platform drivers" sh -c 'ls /sys/bus/platform/drivers/ 2>&1'
run 09-ec-power.txt "platform devices"  sh -c 'ls /sys/bus/platform/devices/ 2>&1'
for ps in /sys/class/power_supply/*; do
    [ -e "$ps" ] || continue
    echo "--- $ps" >> "$OUT/09-ec-power.txt"
    grep -r . "$ps/" 2>/dev/null | sed 's|^|    |' >> "$OUT/09-ec-power.txt"
done
run 09-ec-power.txt "thermal zones" \
    sh -c 'for t in /sys/class/thermal/thermal_zone*; do echo "$t: $(cat "$t/type" 2>/dev/null) $(cat "$t/temp" 2>/dev/null)"; done 2>&1'
run 09-ec-power.txt "cpufreq" \
    sh -c 'cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_available_frequencies 2>&1; cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>&1'
run 09-ec-power.txt "typec ports" sh -c 'ls -l /sys/class/typec/ 2>&1'

# ------------------------------------------------------- 10 wifi / bt / bus
echo "--- wifi / bt / buses"
run 10-wifi-bt.txt "ath11k in dmesg" sh -c 'dmesg | grep -iE "ath11k|wcn|qmi|bluetooth|hci_qca|btqca" 2>&1'
run 10-wifi-bt.txt "net interfaces"  ip -d link show
run 10-wifi-bt.txt "rfkill"          rfkill list
run 10-wifi-bt.txt "hci devices"     hciconfig -a
run 10-wifi-bt.txt "pci devices"     lspci -nnk
run 10-wifi-bt.txt "usb devices"     lsusb -t
run 10-wifi-bt.txt "loaded modules"  lsmod

# ------------------------------------------------------------ 11 devicetree
# The live DT is the ground truth for what the AOSP device tree must describe.
echo "--- device tree"
run 11-devicetree.dts "live DT decompiled" dtc -I fs -O dts /proc/device-tree
run 11-devicetree.dts "remoteproc"         sh -c 'for r in /sys/class/remoteproc/*; do echo "$r: $(cat "$r/name" 2>/dev/null) state=$(cat "$r/state" 2>/dev/null) fw=$(cat "$r/firmware" 2>/dev/null)"; done 2>&1'
run 11-devicetree.dts "interconnect"       sh -c 'cat /sys/kernel/debug/interconnect/interconnect_summary 2>&1'

# ------------------------------------------------------------------ wrap up
{
    echo "collected:  $(date -Iseconds)"
    echo "kernel:     $(uname -r)"
    echo "host:       $(cat /sys/class/dmi/id/product_name 2>/dev/null || cat /proc/device-tree/model 2>/dev/null)"
    echo "script:     collect-hw-inventory.sh"
    echo
    echo "missing tools:${MISSING:- none}"
} > "$OUT/MANIFEST.txt"

echo
echo "=== done ==="
cat "$OUT/MANIFEST.txt"
echo
if [ -n "$MISSING" ]; then
    echo "Install the missing tools and re-run to fill the gaps. Typical package names:"
    echo "  modetest   -> libdrm-tests / libdrm-utils"
    echo "  vulkaninfo -> vulkan-tools"
    echo "  glxinfo    -> mesa-utils"
    echo "  evtest     -> evtest"
    echo "  dtc        -> device-tree-compiler"
    echo "  nvme       -> nvme-cli"
    echo "  sgdisk     -> gdisk"
    echo
fi

TARBALL="hw-inventory-${TS}.tar.gz"
tar czf "$TARBALL" -C "$(dirname "$OUT")" "$(basename "$OUT")" 2>/dev/null \
    && echo "tarball: $(pwd)/$TARBALL  ($(du -h "$TARBALL" | cut -f1))"
echo "Copy that back to the project box."
