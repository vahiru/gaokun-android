#!/usr/bin/env bash
# 取 Windows 安装器随程序分发的 systemd-bootaa64.efi（gk3w apply --sdboot；LGPL-2.1-or-later）。
#
#   bash installer/backend/sdboot/fetch.sh   → installer/backend/build/sdboot/systemd-bootaa64.efi
#
# 来源：Debian 的 systemd-boot-efi，与 LiveCD 同一版本（installer/tools/build/packages-live.lock），所以 Windows 端装出来的
# ESP 与 LiveCD 装的相同（notes：VHDX 上逐文件对拍过）。包与包里的 efi 都按 sha256 钉死。
# 先从 deb.debian.org 的 pool 取；那边换成新的修订版后旧包会下架，再按包的 sha1 从 snapshot.debian.org 取。
# 只用 curl、ar（binutils）、tar、xz、sha256sum。
set -euo pipefail
VER=257.13-1~deb13u1
DEB="systemd-boot-efi_${VER}_arm64.deb"
DEB_SHA256=0e4529d9a72b35c31f34151ef8a0df20f322cfef679f4f5439394e60505b91a2
DEB_SHA1=da0023d6c8fd04220dd9cbd6bfb69cc81082994f
EFI_SHA256=90b2d5b58dc7fd0b4e62e64b0705420b4d5064b0f9314516e950fb14e1059eac
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=$HERE/../build/sdboot
EFI=$OUT/systemd-bootaa64.efi
die() { echo "sdboot/fetch.sh: $*" >&2; exit 1; }

if [ -f "$EFI" ] && [ "$(sha256sum "$EFI" | cut -d' ' -f1)" = "$EFI_SHA256" ]; then
    echo "OK (already there): $EFI"; exit 0
fi
mkdir -p "$OUT"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
got=
for url in "https://deb.debian.org/debian/pool/main/s/systemd/${DEB//\~/%7E}" "https://snapshot.debian.org/file/$DEB_SHA1"; do
    if curl -fsSL --retry 3 --max-time 120 -o "$TMP/pkg.deb" "$url" && [ "$(sha256sum "$TMP/pkg.deb" | cut -d' ' -f1)" = "$DEB_SHA256" ]; then
        got=$url; break
    fi
    echo "sdboot/fetch.sh: $url unavailable or wrong sha256, trying the next source" >&2
done
[ -n "$got" ] || die "cannot download $DEB"
( cd "$TMP" && ar x pkg.deb data.tar.xz && tar -xJf data.tar.xz ./usr/lib/systemd/boot/efi/systemd-bootaa64.efi )
[ "$(sha256sum "$TMP/usr/lib/systemd/boot/efi/systemd-bootaa64.efi" | cut -d' ' -f1)" = "$EFI_SHA256" ] || die "systemd-bootaa64.efi in $DEB has an unexpected sha256"
cp "$TMP/usr/lib/systemd/boot/efi/systemd-bootaa64.efi" "$EFI.tmp" && mv "$EFI.tmp" "$EFI"
echo "OK: $EFI (from $got)"
