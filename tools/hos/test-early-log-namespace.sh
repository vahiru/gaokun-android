#!/usr/bin/env bash
# Native Linux integration rehearsal using only a temporary loop-backed GPT disk.
# No physical disks or ESP are changed. Requires sudo, cc, sgdisk and namespaces.
set -euo pipefail
[[ $EUID == 0 && $# == 3 ]] || { echo 'Usage: sudo bash script SOURCE.c UUID DECODER.py' >&2; exit 2; }
SOURCE=$1
UUID=$2
DECODER=$3
[[ $UUID =~ ^[0-9a-f-]{36}$ ]] || exit 2
TASK_DIR=$(mktemp -d /tmp/hos-namespace.XXXXXX)
LOOP_DEVICE=
cleanup() {
    if [[ -n $LOOP_DEVICE ]]; then losetup -d "$LOOP_DEVICE"; fi
    rm -rf -- "$TASK_DIR"
}
trap cleanup EXIT
mkdir "$TASK_DIR/root"
cc -static -O2 -Wall -Wextra -Werror -std=c11 -DHOS_NAMESPACE_TEST \
    "-DHOS_MISC_UUID=\"$UUID\"" "$SOURCE" -o "$TASK_DIR/root/init"
cat > "$TASK_DIR/stub.c" <<'C'
#include <unistd.h>
#include <stdio.h>
int main(void) {
    if (getpid() != 1 || !access("/hos-proc", F_OK) ||
        !access("/hos-sys", F_OK) || !access("/hos-dev", F_OK)) return 2;
    puts("PID1_HANDOFF_PASS: wrapper mountpoints removed before real init");
    return 0;
}
C
cc -static -O2 -Wall -Wextra -Werror "$TASK_DIR/stub.c" -o "$TASK_DIR/root/init.hos.real"
printf '%s\n' 'androidboot.force_super_partition=super_hos androidboot.hos_earlylog=misc-ring-v1' \
    > "$TASK_DIR/root/hos-test-cmdline"
truncate -s 12M "$TASK_DIR/disk.img"
sgdisk --new=1:2048:+4M --typecode=1:8300 --change-name=1:misc_hos \
    "--partition-guid=1:$UUID" "$TASK_DIR/disk.img" >/dev/null
LOOP_DEVICE=$(losetup --find --show --partscan "$TASK_DIR/disk.img")
PARTITION=${LOOP_DEVICE}p1
for i in $(seq 1 50); do [[ -b $PARTITION ]] && break; sleep 0.1; done
[[ -b $PARTITION ]]
python3 - "$PARTITION" <<'PY'
import sys
with open(sys.argv[1], 'r+b', buffering=0) as f:
    f.write((b'BCB-PRESERVE-' * 6000)[:65536])
PY
timeout --signal=KILL 20s unshare --mount --pid --fork --kill-child \
    --propagation private chroot "$TASK_DIR/root" /init
# Read the test loop partition; never copy from a physical disk.
dd "if=$PARTITION" "of=$TASK_DIR/misc.img" bs=1M count=4 status=none
python3 - "$TASK_DIR/misc.img" "$DECODER" <<'PY'
import importlib.util, pathlib, sys
raw = pathlib.Path(sys.argv[1]).read_bytes()
assert raw[:65536] == (b'BCB-PRESERVE-' * 6000)[:65536]
spec = importlib.util.spec_from_file_location('decoder', sys.argv[2])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
info, log = m.decode(raw)
assert len(log) > 0
print('NAMESPACE_LOOP_CAPTURE_PASS:', len(log), 'bytes; reserved prefix preserved')
print(info)
PY
