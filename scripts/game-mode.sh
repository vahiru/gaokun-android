#!/usr/bin/env bash
# 游戏模式：运行时让设备对游戏 / 检测软件"看起来像正常机器"（可逆、不改镜像）。
#
#   bash scripts/game-mode.sh identity     # 只做身份清理（不动 adb，电脑照常连）
#   bash scripts/game-mode.sh install      # 装"开机身份归一"（每开机自动重打）+ 立即生效
#   bash scripts/game-mode.sh uninstall    # 卸掉开机归一（当前已改的值重启即还原）
#   bash scripts/game-mode.sh on [分钟]    # 身份清理 + 关 adb / 开发者选项；
#                                          #   默认 60 分钟后自动恢复（先跑 identity 可续）
#   bash scripts/game-mode.sh off          # 立刻恢复一切（需要 adb 活着）
#   bash scripts/game-mode.sh hide         # 隐藏 root 管理器 / 破解工具包（卸载 + APK 备份）
#   bash scripts/game-mode.sh unhide       # 把 hide 卸掉的包装回来
#   bash scripts/game-mode.sh noforge      # 关掉 TrickyStore + PIF（证明变「没有」而非「假的」）
#   bash scripts/game-mode.sh forge        # 恢复 TrickyStore + PIF
#   bash scripts/game-mode.sh status       # 看当前状态
#   bash scripts/game-mode.sh -s <serial> …  # 多设备时指定
#
# 做什么：
#   1. SEC-5 一致性：全部 ro.*.build.fingerprint 尾部 :userdebug/ → :user/，
#      与 ro.build.type=user 对齐（SystemPropertiesDetector 对指纹含 "userdebug"
#      报 warning，对指纹尾部与 Build.TYPE 漂移再报一条；Bootloader 也查同一漂移）。
#      ⚠️ Build.FINGERPRINT 常量运行时改不掉（zygote 预加载）；ROM 级对齐见
#      crdroid-tree-fixes.py tree-fix [23]（下次 ROM 构建生效）。
#   2. 删 ro.crdroid.version（SystemPropertiesDetector 对任意非空值直接 danger）。
#      ro.crdroid.device 别删——find-device.sh 的身份判据。
#   3. on 额外：development_settings_enabled=0（warning 项）、adb_enabled=0、
#      TCP 端口清空 ⇒ AdbRuntime 转绿。adb 只经 adb_enabled 开关：AdbService 起停 adbd，
#      UsbDeviceManager 增删 USB 功能里的 adb；sys.usb.config / persist.sys.usb.config 归框架管，不直接写。
#   4. hide：卸载 com.resukisu.resukisu / bin.mt.termex（dangerous_apps 与
#      root_managers 的命中，uninstall -k --user 0 保留数据 + APK 备份，备份不全不卸；unhide 装回）。
#
# ⚠️ 边界（不在本脚本里做）：verifiedbootstate / flash.locked / vbmeta /
#    attestation 一律不动——伪造证明链在边界外，那块归后续 TrickyStore。
#
# ⚠️ on 之后 adb 会断，off 就跑不了了！恢复有两条兜底：
#      a) 定时器（默认 60 分钟）自动还原——已做 trap '' HUP / setsid 脱离会话，
#         否则 `stop adbd` 会连它一起杀掉（2026-10-10 实测踩过）；
#      b) 开机兜底 /data/adb/service.d/gk3-game-mode-safety.sh——只要
#         /data/local/tmp/gk3-game-mode/active 还在，下次开机把 adb 拉回来。
#    最坏情况：按电源键重启一次，adb 就回来了。⇒ 分钟数要盖住整局游戏。
#
# ⚠️ resetprop 改的是内存里的属性区，重启自还原；persist 两个 usb/adb 值
#    是真持久属性，靠状态文件还原——所以别在 on 之后直接删
#    /data/local/tmp/gk3-game-mode/ 目录。
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
DEV_SCRIPT="$HERE/game-mode-device.sh"
REMOTE_DIR=/data/local/tmp/gk3-game-mode
REMOTE_SCRIPT=/data/local/tmp/gk3-game-mode-device.sh

# 打印第 2 行起连续的注释块（不写死行号：加用法行后原来的 '2,38p' 会截断）
usage() { awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 1; }

# ---- 设备选择：-s 指定；否则用第一台 device 状态的（USB/TCP 同机时都行）----
SERIAL=""
[ "${1:-}" = "-s" ] && { SERIAL="$2"; shift 2; }
if [ -z "$SERIAL" ]; then
  SERIAL=$(adb devices | awk '$2=="device"{print $1; exit}')
  [ -n "$SERIAL" ] || { echo "没有已连接的 adb 设备"; exit 1; }
fi
A() { adb -s "$SERIAL" "$@"; }
# 在设备上跑一个子命令；adb 不通或设备端失败（adb shell 回传远端退出码）就如实报错退出，
# 不往下打印"已恢复 / 已装回"之类的成功提示。
R() {
  A shell "sh $REMOTE_SCRIPT $*"
  rc=$?
  [ "$rc" = 0 ] || { echo "✗ 设备端 '$*' 失败（退出码 ${rc}）—— 上面是设备的输出" >&2; exit "$rc"; }
}

CMD=${1:-}; [ -n "$CMD" ] || usage
MIN=${2:-60}

case "$CMD" in
  identity|on|off|status|install|uninstall|hide|unhide|noforge|forge) ;;
  *) usage ;;
esac

# 推设备端脚本（幂等；每次覆盖，保证改过的逻辑生效）
A push "$DEV_SCRIPT" "$REMOTE_SCRIPT" >/dev/null || { echo "✗ 推设备端脚本失败（adb 不通？）" >&2; exit 1; }
A shell "mkdir -p $REMOTE_DIR" 2>/dev/null || { echo "✗ adb shell 不通" >&2; exit 1; }

case "$CMD" in
  status)
    R status
    ;;
  identity)
    R identity
    ;;
  install)
    R install
    echo "开机身份归一已装好；当前值已立即生效。"
    ;;
  uninstall)
    R uninstall
    echo "开机归一已卸；如需立刻还原当前值，跑 game-mode.sh off（或重启）。"
    ;;
  on)
    # ⚠️ ${} 花括号：macOS 自带 bash 3.2 会把紧跟在 $VAR 后面的多字节字符吞进变量名
    #    （$SERIAL： 的全角冒号 → "SERIAL­: unbound variable"，2026-10-10 实测）。
    echo "设备 ${SERIAL}：开游戏模式（${MIN} 分钟后自动恢复）……"
    # 定时器与安全网先布好，最后才断 adb（断开瞬间命令通道就没了）
    # ⚠️ 不走 R：设备端最后一步会断开 adb，会话被切断时退出码不可信。
    A shell "sh $REMOTE_SCRIPT on $MIN"
    echo "adb 即将关闭。$MIN 分钟后自动恢复；中途想手动恢复：等定时器到期后跑"
    echo "  bash scripts/game-mode.sh off"
    echo "或直接重启（开机兜底会拉回 adb）。"
    ;;
  off)
    R off
    echo "已恢复。"
    ;;
  hide)
    R hide
    echo "危险包已隐藏（APK 备份在设备 $REMOTE_DIR/apk/）。恢复：game-mode.sh unhide"
    ;;
  unhide)
    R unhide
    echo "已装回。"
    ;;
  noforge)
    R noforge
    echo "证明链已关（TrickyStore + PIF disable）。Play Integrity 会挂。"
    echo "恢复：game-mode.sh forge（完全生效需重启）"
    ;;
  forge)
    R forge
    echo "disable 标记已移除。完全恢复需重启。"
    ;;
esac
