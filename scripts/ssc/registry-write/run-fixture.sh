#!/bin/sh
# Explicit source checkout argument; no network/device use or source mutation.
set -eu
src=$1
build=$2
mkdir -p "$build"
flags="-std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined"
leaks=1
case $(uname -s) in Darwin) leaks=0; flags="$flags -Dst_atim=st_atimespec -Dst_mtim=st_mtimespec -Dst_ctim=st_ctimespec";; esac
cc $flags -I"$src" -I"$src/include" -Dfsync=fixture_fsync -Dwrite=fixture_write -c "$src/hexagonrpcd/hexagonfs_registry.c" -o "$build/registry.o"
cc $flags -I"$src" -I"$src/include" "$(dirname "$0")/test_registry.c" "$src/hexagonrpcd/hexagonfs.c" "$src/hexagonrpcd/hexagonfs_mapped.c" "$build/registry.o" -o "$build/test_registry"
ASAN_OPTIONS=detect_leaks=$leaks "$build/test_registry"
cc $flags -Wno-unused-parameter -I"$src" -I"$src/include" "$(dirname "$0")/test_apps_std.c" "$src/hexagonrpcd/interface/apps_std.c" "$src/hexagonrpcd/hexagonfs.c" "$src/hexagonrpcd/hexagonfs_mapped.c" "$build/registry.o" -o "$build/test_apps_std"
ASAN_OPTIONS=detect_leaks=$leaks "$build/test_apps_std"
cc $flags -Wno-unused-parameter -I"$src" -I"$src/include" "$(dirname "$0")/test_builder.c" "$src/hexagonrpcd/rpcd_builder.c" "$src/hexagonrpcd/hexagonfs.c" "$src/hexagonrpcd/hexagonfs_mapped.c" "$src/hexagonrpcd/hexagonfs_virt_dir.c" "$src/hexagonrpcd/hexagonfs_plat_subtype_name.c" "$build/registry.o" -o "$build/test_builder"
ASAN_OPTIONS=detect_leaks=$leaks "$build/test_builder"
