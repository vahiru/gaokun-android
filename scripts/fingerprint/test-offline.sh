#!/usr/bin/env bash
# Synthetic host tests only; no device access, no real biometric input.
set -euo pipefail
fp_repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
fp_src="$fp_repo/device/huawei/gaokun3/fingerprint"
fp_tmp=$(mktemp -d "${TMPDIR:-/tmp}/gaokun3-fp-tests.XXXXXX")
trap 'rm -rf -- "$fp_tmp"' EXIT
if [ "$(uname -s)" = Darwin ]; then
    fp_crypto=${OPENSSL_PREFIX:-/opt/homebrew/opt/openssl@3}
    fp_inc="-I$fp_crypto/include"
    fp_lib="-L$fp_crypto/lib"
else
    fp_inc=$(pkg-config --cflags libcrypto)
    fp_lib=$(pkg-config --libs-only-L libcrypto)
fi
# Word splitting here is intentional for pkg-config compiler flags.
${CC:-clang} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -c "$fp_src/gaokun3-matcher.c" -o "$fp_tmp/matcher.o"
${CXX:-clang++} -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    $fp_inc "$fp_src/Core.cpp" "$fp_src/test_core.cpp" "$fp_tmp/matcher.o" \
    $fp_lib -lcrypto -o "$fp_tmp/core-test"
"$fp_tmp/core-test"
${CXX:-clang++} -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    "$fp_src/test_matcher.cpp" "$fp_tmp/matcher.o" -o "$fp_tmp/matcher-test"
"$fp_tmp/matcher-test"
python3 "$fp_repo/scripts/fingerprint/test-integration.py"
bash -n "$fp_repo/scripts/fingerprint/configure-kernel.sh" \
    "$fp_repo/scripts/kernel-apply-patches.sh" "$fp_repo/scripts/sync-device-tree.sh"
