#!/usr/bin/env bash
#
# gaokun-android installer — Huawei MateBook E Go (SC8280XP / gaokun3)
#
# Run this from an arm64 Linux live environment on the target machine — our
# own gaokun3 live USB, or any Ubuntu/Debian arm64 live USB. It repartitions
# the chosen disk, writes Android, and installs systemd-boot.
#
#     git clone https://github.com/vahiru/gaokun-android && cd gaokun-android
#     sudo scripts/install-gaokun3.sh /path/to/release-dir
#
# The release directory is what you download from Releases: boot.img plus
# super.img.zst (the .zst is streamed straight onto the disk — no need to
# decompress it first, and no 12 GiB of scratch space).
#
# ⚠️ THIS ERASES THE WHOLE TARGET DISK. Everything on it — Windows included.
#    Installing *next to* another OS is the graphical installer's job.
#
# ────────────────────────────────────────────────────────────────────────────
# This file is deliberately thin. Every step that touches the disk lives in
# scripts/live/installer-lib.sh, which the graphical installer uses too.
# Until 2026-09-24 this script carried its own copy of all of it, and the two
# copies had drifted apart (different rescue partition, different default boot
# entry, one kernel command line read from boot.img and one hand-kept and
# stale). One implementation, two front ends.
#
# Why the layout looks the way it does (sizes in installer-lib.sh):
#
# This machine is UEFI, not fastboot. There is no serial console. systemd-boot
# loads the kernel, DTB and ramdisk as plain files from the ESP.
#
#   esp        300M  systemd-boot + kernels + ramdisks.
#                    ★ PARTLABEL must be exactly "esp": the boot_control HAL
#                      finds it through /dev/block/by-name/esp to mirror the
#                      active A/B slot into loader.conf. The customary name
#                      "EFI system partition" has spaces and cannot be used
#                      by-name. UEFI identifies the ESP by partition *type*
#                      GUID, so renaming it is harmless.
#   misc        4M   bootloader_control (A/B slot state). Required:
#                    get_misc_blk_device() only accepts an fstab entry whose
#                    mount point is exactly "/misc" — there is no by-name
#                    fallback — and libboot_control cannot start without it.
#   metadata   32M   Android metadata.
#   boot_a/b   64M   Standard Android boot images, in AB_OTA_PARTITIONS.
#   super      12G   system/system_ext/product/vendor (Virtual A/B: one
#                    physical copy plus COW snapshots in userdata).
#   gk3rescue   1G   Optional rescue system: a squashfs mounted read-only from
#                    this partition, its writable layer in RAM
#                    (scripts/live/README.md). Installed only when
#                    rescue.squashfs + initramfs.img are available — in the
#                    release directory, or on our live USB. Two boot entries
#                    use it: one with slot a's kernel, one with slot b's.
#                    The old 24 GiB "clone whatever live system you booted"
#                    rescue is gone.
#   userdata   rest  /data. Last on disk so it can be grown without moving
#                    anything.
# ────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REL="${1:-}"
DISK="${DISK:-/dev/nvme0n1}"
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

die() { echo "!! $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }

[ -n "$REL" ] || die "usage: $0 <release-dir>"
[ -d "$REL" ] || die "no such directory: $REL"
[ -b "$DISK" ] || die "no such disk: $DISK (set DISK=/dev/...)"

# The backend: next to this script in a checkout, or where the live image puts it.
LIB=
for d in "$HERE/live" /usr/share/gaokun3; do
    [ -f "$d/installer-lib.sh" ] && { LIB=$d/installer-lib.sh; break; }
done
[ -n "$LIB" ] || die "installer-lib.sh not found — run this from a full checkout of the repository (it lives in scripts/live/), not a lone copy of this file"
# shellcheck source=live/installer-lib.sh
. "$LIB"

# Compatibility with the old knobs
[ -n "${MID:-}" ] && export GK3_MACHINE_ID=$MID
[ "${ENABLE_RECOVERY_ENTRY:-0}" = 1 ] && export GK3_ENABLE_RECOVERY_ENTRY=1

# ── preflight ──────────────────────────────────────────────────────────────
say "Preflight"
blocked=0
# GK3_SKIP_PREFLIGHT=1 exists only for scripts/live/test-apply.sh, which runs
# this script inside a container against a loop device — there is no UEFI there.
if [ "${GK3_SKIP_PREFLIGHT:-0}" = 1 ]; then
    echo "  !! PREFLIGHT SKIPPED (GK3_SKIP_PREFLIGHT=1 — test use only)"
else
while read -r _ id ok rest; do
    id=${id#id=}; ok=${ok#ok=}
    case "$ok" in
        yes)     printf '  ✓ %-11s %s\n' "$id" "$rest" ;;
        unknown) printf '  ? %-11s %s  (could not tell — continuing)\n' "$id" "$rest" ;;
        *)       printf '  ✗ %-11s %s\n' "$id" "$rest"; blocked=1 ;;
    esac
done < <(gk3_preflight)
if [ "$blocked" = 1 ]; then
    cat >&2 <<EOF

Refusing to continue. What the failures mean:
  model       this installer is for the MateBook E Go 2022 (GK-W7X) only
              (override: GK3_SKIP_MODEL_CHECK=1)
  power       battery below 15% and no charger: plug in the charger
              (override the threshold: GK3_POWER_MIN_PCT=<percent>)
EOF
    exit 1
fi
fi

# ── what will happen ───────────────────────────────────────────────────────
RESCUE=no
# Same lookup as the graphical installer (gk3__find_rescue_squashfs): the
# release directory first, then the medium this live system was started from.
if gk3__find_rescue_squashfs "$REL" >/dev/null \
   && gk3__find_file initramfs.img "$REL" /media/gk3/gaokun3 >/dev/null; then
    RESCUE=yes
fi

say "Target disk: $DISK"
sgdisk -p "$DISK" 2>/dev/null || echo "  (no partition table)"

say "New layout"
PLAN=$(gk3_plan --disk "$DISK" --mode wipe --rescue "$RESCUE") || true
if printf '%s\n' "$PLAN" | grep -q '^PLANERR'; then
    die "cannot lay out $DISK: $(printf '%s\n' "$PLAN" | grep '^PLANERR')"
fi
printf '%s\n' "$PLAN" | awk '/^PLAN op=mkpart/{
    for (i=3;i<=NF;i++){split($i,kv,"="); f[kv[1]]=kv[2]}
    printf "  %-10s %8d MiB\n", f["name"], f["size_mib"] }'
if [ "$RESCUE" = no ]; then
    echo
    echo "  (no rescue system: rescue.squashfs / initramfs.img are neither in the"
    echo "   release directory nor on a gaokun3 live USB — Android only)"
fi

cat <<EOF

This will DESTROY every partition on $DISK, including any Windows install.
There is no undo. Type exactly: ERASE
EOF
read -r confirm
[ "$confirm" = "ERASE" ] || die "aborted"

# ── install ────────────────────────────────────────────────────────────────
say "Installing"
# On failure, say whether the disk was touched at all (GK3__TOUCHED, set by gk3_apply):
# most failures happen during the checks before the first write, and then the old
# system is still intact.
if ! gk3_apply --disk "$DISK" --mode wipe --rescue "$RESCUE" --release "$REL"; then
    if [ "${GK3__TOUCHED:-}" = no ]; then
        die "installation failed before anything was written — $DISK has not been changed. See the messages above."
    fi
    die "installation failed — $DISK may be partly written and will not boot as it is. Fix the cause and run this again; the messages above also have the partition-table backup (sgdisk --load-backup=… restores the old table)."
fi

say "Done"
cat <<EOF

Reboot. The boot menu waits 15 seconds and then starts
"crDroid 16.0 (gaokun3) — slot _a".$( [ "$RESCUE" = yes ] && printf '\nThe rescue system is in the same menu, twice (using the kernel of slot a,\nand of slot b); it is never the default.' )

First boot takes a couple of minutes. Afterwards:
  * connect Wi-Fi once by hand. That is the only manual step left: the
    framework permanently disables a network it has decided has no internet,
    and only a user-initiated connection with a password clears that.
EOF
