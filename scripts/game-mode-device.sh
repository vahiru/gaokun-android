#!/system/bin/sh
# 游戏模式设备端（由 scripts/game-mode.sh push 到 /data/local/tmp/ 使用；
# 也可以在设备 root shell 里直接跑）：
#
#   sh /data/local/tmp/gk3-game-mode-device.sh identity|on <分钟>|off|status
#
# 动作与边界见 game-mode.sh 顶部。这里只记实现上踩过的点：
#   * resetprop 用 KSU 的 /data/adb/ksu/bin/resetprop（Magisk 语法）。
#     ro.* 删掉只影响内存属性区，重启自还原；persist.* 是真持久，靠状态文件还原。
#   * on 的最后一步是【延迟关 adb】—— 立即关会把正在执行的 shell 一起杀掉。
#     定时器、安全网必须先布好。开关 adb 只改 adb_enabled：AdbService 据此起停 adbd，
#     UsbDeviceManager（patches/0096 补上 android_usb 类以后才有）把 adb 加进或移出 USB 功能，
#     并维护 persist.sys.usb.config。sys.usb.config / persist.sys.usb.config 归框架管，
#     脚本不直接写、也不直接 stop / start adbd，否则框架记录的状态和 gadget 实际对不上（#41 评审）。
#     关 adb 会立刻断开本会话。
#   * ⚠️★★★ 定时器必须先【搬出 adbd 的 cgroup】：Mac 侧 `adb shell` 拿到的 root shell（KSU 自动给的）
#     在 adbd 的 cgroup（/system/uid_0/pid_<adbd>）里，后台起的子进程跟着在；`stop adbd` 时 init
#     把这个 cgroup 里的进程【全部 SIGKILL】—— setsid / trap HUP 都挡不住。2026-10-10、10-11 两次
#     定时器都是这样死的（10-10 当时误归因成 SIGHUP）。10-11 实机：普通 adb shell 起的 setsid 进程
#     在 ctl.restart adbd 后 DEAD，`su -c` 起的（KSU su 落在根 cgroup "/"）ALIVE。
#     ⇒ escape_cgroup：写自己的 pid 进 /sys/fs/cgroup/cgroup.procs。trap '' HUP / setsid 仍留着（PTY 销毁）。
#   * 恢复靠三层：定时器 → game-mode.sh off → 开机安全网
#     /data/adb/service.d/gk3-game-mode-safety.sh（active 标记还在就拉回 adb）。

RP=/data/adb/ksu/bin/resetprop
DIR=/data/local/tmp/gk3-game-mode
STATE=$DIR/state
ACTIVE=$DIR/active
TIMER_PID=$DIR/timer.pid
SAFETY=/data/adb/service.d/gk3-game-mode-safety.sh

# 只删 ro.crdroid.version —— Duck Detector 的判据表（SystemPropertiesCatalog /
# CustomRomCatalog）只查这一个键。⚠️ 别扩大到整个 ro.crdroid.* 家族：
# ro.crdroid.device 是 find-device.sh 认设备的身份判据（CLAUDE.md），砸了会连不上排查；
# ro.crdroid.build.version / display.version / ro.crdroidlegal.url 没有检测器查，留着。
CRDROID_PROPS="ro.crdroid.version"

# 会话里要藏起来的包（baseline 实测的 dangerous_apps / root_managers 命中）。
# 用 uninstall -k --user 0 而不是 disable-user：检测器走 getInstalledPackages，
# disable 后仍算"已安装"。-k 保留应用数据（不加 -k 单用户应用的 /data/data 会被删，unhide 只能装回空壳），
# 代价：带 MATCH_UNINSTALLED_PACKAGES 查询的检测器仍能看到它。卸前把 APK 备份到 $DIR/apk/，
# 任何一个备份失败就不卸（否则装不回来）。unhide 先 install-existing（-k 留下的就能直接恢复），不行再从备份装。
HIDE_PKGS="com.resukisu.resukisu bin.mt.termex"
# 残留目录：DangerousAppsCatalog 把 /sdcard/MT2 直接映射成 bin.mt.termex 命中，
# 空目录也算。mv 到一旁而不是删（用户数据，unhide 放回）。
HIDE_PATHS="/sdcard/MT2"
# 指纹族全列：2026-10-10 baseline 实测 ro.system/vendor.build.fingerprint 仍为
# :userdebug/，检测器（Bootloader/SystemProperties 的 "fingerprint tail" 漂移判据）
# 会拿它们当反射源，漏改就多两条 Type drift。ro.product.{system,vendor}.build.fingerprint
# 与 ro.bootimage.build.fingerprint 本机为空，列上无害。
# ⚠️ Build.FINGERPRINT（android.os.Build 常量）在 zygote 预加载时定死，运行时改不了；
#    运行时改属性反而会制造 property↔constant 漂移。ROM 级对齐在
#    scripts/crdroid-tree-fixes.py tree-fix [23]（config.mk 的 fingerprint 变体段 → user），
#    下次 ROM 构建后这里的 sed 就是无操作（尾部本来就是 :user/）。
FP_PROPS="ro.build.fingerprint ro.product.build.fingerprint ro.system.build.fingerprint ro.vendor.build.fingerprint ro.product.system.build.fingerprint ro.product.vendor.build.fingerprint ro.bootimage.build.fingerprint"

log() { echo "game-mode: $*"; }

# 把【调用者自己】搬到根 cgroup（见文件头）。⚠️ 子 shell 里 $$ 是父脚本的 pid，
# 所以用 read 内建读 /proc/self/stat（内建不 fork ⇒ self 就是当前这个 shell）。
escape_cgroup() {
  read -r me _ < /proc/self/stat
  echo "$me" > /sys/fs/cgroup/cgroup.procs 2>/dev/null
}

need_root() {
  [ "$(id -u)" = "0" ] || { echo "需要 root（adb shell 在本机经 KSU 即 root）"; exit 1; }
  [ -x "$RP" ] || { echo "找不到 $RP"; exit 1; }
}

save_orig() { # save_orig <prop>
  [ -f "$STATE/$1" ] && return 0
  getprop "$1" > "$STATE/$1"
}

save_setting() { # save_setting <namespace> <key>
  [ -f "$STATE/setting.$2" ] && return 0
  settings get "$1" "$2" > "$STATE/setting.$2" 2>/dev/null
}

do_identity() {
  mkdir -p "$STATE"
  for p in $FP_PROPS $CRDROID_PROPS; do save_orig "$p"; done
  # SEC-5：指纹尾部 :userdebug/ → :user/（与 ro.build.type=user 对齐）
  for p in $FP_PROPS; do
    cur=$(getprop "$p")
    [ -n "$cur" ] || continue
    new=$(echo "$cur" | sed 's|:userdebug/|:user/|')
    if [ "$cur" != "$new" ]; then
      "$RP" "$p" "$new"
      log "$p 尾部 userdebug→user"
    fi
  done
  # SystemProperties：ro.crdroid.version 任意非空 = danger
  for p in $CRDROID_PROPS; do
    if [ -n "$(getprop "$p")" ]; then
      "$RP" --delete "$p"
      log "删除 $p"
    fi
  done
}

# 藏包 / 恢复：只动 $HIDE_PKGS。hide 会真卸载（对 user 0），APK 备份到 $DIR/apk/。
do_hide() {
  mkdir -p "$STATE" "$DIR/apk"
  fail=0
  for pkg in $HIDE_PKGS; do
    pm path "$pkg" >/dev/null 2>&1 || { log "$pkg 未安装，跳过"; continue; }
    if [ ! -f "$STATE/hidden.$pkg" ]; then
      pm path "$pkg" | sed 's/^package://' > "$STATE/hidden.$pkg.new"
      # 每个 path 都备份（split APK 会有多个）；有一个失败就整包不卸
      bad=0
      while read -r apk; do
        [ -f "$apk" ] || { bad=1; continue; }
        base="$DIR/apk/${pkg}.$(basename "$apk")"
        cp "$apk" "$base" 2>/dev/null && cmp -s "$apk" "$base" || { log "备份失败 $apk"; bad=1; }
      done < "$STATE/hidden.$pkg.new"
      if [ "$bad" = 1 ]; then
        rm -f "$STATE/hidden.$pkg.new"
        log "✗ $pkg 备份不全，不卸"; fail=1; continue
      fi
      mv "$STATE/hidden.$pkg.new" "$STATE/hidden.$pkg"
    fi
    # ⚠️ 不再退回不带 --user 的 pm uninstall：那会连数据彻底删掉
    if pm uninstall -k --user 0 "$pkg" >/dev/null 2>&1; then
      log "已隐藏 $pkg（数据保留）"
    else
      log "✗ 卸载 $pkg 失败"; fail=1
    fi
  done
  for p in $HIDE_PATHS; do
    [ -e "$p" ] || continue
    mv "$p" "$p.gk3hidden" 2>/dev/null && log "残留目录 $p 已移开"
  done
  return $fail
}

do_unhide() {
  fail=0
  for pkg in $HIDE_PKGS; do
    [ -f "$STATE/hidden.$pkg" ] || continue
    # -k 卸的（数据与 APK 都还在）直接 install-existing 就回来
    if pm install-existing "$pkg" >/dev/null 2>&1 && pm path "$pkg" >/dev/null 2>&1; then
      log "已恢复 $pkg"; rm -f "$STATE/hidden.$pkg"; continue
    fi
    # 否则从备份装回：split APK 必须一次装齐（单装 base 会 INSTALL_FAILED_MISSING_SPLIT）
    set -- "$DIR/apk/${pkg}."*.apk
    if [ ! -f "$1" ]; then
      log "✗ 恢复失败 $pkg —— 没有备份"; fail=1; continue
    fi
    if [ $# -gt 1 ]; then
      pm install-multiple -r "$@" >/dev/null 2>&1
    else
      pm install -r "$1" >/dev/null 2>&1
    fi
    if pm path "$pkg" >/dev/null 2>&1; then
      log "已恢复（备份）$pkg"; rm -f "$STATE/hidden.$pkg"
    else
      # 记录留着：下次 unhide 还会再试，不会静默跳过
      log "✗ 恢复失败 $pkg —— 备份在 $DIR/apk/，手动 pm install-multiple"; fail=1
    fi
  done
  for p in $HIDE_PATHS; do
    [ -e "$p.gk3hidden" ] || continue
    mv "$p.gk3hidden" "$p" 2>/dev/null && log "残留目录 $p 已放回"
  done
  return $fail
}

# ⚠️ 每次都按本脚本的版本重写（写临时文件，内容不同才替换）。原来"已存在就返回"，
#    改过兜底逻辑后已装过旧版的设备永远拿不到新版（审查 2026-10-11）。
write_safety() {
  # KSU 不一定建了 service.d（10-17 用户的机器上就没有）；没有它兜底写不进去
  mkdir -p "${SAFETY%/*}"
  cat > "$SAFETY.new" <<'EOF'
#!/system/bin/sh
# gk3 游戏模式兜底：开机时若仍有残留标记，把 adb / 开发者选项恢复回来。
# 由 scripts/game-mode.sh 的 on 安装；off 不删它（无害，平时直接退出）。
#
# ⚠️ 2026-10-10 实测：service.d 阶段 SettingsProvider 可能还没起来，
#    `settings put` 会静默失败 ⇒ 开发者选项回不来。必须等 boot_completed
#    并用读回校验重试。
MARK=/data/local/tmp/gk3-game-mode/active
[ -f "$MARK" ] || exit 0
# 定时器活不过重启。它的 pid 文件留着的话，之后手动 off 会去 kill 一个不相干的进程（pid 已被复用）
rm -f /data/local/tmp/gk3-game-mode/timer.pid

# 等 boot_completed（最长 90 秒）
i=0
while [ $i -lt 90 ]; do
  [ "$(getprop sys.boot_completed)" = "1" ] && break
  sleep 1
  i=$((i+1))
done

# settings put 重试 + 读回校验（settings 在 provider 未就绪时返回 0 但不落盘）
restore_setting() {
  key=$1; want=$2
  j=0
  while [ $j -lt 30 ]; do
    settings put global "$key" "$want" >/dev/null 2>&1
    got=$(settings get global "$key" 2>/dev/null)
    [ "$got" = "$want" ] && return 0
    sleep 1
    j=$((j+1))
  done
  echo "gk3-safety: $key 还原失败（期望 $want，实读 $got）" > /data/local/tmp/gk3-game-mode-safety.log
  return 1
}

# TCP adb 按 on 存下的原值还原，要在 adb_enabled=1 之前：adbd 起来时才会监听
T=/data/local/tmp/gk3-game-mode/state/persist.adb.tcp.port
if [ -s "$T" ]; then
  setprop persist.adb.tcp.port "$(cat "$T")"
  setprop service.adb.tcp.port "$(cat "$T")"
fi

restore_setting development_settings_enabled 1
# adb_enabled=1：AdbService 起 adbd，UsbDeviceManager 把 adb 加回 USB 功能（不直接写 sys.usb.config）
restore_setting adb_enabled 1

# 校验通过才删标记，下次开机还能再兜一次
if [ "$(settings get global development_settings_enabled)" = "1" ] && \
   [ "$(settings get global adb_enabled)" = "1" ]; then
  rm -f "$MARK"
fi
EOF
  chmod 755 "$SAFETY.new"
  if cmp -s "$SAFETY.new" "$SAFETY"; then
    rm -f "$SAFETY.new"
  else
    mv "$SAFETY.new" "$SAFETY" && log "已安装 / 更新开机兜底 $SAFETY"
  fi
  # 和定时器一样：兜底没装上就不关 adb，免得把自己锁在外面
  [ -x "$SAFETY" ] || { log "✗ 开机兜底装不上（$SAFETY）—— 不关 adb"; rm -f "$SAFETY.new"; return 1; }
}

# 开机身份归一（独立于 game-mode 开关；resetprop 不持久，每开机都要重打）
IDENTITY_BOOT=/data/adb/service.d/gk3-game-identity.sh
install_identity_boot() {
  cat > "$IDENTITY_BOOT" <<'EOF'
#!/system/bin/sh
# gk3 身份归一（开机）：SEC-5 指纹尾部 userdebug→user；删 ro.crdroid.version。
# 由 scripts/game-mode.sh install 安装 / uninstall 卸载。只动这两个判据点，
# 别扩大范围（ro.crdroid.device 是 find-device.sh 的身份判据）。
RP=/data/adb/ksu/bin/resetprop
[ -x "$RP" ] || exit 0
for p in ro.build.fingerprint ro.product.build.fingerprint ro.system.build.fingerprint ro.vendor.build.fingerprint; do
  cur=$(getprop "$p")
  [ -n "$cur" ] || continue
  new=$(echo "$cur" | sed 's|:userdebug/|:user/|')
  [ "$cur" != "$new" ] && "$RP" "$p" "$new"
done
[ -n "$(getprop ro.crdroid.version)" ] && "$RP" --delete ro.crdroid.version
exit 0
EOF
  chmod 755 "$IDENTITY_BOOT"
  log "已安装开机身份归一 $IDENTITY_BOOT"
}
uninstall_identity_boot() {
  rm -f "$IDENTITY_BOOT"
  log "已卸载开机身份归一"
}

start_timer() { # start_timer <分钟>
  if [ -f "$TIMER_PID" ]; then
    old=$(cat "$TIMER_PID" 2>/dev/null)
    [ -n "$old" ] && kill "$old" 2>/dev/null
    rm -f "$TIMER_PID"
  fi
  min=$1
  # ⚠️★★ 定时器必须活得过 `stop adbd`：那一下会毁掉 PTY，同会话进程收 SIGHUP。
  #    2026-10-10 实测代价（首次 on/off 链路测试）：没隔离的定时器跟着 adb 一起死，
  #    adb 关了就再也回不来，只能按电源键重启走开机兜底。trap '' HUP 是最小隔离；
  #    有 setsid（toybox 自带）就再套一层新会话，双保险。
  if command -v setsid >/dev/null 2>&1; then
    setsid sh -c "trap '' HUP; echo \$\$ > /sys/fs/cgroup/cgroup.procs 2>/dev/null; sleep $((min * 60)); export GK3_TIMER_FIRED=1; exec sh \"$0\" off" >/dev/null 2>&1 &
  else
    ( trap '' HUP; escape_cgroup; sleep $((min * 60)); GK3_TIMER_FIRED=1 sh "$0" off ) >/dev/null 2>&1 &
  fi
  # ⚠️ toybox setsid 在后台作业里不 fork（不是进程组长 ⇒ 直接 setsid()+exec），所以 $! 就是那个
  #    sh -c 的 pid，而它到点后 exec 成 `sh $0 off`，pid 不变 —— do_off 必须认出"这是我自己"
  #    （GK3_TIMER_FIRED），否则会先把自己杀掉、什么都没还原（2026-10-11 实机核过 pid 关系）。
  echo $! > "$TIMER_PID"
  # 核对真的搬出来了：还在 adbd 的 cgroup 里就是白布 —— 宁可不关 adb 也不把自己锁在外面
  sleep 1
  cg=$(grep '^0::' "/proc/$!/cgroup" 2>/dev/null)
  case "$cg" in
    "0::/") log "定时器已布：$min 分钟后自动恢复（pid $!，已移出 adbd 的 cgroup）" ;;
    *) log "✗ 定时器没能移出 adbd 的 cgroup（${cg:-进程已不在}）—— stop adbd 会杀掉它，不关 adb"
       kill "$!" 2>/dev/null; rm -f "$TIMER_PID"; return 1 ;;
  esac
}

do_on() {
  need_root
  min=${1:-60}
  mkdir -p "$STATE"
  do_identity
  save_setting global development_settings_enabled
  save_setting global adb_enabled
  [ -f "$STATE/persist.adb.tcp.port" ] || getprop persist.adb.tcp.port > "$STATE/persist.adb.tcp.port"
  write_safety || exit 1
  start_timer "$min" || exit 1
  touch "$ACTIVE"
  # ⚠️ 关 adb 那一下，走 USB 的 adb 会话当场断（stdout 管道关闭 ⇒ 下一次 echo 收 SIGPIPE），
  #    排在它后面的步骤就不执行了 ⇒ 不断开会话的先做，会断开的全部放进脱离会话、输出丢弃的子 shell。
  #    定时器与安全网必须先就位（上面已布好）。
  # adb 只改 adb_enabled（见文件头）：AdbService 停 adbd，UsbDeviceManager 把 adb 移出 USB 功能，
  #    USB 回到用户选的用途（或只充电）。TCP 端口先清掉，adbd 被别处拉起来时也不会监听 5555。
  "$RP" --delete persist.adb.tcp.port
  settings put global development_settings_enabled 0
  log "关闭 adb……"
  ( trap '' HUP PIPE; escape_cgroup; sleep 1
    setprop service.adb.tcp.port ""
    settings put global adb_enabled 0 ) >/dev/null 2>&1 &
}

do_off() {
  need_root
  if [ -f "$TIMER_PID" ]; then
    old=$(cat "$TIMER_PID" 2>/dev/null)
    # 定时器触发的 off 不杀定时器（pid 可能就是自己，见 start_timer）；手动 off 才杀
    if [ -z "${GK3_TIMER_FIRED:-}" ] && [ -n "$old" ] && [ "$old" != "$$" ]; then
      # 只杀确实是定时器的进程（pid 可能已被复用）
      case "$(tr '\0' ' ' < "/proc/$old/cmdline" 2>/dev/null)" in
        *gk3-game-mode*)
          # 定时器的 sh 正在等前台的 sleep；sh（mksh）接管了 SIGTERM，要等 sleep 返回才处理，
          # 光 kill 它，它会一直等到时间到（10-17 实机：off 之后定时器还活着）。所以连 sleep 一起杀。
          # 先 sh 后 sleep：sleep 一结束 sh 就按挂起的 SIGTERM 退出，不会再跑后面的 off。
          kill "$old" 2>/dev/null
          pkill -P "$old" 2>/dev/null ;;
      esac
    fi
    rm -f "$TIMER_PID"
  fi
  for p in $FP_PROPS $CRDROID_PROPS; do
    if [ -f "$STATE/$p" ]; then
      v=$(cat "$STATE/$p")
      # 原来就没有 / 是空的 ⇒ 删掉，别还原成一个空属性
      if [ -n "$v" ]; then "$RP" "$p" "$v"; else "$RP" --delete "$p" >/dev/null 2>&1; fi
      rm -f "$STATE/$p"
    fi
  done
  # 旧版 on 存过 persist.sys.usb.config；现在由框架维护，不再还原
  rm -f "$STATE/persist.sys.usb.config"
  # TCP 端口按原值还原（一般是 5555 或空），要在 adb_enabled=1 之前：adbd 起来时才会监听。
  # 用 setprop 而不只是 resetprop：属性服务才写持久区、触发 init 的属性钩子。
  if [ -f "$STATE/persist.adb.tcp.port" ]; then
    v=$(cat "$STATE/persist.adb.tcp.port")
    [ -n "$v" ] && { "$RP" persist.adb.tcp.port "$v"; setprop persist.adb.tcp.port "$v"; }
    rm -f "$STATE/persist.adb.tcp.port"
  fi
  tcp=$(getprop persist.adb.tcp.port)
  [ -n "$tcp" ] && setprop service.adb.tcp.port "$tcp"
  for k in development_settings_enabled adb_enabled; do
    if [ -f "$STATE/setting.$k" ]; then
      v=$(cat "$STATE/setting.$k")
      [ "$v" = "null" ] && v=0
      settings put global "$k" "$v"
      rm -f "$STATE/setting.$k"
    fi
  done
  rm -f "$ACTIVE"
  # 最后确保 adb 打开：AdbService 起 adbd，UsbDeviceManager 把 adb 加回 USB 功能
  settings put global adb_enabled 1
  log "已恢复"
}

do_status() {
  echo "== game-mode status =="
  [ -f "$ACTIVE" ] && echo "active 标记：在（游戏模式生效中）" || echo "active 标记：无"
  [ -f "$TIMER_PID" ] && echo "定时器 pid：$(cat "$TIMER_PID")" || echo "定时器：无"
  for p in $FP_PROPS $CRDROID_PROPS ro.build.type ro.build.tags; do
    echo "  $p = $(getprop "$p")"
  done
  echo "  persist.sys.usb.config = $(getprop persist.sys.usb.config)"
  echo "  sys.usb.config / state = $(getprop sys.usb.config) / $(getprop sys.usb.state)"
  echo "  persist.adb.tcp.port   = $(getprop persist.adb.tcp.port)"
  echo "  adb_enabled            = $(settings get global adb_enabled)"
  echo "  development_settings   = $(settings get global development_settings_enabled)"
  [ -f "$SAFETY" ] && echo "开机兜底：已安装" || echo "开机兜底：未安装"
  for m in tricky_store playintegrityfix zygisk_lsposed; do
    [ -f "/data/adb/modules/$m/disable" ] && echo "  $m: DISABLED" || echo "  $m: enabled"
  done
}

# ── D 类：关掉证明伪造链（TrickyStore + PIF），让证明变成「没有」而不是「假的」──
# 这不是伪装——是拆掉伪装。Duck 的 verifiedBootHash 全零 / TEE generate-mode 指纹
# 都是 TrickyStore generate 模式的固有产物（B 类，不修伪造）。
# 关掉后 attestation 要么失败（Duck 报 Unavailable 而不是 Tampered），
# 要么走非安全 keymint 的自签链（诚实但没有 Google root）。
# ⚠️ 取舍：Play Integrity 会挂。游戏如果查 PI 就进不去。
# ⚠️ zygisk 模块 disable 要重启才完全生效；TrickyStore daemon 可以当场杀。
NOFORGE_MODULES="tricky_store playintegrityfix"

do_noforge() {
  need_root
  echo "── 关掉证明伪造链（noforge）──"
  for m in $NOFORGE_MODULES; do
    MOD=/data/adb/modules/$m
    [ -d "$MOD" ] || { echo "  $m: 未安装，跳过"; continue; }
    touch "$MOD/disable"
    echo "  $m: disable 标记已写"
  done
  # 杀 TrickyStore 进程 —— 不等重启，当场停掉 keystore 钩子。
  # ⚠️ 它是 app_process64 起的：cmdline = "TrickyStore"，但 comm = "main"（2026-10-11 实机 /proc/891）。
  #    toybox `pgrep -x` 比的是 comm ⇒ `pgrep -x TrickyStore` 和 `pgrep -x daemon` 都找不到；
  #    `pidof` 比 cmdline 的 argv[0]，找得到。旧版本 / 包装没 exec 的情况再按 daemon 名找，cwd 或 cmdline 带 tricky_store 才算。
  #    模块 service.sh 的循环是 `./daemon; [ $? -ne 0 ] && exit 1` ⇒ 被 kill（非 0 退出）后不会被拉起。
  PIDS=$(pidof TrickyStore 2>/dev/null)
  for pid in $(pgrep -x daemon 2>/dev/null); do
    where="$(readlink /proc/$pid/cwd 2>/dev/null) $(tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null)"
    case "$where" in *tricky_store*) PIDS="$PIDS $pid" ;; esac
  done
  if [ -n "$(echo $PIDS)" ]; then
    for pid in $PIDS; do kill "$pid" 2>/dev/null && echo "  TrickyStore (pid $pid) 已杀"; done
    sleep 2
    again=$(pidof TrickyStore 2>/dev/null)
    # 模块的 service.sh 可能带守护循环，会把它拉起来 —— 如实报，不假装生效
    [ -n "$again" ] && echo "  ⚠️ TrickyStore 又起来了（pid $again）—— 被模块重启了，要重启设备才真正停"
  else
    echo "  TrickyStore 未在跑"
  fi
  echo "⚠️ zygisk 模块（PIF）要重启才完全摘掉。"
  echo "⚠️ Play Integrity 会挂。恢复：$0 forge"
}

do_forge() {
  need_root
  echo "── 恢复证明链（forge）──"
  for m in $NOFORGE_MODULES; do
    MOD=/data/adb/modules/$m
    [ -d "$MOD" ] || continue
    rm -f "$MOD/disable"
    echo "  $m: disable 已移除"
  done
  echo "⚠️ 完全恢复要重启（zygisk / daemon 都在开机时装载）。"
}

case "${1:-}" in
  identity)  need_root; do_identity ;;
  on)        do_on "${2:-60}" ;;
  off)       do_off ;;
  status)    do_status ;;
  install)   need_root; install_identity_boot; do_identity ;;
  uninstall) need_root; uninstall_identity_boot ;;
  hide)      need_root; do_hide ;;
  unhide)    need_root; do_unhide ;;
  noforge)   do_noforge ;;
  forge)     do_forge ;;
  *)         echo "用法：$0 identity|on <分钟>|off|status|install|uninstall|hide|unhide|noforge|forge"; exit 1 ;;
esac
