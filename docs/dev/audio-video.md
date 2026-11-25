# 音频子系统与硬件视频编解码（Audioreach + `qcom-iris`）

本文档说明`gaokun3`的LPASS/SoundWire音频拓扑、扬声器/耳机/麦克风通路配置、低延迟播放修复、蓝牙耳机通话以及基于`qcom-iris`与`v4l2_codec2`的硬件视频编解码架构。

## 一、音频拓扑与内核驱动要求

### 1. 声卡与PCM节点映射

`gaokun3`采用高通Audioreach架构（`q6apm-lpass-dais` + SoundWire），ALSA声卡0为`SC8280XP-HUAWEI-GAOKUN3`，加载拓扑文件`/vendor/firmware/qcom/sc8280xp/SC8280XP-HUAWEI-GAOKUN3-tplg.bin`：

| ALSA设备 | 前端DAI | 后端DAI | 物理编解码器与外设 | 用途 |
|---|---|---|---|---|
| `hw:0,0` | `MultiMedia1` | `RX_CODEC_DMA_RX_0` | WCD9380/9385（SoundWire） | 3.5mm有线耳机输出 |
| `hw:0,1` | `MultiMedia2` | `WSA_CODEC_DMA_RX_0` | 双路WSA8830功放（SoundWire） | 机身四扬声器输出 |
| `hw:0,2` | `MultiMedia3` | `TX_CODEC_DMA_TX_3` | WCD9380/9385 ADC2 | 有线耳麦麦克风输入 |
| `hw:0,3` | `MultiMedia4` | `VA_CODEC_DMA_TX_0` | LPASS VA Macro DMIC0/1 | 机身内置双数字麦克风输入 |

### 2. 必须内建（`=y`）的内核配置

由于Android无外置模块加载流程，且Audioreach/SoundWire组件间存在严格的probe时序依赖，以下驱动在`scripts/kernel-config-android.sh`中必须设为`=y`：

- `CONFIG_QRTR_SMD=y`：使AP能够通过RPMSG与ADSP上的Q6服务建立QRTR通信。
- `CONFIG_PINCTRL_LPASS_LPI=y`、`CONFIG_PINCTRL_SC8280XP_LPASS_LPI=y`、`CONFIG_SC_LPASSCC_8280XP=y`：LPASS LPI引脚与时钟控制器；若缺失则`swr0`/`swr1`/`swr2`与`rxmacro`/`txmacro`/`vamacro`/`wsamacro`全部因`EPROBE_DEFER`挂起。
- `CONFIG_SND_SOC_WSA883X=y`、`CONFIG_SND_SOC_WCD938X=y`、`CONFIG_SND_SOC_WCD_MBHC=y`：扬声器功放与耳放编解码器驱动。

## 二、扬声器、有线耳机与混音器配置（`audio-route.sh`）

开机时由`init.gaokun3.rc`触发`/vendor/bin/audio-route.sh`配置ALSA混音器路由。

### 1. 扬声器通路与防破音/防爆音（`patches/kernel/audio/0001`）

- **路由控件**：
  ```
  WSA_CODEC_DMA_RX_0 Audio Mixer MultiMedia2 = 1
  WSA RX0 Mux = AIF1_PB, WSA RX1 Mux = AIF1_PB
  WSA_RX0 INP0 = RX0, WSA_RX1 INP0 = RX1
  SpkrLeft COMP Switch = 1, SpkrRight COMP Switch = 1
  SpkrLeft DAC Switch = 1, SpkrRight DAC Switch = 1
  ```
- **数字增益与模拟功放增益分离**：
  - `WSA_RX0 Digital Volume`与`WSA_RX1 Digital Volume`必须保持在**`84`（0 dB单位增益）**。若将其抬高至90（+6 dB），满幅信号会在数字域直接硬削顶，产生约−20 dB的三次谐波失真（刺耳破音）。
  - 响度提升通过WSA8830的模拟功放增益`SpkrLeft/Right PA Volume`实现。根据内核`wsa883x.c`的TLV定义（`0..14`固定为−3 dB，`15..29`每档+1.5 dB），上游机器驱动`sc8280xp.c`原本调用`snd_soc_limit_volume(card, "SpkrLeft PA Volume", 17)`将上限锁在17（0 dB）。`patches/kernel/audio/0001`将该上限放宽至23（+9 dB），`audio-route.sh`在运行期将其设为**`21`（+6 dB）**，在不触发数字削顶的前提下获得充足外放音量。
- **消除启停流爆音（Pop）**：
  - `SpkrLeft BOOST Switch`与`SpkrRight BOOST Switch`控制WSA8830的Boost SoundWire数据端口，与模拟PA增益无关。启用该开关会在每次音频流启动/停止时引发明显的瞬态电流冲击声（Pop），而稳态音量完全不变。因此`audio-route.sh`显式将其保持为`0`（关闭）。

### 2. 3.5mm有线耳机与插拔检测（`patches/android/hardware-interfaces/0001`）

- **WCD938x耳放完整插值器链路**：
  仅打开`HPHL_RDAC Switch`与`RX_MACRO RX0/1 MUX`不足以让WCD938x输出模拟信号，必须同时打通RX Macro的插值器混音链与Class-H控制器：
  ```
  RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1 = 1
  RX_MACRO RX0 MUX = AIF1_PB, RX_MACRO RX1 MUX = AIF1_PB
  RX INT0_1 MIX1 INP0 = RX0, RX INT1_1 MIX1 INP0 = RX1
  RX INT0 DEM MUX = CLSH_DSM_OUT, RX INT1 DEM MUX = CLSH_DSM_OUT
  RX_COMP1 Switch = 0, RX_COMP2 Switch = 0
  HPHL_COMP Switch = 0, HPHR_COMP Switch = 0
  HPHL_RDAC Switch = 1, HPHR_RDAC Switch = 1
  HPHL Switch = 1, HPHR Switch = 1
  CLSH Switch = 1, LO Switch = 1
  RX HPH Mode = CLS_AB_LOHIFI, RX_HPH PWR Mode = LOHIFI
  RX_RX0 Digital Volume = 84, RX_RX1 Digital Volume = 84
  HPHL Volume = 8, HPHR Volume = 8
  ```
- **压扩器两侧都关**：RX Macro的压扩器（`RX_COMPn`）与WCD938x的HPH压扩器（`HPHx_COMP`）是一对，前者按信号电平给耳放发增益码，同时在数字域补回耳放降掉的增益。只开数字侧时，`wcd938x`把耳放的增益来源固定在寄存器，数字补偿就成了净增益：低于约−22 dBFS的信号被抬高约20 dB，超过后又掉回，听感是很重的压缩。上游alsa-ucm-conf已从耳机序列中去掉`RX_COMPn`（PR #670），这里两侧都显式关闭。
- **AB类耳放模式**：`RX HPH Mode`与上游sc8280xp的`HeadphoneABEnableSeq.conf`一致，用`CLS_AB_LOHIFI`。H类模式（`CLS_H_ULP`、`CLS_H_LOHIFI`）的电源轨随信号包络升降，耳放增益抬高后在接近满幅时削波；AB类的电源轨固定，HPH 12时到−1 dBFS未见削波，代价是耳机播放时功耗略高（尚未实测）。
- **耳放增益**：`HPHL/R Volume`的刻度为−30 + 1.5·v dB（`wcd938x.c`的`line_gain`），8即−18 dB。Linux上UCM把它当用户音量，Android的音量在框架里做，这里只设一个固定值。同一档下`CLS_AB_LOHIFI`比`CLS_H_ULP`高约5.6 dB，8的响度与`CLS_H_ULP`下试听合适的12相当，满幅比测过不削波的点低约5 dB。只用一副耳机试听过。
- **调试注意**：`RX_COMPn Switch`只能在耳机流关闭时改。流开着时由1改为0，`lpass-rx-macro`在流关闭时会跳过关压扩器的步骤，压扩器在硬件里一直开着，直到改回1并再开关一次流，或重启。`HPHx_COMP Switch`由0改为1时，`wcd938x`不会把耳放的增益来源切回压扩器，同样要重启才能生效。
- **耳机插拔检测与HAL动态端口填充**：
  1. **Framework输入事件检测**：主线内核`wcd938x` MBHC通过`/dev/input/event*`上报`SW_HEADPHONE_INSERT`与`SW_MICROPHONE_INSERT`，不创建旧版`/sys/class/switch/h2w`。设备树overlay必须设置`config_useDevInputEventForAudioJack = true`，`WiredAccessoryManager`才能收到插拔事件。
  2. **策略XML与`ModulePrimary`修复（`patches/android/hardware-interfaces/0001`）**：
     - 在`audio_policy_configuration.xml`中，可热插拔设备端口（`Wired Headphones`、`Wired Headset`、`Wired Headset Mic`）不可写死`address="CARD_0_DEV_0"`，因为`WiredAccessoryManager`构造连接通知时固定传入`address=""`，写死非空地址会导致策略引擎匹配不到模板端口而丢弃事件。省略`address`后，AOSP默认ALSA地址解析自动回退至`(card=0, device=0)`。
     - AOSP `libaudiohal@aidl`的`ModulePrimary`继承自基类空实现`Module::populateConnectedDevicePort`（不会像USB/蓝牙子类那样探测硬件填充采样率与通道掩码），而`setDeviceConnectionState`要求连接后的端口必须带有非空`profiles`。`patches/android/hardware-interfaces/0001`在`ModulePrimary`中重写`populateConnectedDevicePort`，当目标端口`profiles`为空时直接复用模板端口声明的`profile`列表。

## 三、内置麦克风录音修复（`patches/android/hardware-interfaces/0002`、`0003`、`0004`）

### 1. 混音器路由

内置双数字麦克风通过LPASS VA Macro采集至`MultiMedia4`（`hw:0,3`）：

```
MultiMedia4 Mixer VA_CODEC_DMA_TX_0 = 1
VA DEC0 MUX = MSM_DMIC, VA DEC1 MUX = MSM_DMIC
VA DMIC MUX0 = DMIC0, VA DMIC MUX1 = DMIC1
TX_DEC0 Volume = 102, TX_DEC1 Volume = 102
```

### 2. 三处用户态HAL修复

1. **ALSA设备地址解析（`patches/android/hardware-interfaces/0002`）**：
   `XsdcConversion.cpp`在处理`AUDIO_DEVICE_IN_BUILTIN_MIC`时无条件将XML中的`address`覆写为`"bottom"`，抹掉了`audio_policy_configuration.xml`中配置的`"bottom CARD_0_DEV_3"`，导致HAL回退去打开并不存在采集流的`hw:0,0`。`patches/android/hardware-interfaces/0002`保留带有`CARD_x_DEV_y`后缀的自定义地址。
2. **输入Profile约束与移除假MMAP输入**：
   - `hw:0,3`（`MultiMedia4`）的Audioreach拓扑仅支持`48000 Hz` + 双声道（`AUDIO_CHANNEL_IN_STEREO`）。若在XML中声明`16000`/`44100`或`AUDIO_CHANNEL_IN_MONO`，HAL会直接以不支持的参数调用`pcm_open`并失败。将其收敛为`48000` + `AUDIO_CHANNEL_IN_STEREO`后，`AudioFlinger`会自动为请求16 kHz单声道的应用插入重采样与声道转换。
   - 移除XML中的`mmap_no_irq_in`端口（AOSP `ModulePrimary`对该标志实例化的是只产生随机白噪声的`StreamInMmapStub`测试桩）。
3. **消除周期性丢块与启动静音（`patches/android/hardware-interfaces/0003`、`0004`）**：
   - AOSP `StreamAlsa`的采集线程每10 ms从ALSA读取480帧写入容量为3840帧（8块）的`MonoPipe`，但其内部的`OffsetProvider`设置`setpoint = capacity / 2`，将写入速率钳制在平均每11.25 ms一块，导致每8块必然溢出丢弃1块。`patches/android/hardware-interfaces/0003`将采集`MonoPipe`容量翻倍。
   - AOSP `StreamPrimary::transfer`原本对采集流也按墙钟休眠（`sleep`）定拍，而非按ALSA硬件就绪数据驱动，造成开头约171 ms全零静音与124 ms交付延迟。`patches/android/hardware-interfaces/0004`将采集方向的`transfer`改为直接在`mAlsaDeviceProxy->read()`上阻塞等待硬件数据就绪，启动静音降至0 ms、单次交付耗时降至约1 ms。

## 四、低延迟音频播放与音游偏移修复（`patches/android/tinyalsa/0001`、`patches/android/hardware-interfaces/0006`、`0011`、`patches/kernel/audio/0013`）

1. **`tinyalsa`周期重算修复（`patches/android/tinyalsa/0001`）**：
   AOSP `libaudiohal@aidl`请求打开`hw:0,1`时传入`period_size=1024, period_count=4`，而内核`q6apm-dai`要求period是固定步长的整数倍（当时为480帧），在`SNDRV_PCM_IOCTL_HW_REFINE`中将`period_size`向上取整为`1440`（`buffer_size=5760`）。然而`external/tinyalsa_new/src/pcm.c`的`pcm_set_config()`仍用修改前的`1024`计算`sw_params.stop_threshold = 4096`。当环形缓冲区剩余帧数达到`5760 - 4096 = 1664`帧时，ALSA立即误判XRUN并中断流，引发持续爆音与断流。`patches/android/tinyalsa/0001`修正`pcm_set_config()`以内核返回的实际`period_size`计算`buffer_size`与`stop_threshold`。
2. **移除假MMAP输出端口**：
   与输入侧同理，从`audio_policy_configuration.xml`中移除`mmap_no_irq_out`（对应的`StreamOutMmapStub`会把走AAudio MMAP路径的游戏音频静默丢弃）。
3. **播放方向取消墙钟跳帧（`patches/android/hardware-interfaces/0006`）**：
   AOSP示例实现`StreamPrimary::transfer`在播放方向同样维护一条合成墙钟：一旦上层混音线程唤醒稍晚，HAL就打印`skipping transfer`并将整块音频直接丢弃，导致音乐游戏出现严重的节奏漂移与吞音。`patches/android/hardware-interfaces/0006`在播放方向当ALSA proxy就绪时直接调用阻塞式ALSA写入，由声卡硬件时钟自然反压上层混音线程。
4. **播放缓冲只用3个period（`patches/android/hardware-interfaces/0011`）**：
   alsa_utils给内置设备配ALSA缓冲时，period数统一取`ro.hardware.audio.tinyalsa.period_count`（默认4），播放和录音共用。`patches/android/hardware-interfaces/0011`在`openProxyForAttachedDevice()`里只对输出设备改用`ro.vendor.audio.primary.out_period_count`（`device.mk`设3）：块大小不变，只是切成3个period，内核`q6apm-dai`再按播放period步长向上取整。播放块为5 ms（`ro.vendor.audio.primary.out_latency_ms=5`）时得到3 × 240帧（15 ms）。录音方向保持4个，因为采集钩子在I/O线程里处理85 ms的块（通话降噪最长约51 ms才回退），这段时间新来的数据要靠录音缓冲接住。播放不能再减到2个：DSP缺数据时会扣着最后一个period不放，ALSA不会报XRUN，FastMixer与轨道的欠载计数也都是0，断音只能在游戏里听出来。
5. **播放period步长240帧（`patches/kernel/audio/0013`）**：
   主线`q6apm-dai`把播放和录音的period与缓冲都限制为480帧（10 ms）的整数倍，注释称是为了适应DSP的限制。这样HAL即使改用5 ms的块，ALSA缓冲仍至少是3 × 480帧。实测SC8280XP的ADSP播放5 ms的period没有问题：扬声器（`MultiMedia2`，WSA codec DMA）和有线耳机（`MultiMedia1`，RX codec DMA）用3 × 240帧在游戏里都没有断音，扬声器上声学回环测得的时间戳误差与10 ms时相同。DSP真正需要的是排队的period多于两个，所以`0011`的3个period仍然必要。录音保持480帧。
   代价：HAL写入时DSP手上剩两个period，即10 ms，原来是20 ms，FastMixer或HAL写线程偶尔停顿超过10 ms就可能断音，而且没有计数器能看到。primary是唯一的输出，息屏放音乐等所有播放都按5 ms的周期运行，FastMixer和HAL的唤醒次数翻倍，对续航的影响尚未经实测。
   `0013`必须与`out_latency_ms=5`一起使用：内核还是480帧步长时，period会被取整到480帧，缓冲并不缩短；内核是240帧步长、播放块却是10 ms时，ALSA是3 × 240帧而HAL一次写入两个period，DSP手上可能只剩一个period，有断音风险，尚未经实测。若发布后出现断音，可先把`out_period_count`改为4（4 × 240帧，已测无断音），不必改内核。

## 五、蓝牙耳机通话（HFP软件数据通路，`patches/android/hardware-interfaces/0012`）

### 1. 数据通路

手机上的SCO音频（蓝牙通话的双向语音）通常由蓝牙芯片经PCM/I2S接口交给音频DSP处理，即硬件offload。本机声卡只有第一节表中的codec DMA后端，没有接蓝牙芯片的接口，因此采用AOSP的HFP软件数据通路：

- WCN6855经UART（内核`hci_qca`，蓝牙HAL以`HCI_CHANNEL_USER`独占`hci0`）收发SCO包。协议栈以增强型SCO建链，并指定HCI数据通路（日志中`btm_send_connect_request`的`path=0x0`）。
- CVSD（8 kHz，窄带）由蓝牙芯片编解码，HCI上传输的是PCM；mSBC（16 kHz，宽带）与LC3（32 kHz，超宽带）由协议栈编解码（`system/stack/btm/btm_sco_hci.cc`）。
- 协议栈经蓝牙音频HAL（`android.hardware.bluetooth.audio-impl`，运行在音频HAL的APEX进程中）的`HFP_SOFTWARE_ENCODING_DATAPATH`与`HFP_SOFTWARE_DECODING_DATAPATH`两个会话，与音频HAL的`ModuleBluetooth`交换PCM，每块7.5 ms。
- 耳机成为HFP活动设备时，协议栈启动两个会话，音频框架随之把耳机的SCO设备接入蓝牙模块。音频策略在耳机的SCO设备接入时就打开蓝牙模块的`hfp output`。SCO由音频框架按路由开关：应用进入通话模式并把通信设备设为蓝牙耳机后，通话的播放被路由到`hfp output`，开始录音时打开`hfp input`；流开始传输时HAL请求启动音频流，协议栈收到请求后与耳机协商编解码器并建立SCO。两个方向的流都进入待机后，协议栈才断开SCO。

### 2. 配置

以下三处要一起生效，缺一处都无法通话：

1. **`device.mk`的两个属性**：`bluetooth.sco.managed_by_audio=true`让音频框架按路由开关SCO，框架在`AudioDeviceBroker`构造时读入，蓝牙协议栈经`AudioManager.isScoManagedByAudio()`获知，只在这个模式下建立HFP会话（`bta_ag_init_hfp_client_interface()`）；`bluetooth.hfp.software_datapath.enabled=true`只由协议栈读取，让HFP会话使用软件数据通路，否则协议栈按硬件offload建链。修改后要重启。前者还依赖ROM中的两个aconfig开关`android.media.audio.sco_managed_by_audio`与`android.media.audio.amsco_available_api`（A17默认开启）。
2. **`audio/bluetooth_audio_policy_configuration.xml`**：frameworks/av的`bluetooth_with_le_audio_policy_configuration_7_0.xml`加上HFP的混音口、设备口与路由。混音口按`ModuleBluetooth`的约定声明（`audio/aidl/default/bluetooth/DevicePortProxy.cpp`）：采样率固定为32 kHz，HAL再重采样到协商出的8、16或32 kHz；输出为立体声，因为16位单声道的会话在`loadAudioConfig()`中按立体声报给框架，写入时由HAL转成单声道，写成单声道时打开输出会因声道数不符而失败，SCO输出设备无法注册；输入为单声道。
3. **`patches/android/hardware-interfaces/0012`**：示例HAL的`StreamBluetooth::bluetoothParametersUpdated()`按`BT_SCO`参数（`IBluetooth`的`ScoConfig.isEnabled`，默认为false）开关HFP输出流，而SCO由音频托管时框架从不下发`BT_SCO=on`，初始化时还会下发`BT_SCO=off`。不打这个补丁时，SCO建立后耳机状态机一下发耳机名称与WBS状态，输出流就被挂起，此后的数据都被丢弃：路由、SCO链路与会话看上去都正常，耳机里却没有声音。补丁让HFP输出流不再受这个参数控制。

### 3. 调试

- `dumpsys audio | grep mScoManagedByAudio`应为`true`，`aflags list | grep amsco_available_api`应为`enabled`（前者只反映框架一侧）；`dumpsys media.audio_policy`中蓝牙模块应列出`hfp output`、`hfp input`与四个`BT SCO`设备口。
- 耳机连接后，日志中有`StartSession: encode`与`StartSession: decode`，`AHAL_BluetoothAudioPort`为`HFP_SOFTWARE_ENCODING_DATAPATH`与`HFP_SOFTWARE_DECODING_DATAPATH`各注册一个端口；`AHAL_StreamBluetooth: checkConfigParams: Channel count mismatch`说明策略文件中的声道数与HAL的约定不符。
- 通话开始时依次可见`StartRequest: ConnectAudio status = BT_SUCCESS`、`bta_ag_sco_codec_nego: Succeeded`、`btm_send_connect_request`的`path=0x0`、`[BTA_AG_SCO_OPEN_ST(0x06)]`，以及两个HAL端口的`STARTING -> STARTED`；之后`bluetoothParametersUpdated: mEnabled`应为1。

### 4. 已知限制

- 只支持CVSD的窄带耳机尚未经实测；LC3超宽带需要耳机支持，也尚未经实测。
- 通话中由耳机断开SCO（例如在耳机上把声音切回平板）尚未经实测。HAL端口不会因此离开启动状态；耳机主动断开时，框架（A17默认开启`android.media.audio.bt_audio_disconnect_api`）取消蓝牙路由，改走默认的通信设备（平板的扬声器与麦克风）。链路超时等其他原因断开时，框架收不到通知。
- HFP端口进入`DISABLED`后不会自动恢复：蓝牙音频会话在SCO设备仍连接时重启（`session_changed_cb`），或HAL无法向会话发出启动请求、挂起失败后，通话没有声音，直到耳机重新连接。示例HAL原本在收到`BT_SCO=on`时恢复端口，SCO由音频托管时这个参数不会到达。
- 同时连接ASHA助听器与蓝牙耳机时，耳机状态机下发的耳机名称、WBS等参数会让助听器的输出流停止传输，直到重新打开。这是示例HAL原有的问题（`bluetoothParametersUpdated()`不认助听器会话，A2DP与LE Audio的挂起参数也会触发），蓝牙模块声明SCO端口后，耳机状态机下发的参数也会到达它。

## 六、硬件视频编解码（`qcom-iris` + `v4l2_codec2`）

### 1. 从`qcom-venus`迁移至`qcom-iris`

在Linux 6.18+/7.x主线内核中，高通为Gen2 HFI硬件（含SC8280XP / SM8450 / SM8550）引入了全新的`qcom-iris`驱动（`drivers/media/platform/qcom/iris/`），替代旧版`qcom-venus`。

- **内核配置与固件**：启用`CONFIG_VIDEO_QCOM_IRIS=y`（禁用`CONFIG_VIDEO_QCOM_VENUS`），设备树`video-codec@aa00000`指定固件`qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn`。
- **Android Codec2集成**：
  - 使用`android.hardware.media.c2@1.2-service-v4l2`（通过属性`media.c2.hal.selection=aidl`告知`MediaCodec`连接AIDL/HIDL服务）。
  - 注册解码器组件：`c2.v4l2.avc.decoder`（H.264）、`c2.v4l2.hevc.decoder`（HEVC / H.265）、`c2.v4l2.vp9.decoder`（VP9）。实测单路实时解码：3840x2160的HEVC（含10位）与VP9到120 fps，H.264在3840x2160下到60 fps、在2560x1440下到120 fps；`media_codecs_c2.xml`按此申报`blocks-per-second`与性能点（H.264 4K@60与2560x1440@120，HEVC与VP9 4K@120）。Media3等应用按性能点判断帧率，原来只申报4K@30时，60 fps以上的视频会交给软解。一个性能点只覆盖帧尺寸、帧率与宏块速率都不超过它的格式，所以H.264的4K@60无法覆盖1080p@120，另申报2560x1440@120。
  - 注册编码器组件：`c2.v4l2.avc.encoder`（H.264）、`c2.v4l2.hevc.encoder`（HEVC Main与Main 10），见本节第3小节。

### 2. 关键内核与用户态修复

| 补丁 | 位置 | 解决的问题 |
|---|---|---|
| `patches/android/v4l2-codec2/0003` | `external/v4l2_codec2` | `V4L2Device::getDeviceInfosForType()`原本写死只扫描`/dev/video0..9`；启用相机`camss`后VFE占用了`video0..31`，解码器被推到`video32`，导致硬解静默消失。将扫描上界扩大至64 |
| `patches/android/v4l2-codec2/0005` | `external/v4l2_codec2` | HEVC码流的首个配置缓冲（VPS/SPS/PPS）合并进第一帧DMA-BUF（不小于输入缓冲区），避免固件因缺少`CODECCONFIG`标志在`SOURCE_CHANGE`前等待输出缓冲而死锁 |
| `patches/kernel/iris/0001` | `drivers/media/platform/qcom/iris/` | 引入`core->hw_powered`标志，防止解码器下电后残余中断触发未供电寄存器读取导致内核崩溃 |
| `patches/kernel/iris/0003` | `drivers/media/platform/qcom/iris/` | 客户端订阅了`V4L2_EVENT_SOURCE_CHANGE`、事件还没到时，让`VIDIOC_G_FMT(CAPTURE)`返回`-EINVAL`，与venus相同（参数`venus_compat_gfmt`，默认`Y`）。`v4l2_codec2`在启动时据此不建初始输出队列（`patches/android/v4l2-codec2/0002`，只记一行INFO）。设为`N`时，`v4l2_codec2`会在码流头之前按128x128开启CAPTURE队列，gen1固件解H.264与HEVC时随即报致命错误（`sys error`，类型1），所以必须保持`Y` |
| `patches/kernel/iris/0005` | `drivers/media/platform/qcom/iris/` | 在非动态分辨率切换（非DRC）的`CAPTURE streamoff`路径发送`HFI_FLUSH_ALL`而非`HFI_FLUSH_OUTPUT`，修复视频拖动进度条（seek）或关闭播放器时固件等待`HFI_FLUSH_OUTPUT`响应超时导致的卡死 |
| `patches/kernel/dts/0026` | `arch/arm64/boot/dts/qcom/sc8280xp.dtsi` | iris加载固件时把`0x0..0x25800000`作为视频内容保护范围交给TZ，只有安全会话可以使用。`iris`主域（SID `0x2a00`）的IOVA从高往低分配，在`v4l2-codec2/0011`之前，每路1080p解码约占260–310 MiB，1440p约占735 MiB（之后分别约为150 MiB和290 MiB），同时打开的会话较多时，普通会话的缓冲区会被映射进这个范围，解码器一访问，整板即复位，内核不留任何记录。补丁增加一个只带`iommu-addresses`的reserved-memory节点，并在`iris`的`memory-region`中引用（固件区域须排在第一个），使该范围在主域中预留。地址空间不足时，映射失败，对应会话的`VIDIOC_QBUF`或`VIDIOC_STREAMON`返回错误，该路解码失败，不再复位。CB15（编码器ARP，见下文）有独立的页表，不受影响 |
| `patches/kernel/dts/0035` | `arch/arm64/boot/dts/qcom/sc8280xp.dtsi` | 驱动按原始帧大小估算编码负载，单路1080p30实时编码就请求约1.08 GHz，视频编解码硬件的核心时钟（`video_cc_mvs0_clk`）原来一直处于最高档560 MHz。在这个频率下，编码器的VSP偶尔在一帧中途停住，固件看门狗随即复位硬件（`VCODEC HW is hung due to (CCE 0 VPP 0 VSP 1)`），丢掉3帧后以IDR重新开始，约每7500帧一次，与会话数、CPU空闲状态和DDR带宽投票无关；MMCX与MX电压不变而核心时钟为444 MHz或240 MHz时不出现。Windows驱动在这颗SoC上最高也只用到444 MHz。补丁去掉533与560 MHz两档，驱动请求更高的频率时落在444 MHz。444 MHz下单路按120 fps实时解码3840x2160的HEVC（8位与10位）与VP9仍没有一帧晚过一个帧周期，H.264 4K不限速约100 fps，4K编码在输入预先填好时约160 fps，吞吐量约比560 MHz低20% |
| `patches/kernel/iris/0019` | `drivers/media/platform/qcom/iris/` | 解码器原来不论分辨率，都按4096x2304的最坏情况确定输入（码流）缓冲区大小（H.264/HEVC约6.75 MiB，VP9约13.5 MiB），并忽略客户端在`S_FMT`中设置的值，因此`v4l2_codec2`只能为每路会话分配16个16 MiB的输入缓冲区。补丁改为采用客户端设置的值，下限为分辨率所需的大小（4:2:0半帧，VP9为整帧，不低于720p），上限为原来的最坏值；客户端填0时仍为最坏值。`buf_prepare`改为按`S_FMT`确定的大小检查输入缓冲区，码流中途提高分辨率后，原有的输入缓冲区仍可使用 |
| `patches/kernel/iris/0020` | `drivers/media/platform/qcom/iris/` | 屏幕关闭而系统没有休眠时（例如熄屏后继续录像或导出），CPU空闲时间长，CPU簇常进入最深的簇状态（PSCI `0x4100c344`，AOSS级休眠，进入加退出延迟9825 µs）。这期间视频硬件（VPP/VSP）会在一帧中途挂起，固件报告`VCODEC HW is hung`并复位硬件：编码丢帧、多出关键帧，随后出现SMMU context fault，之后的编码会话可能一直失败到重启；解码丢一帧。只断APSS的簇状态（`0x41000044`，5800 µs）实测不会触发；Windows的电源插件在视频持有总线与电压投票时同样不让平台进入AOSS级休眠。补丁让SC8280XP上的`iris`在上电期间持有7000 µs的CPU延迟请求，挡住AOSS级休眠与未经验证的CX级休眠（`0x41002344`，DT同样尚未提供），仍允许只断APSS的状态（DT目前没有提供，提供后即可使用）；`iris`运行时挂起后请求撤销，空闲时的簇休眠不受影响 |
| `patches/kernel/iris/0022` | `drivers/media/platform/qcom/iris/` | 回移主线的实例引用计数（`af5f7df6c8b3`）。中断线程按会话号找到实例后，先解开`core->lock`再取`inst->lock`；这期间`close()`可能已经销毁锁并释放实例。补丁给实例加上引用计数：查找时取引用，处理完再放，最后一个引用放掉时才释放实例 |
| `patches/kernel/iris/0023` | `drivers/media/platform/qcom/iris/` | 固件对flush或停流的回应里可能带回驱动已不再排队的缓冲：VP9在消费者停止归还输出缓冲之后，对CAPTURE停流时发出的`HFI_FLUSH_ALL`会多回一个输出包，既没有缓冲地址也没有分配长度，输出标号为0。原来这类包一律把会话置为错误并报`error in ftb done`，播放器被强制停止时常见。补丁把这种没有缓冲地址的输出包直接丢弃（不带EOS的才丢，EOS包都带着所归还缓冲的地址），以免它的标号0与偏移0匹配上CAPTURE缓冲0；在flush或停流进行中、或CAPTURE已经停流时，把其余对不上号的包当作迟到的归还丢弃，只留调试信息；CAPTURE在流动且没有flush或停流时仍按错误处理，并在日志中给出流、标号、长度与会话状态 |
| `patches/kernel/iris/0024` | `drivers/media/platform/qcom/iris/` | `iris_close()`先释放控件和m2m上下文，最后才发`SESSION_END`，这期间固件仍可能回包，gen1的缓冲完成与事件处理会访问已释放的m2m上下文。补丁在释放之前于`inst->lock`下把会话标记为正在关闭，此后gen1的处理丢弃缓冲完成包、忽略分辨率变化事件，会话出错时也不再去标记vb2队列。gen2的处理没有加这一判断 |
| `patches/kernel/iris/0025` | `drivers/media/platform/qcom/iris/` | 编码器的帧不带码流返回（固件丢帧、数据损坏，或长度为0且不在排空中）、编码输入带回错误状态、固件拒绝某条命令（非致命）时，原来都不留日志，最后一种只有`dev_dbg`。补丁为这几种情况加上限速告警：`encoder frame without bitstream`、`encoder input … returned with status`与`firmware rejected command`。flush与停流期间不告警，用户态收到的内容不变 |
| `patches/kernel/iris/0026` | `drivers/media/platform/qcom/iris/` | `seek_mode=1`时，CAPTURE停流之后的OUTPUT停流只做flush，会话保持已加载、已启动，以便下一次开流继续解码；`iris_close()`在释放m2m上下文之前会把这样的会话停掉。两个队列都还在流动时关闭文件（例如持有它的进程退出，包括Codec2 HAL进程崩溃），停流由`v4l2_m2m_ctx_release()`在这次检查之后完成，OUTPUT停流走了只flush的分支，`SESSION_END`发给仍在运行的会话而被固件拒绝，会话进入错误状态；随后驱动释放内部缓冲时固件可能仍在使用，导致SMMU context fault、固件看门狗与固件重载。补丁让正在关闭的会话在OUTPUT停流时完整停止（STOP与RELEASE_RESOURCES），依赖`0024`的关闭标记 |
| `patches/kernel/iris/0027` | `drivers/media/platform/qcom/iris/` | `iris_core_init()`遇到处于错误状态的核心时，原来直接改成DEINIT再加载固件，不做拆除；SYS_INIT超时或出错时也不拆除，固件、电源与消息队列都留着。下一次打开在其上再次初始化，泄漏GDSC、时钟与互连的引用以及PAS状态，此后的打开可能一直失败到重启。补丁让错误状态下的打开返回`-EBUSY`，等待恢复完成；SYS_INIT失败时由初始化自己拆除核心，下一次打开从DEINIT冷启动；初始化与恢复由新增的`init_lock`串行化 |
| `patches/kernel/iris/0028` | `drivers/media/platform/qcom/iris/` | 重载固件期间持有运行时PM引用，与`iris_open()`在`iris_core_init()`前后的做法一致。原来拆除中的引用释放可能在DEINIT状态下触发运行时挂起，挂起什么也不做，重载后的核心在运行时PM看来却已挂起。系统休眠期间运行时PM关闭（`-EACCES`）时稍后重试 |
| `patches/kernel/iris/0029` | `drivers/media/platform/qcom/iris/` | 运行时恢复时固件没有从断电中恢复（热启动失败），原来只返回错误，之后每次恢复都重试同一个热启动，硬件编解码一直不可用到重启。补丁把它升级为一次完整的固件重载，与固件崩溃后的处理相同；拆除时先给核心上电，再关闭固件 |
| `patches/kernel/iris/0030` | `drivers/media/platform/qcom/iris/` | `iris_hfi_queue_cmd_write()`在`pm_runtime_resume_and_get()`失败后仍调用`pm_runtime_put_sync()`，运行时PM的使用计数下溢。补丁在恢复失败时直接返回 |
| `patches/kernel/iris/0031` | `drivers/media/platform/qcom/iris/` | 固件启动失败（`invalid uc_region`或超时）时打印CTRL_INIT、CTRL_STATUS，并回读UC区域与队列表地址；记录`qcom_scm_pas_shutdown()`的失败与重载结果（`firmware reloaded`、`firmware reload failed`），便于判断固件恢复卡在哪一步 |
| `patches/kernel/iris/0032` | `drivers/media/platform/qcom/iris/` | 冷启动时，在释放固件之前清零CTRL_INIT与CTRL_STATUS，值不为0时告警`stale boot handshake`。固件因此总在复位处理里等主机完成内存保护与UC区域的设置，主机的轮询也不会读到上一次留下的状态。热恢复路径不变 |
| `patches/kernel/iris/0033` | `drivers/media/platform/qcom/iris/` | 固件重载任务改在可冻结的工作队列上运行。原来它在系统挂起期间照常运行，可能与`pm_runtime_force_suspend()`交叠：正在等待SYS_INIT的重载会让断电失败从而中止挂起，或者让仍在启动的固件断电。现在冻结器会等进行中的重载完成，之后排入的重载（包括热启动失败时排入的）在系统恢复后运行 |
| `patches/kernel/iris/0036` | `drivers/media/platform/qcom/iris/` | `iris_hfi_get_v4l2_color_primaries()`把固件报告的解码码流三原色（H.273编号）换算成V4L2色彩空间，其中DCI-P3一项的标签与返回值写反（主线同样如此）：三原色11（SMPTE RP 431-2，DCI-P3）被报告为默认值，12（SMPTE EG 432-1，D65白点的P3）被报告为`V4L2_COLORSPACE_RAW`。补丁把11映射为`V4L2_COLORSPACE_DCI_P3`，12没有对应的V4L2色彩空间，报告为默认值。SC8280XP上gen1解码器不会用到这一映射：其固件在色彩属性里只给三原色编号，驱动按gen2的打包格式解析，未找到视频信号类型标志，因此在补丁前后都报告默认值；其他gen1平台尚未验证。`v4l2_codec2`不读取驱动报告的色彩信息，Android上没有可见变化 |
| `patches/android/v4l2-codec2/0011` | `external/v4l2_codec2` | 解码器的输入缓冲区改回1080p及以下2 MiB、以上8 MiB（LineageOS为满足旧版iris的最坏值改成了16 MiB和64 MiB），每路1080p会话的输入缓冲区从256 MiB降到32 MiB。应用设置了更大的`max-input-size`时仍采用应用的值。大小在配置解码器时确定：按1080p及以下配置的会话，单帧码流不能超过2 MiB，码流中途提高分辨率后也是如此；1080p以上的VP9不能超过8 MiB（原为13.5 MiB）。Chromium在关键帧放不下时会用更大的缓冲区重建解码器，Media3按自适应码流中最大的格式分配。必须与`patches/kernel/iris/0019`一起使用：在不含0019的内核上，1080p及以下的解码与1080p以上的VP9解码会在启动时报错退出 |
| `patches/android/v4l2-codec2/0014` | `external/v4l2_codec2` | `flush()`把仍排在驱动里的输出帧移入复用队列，再逐个投递任务重新排队。若这期间来了分辨率或位深变化（例如flush后立即seek到位深不同的部分），输出队列按新格式重新分配，已投递的旧帧却拿到新队列的缓冲编号、带着按旧格式分配的缓冲排进去：8位切到10位时NV12缓冲小于P010平面，`VIDIOC_QBUF`失败（videobuf2：`failed to attach dmabuf`），组件报错；反方向时P010缓冲会被当成NV12写入。补丁给输出队列的分配计数，丢弃早于最近一次分配取出的复用帧，块回到原来的缓冲池 |
| `patches/android/v4l2-codec2/0015` | `external/v4l2_codec2` | 框架把`C2StreamPictureSizeInfo::output`放进输出格式；对使用输出缓冲数组的客户端（同步模式下的NDK客户端，或调用`getOutputBuffers()`的客户端），按它的宽高分配复制帧用的缓冲。组件原来在分辨率变化后不更新画面尺寸，分辨率变大后框架无法复制（`tried 5 realloc and failed`），解码报错。补丁在新尺寸的第一帧把画面尺寸写回接口并放进该帧的`configUpdate`，与软件解码器相同 |
| `patches/android/v4l2-codec2/0016` | `external/v4l2_codec2` | 框架只在解码器的输出像素格式允许`HAL_PIXEL_FORMAT_YCBCR_P010`时，才在颜色格式里列出`COLOR_FormatYUVP010`。HEVC与VP9解码器对HEVC Main 10与VP9 Profile 2输出P010，但原来没有这个参数。补丁按软件VP9/AV1解码器的方式声明`YCBCR_420_888`、P010（支持10位档次且gralloc能分配P010时）与`IMPLEMENTATION_DEFINED`；解码器仍按码流决定输出NV12还是P010 |
| `patches/android/v4l2-codec2/0019` | `external/v4l2_codec2` | 轮询线程在`poll()`只返回`POLLERR`时原来立即重新轮询缓冲。两个队列都没有排队的缓冲时，`v4l2_m2m_poll()`马上返回`POLLERR`，于是每次解码器启动到第一次送入码流之间（3–40 ms）空转上万次，vb2队列出错后则一直空转，错误也传不到解码器。补丁在这种情况下只投递一次回调（真正的队列错误由`VIDIOC_DQBUF`返回`-EIO`暴露），停止轮询缓冲，直到下一次`VIDIOC_QBUF`或`VIDIOC_STREAMON`。编码器共用这个轮询线程 |
| `patches/android/v4l2-codec2/0020` | `external/v4l2_codec2` | 事件回调投递之后、解码器线程把事件取走之前，`poll()`一直返回`POLLPRI`，轮询线程反复投递回调，多出来的回调`VIDIOC_DQEVENT`拿到`ENOENT`。补丁在事件回调执行期间不轮询`POLLPRI`，回调执行完再恢复 |
| `patches/android/v4l2-codec2/0021` | `external/v4l2_codec2` | 解码器进入错误状态后，`serviceDeviceTask()`不再处理设备，没有取走的事件让`poll()`一直返回`POLLPRI`，轮询线程与解码器线程合计占用接近两个CPU核心，直到解码器被销毁。补丁在`onError()`中停止轮询 |
| `patches/android/v4l2-codec2/0022` | `external/v4l2_codec2` | 没有任何解码设备列出该编码格式（或级别控件没有可识别的级别）时，`DecodeInterface`使用内置的默认级别表，VP9原来只到5级（3840x2160约37 fps）。补丁在这张表里补上5.1与5.2级（4K约141 fps），与`0023`改过的`V4L2Device`默认表保持一致。iris gen1解码器没有级别控件，`DecodeInterface`收到的是`V4L2Device`的默认表，见`0023` |
| `patches/android/v4l2-codec2/0023` | `external/v4l2_codec2` | 驱动没有VP9级别控件时，`V4L2Device::queryC2Levels()`返回自己的默认级别表，VP9原来也只到5级。iris gen1解码器没有VP9级别控件，应用看到的就是这张表；按码流级别判断时，会把4K60的VP9（5.1级）判为超出硬件能力而转用软解。补丁补上5.1与5.2级，实际可用的尺寸与帧率仍受`media_codecs_c2.xml`的限制。实测`MediaCodecList.findDecoderForFormat()`对4K60（5.1级）与4K120（5.2级）的VP9选中`c2.v4l2.vp9.decoder`，此前返回空 |

### 3. 硬件视频编码

硬件编码要内核、`v4l2_codec2`和设备配置三处一起改。只改设备配置、内核却没有CP非像素上下文时，第一次编码就会让整板复位，所以`device.mk`里的编码器属性只能配合本仓的内核补丁使用。

#### 内核侧

`qcom-iris`的gen1编码路径在SC8280XP上要额外满足一条硬件约束：编码器固件通过内容保护非像素流SID `0x2a04`访问它的ARP（auto-register programming）PERSIST缓冲，而这条流只能经由`apps_smmu`上CBAR VMID为`0xb`（CP_NON_PIXEL）的stage-1上下文bank翻译。ARP若留在`iris`主域（SID `0x2a00`）上，第一次编码就会让整板复位。Windows的`qcsmmu8280.sys`从host侧配置这块bank，主线`arm-smmu`没有secure-VMID支持，所以由下游补丁完成：

- **`patches/kernel/iommu/0001`（`arm-smmu-qcom`）**：`cfg_probe`时预留CB15、写CBAR（VMID `0xb`）、加`0x2a04`的SME，成功时打印`cp-nonpixel: CB15 ready`。bank号固定为15，与Windows相同：把`0x2a04`绑到另一个VMID相同的bank时启动挂住。页表经io-pgtable的自定义分配器分配，每个表页用`qcom_scm_assign_mem`赋给`{HLOS RW, 0xb R}`。系统resume的reset会清掉这块bank，SoC的reset钩子随后重新编程它。`0x2a04`不能写进`iris`主节点的`iommus`：那样它会被绑到`iris`的HLOS bank，启动时挂住。
- **`patches/kernel/iommu/0002`**：CB15允许同时存在多个映射，只在映射数从0变为1时编程一次。编程过程先把SCTLR写成0，若在另一路编码进行中重写，那一路的ARP访问会落到翻译已关闭的bank上。映射前检查IOVA在窗口内且按页对齐，映射可以跨越叶页表；解除映射时计数，大小对不上时告警并保留计数。系统resume时仍无条件重新编程。
- **`patches/kernel/iris/0013`**：编码器的`BUF_ARP`分配为连续内存，页独占赋给VMID `0xb`（HLOS与`0xb`共享的授予会被hypervisor拒绝），映射进CB15在窗口`0x01000000..0x25800000`内的固定IOVA `0x01000000`，并把该IOVA交给固件；拆除时反向还原，页无法交还HLOS时宁可泄漏也不释放。
- **`patches/kernel/iris/0034`**：ARP改按会话分配IOVA：窗口起点起8个256 KiB的槽位（槽位0即原来的`0x01000000`），由一个全局的IDA管理。槽位在任何SCM调用之前取得，在解除映射之后释放，页泄漏时也释放；8个都占用时开流返回`-EBUSY`并记录`all … CP non-pixel slots in use`。8个槽位都在已有的那一张2 MiB叶页表里，不需要新的页表页。IDA放在文件作用域而不是`iris_core`里，因为CB15与它的页表是全系统唯一的，比单个`iris_core`活得久。
- **`patches/kernel/iris/0035`**：固件拒绝SESSION_INIT时（例如会话数超出固件上限），原来只把实例置为错误，REQBUFS照常成功，直到STREAMON才返回`-EBUSY`；现在第一次REQBUFS即失败，并记录`init failed`。
- **`patches/kernel/iris/0037`**：gen1编码器接受P010输入，只用于HEVC Main 10，并且只在有4条VPP管线的gen1核心（SM8250、SC8280XP）上提供。P010的步长为`ALIGN(宽×2, 256)`，Y平面行数按32对齐、UV平面按16对齐，与解码器的输出和gralloc的分配一致；向固件发送P010格式、按字节计的步长与平面约束。码流缓冲在HEVC 10位输入时放大四分之一。宽度上限为4096（平面约束的步长上限为8192字节），`TRY_FMT`与`S_FMT`钳制宽度，`ENUM_FRAMESIZES`与`ENUM_FRAMEINTERVALS`按此申报。开流时检查位深与档次：P010输入与H.264或HEVC Main的组合、NV12输入与Main 10的组合都返回`-EINVAL`并记录原因（此前Main 10与NV12输入的组合可以设置成功，固件按10位会话运行，编码结果错误）。编码格式从HEVC改为H.264时，输入格式退回NV12。
- **`patches/kernel/iris/0038`**：按输入（OUTPUT）格式的色彩参数发送`HFI_PROPERTY_PARAM_VENC_VIDEO_SIGNAL_INFO`，固件把它写进码流的VUI（原来硬件编码的H.264与HEVC都不带色彩描述）。留为默认的字段按色彩空间的V4L2默认值补全；四项都为默认时不发送，未设置色彩参数的编码不受影响。色彩空间为默认时，V4L2核心会把其余三项一并清除（驱动替换不支持的色彩空间时同样如此），这时不发送色彩描述；opRGB与RAW没有对应的编号，三原色写为未指定（opRGB的传输特性同样写为未指定）。V4L2没有HLG传输特性，HLG无法写入。CAPTURE改为报告OUTPUT的色彩参数。
- **`patches/kernel/iris/0039`**：gen1编码器增加`V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY`与`V4L2_CID_COLORIMETRY_HDR10_CLL_INFO`两个控件。HEVC Main 10且传输特性为PQ时，以`HFI_PROPERTY_PARAM_VENC_HDR10_PQ_SEI`写入母版显示器与内容亮度SEI；客户端未设置时使用默认值（P3 D65，1000与0.005 cd/m²，内容亮度为0即未知）。venus同样在客户端未设置时使用默认值，但数值不同，并且对每个Main 10码流都写入。三原色按V4L2规定的绿、蓝、红顺序交给固件，码流中的顺序与此一致。开流后无法修改。
- **`patches/kernel/iris/0014`**：gen1编码器的档次/级别改发HFI位掩码值（原来发V4L2枚举，HEVC Main会被固件当成Main Still Picture）；级别按分辨率和帧率推导。同时修正该属性的payload大小（原来少分配4字节）。
- **`patches/kernel/iris/0015`**：编码器EMPTY_BUFFER时间戳按微秒发（固件码率控制按微秒读，发纳秒会让码率严重超标），响应侧转回纳秒。
- **`patches/kernel/iris/0016`**：回移主线v7.3的强制关键帧控制`V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`。`v4l2_codec2`每到关键帧间隔都设这个控件，设不上就判编码失败。
- **`patches/kernel/iris/0017`**：gen1编码器的输入帧尺寸改发原始尺寸、输出帧尺寸改发请求的输出尺寸（`enc_scale`）。原来输入发的是按128x32对齐的缓冲尺寸，宽度不是128的倍数时（竖拍1080x1920），固件把右侧填充当成画面读进来再缩放，画面右侧出现绿边、整体被压窄。步长和行数另由`UNCOMPRESSED_PLANE_ACTUAL_INFO`发给固件；固件把编码图像补齐到整宏块，并在SPS里写明裁剪。gen1没有输入裁剪偏移，比原始尺寸小的裁剪仍会被缩放而不是裁掉，`v4l2_codec2`不会这样设置。
- **`patches/kernel/iris/0018`**：EMPTY_BUFFER包清零。没有开启`CONFIG_INIT_STACK_ALL_ZERO`的内核会把`extradata_buffer`与`data`两个字段的栈上旧值发给固件；本仓内核开启了这个选项，实际不受影响。
- **`patches/kernel/iris/0021`**：gen1编码器在GOP长度为0时向固件发最长的帧内间隔（`0x0fffffff`个P帧，高通自家驱动对无限长GOP发的值）。`V4L2_CID_MPEG_VIDEO_GOP_SIZE`为0表示客户端自己请求关键帧、不要周期性的帧内帧；原来`iris_set_intra_period()`对0返回`-EINVAL`而调用方忽略，固件收不到帧内间隔，就把每一帧都编成帧内帧。

#### `v4l2_codec2`侧

- **`patches/android/v4l2-codec2/0008`**：编码器输入块的用途加上`C2MemoryUsage::CPU_READ`。组件要在CPU上映射第一块输入来读平面布局，录屏送来的RGB还要逐帧在CPU上转成NV12；输入块没有CPU读用途时gralloc拒绝lock，相机录像和录屏在第一帧就失败。`C2StreamUsageTuning`里放的是C2的用途位，不能填gralloc常量：gralloc的`SW_READ_OFTEN`（`0x3`）按C2的位读是`READ_PROTECTED`，录屏会一帧都拿不到。
- **`patches/android/v4l2-codec2/0009`**：新增`c2.v4l2.hevc.encoder`，由`ro.vendor.v4l2_codec2.encoder.supported.hevc`把守。只提供Main档次：输入是8位NV12，Main Still Picture一个码流只能有一张图，Main 10要10位输入。级别1到5.2，默认4.1，按尺寸、帧率和码率自动抬到够用的最低一级（H.265表A.8、A.9，以及A.4.1对单边长度的限制）。iris gen1没有`V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR`，参数集由组件缓存后插到每个IRAP图像前：H.264是SPS/PPS（上游已有），HEVC是VPS/SPS/PPS。
- **`patches/android/v4l2-codec2/0010`**：单平面输入（NV12而不是NV12M）送缓冲时，长度改按驱动要求的`sizeimage`给。原来按一帧的数据量（步长×按32对齐的高×1.5）给，而iris把整帧大小补齐到4 KiB的倍数。步长是128的奇数倍、对齐后的高是32的奇数倍时（640x480、800x480、854x480、352x288、120x160等），长度差2048字节，第一次`VIDIOC_QBUF`就被videobuf2拒绝，组件随即报错；打开`videobuf2_common`的`debug`参数后，内核日志里能看到`invalid dmabuf length`。多平面分支本来就用驱动给的大小。
- **`patches/android/v4l2-codec2/0012`**：编码器格式转换器的帧改按驱动格式中的高分配。需要转换的输入（录屏送来的RGB、应用用GL渲染后送入的Surface）由组件逐帧转成NV12，转换用的帧按`codedSize()`分配，这个尺寸原来由`V4L2Device::allocatedSizeFromV4L2Format()`从`sizeimage`反推。iris把`sizeimage`补齐到4 KiB的倍数后，640x480反推成640x482，minigbm把482行对齐到512行，色度平面放在其后；驱动按单平面NV12的布局，在`bytesperline`×高（640×480）处读色度。每帧的色度因此取自写入位置之上32行，画面顶部64行变绿，整幅颜色下移。步长是128的奇数倍、对齐后的高是32的奇数倍时一帧比整页少2048字节，只有步长不超过1152时这2048字节才够一行，反推出的高才偏大，所以受影响的是640x480、800x480、854x480、352x288、120x160这类尺寸；1920x1440等更宽的尺寸虽然也补齐，反推出的高仍然准确。字节缓冲输入和NV12的Surface不经转换，不受影响。补丁只对单平面格式改用格式中的高，多平面格式的各平面本来就分别带偏移。
- **`patches/android/v4l2-codec2/0013`**：编码器的宏块码率控制（`MB_RC_ENABLE`）与GOP长度改为各设一次。原来两者放在同一次`VIDIOC_S_EXT_CTRLS`里，iris没有`MB_RC_ENABLE`，整次调用被拒，GOP长度停在驱动默认的30，固件在组件请求的IDR之间每30帧再插一个I帧（录屏120 fps、2 s间隔时，这些I帧约占码流的六分之一）。现在先查GOP长度的取值范围：最小值为0时设为0，驱动不接受0或最小值大于0时改设它支持的最大值（控件框架会把超出范围的整数夹到范围内而不报错，直接设0在最小值为1的驱动上就成了每帧一个关键帧）；关键帧间隔为负时只在第一帧和请求时出关键帧。必须与`patches/kernel/iris/0021`一起使用。
- **`patches/android/v4l2-codec2/0017`**：固件不带码流地返回一帧（丢帧、数据损坏，或长度为0），iris给出带`V4L2_BUF_FLAG_ERROR`或长度为0的缓冲，组件原来直接丢弃它。工作项按顺序完成，每一项都要等到输出，于是会话从这一帧起一帧不出，也不报错。没有B帧时输出按输入顺序返回，补丁把最早一个还在等待输出的工作项以`FLAG_DROP_FRAME`完成（与解码组件相同），连续丢30帧（30 fps下约1 s）时报错，让应用可以重建编码器或换用软编。排空时末尾带`V4L2_BUF_FLAG_LAST`的空缓冲仍只结束排空。
- **`patches/android/v4l2-codec2/0018`**：编码器实例计数原来在组件析构时才减少，而框架在`release()`返回后还会持有组件一段时间；资源管理器收回编码器后立即重试时，可能因为旧组件对象还没析构而再次撞到上限。补丁在`release()`关闭设备之后减少计数，并保证只减一次。
- **`patches/android/v4l2-codec2/0024`**：设备的HEVC编码器接受P010输入、gralloc也能分配P010时，HEVC编码组件提供Main 10档次并声明P010输入格式；否则只提供Main，与原来相同。Main 10会话使用单平面P010输入，布局与设备一致的P010帧直接送入硬件，其他布局逐帧复制。8位YUV输入扩展为10位（无损），RGB输入按BT.601有限范围转换后扩展；P010帧送进8位会话时四舍五入为NV12（此前按NV12编码，编码结果错误）。同时修正P010的`bytesPerElement`（UV平面少算一半）与`videoPixelFormatToString`。
- **`patches/android/v4l2-codec2/0025`**：Main 10时声明输入色彩参数与VUI色彩参数；设备有HDR10控件时还声明`C2StreamHdrStaticInfo::input`，框架随之列出Main 10 HDR10。10位会话把色彩参数作为输入格式的色彩描述交给设备，只在V4L2能完整描述时发送：三原色、传输特性与矩阵都已指定（范围未指定时按有限范围）。HLG、D65白点的P3、gamma 2.2等V4L2没有的值，以及缺少三原色、传输特性或矩阵的组合，码流不带色彩描述，播放时使用容器中的色彩信息。VUI色彩参数报告码流实际携带的值。RGB输入在10位会话中按BT.601有限范围转换，第一帧到来时组件以BT.709三原色、sRGB传输特性、BT.601矩阵、有限范围的描述重新启动编码器；容器中的色彩信息由框架按输入Surface的数据空间写入，可能与码流不同。HDR10静态元数据在开流前写入两个控件。8位会话不传色彩参数，行为不变。

#### 设备配置

- `device.mk`：`ro.vendor.v4l2_codec2.encoder.supported.h264`、`.hevc`设为`true`，`ro.vendor.v4l2_codec2.encode_concurrent_instances=3`（AVC与HEVC共用这一个计数，需`kernel/iommu/0002`、`kernel/iris/0034`与`kernel/dts/0026`）。VP8/VP9编码iris没有，保持关闭。
- `media_codecs_c2.xml`：两个编码器的`<Encoders>`条目，`concurrent-instances`为3。尺寸和吞吐量上限按4K30申报，偏保守：视频编解码硬件的核心时钟上限为444 MHz（见`kernel/dts/0035`），单路裸V4L2编码在输入预先填好时3840x2160约160 fps（4096x2176按像素数折算约150 fps）；逐帧在CPU上填充时受填充速度限制，1080p约380 fps。
- 组件rank是0x80，排在软编（0x200）前面。第四个编码请求在分配组件时被拒（`maximum number of instances reached`，返回资源不足）。`MediaCodec`遇到资源不足会先请资源管理器从优先级更低的进程收回一个硬件编解码器（解码器也算在内）再重试，收不回才试下一个编码器，落到`c2.android.*`软编。因此按框架代码，优先级更高的一方会让优先级较低的应用失去编解码器（`ERROR_RECLAIMED`），尚未经实测；优先级不高于对方时，才会直接换软编。原生进程（例如shell里的`screenrecord`）既不会收回别人的编码器，也不会被收回。硬件编码器配置失败时，`MediaRecorder`同样会换下一个编码器；自己调用`configure`的应用拿到的是错误。
- 验证：`scripts/verify-hw-codec2.sh`检查属性、编码节点属主、`cp-nonpixel: CB15 ready`和组件列表，亮屏时用`screenrecord --codec-name c2.v4l2.avc.encoder`在1280x720和640x480各真编一段。

#### 已知限制

- **同时最多3路编码**：内核有8个ARP槽位，`v4l2_codec2`按3路限制。固件出错重载时，所有正在进行的编解码会话一起报错。
- **HEVC Main 10与HDR10编码**：只有HEVC提供10位档次，H.264不提供。Main 10编码的宽度最多为4096，与编码器申报的尺寸上限相同。HLG与D65白点的P3无法写入码流的色彩描述，只记录在容器中。8位编码不写色彩描述。RGB输入在10位会话中按8位sRGB内容处理；输入格式为10位RGB（`RGBA_1010102`）时无法编码；10位YUV（P010）输入不受影响。
- **系统录屏**：系统录屏按30 fps申报，横屏录2560x1600，竖屏按解码器的能力缩到1360x2176（竖屏尚未经实测），默认用硬件H.264编码器，打开“HEVC编码”时用硬件HEVC编码器，不会换用软编。输入是可变帧率，滑动画面时可有接近120 fps的突发。`adb shell screenrecord`不带`--size`时会改用1280x720重试。
- **很小的尺寸**：宽或高按驱动的对齐（128x32）之后仍小于128时（例如160x90），内核在申请缓冲时拒绝（`current session not supported`），应用在开始编码时拿到错误。原始尺寸小于128、对齐后不小于128的（例如160x120、100x100），固件能正常编码，并在SPS里写明裁剪。
- **唤醒后偶发一次无输出**：休眠唤醒后紧接着编码，偶尔会有一次固件把每一帧都不带码流地返回，持续二三十秒，随后的编码自行恢复。内核记录`encoder frame without bitstream`告警（`patches/kernel/iris/0025`），组件按丢帧处理，连续丢30帧时向应用报错（`patches/android/v4l2-codec2/0017`）。原因不明。
