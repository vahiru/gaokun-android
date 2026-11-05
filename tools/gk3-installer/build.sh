#!/usr/bin/env bash
# gk3-installer 的构建 / 测试 / 静态交叉编译（见 docs/dev/installer-rust-design.md）。
#
#   bash tools/gk3-installer/build.sh test     # 主机单测（macOS / Linux 都行）
#   bash tools/gk3-installer/build.sh lint     # clippy（-D warnings，含 unwrap / expect / panic 禁令）+ rustfmt --check
#   bash tools/gk3-installer/build.sh musl     # aarch64-unknown-linux-musl 静态 ELF（放进 live 镜像 / 容器里对拍用的就是它）
#   bash tools/gk3-installer/build.sh fmt      # rustfmt 就地改
#   bash tools/gk3-installer/build.sh all      # test + lint + musl
#
# 工具链：rustup 装的 stable（版本钉在 rust-toolchain.toml），加 `rustup target add aarch64-unknown-linux-musl`。
#   链接器用工具链自带的 rust-lld —— musl 目标的 crt 与 libc.a 随 rust-std 一起来（self-contained），
#   所以 macOS 上不需要任何 C 交叉工具链。
#   ⚠️ macOS 上 rustup 的 rust-lld 按 @rpath 找 libLLVM.dylib 找错了目录（找 lib/rustlib/<host>/lib/，它在 <工具链>/lib/），
#      直接跑会 "Library not loaded: libLLVM.dylib"。下面用 DYLD_FALLBACK_LIBRARY_PATH 指过去（2026-10-06 实测）。
#   ⚠️ 不经 ~/.cargo/bin 的代理（本机没装代理，只有 /opt/homebrew/bin/rustup）：直接把工具链的 bin 放进 PATH。
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TARGET=aarch64-unknown-linux-musl
die() { echo "✗ $*" >&2; exit 1; }

WANT=$(sed -n 's/^channel *= *"\(.*\)"/\1/p' "$HERE/rust-toolchain.toml")
command -v rustup >/dev/null || die "没有 rustup（macOS：brew install rustup；然后 rustup toolchain install $WANT && rustup target add $TARGET --toolchain ${WANT}）"
TC=""
for t in "$WANT" stable; do
    r=$(rustup which --toolchain "$t" rustc 2>/dev/null) || continue
    v=$("$r" --version | awk '{print $2}')
    if [ "$v" = "$WANT" ]; then TC=$(cd "$(dirname "$r")/.." && pwd); break; fi
done
[ -n "$TC" ] || die "没有版本为 $WANT 的工具链（rust-toolchain.toml 钉的）：rustup toolchain install $WANT"
export PATH="$TC/bin:$PATH"
[ -d "$TC/lib/rustlib/$TARGET" ] || die "工具链里没有 ${TARGET}：rustup target add $TARGET --toolchain $WANT"
case "$(uname -s)" in Darwin) export DYLD_FALLBACK_LIBRARY_PATH="$TC/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}" ;; esac
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER=rust-lld
cd "$HERE"
[ -f Cargo.lock ] || cargo generate-lockfile --offline   # 零依赖：锁文件只记本 crate，离线即可生成

do_test() { echo "══ cargo test（主机）"; cargo test --locked -q; }
do_lint() {
    echo "══ cargo clippy（-D warnings）"; cargo clippy --locked --all-targets -q -- -D warnings
    echo "══ cargo fmt --check"; cargo fmt --check
}
do_musl() {
    echo "══ cargo build --release --target $TARGET"
    cargo build --locked --release --target "$TARGET" -q
    local bin=$HERE/target/$TARGET/release/gk3-installer
    [ -x "$bin" ] || die "没编出来：$bin"
    # 判据写成【失败条件】：是不是动态链接（scripts/live/README.md "static-pie linked ≠ statically linked" 那一坑）
    local f; f=$(file -b "$bin")
    case "$f" in *"dynamically linked"*|*interpreter*) die "不是静态链接：$f" ;; esac
    case "$f" in *ELF*aarch64*|*ELF*ARM\ aarch64*) ;; *) die "不是 aarch64 ELF：$f" ;; esac
    ( cd "$(dirname "$bin")" && shasum -a 256 gk3-installer 2>/dev/null || sha256sum gk3-installer ) | tee "$bin.sha256"
    echo "✓ ${bin}（$(wc -c < "$bin") 字节；${f}）"
}
case "${1:-all}" in
    test) do_test ;;
    lint) do_lint ;;
    musl) do_musl ;;
    fmt) cargo fmt ;;
    all) do_test; do_lint; do_musl ;;
    *) die "用法：$0 test|lint|musl|fmt|all" ;;
esac
