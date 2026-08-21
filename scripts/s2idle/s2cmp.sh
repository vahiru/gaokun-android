#!/bin/bash
# 在救援 Ubuntu 上跑：把"能过"和"不能过"的开机指纹逐项对比。
B=/var/log/s2fp
PASS=(); FAIL=()
for d in $B/boot-*/; do
  r=$(cat $d/RESULT 2>/dev/null)
  n=$(basename $d)
  case "$r" in
    "DONE PASS=10/10") PASS+=("$n");;
    PENDING|PASS=*)    FAIL+=("$n");;   # 中途被复位打断 = 失败
    *)                 FAIL+=("$n");;
  esac
  printf "  %-10s %s\n" "$n" "${r:-（无 RESULT：整板复位在采集阶段）}"
done
echo "能过: ${PASS[*]:-无}"
echo "不能过: ${FAIL[*]:-无}"
[ ${#PASS[@]} -eq 0 ] || [ ${#FAIL[@]} -eq 0 ] && { echo "两类都要有样本才能比对"; exit 0; }
P=${PASS[0]}; F=${FAIL[0]}
for f in bound-devices.txt wakeup.txt pci.txt pcie-nvme.txt net.txt runtime.txt genpd.txt cmdline; do
  echo "════════ $f  ($P vs $F) ════════"
  diff -u $B/$P/$f $B/$F/$f | grep -E "^[+-]" | grep -v "^[+-][+-]" | head -40
  echo "  （差异行数: $(diff $B/$P/$f $B/$F/$f | grep -cE "^[<>]")）"
done
echo "════════ interrupts 只比"有无"，不比计数 ════════"
diff <(cut -d" " -f1 $B/$P/interrupts.txt | sort) <(cut -d" " -f1 $B/$F/interrupts.txt | sort) | head -20
echo "════════ dmesg 差异（去掉时间戳）════════"
diff <(sed "s/^\[ *[0-9.]*\] //" $B/$P/dmesg.txt) <(sed "s/^\[ *[0-9.]*\] //" $B/$F/dmesg.txt) | grep -E "^[<>]" | head -40
