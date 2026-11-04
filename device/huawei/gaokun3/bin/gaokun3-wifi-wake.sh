#!/vendor/bin/sh
# 待机时让推送消息叫醒系统：给 Wi-Fi 固件设收包唤醒规则，只匹配推送连接（Google Play 服务的 FCM 长连接）的服务器发来的 TCP 包。
#
# 做法照 ChromeOS：Chrome 的 GCM 客户端报告自己那条长连接的服务器地址，shill 让 Wi-Fi 收到从这个地址来的包时
# 叫醒系统（Chrome 的 GCMConnectionObserver，shill 的 wake_on_wifi.cc）。这里的推送客户端是 Google Play 服务，
# 改不了它，所以从 /proc/net/tcp、/proc/net/tcp6 里找远端端口是 5228–5230 的已建立连接
# （mtalk.google.com，FCM 的长连接）。每个服务器地址一条规则，最多 MAXPAT 条（固件最多收 22 条）。
# 这里分不出连接属于哪个 App（vendor 域读不了 packages.list，拿不到 Play 服务的 uid），
# 别的 App 连到这几个端口也会被当成推送服务器，还可能先占满 MAXPAT 条名额；下面的限流兜底。
# 另外两个条件是 wifi/wpa_supplicant.conf 本来就要的断线和 GTK 更新失败（wowlan_triggers）。
# Play 服务没连着，或者改走了 443 端口，就找不到地址，只设这两个条件，和没有这个服务时一样。
# 走 VPN，或者 IPv4 连接经 464XLAT 转成 IPv6 时，/proc 里的地址和 Wi-Fi 上实际的包对不上，规则不会匹配。
#
# 规则跟着连接走：醒着时每 POLL 秒看一次（挂起时 sleep 不走，所以它自己不会叫醒系统），
# 地址变了就重设；wpa_supplicant 重设触发条件、把规则冲掉时，也再设一次。
# 所以 Play 服务换了服务器后，要等系统醒着满 POLL 秒，新服务器的包才能叫醒系统。
#
# 和 shill 一样防唤醒风暴：一分钟内固件叫醒系统超过 3 次，或十分钟内超过 10 次，就撤掉规则 BACKOFF 秒。
# 次数取自内核 ath11k_wow 唤醒源的 event_count（patches/0202），这个唤醒源还让系统每次被叫醒后醒 1 秒。
# 内核里没有这个唤醒源，就说明 WAKE# 那几个补丁（patches/0199–0202）也不在，什么也不做。
# 原理和排查见 docs/deep-sleep.md 的 Wi-Fi 一节。
#
# 只用 mksh 和 toybox 的命令，不依赖 awk。

POLL=10             # 秒，只算醒着的时间
BACKOFF=600         # 秒
MAXPAT=8
BASE="disconnect gtk-rekey-failure"
IW=/vendor/bin/iw

say() {
    log -t gaokun3-wifiwake "$*"
}

# ath11k 在 probe 时把 ath11k_wow 注册在 Wi-Fi 的 PCI 设备下面（patches/0202），比 wlan0 出现得早。
# 只在 ath11k_pci 绑定的设备目录里找：/sys/class/wakeup 下的别的唤醒源，有的在本域读不了的目录里（比如 EC 的 power_supply）。
wakeup_source() {
    for _d in /sys/bus/pci/drivers/ath11k_pci/*/wakeup/wakeup*; do
        if [ "$(cat "$_d/name" 2>/dev/null)" = ath11k_wow ]; then
            echo "$_d"
            return
        fi
    done
}

# iw wowlan show 的输出里有几条规则。不用 here-document：mksh 要为它在临时目录建文件，本服务写不了。
npatterns() {
    _k=0
    _s=$1
    while :; do
        case $_s in
        *"wake up on packet"*)
            _k=$((_k + 1))
            _s=${_s#*wake up on packet}
            ;;
        *)
            break
            ;;
        esac
    done
    echo $_k
}

# /proc/net/tcp* 里地址的一个 32 位字是主机序（小端）的 8 个十六进制数字，转成网络序的 4 个字节 "AA:BB:CC:DD"。
word() {
    echo "${1:6:2}:${1:4:2}:${1:2:2}:${1:0:2}"
}

# 每个推送服务器一行，"v4 AA:BB:CC:DD" 或 "v6 <16 个字节>"，不重复，最多 MAXPAT 行。
# /proc/net/tcp* 的列：sl local_address rem_address st …；端口是大端十六进制，5228–5230 = 146C–146E；
# st 01 = ESTABLISHED。双栈套接字连 IPv4 地址时出现在 tcp6 里，写成 ::ffff:a.b.c.d，包还是 IPv4 的。
servers() {
    cat /proc/net/tcp /proc/net/tcp6 2>/dev/null | {
        _seen="|"
        _n=0
        while read -r _sl _local _rem _st _rest; do
            [ "$_st" = 01 ] || continue
            case ${_rem#*:} in
            146C|146D|146E) ;;
            *) continue ;;
            esac
            _a=${_rem%:*}
            case ${#_a} in
            8)
                _s="v4 $(word "$_a")"
                ;;
            32)
                if [ "${_a:0:24}" = 0000000000000000FFFF0000 ]; then
                    _s="v4 $(word "${_a:24:8}")"
                else
                    _s="v6 $(word "${_a:0:8}"):$(word "${_a:8:8}"):$(word "${_a:16:8}"):$(word "${_a:24:8}")"
                fi
                ;;
            *)
                continue
                ;;
            esac
            case $_seen in
            *"|$_s|"*) continue ;;
            esac
            _seen="$_seen$_s|"
            echo "$_s"
            _n=$((_n + 1))
            [ $_n -lt $MAXPAT ] || break
        done
    }
}

# 帧里的偏移按以太网算：EtherType 在 12。IPv4：协议在 23，源地址在 26。
# IPv6：下一个头在 20，源地址在 22；带扩展头的包匹配不上，这些服务器发来的 TCP 包没有扩展头。
pattern() {
    case $1 in
    v4) echo "12+08:00:-:-:-:-:-:-:-:-:-:06:-:-:$2" ;;
    v6) echo "12+86:dd:-:-:-:-:-:-:06:-:$2" ;;
    esac
}

# 最近 $1 秒（boottime）里固件叫醒系统的次数。样本只在醒着时取，窗口开始之前的那个样本可能隔着一次长睡眠；
# 它和窗口里第一个样本之间的唤醒，按时间平均摊开来算。
wakeups() {
    _start=$((now - $1))
    _ot=
    _oc=
    _it=$now
    _ic=$count
    for _x in $samples; do
        _t=${_x%:*}
        _c=${_x#*:}
        if [ "$_t" -le "$_start" ]; then
            _ot=$_t
            _oc=$_c
        else
            _it=$_t
            _ic=$_c
            break
        fi
    done
    _d=$((count - _ic))
    if [ -n "$_ot" ] && [ "$_it" -gt "$_ot" ]; then
        _d=$((_d + (_ic - _oc) * (_it - _start) / (_it - _ot)))
    fi
    echo $_d
}

# 留下十分钟以前最新的那个样本，和它之后的全部。
prune() {
    _keep=
    for _x in $samples; do
        if [ "${_x%:*}" -le $((now - 600)) ]; then
            _keep=$_x
        else
            _keep="$_keep $_x"
        fi
    done
    samples=$_keep
}

# ath11k 在 post-fs-data 就绑定了（init.gaokun3.rc），开机完成时唤醒源一般早就在了；给它 60 秒。
i=0
until WS=$(wakeup_source); [ -n "$WS" ] || [ $i -ge 12 ]; do
    i=$((i + 1))
    sleep 5
done
if [ -z "$WS" ]; then
    say "ath11k_pci 的设备下没有 ath11k_wow 唤醒源（内核没有 WAKE# 那几个补丁，或者 Wi-Fi 驱动没有绑定），不设收包唤醒规则"
    exit 0
fi
say "开始，唤醒源 $WS"

samples=
applied=
lasterr=
backoff=0
while :; do
    read -r now _rest < /proc/uptime
    now=${now%.*}
    # 驱动重新绑定后唤醒源换了编号、计数从 0 开始，旧样本就作废了。
    if [ "$(cat "$WS/name" 2>/dev/null)" != ath11k_wow ]; then
        WS=$(wakeup_source)
        samples=
        if [ -z "$WS" ]; then
            sleep $POLL
            continue
        fi
        say "唤醒源重新注册了：$WS"
    fi
    if ! count=$(cat "$WS/event_count" 2>/dev/null); then
        sleep $POLL
        continue
    fi
    if [ -n "$samples" ] && [ "$count" -lt "${samples##*:}" ]; then
        samples=
    fi
    samples="$samples $now:$count"
    prune
    m1=$(wakeups 60)
    m10=$(wakeups 600)
    if [ "$now" -ge "$backoff" ] && { [ "$m1" -gt 3 ] || [ "$m10" -gt 10 ]; }; then
        backoff=$((now + BACKOFF))
        say "最近一分钟固件叫醒系统 $m1 次、十分钟 $m10 次，撤掉规则 $BACKOFF 秒"
    fi

    phy=$(cat /sys/class/net/wlan0/phy80211/name 2>/dev/null)
    if [ -n "$phy" ]; then
        list=$(servers)
        set --
        if [ "$now" -ge "$backoff" ] && [ -n "$list" ]; then
            set -- $(echo "$list" | while read -r _v _a; do pattern "$_v" "$_a"; done)
        fi
        want="$BASE${1:+ patterns $*}"
        # 不拿设置之后读回来的结果作比较：wpa_supplicant 恰好在那一刻改了触发条件的话，就再也发现不了。
        # 比规则条数；wpa_supplicant 设的是 BASE，没有规则。
        have=$("$IW" phy "$phy" wowlan show 2>/dev/null)
        if [ "$want" != "$applied" ] || [ "$(npatterns "$have")" != $# ]; then
            [ "$want" = "$applied" ] && [ -z "$lasterr" ] && say "触发条件被别人改过，重新设置"
            if err=$("$IW" phy "$phy" wowlan enable $want 2>&1); then
                applied=$want
                lasterr=
                if [ "$now" -lt "$backoff" ]; then
                    say "撤着规则，到 boottime $backoff 秒为止"
                else
                    say "$# 条规则：$(echo $list)"
                fi
            elif [ "$err" != "$lasterr" ]; then
                lasterr=$err
                say "设置失败：$err"
            fi
        else
            lasterr=
        fi
    fi
    sleep $POLL
done
