#!/usr/bin/env bash
# 对拍：本仓编的 mke2fs（build.sh native：同一份源码 + 补丁 + sparse_shim，-E android_sparse）与 LiveCD 用的
# Debian mkfs.ext4，按 gk3w apply 的参数、固定 UUID / 哈希种子 / 时间，格式化出来的东西是否相同。
#   bash installer/backend/mke2fs/test-align.sh      （宿主上跑；Debian 那一侧在 scripts/live 的测试容器里）
# 判据：dumpe2fs 全文只许 "Lifetime writes" 与 "Checksum" 两行不同 —— sparse I/O 管理器不统计写入量（s_kbytes_written），
#   超级块校验和随之不同；不同的字节数也只许是每个超级块副本 6 个（s_kbytes_written 的低 2 字节 + s_checksum）。
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
M=$HERE/../build/mke2fs-native/mke2fs
[ -x "$M" ] || bash "$HERE/build.sh" native >/dev/null
W=$(mktemp -d "$HERE/../build/align.XXXXXX")
trap 'rm -rf "${W:?}"' EXIT
U=01234567-89ab-cdef-0123-456789abcdef
H=fedcba98-7654-3210-fedc-ba9876543210
export MKE2FS_CONFIG=$HERE/../build/mke2fs-native/mke2fs.conf E2FSPROGS_FAKE_TIME=1700000000
# 与 apply.c 的 make_ext4 相同的参数（metadata 32 MiB、userdata 取一个不整的大小）
"$M" -q -t ext4 -U $U -E android_sparse,lazy_itable_init=0,hash_seed=$H -L metadata "$W/metadata.simg" 32768k
"$M" -q -t ext4 -U $U -E android_sparse,lazy_itable_init=0,hash_seed=$H -m 0 -L userdata "$W/userdata.simg" 29192192k
H8=$(sha256sum "$REPO/scripts/live/test-env.Dockerfile" | cut -c1-12)
IMG=gk3-test-env:$H8
docker image inspect "$IMG" >/dev/null 2>&1 || docker build -q -t "$IMG" -f "$REPO/scripts/live/test-env.Dockerfile" "$REPO/scripts/live" >/dev/null
docker run --rm -v "$W:/w" "$IMG" bash -c '
set -u; cd /tmp; export E2FSPROGS_FAKE_TIME=1700000000; fail=0
for spec in metadata:33554432: userdata:29892804608:-m0; do
    n=${spec%%:*}; rest=${spec#*:}; size=${rest%%:*}; m=${rest#*:}
    truncate -s "$size" deb.raw
    mkfs.ext4 -q -F -U '$U' -E lazy_itable_init=0,nodiscard,hash_seed='$H' ${m:+-m 0} -L "$n" deb.raw
    simg2img "/w/$n.simg" ours.raw
    dumpe2fs deb.raw 2>/dev/null > deb.txt; dumpe2fs ours.raw 2>/dev/null > ours.txt
    other=$(diff deb.txt ours.txt | grep "^[<>]" | grep -vE "^[<>] (Lifetime writes|Checksum):" | wc -l)
    sbs=$(grep -c "superblock at" deb.txt)
    bytes=$(cmp -l deb.raw ours.raw | wc -l)
    e2fsck -fn ours.raw >/dev/null 2>&1; fsck=$?
    echo "$n: $other other dumpe2fs lines differ; $bytes bytes differ (limit $((sbs * 6)), $sbs superblocks); e2fsck exit $fsck"
    [ "$other" = 0 ] && [ "$bytes" -le $((sbs * 6)) ] && [ "$fsck" = 0 ] || fail=1
    rm -f deb.raw ours.raw
done
exit $fail' && echo "OK: matches Debian mkfs.ext4 (only the write counters and superblock checksums differ)"
