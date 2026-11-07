#!/usr/bin/env bash
# M3 验收：确认 Android 真的跑在硬件 turnip 上，且 GPU 没有暗伤。
# 在【宿主机】跑（走 adb），设备需已进 Android。全程只读（帧读回走 exec-out，不在设备上写文件）。
#
#   [SER=gaokun3] bash scripts/verify-turnip.sh [浸泡秒数]
#   退出码：0 = 全过，1 = 有失败项，2 = adb 不通。scripts/accept.sh 的 A 档靠它。
#
# 每一项都对应 Stage 5 踩过的一个坑，别删：
#   ro.hardware.vulkan   —— 加载哪个 Vulkan HAL（pastel=软渲染兜底）
#   Turnip Adreno 690    —— 只有真正初始化成功才会有；属性对了不等于在用
#   GMU 错误计数         —— GX_BW_PERF_VOTE 超时 / watchdog / gdsc didn't collapse
#                           三者任一非 0 = D3 那条死亡链又起来了
#   smmustall 服务       —— 常驻解锁器；它不跑 = 第一次页错误就永久挂死
#   抓 fault=N           —— 解锁器抓到的真实 GPU 页错误（0004 v3 之后应为 0）
#   screencap            —— 帧读回；Stage 5 时它会永久卡死，是最灵敏的探针
#
# ⚠️ 2026-10-04 改（PERF-12）：开机几小时后跑，旧版三处判据会【假阴性】——
#   * 第 2 步原来在 logcat -d 里找 "Turnip Adreno"：main 缓冲只有 256 KiB，开机 7 小时后一条都不剩。
#     改读 dumpsys SurfaceFlinger 的 "GLES:" 行（RenderEngine 走 ANGLE → Vulkan，那一行常驻），
#     实机：GLES: … ANGLE (Qualcomm, Vulkan 1.3.335 (Turnip Adreno (TM) 690 …), turnip Mesa driver-…)
#   * GMU / a6xx_recover 原来数 dmesg：CONFIG_LOG_BUF_SHIFT=17（128 KiB），开机几小时只剩最后一千多行，
#     被 healthd 与 SLPI handover 冲掉。改成 dmesg 与 logd 的 kernel 缓冲（通常从开机 4 秒起）取较大值，
#     并把两者各自从几秒开始打出来 —— 覆盖不全时自己看得见。
#   * FAULT# 原来在 logcat 里数：同样会滚掉。改读最近一次心跳里的累计值
#     （smmu-nostall.sh 每约 60 秒打一行 "心跳 round=… 清 CFCFG=… 抓 fault=N"）。
set -u
export MSYS_NO_PATHCONV=1
SECS=${1:-0}          # 可选：浸泡秒数，之后再复查一次
SER=${SER:-${SERIAL:-}}
ADB="adb ${SER:+-s $SER}"
PASS=0; FAIL=0
ok()   { echo "  [OK]   $*"; PASS=$((PASS + 1)); }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }
info() { echo "  [INFO] $*"; }
A() { $ADB shell "$@" 2>/dev/null | tr -d '\r'; }

# 不用 wait-for-device：没有设备时它会一直阻塞，到不了"退出 2"
$ADB get-state >/dev/null 2>&1 || { echo "adb 不通（SER=${SER:-未设}），停"; exit 2; }
[ -n "$(A getprop sys.boot_completed)" ] || { echo "adb 不通，停"; exit 2; }

# GMU 死亡链与 recover：两个来源取较大值（它们重叠，相加会重复计数）。
# 模式比旧版的裸 "timed out" 收窄到 GMU / HFI：
# logd 的 kernel 缓冲覆盖整个开机过程，别的驱动的 "timed out" 会混进来误判。
GMU_RE="HFI_.*timed out|gmu.*timed out|watchdog expired|gdsc didn't collapse"
gmu_count() {
    A "a=\$(dmesg 2>/dev/null | grep -ciE \"$GMU_RE\"); b=\$(logcat -b kernel -d 2>/dev/null | grep -ciE \"$GMU_RE\");
       [ \"\${a:-0}\" -gt \"\${b:-0}\" ] && echo \${a:-0} || echo \${b:-0}"
}
recover_count() {
    A 'a=$(dmesg 2>/dev/null | grep -c a6xx_recover); b=$(logcat -b kernel -d 2>/dev/null | grep -c a6xx_recover);
       [ "${a:-0}" -gt "${b:-0}" ] && echo ${a:-0} || echo ${b:-0}'
}
# 最近一次 smmustall 心跳里的累计 fault 数；没有心跳输出空串
fault_count() {
    A 'logcat -d -s smmustall 2>/dev/null | grep "心跳" | tail -1' | sed -n 's/.*抓 fault=\([0-9][0-9]*\).*/\1/p'
}

echo "═══ 1. 属性与 HAL ═══"
A 'echo -n "ro.hardware.vulkan = "; getprop ro.hardware.vulkan
echo -n "debug.hwui.renderer = "; getprop debug.hwui.renderer
echo -n "persist.graphics.egl = "; getprop persist.graphics.egl
ls -la /vendor/lib64/hw/vulkan.*.so'

echo; echo "═══ 2. turnip 真的被初始化了吗（SurfaceFlinger 的 GLES 行）═══"
GLES=$(A 'dumpsys SurfaceFlinger 2>/dev/null | grep -m1 "^GLES:"')
echo "  $GLES"
case "$GLES" in
    *"Turnip Adreno (TM) 690"*"turnip Mesa"*) ok "RenderEngine 跑在 turnip 上" ;;
    "") bad "dumpsys SurfaceFlinger 里没有 GLES 行（SurfaceFlinger 没起来？）" ;;
    *)  bad "GLES 行里没有 Turnip Adreno 690（软渲染兜底？看 ro.hardware.vulkan）" ;;
esac

echo; echo "═══ 3. GPU / GMU 内核侧 ═══"
A 'echo "  日志覆盖：dmesg 从 $(dmesg 2>/dev/null | head -1 | sed -n "s/^\[ *\([0-9.]*\)\].*/\1/p") 秒起，logd kernel 缓冲从 $(logcat -b kernel -d -v monotonic 2>/dev/null | grep -m1 "^ *[0-9]" | awk "{print \$1}") 秒起"'
G=$(gmu_count); R=$(recover_count)
[ "${G:-0}" = 0 ] && ok "GMU 错误行数 0" || bad "GMU 错误行数 ${G}（D3 死亡链）"
[ "${R:-0}" = 0 ] && ok "a6xx_recover 0" || bad "a6xx_recover $R 次"
echo "  --- adreno probe（开机早期的行，日志滚掉后为空属正常）---"
A 'dmesg | grep -iE "adreno|zap" | tail -4' | sed 's/^/  /'

echo; echo "═══ 4. SMMU 解锁器 ═══"
S=$(A getprop init.svc.smmustall)
[ "$S" = running ] && ok "init.svc.smmustall = running" || bad "init.svc.smmustall = [$S]（不跑 = 第一次页错误就永久挂死）"
echo "  --- 最近心跳 ---"; A 'logcat -d -s smmustall 2>/dev/null | tail -2' | sed 's/^/  /'
F0=$(fault_count)
if [ -z "$F0" ]; then
    bad "logcat 里找不到 smmustall 心跳（服务在跑的话约每 60 秒一行；没有 = 脚本卡住或日志没进 logd）"
elif [ "$F0" = 0 ]; then ok "心跳：抓 fault=0"
else bad "心跳：抓 fault=${F0}（真实 GPU 页错误，FAULT# 行里有地址）"; fi

echo; echo "═══ 5. 桌面进程 ═══"
# ⚠️ 不用 pgrep -f：它的模式串就在 adb 拉起的那个 sh 的命令行里，会把自己也数进去（每次 PID 都不同，浸泡对比必假阳性）
DESK="surfaceflinger system_server com.android.systemui com.android.launcher3"
A "getprop sys.boot_completed; for p in $DESK; do echo \"\$(pidof \$p) \$p\"; done" | sed 's/^/  /'
P0=$(A "pidof $DESK")

echo; echo "═══ 6. 帧读回（Stage 5 时这一步永久卡死）═══"
# exec-out 直接把 PNG 流回宿主机数字节 —— 不在设备上落文件。判据看字节数，不看管道尾巴的退出码。
N=$($ADB exec-out screencap -p 2>/dev/null | wc -c | tr -d ' ')
[ "${N:-0}" -gt 1000 ] && ok "screencap 读回 ${N} 字节" || bad "screencap 只读回 ${N:-0} 字节"

echo; echo "═══ 7. 崩溃残留 ═══"
A 'logcat -d -b crash 2>/dev/null | grep -E ">>> " | tail -3' | sed 's/^/  /'

if [ "$SECS" -gt 0 ]; then
  echo; echo "═══ 浸泡 ${SECS}s 后复查 ═══"
  sleep "$SECS"
  G2=$(gmu_count); R2=$(recover_count); F2=$(fault_count)
  # 用 -gt 判新增：浸泡期间旧行可能从 dmesg / logd 缓冲里滚掉，计数变小不是"新增"
  # （局限：滚掉 n 行的同时又新增 n 行会看不出来 —— 只在原本就 > 0 时才可能，那时本来就已经 FAIL 了）
  if [ "${G2:-0}" -gt "${G:-0}" ]; then bad "浸泡期间新增 GMU 错误：${G:-0} → $G2"
  else ok "GMU 错误 ${G:-0} → ${G2:-0}"; [ "${G2:-0}" -lt "${G:-0}" ] && info "计数变小 = 旧行从日志缓冲里滚掉了，不是好转"; fi
  if [ "${R2:-0}" -gt "${R:-0}" ]; then bad "浸泡期间新增 a6xx_recover：${R:-0} → $R2"
  else ok "a6xx_recover ${R:-0} → ${R2:-0}"; [ "${R2:-0}" -lt "${R:-0}" ] && info "计数变小 = 旧行从日志缓冲里滚掉了，不是好转"; fi
  if [ -z "$F2" ]; then bad "浸泡后找不到 smmustall 心跳"
  elif [ "$F2" = "${F0:-0}" ]; then ok "SMMU fault ${F0:-?} → $F2"
  else bad "浸泡期间新增 SMMU fault：${F0:-?} → $F2"; fi
  [ "$(A getprop sys.boot_completed)" = 1 ] && ok "boot_completed 仍为 1" || bad "boot_completed 不是 1（重启过？）"
  P2=$(A "pidof $DESK")
  [ "$P2" = "$P0" ] && ok "桌面四进程 PID 不变（${P2}）" || bad "桌面进程 PID 变了：[$P0] → [$P2]（有进程崩溃重启）"
fi

echo; echo "═══ 小结：通过 $PASS · 失败 $FAIL ═══"
[ "$FAIL" -eq 0 ]
