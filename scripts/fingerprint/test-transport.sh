#!/usr/bin/env bash
# Linux host only; mocks device syscalls, no hardware access.
set -euo pipefail
fp_repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
fp_tmp=$(mktemp -d)
trap 'rm -rf -- "$fp_tmp"' EXIT
mkdir -p "$fp_tmp/android-base"
cat > "$fp_tmp/android-base/logging.h" <<'HEADER'
#pragma once
#include <iostream>
#define LOG(x) std::cerr
#define PLOG(x) std::cerr
HEADER
${CXX:-g++} -std=c++17 -Wall -Wextra -Werror -pthread -I"$fp_tmp" \
    "$fp_repo/device/huawei/gaokun3/fingerprint/test_transport.cpp" -o "$fp_tmp/test"
"$fp_tmp/test"
