#!/usr/bin/env bash
# 在局域网里找回设备。
#
#   bash scripts/find-device.sh          # 找 Android（adb 5555）
#   bash scripts/find-device.sh --ssh    # 找救援系统（sshd 22）
#
# ⚠️★ 这个脚本被改过两次，两次都是因为【判据认错了对象】：
#
#   第一版：MAC 找不到时退化成"扫谁的端口开着就返回谁" —— 局域网里另有一台
#           开着 sshd 的机器，于是我照着【陌生人的机器】查了两轮
#           "公钥为什么被拒"。
#   第二版：只按 MAC 匹配 —— 而 Android 有【按 SSID 随机化 MAC】，
#           换个频段 MAC 就变了（实测 00:03:7f:12:4b:19 → …:de:1d），
#           于是又找不到了。
#
# ★ 现在的判据是【按协议确认身份】，不是看端口开不开、也不是看 MAC：
#     5555 → adb 连上去问 ro.crdroid.device 是不是 gaokun3
#     22   → ssh 上去问 hostname 是不是 gaokun3-rescue / gaokun3-live
#   端口和 MAC 只用来【缩小候选范围】，确认永远靠协议。
#
# ⚠️ 发布构建（ro.debuggable=0）默认【不监听 5555】—— 只有设备上持久化过
#    persist.adb.tcp.port（开发机迁移步骤）才扫得到。adb 已连上但未授权 / offline 的，
#    单独打到 stderr，和"是 xx，不是 gaokun3"区分开。
#
# ⚠️ 必须绕开沙箱跑（sandbox 代理会把所有 TCP 连接都答应下来，
#    于是端口扫描每个 IP 都"开着"，完全没有信息量）。
set -u
MODE=android; [ "${1:-}" = "--ssh" ] && MODE=rescue
KEY=${GAOKUN3_KEY:-$HOME/.ssh/ed25519}

PORT=5555; [ "$MODE" = rescue ] && PORT=22

# ⚠️ 2026-09-23 改：第三版只认 Windows（ipconfig / `ping -n -w`），在 macOS 上
#    第一步就"认不出本机网段"退出。现在 Windows(Git Bash) / macOS / Linux 都能跑。
# macOS 默认没有 `timeout`：依次用 timeout / gtimeout / perl alarm 兜底。
tmo() {
    local s=$1; shift
    if command -v timeout >/dev/null 2>&1; then timeout "$s" "$@"
    elif command -v gtimeout >/dev/null 2>&1; then gtimeout "$s" "$@"
    else perl -e 'alarm shift; exec @ARGV' "$s" "$@"; fi
}

# 本机【所有】私网 IPv4（本机自己的网段也会漂，还可能同时在两个网段上）。
# 198.18.x 是沙箱代理的 fake-IP 段，不是真网卡。
SELFS=$( { ipconfig 2>/dev/null; ifconfig 2>/dev/null; ip -4 -o addr 2>/dev/null; } \
    | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' \
    | awk -F. '($1==192 && $2==168) || $1==10 || ($1==172 && $2>=16 && $2<=31)' \
    | awk -F. '$4!=255 && $4!=0' | sort -u)
NETS=$(echo "$SELFS" | cut -d. -f1-3 | sort -u)
[ -n "$NETS" ] || { echo "!! 认不出本机网段" >&2; exit 1; }

# ★ 候选 = 整个 /24 上端口开着的主机，【不看 ARP 表、不按"上次在哪"收窄】——
#   ARP 只收录应答过 ping 的邻居，而 2026-09-12 正是"收窄"把一台好好的机器判成了挂死。
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for net in $NETS; do
    for i in $(seq 1 254); do
        ip="$net.$i"
        echo "$SELFS" | grep -qx "$ip" && continue
        ( tmo 2 bash -c "echo > /dev/tcp/$ip/$PORT" 2>/dev/null && : > "$TMP/$ip" ) &
    done
done
wait
CANDS=$(ls "$TMP")
[ -n "$CANDS" ] || { echo "!! 网段 $(echo $NETS) 上没有任何主机开着 $PORT —— 设备关机/休眠，或不在这些网段" >&2; exit 1; }

for ip in $CANDS; do
    if [ "$MODE" = android ]; then
        tmo 8 adb connect "$ip:5555" >/dev/null 2>&1
        # 先看 adb 对这台的状态（device / unauthorized / offline）：未授权时 getprop 必然失败，
        #   不区分的话它会和"不是 gaokun3"混在一起，而它其实可能正是我们的设备。
        st=$(tmo 5 adb devices 2>/dev/null | tr -d '\r' | awk -v t="$ip:5555" '$1==t {print $2; exit}')
        case "$st" in
            unauthorized)
                echo "  $ip 开着 5555 但 adb 未授权（新主机或发布构建？到设备屏幕上点允许，或改用无线调试配对）" >&2
                tmo 5 adb disconnect "$ip:5555" >/dev/null 2>&1; continue ;;
            offline)
                echo "  $ip 开着 5555 但 adb 状态是 offline（握手没完成：设备刚起、正在重启 adbd，或对方不是 adb）" >&2
                tmo 5 adb disconnect "$ip:5555" >/dev/null 2>&1; continue ;;
        esac
        d=$(tmo 8 adb -s "$ip:5555" shell getprop ro.crdroid.device 2>/dev/null | tr -d '\r\n')
        if [ "$d" = gaokun3 ]; then echo "$ip"; exit 0; fi
        # 小米手机（pudding）等也开着 5555：把认出来的东西打到 stderr，好知道扫到了谁
        [ -n "$d" ] && echo "  $ip 是 ${d}，不是 gaokun3" >&2
        tmo 5 adb disconnect "$ip:5555" >/dev/null 2>&1
    else
        h=$(tmo 10 ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
              -o UserKnownHostsFile=/dev/null -o ConnectTimeout=6 -i "$KEY" \
              "root@$ip" hostname 2>/dev/null | tr -d '\r\n')
        # ⚠️ 登录失败也是有用的信息：说明那台机器【不是】我们的救援系统，
        #    或者是我们的但公钥没生效 —— 两者要区分，所以把主机名打到 stderr。
        [ -n "$h" ] && echo "  $ip 的 hostname = $h" >&2
        # rescue profile 叫 gaokun3-rescue，live profile 叫 gaokun3-live
        case "$h" in gaokun3-rescue|gaokun3-live) echo "$ip"; exit 0 ;; esac
    fi
done
echo "!! 扫完 $(echo "$CANDS" | wc -w | tr -d " ") 个邻居，没有一台确认是我们的设备（${MODE}）" >&2
exit 1
