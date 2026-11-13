#!/usr/bin/env bash
# installer-lib.sh 的方案计算自测。
#
# ★ 这部分是【纯计算】，所以不需要目标硬件、不需要 root、不需要磁盘 ——
#   在任何机器上 `bash scripts/live/test-plan.sh` 都能跑。
#
# ⚠️ 重点验的不是"能不能算出方案"，而是两条会毁数据的不变量：
#     1. 分区之间【不重叠】
#     2. 所有分区都【落在允许的区间内】（双系统安装时尤其要命：
#        越界一个扇区就写到别人的 Windows 上了）
set -u
cd "$(dirname "$0")/../.."
. scripts/live/installer-lib.sh

PASS=0; FAIL=0
ok()   { echo "  ✓ $*"; PASS=$((PASS+1)); }
bad()  { echo "  ✗ $*"; FAIL=$((FAIL+1)); }

# 检查一份方案：不重叠、不越界、名字齐全
check_plan() {
    local out=$1 lo=$2 hi=$3 want_names=$4
    local prev_end=$(( lo - 1 )) names="" n=0
    while read -r _ op rest; do
        case "$op" in op=mkpart) ;; *) continue ;; esac
        eval "local $rest"   # start= end= name= size_mib= …
        n=$((n+1)); names="$names $name"
        if [ "$start" -le "$prev_end" ]; then bad "重叠：$name start=$start <= 前一个 end=$prev_end"; return 1; fi
        if [ "$start" -lt "$lo" ] || [ "$end" -gt "$hi" ]; then
            bad "越界：$name [$start,$end] 不在 [$lo,$hi] 内"; return 1
        fi
        if [ $(( start % 2048 )) -ne 0 ]; then bad "未对齐：$name start=$start"; return 1; fi
        prev_end=$end
    done <<EOF
$(printf '%s\n' "$out" | grep '^PLAN op=mkpart')
EOF
    [ "$n" -gt 0 ] || { bad "一个分区都没算出来"; return 1; }
    local w
    for w in $want_names; do
        case " $names " in *" $w "*) ;; *) bad "缺分区 $w"; return 1 ;; esac
    done
    ok "$n 个分区：不重叠、不越界、全部 1 MiB 对齐（$names )"
}

echo "═══ 1. 整盘安装（476 GiB，带救援）═══"
OUT=$(gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue yes --disk-size-mib 476940)
echo "$OUT" | grep '^PLANSUM' | sed 's/^/  /'
check_plan "$OUT" 2048 $(( 476940 * 2048 - 2048 )) "esp misc metadata boot_a boot_b super gk3rescue userdata"

echo "═══ 2. 整盘安装（不带救援）═══"
OUT=$(gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue no --disk-size-mib 476940)
echo "$OUT" | grep -c '^PLAN op=mkpart' | sed 's/^/  分区数 /'
printf '%s' "$OUT" | grep -q 'name=gk3rescue' && bad "不该有救援分区" || ok "没有救援分区"

echo "═══ 3. 双系统：25 GiB 空闲区（复用现有 ESP）═══"
# 假设空闲区从扇区 100000000 开始，25 GiB
RS=100000000; RE=$(( RS + 25 * 1024 * 2048 - 1 ))
OUT=$(gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue no \
      --region-start $RS --region-end $RE --esp /dev/nvme0n1p1)
echo "$OUT" | grep '^PLANSUM' | sed 's/^/  /'
printf '%s' "$OUT" | grep -q '^PLAN op=useesp' && ok "复用现有 ESP" || bad "没有复用 ESP 的记录"
printf '%s' "$OUT" | grep -q 'name=esp ' && bad "双系统模式不该新建 ESP" || ok "没有新建 ESP"
check_plan "$OUT" $RS $RE "misc metadata boot_a boot_b super userdata"

echo "═══ 4. 空间不够要明确报错，不能算出个坏方案 ═══"
RE2=$(( RS + 10 * 1024 * 2048 - 1 ))
OUT=$(gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue no \
      --region-start $RS --region-end $RE2 --esp /dev/nvme0n1p1 2>/dev/null || true)
if printf '%s' "$OUT" | grep -q '^PLANERR msg=not-enough-space'; then
    ok "10 GiB 空闲区被拒：$(printf '%s' "$OUT" | grep '^PLANERR')"
else bad "10 GiB 居然通过了"; fi
# fixed_mib：除 /data 之外的固定开销 —— 必须 = need_mib − /data 下限
PE=$(printf '%s' "$OUT" | grep '^PLANERR')
[ -n "$(gk3__f "$PE" fixed_mib)" ] && [ "$(( $(gk3__f "$PE" fixed_mib) + GK3_USERDATA_MIN_MIB ))" = "$(gk3__f "$PE" need_mib)" ] \
    && ok "PLANERR 带 fixed_mib=$(gk3__f "$PE" fixed_mib)（+ /data 下限 = need_mib）" || bad "PLANERR 的 fixed_mib 不对：$PE"

echo "═══ 5. 双系统模式必须有现成 ESP ═══"
OUT=$(gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue no \
      --region-start $RS --region-end $RE 2>/dev/null || true)
printf '%s' "$OUT" | grep -q 'alongside-needs-existing-esp' && ok "没给 ESP 时明确报错" || bad "没给 ESP 却算出了方案"

echo "═══ 6. userdata 下限 ═══"
OUT=$(gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue no --disk-size-mib 476940 --userdata-mib 100 2>/dev/null || true)
printf '%s' "$OUT" | grep -q 'userdata-too-small' && ok "userdata 太小被拒" || bad "userdata=100MiB 居然通过了"

# ★ macOS 自带的 bash 3.2 在 UTF-8 下把紧跟在 $变量 后面的中文字节读进变量名（"VER�: unbound variable"）——
#   2026-09-27 release-installer.sh 第一次跑就死在这。在 Mac 上跑的脚本一律写 ${变量}
BADV=$(for f in scripts/*.sh scripts/*/*.sh; do
    perl -CSD -ne 'print "'"$f"':$.\n" if /(?<![\\])\$[A-Za-z_][A-Za-z0-9_]*[^\x00-\x7f]/' "$f"; done)
[ -z "$BADV" ] && ok "脚本里没有紧跟中文的 \$变量（bash 3.2 会读错）" || bad "这些地方 \$变量 后面紧跟中文，要写成 \${变量}：$(echo $BADV)"

echo
echo "═══ 通过 $PASS · 失败 $FAIL ═══"
[ "$FAIL" -eq 0 ]
