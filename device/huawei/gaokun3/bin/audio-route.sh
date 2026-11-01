#!/vendor/bin/sh
# 开机把 LPASS 混音器路由到内置扬声器（Android 没有 ALSA UCM，得自己摆）。
#
# 序列来自实机验证过的 UCM2：华为 MateBook E 的 UCM 明确 include
# ThinkPad X13s 的配置（`conf.d/sc8280xp/sc8280xp.conf` 里
# `If.HUAWEI → /Qualcomm/sc8280xp/LENOVO-X13s.conf`），所以 X13s 那套
# 控件序列直接适用；audioreach 拓扑固件同理（都是 X13s 那份）。
#
# 关键映射（`Qualcomm/sc8280xp/HiFi.conf`）：
#   扬声器 = PCM **1**（WSA_CODEC_DMA_RX_0 ← MultiMedia2）
#   耳机   = PCM **0**（RX_CODEC_DMA_RX_0  ← MultiMedia1）
# 自测：`tinyplay /data/local/tmp/tone.wav -D 0 -d 1`（2026-08-19 实机出声）
#
# ⚠️ 功放会卡在 SoundWire Alert 状态刷 "Bus clash detected"。
#    ★ 2026-09-08 复查：**两颗都在 Alert**（sdw:1:...:1 与 :2），不是只有左边。
#    出声不受影响，但同一次复查发现**同一组混音器设置在几分钟内电平漂移最多
#    7.5 dB**（单次连测内只有 0.57 dB），即这条通路上有一个没人解释的增益变量。
#    Alert = 从设备拉了中断没被清，而 WSA883x 的中断源就是保护事件。两件事
#    可能同源，未证实。⚠️ 抬 PA 增益之前值得先把这个查清楚（docs #78 第 5 节）。

# ⚠️ 必须把 PATH 钉在 /system/bin —— 与 smmu-nostall.sh 同一个坑。
# 本脚本以 `#!/vendor/bin/sh` 起，PATH 默认优先 /vendor/bin，于是 log/sleep 每次都去
# exec /vendor/bin/toybox_vendor；而服务跑在 u:r:shell:s0 域，对 vendor_toolbox_exec
# 没有权限 —— permissive 下功能照常，但每次调用都吐 4 行 avc denied。
# 2026-08-19 M4 实机日志实录：
#   avc: denied { getattr / execute / read open / execute_no_trans }
#   for path="/vendor/bin/toybox_vendor" scontext=u:r:shell:s0
#   tcontext=u:object_r:vendor_toolbox_exec:s0
PATH=/system/bin:/system/xbin
export PATH

M=/system/bin/tinymix
[ -x $M ] || M=/vendor/bin/tinymix
[ -x $M ] || { log -t audioroute "找不到 tinymix，放弃"; exit 1; }

n=0
while [ ! -e /dev/snd/controlC0 ] && [ $n -lt 60 ]; do sleep 1; n=$((n+1)); done
[ -e /dev/snd/controlC0 ] || { log -t audioroute "等不到声卡（60s）"; exit 1; }

# ⚠️ BOOST 保持 **关**：一使能，每次流起停都有明显爆音
#    （2026-08-19 A/B 盲听定案）。
#    ★ 但"代价是最大声压低一些"这句是错的，已实测推翻：BOOST 开/关对电平的
#    影响是 +0.07 dB（档案录音）与 -0.28 dB（新测），即**零**。它是 SoundWire
#    端口使能（wsa883x_set_swr_port(WSA883X_PORT_BOOST)），不是功放升压器开关。
#    所以关掉它**不换来任何响度损失**，纯赚。详见 docs #78。
#
# ★★ 增益分配（2026-09-08 实测重排，docs #78）：
#    数字级走**单位增益 84**，响度由 **PA** 出（带 patches/0114 的内核上改为 78，见下）。
#    此前是反过来的（数字 90 = +6 dB、PA 12 = -3 dB），那个组合在 -6 dBFS
#    素材上实测 THD **-20 dB（约 10% 失真）** —— 数字级 +6 dB 把 -6 dBFS
#    的内容顶到 0 dBFS，是**削波**，不是压缩器吃掉了增益
#    （-20 dBFS 素材下同样两档给出满额 +9.07 dB 且 THD -52 dB，这是判据）。
#    改成 84 之后没有任何混音器设置能削爆数字通路。
#
# ★★ 2026-10-26：扬声器有两项设置要跟着内核走（patches/0113–0115）。
#    以 DT 里右功放有没有 qcom,port-mapping（0115 加的）为准；0113–0115 同属一组、
#    一起发，所以用这一个属性代表三者。
#    * 有：VISENSE 0、数字 78。
#      - VISENSE 关掉是因为 VI 数据目前没有任何地方用（没有扬声器保护），与上游 UCM
#        一致。开着也能用：右功放的 VI 输入在控制器 8 号端口，0113 之后才分得出来，
#        没有 0113 时流会起不来（"All ports busy"）。
#      - 0114 让播放时功放切到动态 DRE，增益跟着压扩器走，满幅比固定增益高约 6 dB；
#        数字 78（-6 dB）把最大音量拉回原来附近（内置麦实测仍高约 2 dB）。
#    * 没有（旧内核）：照旧 VISENSE 1、数字 84。没有映射时端口按顺序分配：
#      VISENSE 1 + BOOST 0 大音量时总线冲突，但两边都响；VISENSE 0 + BOOST 0 会让
#      右 DAC 落到 3 号端口、右喇叭不响。
#    ⚠️ 判断要能 stat 这个 sysfs 文件（gaokun3_scripts.te 里 audioroute 的 sysfs 规则）；
#       读不到时按"没有"处理，等于旧行为。
# ★ 这里设的是基准。扬声器低音处理（音频 HAL 里的 speakerfx）在它处理的那段播放期间自己把数字音量
#   抬到 84、结束时放回基准，并按抬高的量补偿 —— 见 speakerfx/README.md 第三节。
SPK_MAP=/sys/firmware/devicetree/base/soc@0/soundwire@3250000/wsa8830-right@0,2/qcom,port-mapping
if [ -e "$SPK_MAP" ]; then
    VISENSE=0; WSA_DIG=78
else
    VISENSE=1; WSA_DIG=84
fi
set -- \
    "WSA_CODEC_DMA_RX_0 Audio Mixer MultiMedia2" 1 \
    "WSA RX0 MUX" AIF1_PB \
    "WSA RX1 MUX" AIF1_PB \
    "WSA_RX0 INP0" RX0 \
    "WSA_RX1 INP0" RX1 \
    "WSA_COMP1 Switch" 1 \
    "WSA_COMP2 Switch" 1 \
    "SpkrLeft COMP Switch" 1 \
    "SpkrLeft BOOST Switch" 0 \
    "SpkrLeft VISENSE Switch" $VISENSE \
    "SpkrLeft DAC Switch" 1 \
    "SpkrRight COMP Switch" 1 \
    "SpkrRight BOOST Switch" 0 \
    "SpkrRight VISENSE Switch" $VISENSE \
    "SpkrRight DAC Switch" 1 \
    "SpkrLeft PA Volume" 17 \
    "SpkrRight PA Volume" 17 \
    "WSA_RX0 Digital Volume" $WSA_DIG \
    "WSA_RX1 Digital Volume" $WSA_DIG

apply() {
    while [ $# -ge 2 ]; do
        $M "$1" "$2" >/dev/null 2>&1 || log -t audioroute "设置失败: $1 -> $2"
        shift 2
    done
}
apply "$@"
# 把基准告诉 speakerfx：它补偿的是混音器高出这个值的部分，播放结束时也放回这个值。
# 混音器设好之后才写；没有这个属性时 speakerfx 不碰混音器。
setprop vendor.gaokun3.audioroute.spkdig "$WSA_DIG" ||
    log -t audioroute "写不进 vendor.gaokun3.audioroute.spkdig（低音处理不会借数字音量余量）"

# ★ PA 音量分两步写，好让**同一份 ROM 在新旧内核上都正确** ——
#   本项目内核与 ROM 是分开发布的，不能假设两边同步。
#   上面已写 17（= 0 dB，上游上限，任何内核都接受）。下面试着抬到 PA_TARGET：
#   打了 patches/0015（上限 17 -> 23）的内核会成功，旧内核直接拒绝、17 留着。
#   ⚠️ 顺序不能反 —— 先写 21 再写 17，新内核上会把音量又压回 0 dB。
#   ⚠️ 单边成功、另一边失败时要把两边一起退回 17，否则左右声道增益不一致。
#
#   PA_TARGET=21 是 +6 dB（刻度：17=0 dB，之后每步 1.50 dB，实测 15->16->17
#   为 +1.55/+1.43 dB）。上限给到 23（+9 dB）是留调音余量，不用重编内核就能
#   在 18..23 之间扫。⚠️ 往上调之前先听起停爆音、并看一眼
#   /sys/class/hwmon/*/temp1_input（实测连续正弦下功放到 41 °C）。
PA_TARGET=21
if $M "SpkrLeft PA Volume" $PA_TARGET >/dev/null 2>&1 &&
   $M "SpkrRight PA Volume" $PA_TARGET >/dev/null 2>&1; then
    PA_NOW=$PA_TARGET
else
    $M "SpkrLeft PA Volume" 17 >/dev/null 2>&1
    $M "SpkrRight PA Volume" 17 >/dev/null 2>&1
    PA_NOW=17
fi

log -t audioroute "扬声器路由已应用（PCM1 / WSA / 数字=$WSA_DIG / PA=$PA_NOW / BOOST=off 防爆音 / VISENSE=$VISENSE）"

# ─────────────────────────────────────────────────────────────────────────────
# 耳机（PCM 0 / RX_CODEC_DMA_RX_0 ← MultiMedia1）
#
# ★ 2026-08-20：耳机孔此前完全不出声，根因不是缺控件而是【插值器链没接】。
#   我原先只设了 RX_MACRO RX0/RX1 MUX 就以为通路成立，实际 DAPM 中间断了一节：
#     RX INT0_1 MIX1 INP0 = ZERO      （插值器混音器没选输入）
#     RX INT0 DEM MUX     = NORMAL_DSM_OUT （解调器没切到 class-H 输出）
#   于是后端 DAI 拿不到完整通路，PCM 0 直接打不开（且内核不报错，极难查）。
#   补齐后实测：tinyplay -D 0 -d 0 rc=0、pcm0p state=RUNNING、
#   hw_ptr 2 秒前进 97920 帧 = 48960 帧/秒（正好实时 48 kHz），dmesg 零报错。
#
# 序列逐条照抄上游 ALSA UCM2（上游 sc8280xp.conf 用 Regex "HUAWEI.*MateBook E.*"
# 匹配本机并 include LENOVO-X13s.conf，所以 X13s 那套就是本机的官配）：
#   codecs/wcd938x/HeadphoneEnableSeq.conf
#   codecs/qcom-lpass/rx-macro/HeadphoneEnableSeq.conf
#   codecs/qcom-lpass/rx-macro/init.conf   (RX_RXn Digital Volume 84)
#   Qualcomm/sc8280xp/LENOVO-X13s.conf     (HPHL/HPHR Volume 2)
#
# ⚠️ HPH 音量为什么是 2 而不是默认的 24 —— 这个方向不能猜，从内核算：
#   sound/soc/codecs/wcd938x.c:2620
#     SOC_SINGLE_TLV("HPHL Volume", WCD938X_HPH_L_EN, 0, 0x18, 1, line_gain)
#   sound/soc/codecs/wcd938x.c:192
#     DECLARE_TLV_DB_SCALE(line_gain, -3000, 150, 0)
#   → 控件值 v 对应 -30 + 1.5*v dB。默认 24 = 【+6 dB】，直接进耳朵；
#     上游的 2 = -27 dB。耳机灵敏度远高于喇叭，衰减是对的，而且框架自己
#     还有一层音量。嫌小就往上调，每 +1 = +1.5 dB（别接近 24）。
#
# 扬声器与耳机是两个不同后端（WSA vs RX），互不抢占，切换发生在 PCM 层面，
# 所以两套路由可以都在开机时摆好；DAPM 按需上电，闲置不耗电。
set --     "RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1" 1     "RX_RX0 Digital Volume" 84     "RX_RX1 Digital Volume" 84     "HPHL Volume" 2     "HPHR Volume" 2     "HPHL_RDAC Switch" 1     "HPHR_RDAC Switch" 1     "HPHL Switch" 1     "HPHR Switch" 1     "HPHR_COMP Switch" 0     "HPHL_COMP Switch" 0     "CLSH Switch" 1     "LO Switch" 1     "RX HPH Mode" CLS_H_ULP     "RX_HPH PWR Mode" LOHIFI     "RX_MACRO RX0 MUX" AIF1_PB     "RX_MACRO RX1 MUX" AIF1_PB     "RX INT0_1 MIX1 INP0" RX0     "RX INT1_1 MIX1 INP0" RX1     "RX INT0 DEM MUX" CLSH_DSM_OUT     "RX INT1 DEM MUX" CLSH_DSM_OUT     "RX_COMP1 Switch" 1     "RX_COMP2 Switch" 1
apply "$@"

log -t audioroute "耳机路由已应用（PCM0 / RX / HPH=-27dB / 含插值器链）"

# 耳机麦克风（PCM 2 / TX_CODEC_DMA_TX_3 → MultiMedia3）
#   codecs/wcd938x/HeadphoneMicEnableSeq.conf
#   codecs/qcom-lpass/tx-macro/HeadphoneMicEnableSeq.conf
# ADC2 Volume 10：analog_gain 是 MINMAX(0,3000) over 0->20，故 10 = +15 dB。
set --     "MultiMedia3 Mixer TX_CODEC_DMA_TX_3" 1     "ADC2_MIXER Switch" 1     "HDR12 MUX" NO_HDR12     "ADC2 MUX" INP2     "ADC2 Switch" 1     "TX1 MODE" ADC_NORMAL     "ADC2 Volume" 10     "TX DEC0 MUX" SWR_MIC     "TX SMIC MUX0" ADC1     "TX_AIF1_CAP Mixer DEC0" 1     "TX_DEC0 Volume" 110
apply "$@"

log -t audioroute "耳机麦路由已应用（PCM2 / TX）"

# ─────────────────────────────────────────────────────────────────────────────
# 内置麦克风（PCM 3 / VA_CODEC_DMA_TX_0 → MultiMedia4）
#
# ★ 2026-08-21：这条通路此前【完全是断的】，而且谁都没发现 —— 音频策略里
#   Built-In Mic 声明的是 CARD_0_DEV_3，但那个 PCM 压根打不开：
#     tinycap -D 0 -d 3  →  "cannot open device 3 for card 0"
#     tinypcminfo -d 3   →  连能力都查不到（"Device does not exist"）
#   原因和耳机口是同一类：**前端混音器没接**
#   （`MultiMedia4 Mixer VA_CODEC_DMA_TX_0` = Off），再加上 va-macro 的
#   DMIC 使能序列一条都没设。DAPM 路径不完整 → 后端拿不到通路 → open 失败。
#   我之前只设了耳机麦那条（MultiMedia3），漏了内置麦这条。
#
# 补齐后实测：`tinycap -D 0 -d 3 -c 2 -r 48000` 录到 384000 帧，
# pcm3c `state: RUNNING`，安静房间 **RMS 981 = -30.5 dBFS**（健康电平，
# 近满幅样本只有 4 个瞬态），确认是真实音频而不是静音或直流。
#
# ⚠️ 这两个采集 PCM 都**只支持双声道**（tinypcminfo: channels min=2 max=2）。
#    传 `-c 1` 会得到 "cannot set hw params: Invalid argument" ——
#    我一度因此以为耳机麦也是坏的，其实它一直是好的。
#
# 序列来自上游 UCM 的 SectionDevice."Mic"：
#   codecs/qcom-lpass/va-macro/DMIC0EnableSeq.conf 与 DMIC1EnableSeq.conf
#
# VA_DECn Volume 的刻度是 v-84 dB，所以 112 = +28 dB。数字增益不改变信噪比。
# ⚠️ 依赖 patches/0165：4.8 MHz 下的脉冲噪声在增益之前，会被一起放大。
set -- \
    "MultiMedia4 Mixer VA_CODEC_DMA_TX_0" 1 \
    "VA DEC0 MUX" VA_DMIC \
    "VA DMIC MUX0" DMIC0 \
    "VA_AIF1_CAP Mixer DEC0" 1 \
    "VA_DEC0 Volume" 112 \
    "VA DEC1 MUX" VA_DMIC \
    "VA DMIC MUX1" DMIC1 \
    "VA_AIF1_CAP Mixer DEC1" 1 \
    "VA_DEC1 Volume" 112
apply "$@"

log -t audioroute "内置麦路由已应用（PCM3 / VA / DMIC0+1）"

# ⚠️ 与上游 UCM 刻意不同的两处，记下来免得以后当成漏配：
#   * `SpkrLeft/Right BOOST Switch` 我们是 0，上游是 1 ——
#     一使能，每次流起停都有明显爆音（2026-08-19 A/B 盲听定案）。
#     ★ 它**不是**功放升压器开关（这个叫法本仓沿用了很久，是错的），而是
#     SoundWire 端口使能；对稳态电平的影响实测为 ±0.3 dB 以内，即零。
#   * `SpkrLeft/Right VISENSE Switch`：带 patches/0115 的内核上是 0（与上游一致），
#     旧内核上仍是 1；`WSA_RXn Digital Volume` 相应是 78 / 84（低音处理播放时由 speakerfx 临时抬到 84）。
#   另两处查过是【已经一致】的，不用设：`WSA MODE` 默认就是上游的 0；
#   ⚠️★ `WSA_RXn Digital Volume` 的上限是【内核机器驱动故意设的】——
#   sound/soc/qcom/sc8280xp.c 里 snd_soc_limit_volume()，上游写 81，注释是
#   "Set limit of -3 dB ... until we have active speaker protection in place"。
#   控件刻度是 v-84 dB，所以 81 = -3 dB、84 = 单位增益、124 = +40 dB。
#
#   ⚠️★★ **这里此前写着"必须显式设 90"，那是错的，2026-09-08 实测推翻。**
#   90 = +6 dB，而 -6 dBFS 的内容加 6 dB 正好是 0 dBFS —— 是削波。同一次
#   测量里 dig90/PA12 的 THD 是 **-20.08 dB**，dig84/PA17 是 **-39.81 dB**，
#   后者只低 2.12 dB 却干净 19.7 dB。旧注释把缺掉的增益归给"WSA883x 的
#   压缩器"，也是错的：-20 dBFS 素材下 81->90 给出满额 **+9.07 dB** 且
#   THD -52.8 dB，压缩器根本没参与，缺的那部分就是削波削掉的。
#   patches/0015 已改成把上限压在 **84（单位增益）**，让任何混音器设置都
#   削不爆数字通路；响度改由 PA 出（上限 17 -> 23）。详见 docs #78。
