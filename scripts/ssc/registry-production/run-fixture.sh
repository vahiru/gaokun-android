#!/bin/sh
set -eu
src=$1
build=$2
mkdir -p "$build"
python3 "$(dirname "$0")/verify-names.py" "$src"
flags="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined"
leaks=1
case $(uname -s) in Darwin) leaks=0; flags="$flags -Dst_atim=st_atimespec -Dst_mtim=st_mtimespec -Dst_ctim=st_ctimespec";; esac
cc $flags -I"$src" -I"$src/include" -Dfsync=fixture_fsync -Dwrite=fixture_write -c "$src/hexagonrpcd/hexagonfs_registry.c" -o "$build/registry.o"
cc $flags -I"$src" -I"$src/include" "$(dirname "$0")/test_apps_std.c" "$src/hexagonrpcd/interfaces.c" "$src/hexagonrpcd/hexagonfs.c" "$src/hexagonrpcd/hexagonfs_mapped.c" "$build/registry.o" -o "$build/test_apps_std"
UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=$leaks "$build/test_apps_std"

cc $flags -I"$src" -I"$src/include" "$(dirname "$0")/test_registry.c" "$src/hexagonrpcd/hexagonfs.c" "$src/hexagonrpcd/hexagonfs_mapped.c" "$build/registry.o" -o "$build/test_registry"
UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=$leaks "$build/test_registry"
cc $flags -I"$src" -I"$src/include" "$(dirname "$0")/test_builder.c" "$src/hexagonrpcd/rpcd_builder.c" "$src/hexagonrpcd/hexagonfs.c" "$src/hexagonrpcd/hexagonfs_mapped.c" "$src/hexagonrpcd/hexagonfs_virt_dir.c" "$src/hexagonrpcd/hexagonfs_plat_subtype_name.c" "$build/registry.o" -o "$build/test_builder"
UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=$leaks "$build/test_builder"
cc $flags -pthread -I"$src" -I"$src/include" "$(dirname "$0")/test_quota.c" "$src/hexagonrpcd/hexagonfs.c" "$src/hexagonrpcd/hexagonfs_mapped.c" "$build/registry.o" -o "$build/test_quota"
UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=$leaks "$build/test_quota"
cc $flags -I"$(dirname "$0")/compat" -I"$src" -I"$src/include" "$(dirname "$0")/test_listener.c" "$src/hexagonrpcd/interfaces.c" "$src/hexagonrpcd/iobuffer.c" "$src/hexagonrpcd/hexagonfs.c" "$src/hexagonrpcd/hexagonfs_mapped.c" "$build/registry.o" -o "$build/test_listener"
UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=$leaks "$build/test_listener"
