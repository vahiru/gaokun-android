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
# ⚠️ 左功放（sdw:1:...:1）会卡在 Alert 状态刷 "Bus clash detected"，
#    右功放正常；出声不受影响，但这是个待查项（docs/stage5-audio.md）。

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

# ⚠️ BOOST 保持 **关**：功放升压器一使能，每次流起停都会有明显爆音
#    （2026-08-19 A/B 盲听实测：BOOST 关 = 无爆音，BOOST 开 = 明显爆音）。
#    代价是最大声压低一些，对平板的小喇叭是划算的取舍。
# ⚠️ PA Volume 用 UCM BootSequence 的原厂值 **12**（范围 0->17）。
#    我一度设 17，结果起停削波很难听。
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
    "SpkrLeft VISENSE Switch" 1 \
    "SpkrLeft DAC Switch" 1 \
    "SpkrRight COMP Switch" 1 \
    "SpkrRight BOOST Switch" 0 \
    "SpkrRight VISENSE Switch" 1 \
    "SpkrRight DAC Switch" 1 \
    "SpkrLeft PA Volume" 12 \
    "SpkrRight PA Volume" 12 \
    "WSA_RX0 Digital Volume" 90 \
    "WSA_RX1 Digital Volume" 90

apply() {
    while [ $# -ge 2 ]; do
        $M "$1" "$2" >/dev/null 2>&1 || log -t audioroute "设置失败: $1 -> $2"
        shift 2
    done
}
apply "$@"

log -t audioroute "扬声器路由已应用（PCM1 / WSA / PA=12 / BOOST=off 防爆音）"

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
set -- \
    "MultiMedia4 Mixer VA_CODEC_DMA_TX_0" 1 \
    "VA DEC0 MUX" VA_DMIC \
    "VA DMIC MUX0" DMIC0 \
    "VA_AIF1_CAP Mixer DEC0" 1 \
    "VA_DEC0 Volume" 100 \
    "VA DEC1 MUX" VA_DMIC \
    "VA DMIC MUX1" DMIC1 \
    "VA_AIF1_CAP Mixer DEC1" 1 \
    "VA_DEC1 Volume" 100
apply "$@"

log -t audioroute "内置麦路由已应用（PCM3 / VA / DMIC0+1）"

# ⚠️ 与上游 UCM 刻意不同的两处，记下来免得以后当成漏配：
#   * `SpkrLeft/Right BOOST Switch` 我们是 0，上游是 1 ——
#     功放升压器一使能，每次流起停都有明显爆音（2026-08-19 A/B 盲听定案）。
#   * `SpkrLeft/Right VISENSE Switch` 我们是 1，上游是 0 ——
#     现状实测出声正常、dmesg 无抱怨，故未动；但这是个未验证的偏离，
#     若将来查扬声器功耗或保护逻辑，先看这里。
#   另两处查过是【已经一致】的，不用设：`WSA MODE` 默认就是上游的 0；
#   ⚠️★ `WSA_RXn Digital Volume` 那条旧注释已作废（原文："本机范围是 0->81
#   且已在 81（最大），上游写的 84 在本机是超范围值"）。真相是：81 这个上限
#   是【内核机器驱动故意设的】——sound/soc/qcom/sc8280xp.c 里
#   snd_soc_limit_volume(card, "WSA_RX0 Digital Volume", 81)，注释写着
#   "Set limit of -3 dB ... until we have active speaker protection in place"。
#   控件刻度是 v-84 dB，所以 81 = -3 dB、124 = +40 dB，被锁掉的是 43 dB。
#   本仓 patches/0015 把上限抬到 90（+6 dB），所以这里【必须显式设 90】：
#   驱动默认是 84（0 dB），不设就白抬了。
#   实测（内置麦克风、440 Hz Goertzel）：81 → -28.0 dBFS，90 → -22.3 dBFS，
#   +5.7 dB（理论 +9，差额被 WSA883x 的压缩器吃掉）。详见 docs #67。
