#!/usr/bin/env bash
# Build hxsim on the host from a kernel tree's own hx-algo.c, with shim/
# standing in for the few kernel headers it includes.  The tree needs the
# hand map (patches/0082) and the tunables of patches/0119-0121 and 0142-0143,
# so run scripts/kernel-apply-patches.sh on it first.  To replay a tree from
# before them, build that tree's own scripts/touch/hxsim/hxsim.c instead.
#
#   bash scripts/touch/hxsim/build.sh /path/to/linux [output]
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TREE=${1:?usage: build.sh <kernel tree> [output]}
OUT=${2:-hxsim}
TS=$TREE/drivers/input/touchscreen

[ -f "$TS/hx-algo.c" ] || { echo "no $TS/hx-algo.c" >&2; exit 2; }
grep -q hx_hand_update "$TS/hx-algo.h" && grep -q hx_track_report_pos "$TS/hx-algo.h" &&
	grep -q baseline_mode "$TS/hx-algo.h" && grep -q track_fw_lost_frames "$TS/hx-algo.h" ||
	{ echo "$TS/hx-algo.h lacks the hand map or patches/0119-0121, 0142-0143; apply patches/ first" >&2; exit 2; }

${CC:-cc} -O2 -Wall -Wno-unused-function -I "$HERE/shim" -I "$TS" \
	-o "$OUT" "$HERE/hxsim.c" "$TS/hx-algo.c"
echo "$OUT"
