#!/bin/bash
# gk3-misc init（S10，安装器初始化 misc）的主机测试：期望字节由 Python 独立算（zlib.crc32 / hashlib.sha1），不抄 C 的实现。
#
#   bash tools/gk3boot/test/misc/run.sh      （或 make -C tools/gk3boot misc-test；先 make gk3-misc）
#
# 断言：
#   * 64 KiB 里只有三处有内容：BCB（0–2 KiB）全零、BCAB（2048，32 字节）、GK3 记录（8192，2 KiB）；其余字节一个不改
#     （"其余"故意先填成 0x5a —— 证明 init 不顺手清别处：清零是安装器 dd 的事，init 只管自己那三块）
#   * BCAB = libboot_control 布局：'_a'、magic、version 1、nb_slot 2、_a 15/6/未成功、_b 0/0、CRC32（前 28 字节）
#   * GK3 记录 = magic/version/size、flags bit0（已迁移）、dispatch_ver 1、seq 1、ev_head 1、
#     migrated_digest = SHA-1(2048 个零)、事件 #1 = migrated(slot 0xff, aux 1)、set_default（--default）、CRC32（前 2044 字节）
#   * BCB 里原有的垃圾被清掉；--slot b；--default android；用法错 / 小于 64 KiB 时退出码 2、文件不变
set -u
G=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
T=$G/build/gk3-misc
[ -x "$T" ] || { echo "先 make -C tools/gk3boot gk3-misc"; exit 2; }
W=$G/build/misc-test; rm -rf "$W"; mkdir -p "$W"
PASS=0; FAIL=0
ok()  { echo "  ✓ $*"; PASS=$((PASS+1)); }
bad() { echo "  ✗ $*"; FAIL=$((FAIL+1)); }

# want <slot a|b> <setdef 0|1|2> <输出文件>：期望的 64 KiB（"其余"是 0x5a）
want() {
    python3 - "$1" "$2" "$3" <<'PY'
import hashlib, struct, sys, zlib
slot, setdef, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
m = bytearray(b"\x5a" * 65536)
m[0:2048] = bytes(2048)                                   # BCB 清零
bc = bytearray(32)
bc[0:2] = b"_" + slot.encode()
struct.pack_into("<I", bc, 4, 0x42414342)                 # magic "BCAB"
bc[8] = 1                                                 # version
struct.pack_into("<H", bc, 9, 2)                          # nb_slot:3 = 2，recovery_tries / merge_status = 0
act = 15 | (6 << 4)                                       # priority 15、tries 6、successful 0
i = 0 if slot == "a" else 1
struct.pack_into("<H", bc, 12 + 2 * i, act)
struct.pack_into("<I", bc, 28, zlib.crc32(bytes(bc[0:28])) & 0xffffffff)
m[2048:2080] = bc
r = bytearray(2048)
struct.pack_into("<IHHIIIB", r, 0, 0x52334B47, 1, 2048, 1, 1, 1, 0)   # magic version size flags dispatch_ver seq streak
r[26] = 1                                                 # ev_head：下一条写到 #1
r[48:68] = hashlib.sha1(bytes(2048)).digest()             # migrated_digest
r[360] = setdef
struct.pack_into("<IHBBII", r, 1024, 1, 8, 0xff, 0, 1, 0) # 事件 #1：seq 1、code 8（migrated）、slot 0xff、aux 1
struct.pack_into("<I", r, 2044, zlib.crc32(bytes(r[0:2044])) & 0xffffffff)
m[8192:10240] = r
open(out, "wb").write(m)
PY
}
fill() { python3 -c 'import sys; open(sys.argv[1], "wb").write(b"\x5a" * int(sys.argv[2]))' "$1" "$2"; }

echo "── 默认（--slot a、没有 --default）"
fill "$W/a.img" 1048576
printf 'boot-recovery' | dd of="$W/a.img" conv=notrunc status=none     # BCB 里原有的垃圾：要被清掉
OUT=$("$T" init "$W/a.img" 2>/dev/null); rc=$?
want a 0 "$W/a.want"
[ "$rc" = 0 ] && cmp -s <(head -c 65536 "$W/a.img") "$W/a.want" && ok "前 64 KiB 与 Python 独立算的逐字节相同（BCB 清零、BCAB、GK3 记录；其余 0x5a 一个没动）" \
    || { bad "字节不对（rc=${rc}）"; cmp <(head -c 65536 "$W/a.img") "$W/a.want" | head -3; }
cmp -s <(tail -c +65537 "$W/a.img") <(python3 -c 'import sys; sys.stdout.buffer.write(b"\x5a" * (1048576 - 65536))') \
    && ok "64 KiB 之后一个字节没动" || bad "64 KiB 之后被改了"
printf '%s' "$OUT" | grep -qE '^MISCINIT slot=a default=none bcab=5f610000424341420102000' && printf '%s' "$OUT" | grep -q ' rest=nonzero ' \
    && ok "输出：$OUT" || bad "输出不对：$OUT"
D=$("$T" dump "$W/a.img")
printf '%s' "$D" | grep -q '_a priority=15 tries=6 successful=0' && printf '%s' "$D" | grep -q '_b priority=0 tries=0 successful=0 .*-> unbootable' \
  && printf '%s' "$D" | grep -q 'GK3      valid  migrated=1 boot_streak=0' && printf '%s' "$D" | grep -q '#1 migrated slot=255 aux=1' \
  && printf '%s' "$D" | grep -q 'set_default=none' \
    && ok "gk3-misc dump 读得回来：_a 15/6 未成功、_b 0/0 不可启动、已迁移、事件 migrated" || { bad "dump 不对"; printf '%s\n' "$D" | sed 's/^/      /'; }

echo "── 安装器的真实用法：先整块清零，再 init"
head -c 1048576 /dev/zero > "$W/z.img"
OUT=$("$T" init "$W/z.img" --slot a --default windows 2>/dev/null); rc=$?
want a 1 "$W/z.want"; python3 - "$W/z.want" <<'PY'
import sys; p = sys.argv[1]; m = bytearray(open(p, "rb").read())
for a, b in ((2080, 8192), (10240, 65536)): m[a:b] = bytes(b - a)
open(p, "wb").write(m)
PY
[ "$rc" = 0 ] && cmp -s <(head -c 65536 "$W/z.img") "$W/z.want" && printf '%s' "$OUT" | grep -q ' default=windows .* rest=zero ' \
    && "$T" dump "$W/z.img" | grep -q 'set_default=windows clean_poweroff=0' \
    && ok "清零后 init --default windows：逐字节对、rest=zero、dump 看到 set_default=windows" || bad "清零后 init 不对（rc=${rc}）：$OUT"

echo "── --slot b、--default android"
head -c 65536 /dev/zero > "$W/b.img"
"$T" init "$W/b.img" --slot b --default android >/dev/null 2>&1; rc=$?
want b 2 "$W/b.want"; python3 - "$W/b.want" <<'PY'
import sys; p = sys.argv[1]; m = bytearray(open(p, "rb").read())
for a, b in ((2080, 8192), (10240, 65536)): m[a:b] = bytes(b - a)
open(p, "wb").write(m)
PY
[ "$rc" = 0 ] && cmp -s "$W/b.img" "$W/b.want" && ok "--slot b --default android：逐字节对（正好 64 KiB 的镜像也行）" || bad "--slot b 不对（rc=${rc}）"

echo "── 反例：都不许写"
head -c 65535 /dev/zero > "$W/s.img"; S0=$(shasum "$W/s.img")
"$T" init "$W/s.img" >/dev/null 2>&1; rc=$?
[ "$rc" = 2 ] && [ "$(shasum "$W/s.img")" = "$S0" ] && ok "小于 64 KiB：退出码 2、文件没变" || bad "小于 64 KiB：rc=${rc}"
cp "$W/a.want" "$W/u.img"; S0=$(shasum "$W/u.img")
for args in "--slot c" "--default linux" "--bogus" "--slot"; do
    # shellcheck disable=SC2086
    "$T" init "$W/u.img" $args >/dev/null 2>&1; rc=$?
    [ "$rc" = 2 ] && [ "$(shasum "$W/u.img")" = "$S0" ] || { bad "用法错（$args）：rc=${rc} 或文件变了"; continue; }
done
[ "$FAIL" = 0 ] && ok "用法错（--slot c / --default linux / --bogus / 缺值）：退出码 2、文件没变"
"$T" init "$W/nope.img" >/dev/null 2>&1; rc=$?
[ "$rc" = 2 ] && [ ! -e "$W/nope.img" ] && ok "文件不存在：退出码 2、不新建" || bad "文件不存在：rc=${rc}"

echo "══ gk3-misc init：通过 ${PASS}，失败 ${FAIL}"
[ "$FAIL" = 0 ]
