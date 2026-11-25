#!/vendor/bin/sh
# v1.0 NET-2：开机时给热点预建第二个 Wi-Fi 接口 wlan1（STA+AP 并发要用）。
#
# 为什么要预建（2026-10-05 构建机 crDroid 树核实）：
#   * 本机的 legacy Wi-Fi HAL 是 libwifi-hal-emu（BOARD_WLAN_DEVICE := emulator，
#     frameworks/opt/net/wifi/libwifi_hal/Android.bp:184），它的函数表里没有
#     wifi_virtual_interface_create（device/generic/goldfish/wifi/wifi_hal/wifi_hal.cpp:423-486），
#     于是 HAL 走"厂商 HAL 不建接口、要求接口已存在"的分支：
#     hardware/interfaces/wifi/aidl/default/wifi_legacy_hal.cpp handleVirtualInterfaceCreateOrDeleteStatus
#     —— NOT_SUPPORTED 时只看 if_nametoindex(ifname)，接口不在就建热点失败。
#   * 热点接口名：开了 STA+AP 组合后 AP 从 idx 1 起（wifi_chip.cpp startIdxOfApIface），
#     名字按 wifi.concurrent.interface → wifi.interface.1 → "wlan1"（wifi_chip.cpp:84-101）⇒ wlan1。
#   * goldfish HAL 在 wifi_initialize 时只枚举 /sys/class/net 下的 wlan0、wlan1
#     （device/generic/goldfish/wifi/wifi_hal/info.cpp:23-37）⇒ wlan1 要在 Wi-Fi 打开之前就在。
#     万一晚于 HAL 启动才建好，关一次再开 Wi-Fi 即可（HalState::init 每次开 Wi-Fi 都重新枚举）。
#
# 副作用：建出来的 wlan1 平时是 down 的 —— mac80211 只在接口 up 时才向固件建 vdev
#   （ieee80211_do_open → drv_add_interface），所以不开热点时对固件、功耗、挂起路径（#16）都没有影响。
#   地址由 mac80211 从 ath11k 给的地址表里挑一个没用过的（mac.c ath11k_mac_setup_mac_address_list，
#   第 2 个是置了本地管理位的变体）；热点真正用的 MAC 由框架随机化后经 HAL 设置。
# 类型建成 managed：hostapd 起来时自己把它切成 AP（nl80211 驱动的 set_mode）。

i=0
while [ ! -e /sys/class/net/wlan0 ]; do
    i=$((i + 1))
    if [ "$i" -ge 120 ]; then
        log -t gaokun3-wlan "60 秒内没等到 wlan0，不建 wlan1（热点将只能与 Wi-Fi 二选一）"
        exit 0
    fi
    sleep 0.5
done

# ★ v1.0 NET-4（2026-10-06 上机改）：wlan0 用由 SoC 序列号派生的稳定 MAC。
#   patches/android/hardware-interfaces/0013只让HAL的"出厂MAC"（ETHTOOL_GPERMADDR那一层）变成派生值；而1.0未开启随机化总开关
#   （rro/Gaokun3WifiOverlay 的 config_wifi_connected_mac_randomization_supported=false），框架连接前
#   根本不碰 MAC ⇒ wlan0 仍是固件每次开机给的 00:03:7f:12:xx:xx（dev.9 实机：框架日志
#   "Primary factory MAC address retrieved: 7e:a1:e8:15:66:f1"，ip link 却是 00:03:7f:12:6d:44）。
#   所以在这里、Wi-Fi 打开之前直接把 wlan0 的地址写成同一个派生值。实机核过：写进去之后关开 Wi-Fi 仍保持、照常连网。
#   wlan0 已经 up（框架抢先开了 Wi-Fi）就不动它 —— 连着网时改地址会掉线，这种开机只能沿用固件地址，下次开机再说。
# gaokun3_stable_mac <接口名> <序列号>：与patches/android/hardware-interfaces/0013（Wi-Fi HAL的gaokun3StableMac）同一算法 ——
#   对 "gaokun3-wifi-mac:<接口名>:<序列号>" 做 FNV-1a 64，取低 6 字节（小端），清组播位、置本地管理位。
#   mksh 的算术是 32 位，所以 64 位哈希拆成 4 个 16 位分量 h3:h2:h1:h0 来算；
#   乘 FNV 素数 0x100000001b3 = 2^40 + 0x1b3 ⇒ h*0x1b3 加上 h<<40（只剩 h 的低 24 位落进高 24 位）。
gaokun3_stable_mac() {
    _h0=$((0x2325)); _h1=$((0x8422)); _h2=$((0x9ce4)); _h3=$((0xcbf2))
    for _c in $(printf '%s' "gaokun3-wifi-mac:$1:$2" | od -An -tu1); do
        _h0=$(( _h0 ^ _c ))
        _a0=$(( _h0 * 0x1b3 )); _a1=$(( _h1 * 0x1b3 + (_a0 >> 16) )); _a2=$(( _h2 * 0x1b3 + (_a1 >> 16) )); _a3=$(( _h3 * 0x1b3 + (_a2 >> 16) ))
        _a0=$(( _a0 & 0xffff )); _a1=$(( _a1 & 0xffff )); _a2=$(( _a2 & 0xffff )); _a3=$(( _a3 & 0xffff ))
        _s2=$(( _a2 + ((_h0 << 8) & 0xffff) ))
        _s3=$(( _a3 + (((_h0 >> 8) | (_h1 << 8)) & 0xffff) + (_s2 >> 16) ))
        _h0=$_a0; _h1=$_a1; _h2=$(( _s2 & 0xffff )); _h3=$(( _s3 & 0xffff ))
    done
    printf '%02x:%02x:%02x:%02x:%02x:%02x\n' $(( ((_h0 & 0xff) & 0xfe) | 0x02 )) $(( _h0 >> 8 )) $(( _h1 & 0xff )) $(( _h1 >> 8 )) $(( _h2 & 0xff )) $(( _h2 >> 8 ))
}

SERIAL=$(cat /sys/devices/soc0/serial_number 2>/dev/null)
if [ -n "$SERIAL" ] && [ "$SERIAL" != 0 ]; then
    WANT=$(gaokun3_stable_mac wlan0 "$SERIAL")
    HAVE=$(cat /sys/class/net/wlan0/address 2>/dev/null)
    if [ "$HAVE" = "$WANT" ]; then
        :
    elif [ "$(( $(cat /sys/class/net/wlan0/flags 2>/dev/null || echo 0) & 1 ))" = 1 ]; then
        log -t gaokun3-wlan "wlan0 已经 up（$HAVE），这次不改成稳定 MAC $WANT"
    elif /vendor/bin/ifconfig wlan0 hw ether "$WANT"; then
        log -t gaokun3-wlan "wlan0 MAC $HAVE → $WANT（由 SoC 序列号派生，与 HAL 的出厂 MAC 相同）"
    else
        log -t gaokun3-wlan "wlan0 改 MAC 失败（$HAVE，想要 $WANT）"
    fi
else
    log -t gaokun3-wlan "读不到 soc0/serial_number，wlan0 沿用固件给的 MAC"
fi

if [ -e /sys/class/net/wlan1 ]; then
    exit 0
fi

if /vendor/bin/iw dev wlan0 interface add wlan1 type managed; then
    log -t gaokun3-wlan "已建 wlan1（给热点用）：$(cat /sys/class/net/wlan1/address 2>/dev/null)"
else
    log -t gaokun3-wlan "iw 建 wlan1 失败（热点将只能与 Wi-Fi 二选一）"
fi
exit 0
