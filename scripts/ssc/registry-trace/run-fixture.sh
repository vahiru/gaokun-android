#!/bin/sh
# Source argument must be an isolated 435ed1c + CR treefix + trace candidate.
set -eu
src=$1
build=$2
mkdir -p "$build"
fixture=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
flags="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined -D__ANDROID__"
case $(uname -s) in Darwin) flags="$flags -Dst_atim=st_atimespec -Dst_mtim=st_mtimespec -Dst_ctim=st_ctimespec";; esac
cc $flags -I"$fixture" -I"$src" -I"$src/include" "$fixture/test_trace.c" "$src/hexagonrpcd/interfaces.c" "$src/hexagonrpcd/hexagonfs.c" "$src/hexagonrpcd/hexagonfs_mapped.c" -pthread -o "$build/test_trace"
"$build/test_trace"
# Optional pristine baseline comparison; this fixture never writes any source.
if [ "$#" -ge 3 ]; then
    baseline=$3
    plainflags=$(printf '%s' "$flags" | sed 's/-D__ANDROID__//')
    for source in "$baseline" "$src"; do
        cc $plainflags -I"$source" -I"$source/include" "$fixture/test_contract.c" "$source/hexagonrpcd/interfaces.c" "$source/hexagonrpcd/hexagonfs.c" "$source/hexagonrpcd/hexagonfs_mapped.c" -o "$build/test_contract"
        "$build/test_contract"
    done
fi
