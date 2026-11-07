# 音频子系统与硬件视频解码（Audioreach + `qcom-iris`）

本文档说明`gaokun3`的LPASS/SoundWire音频拓扑、扬声器/耳机/麦克风通路配置、低延迟播放修复以及基于`qcom-iris`与`v4l2_codec2`的硬件视频解码架构。

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

### 1. 扬声器通路与防破音/防爆音（`patches/0015`）

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
  - 响度提升通过WSA8830的模拟功放增益`SpkrLeft/Right PA Volume`实现。根据内核`wsa883x.c`的TLV定义（`0..14`固定为−3 dB，`15..29`每档+1.5 dB），上游机器驱动`sc8280xp.c`原本调用`snd_soc_limit_volume(card, "SpkrLeft PA Volume", 17)`将上限锁在17（0 dB）。`patches/0015`将该上限放宽至23（+9 dB），`audio-route.sh`在运行期将其设为**`21`（+6 dB）**，在不触发数字削顶的前提下获得充足外放音量。
- **消除启停流爆音（Pop）**：
  - `SpkrLeft BOOST Switch`与`SpkrRight BOOST Switch`控制WSA8830的Boost SoundWire数据端口，与模拟PA增益无关。启用该开关会在每次音频流启动/停止时引发明显的瞬态电流冲击声（Pop），而稳态音量完全不变。因此`audio-route.sh`显式将其保持为`0`（关闭）。

### 2. 3.5mm有线耳机与插拔检测（`patches/0010`）

- **WCD938x耳放完整插值器链路**：
  仅打开`HPHL_RDAC Switch`与`RX_MACRO RX0/1 MUX`不足以让WCD938x输出模拟信号，必须同时打通RX Macro的插值器混音链与Class-H控制器：
  ```
  RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1 = 1
  RX_MACRO RX0 MUX = AIF1_PB, RX_MACRO RX1 MUX = AIF1_PB
  RX INT0_1 MIX1 INP0 = RX0, RX INT1_1 MIX1 INP0 = RX1
  RX INT0 DEM MUX = CLSH_DSM_OUT, RX INT1 DEM MUX = CLSH_DSM_OUT
  RX_COMP1 Switch = 1, RX_COMP2 Switch = 1
  HPHL_COMP Switch = 1, HPHR_COMP Switch = 1
  HPHL_RDAC Switch = 1, HPHR_RDAC Switch = 1
  HPHL Switch = 1, HPHR Switch = 1
  CLSH Switch = 1, LO Switch = 1
  RX_RX0 Digital Volume = 84, RX_RX1 Digital Volume = 84
  HPHL Volume = 20, HPHR Volume = 20
  ```
- **耳机插拔检测与HAL动态端口填充**：
  1. **Framework输入事件检测**：主线内核`wcd938x` MBHC通过`/dev/input/event*`上报`SW_HEADPHONE_INSERT`与`SW_MICROPHONE_INSERT`，不创建旧版`/sys/class/switch/h2w`。设备树overlay必须设置`config_useDevInputEventForAudioJack = true`，`WiredAccessoryManager`才能收到插拔事件。
  2. **策略XML与`ModulePrimary`修复（`patches/0010`）**：
     - 在`audio_policy_configuration.xml`中，可热插拔设备端口（`Wired Headphones`、`Wired Headset`、`Wired Headset Mic`）不可写死`address="CARD_0_DEV_0"`，因为`WiredAccessoryManager`构造连接通知时固定传入`address=""`，写死非空地址会导致策略引擎匹配不到模板端口而丢弃事件。省略`address`后，AOSP默认ALSA地址解析自动回退至`(card=0, device=0)`。
     - AOSP `libaudiohal@aidl`的`ModulePrimary`继承自基类空实现`Module::populateConnectedDevicePort`（不会像USB/蓝牙子类那样探测硬件填充采样率与通道掩码），而`setDeviceConnectionState`要求连接后的端口必须带有非空`profiles`。`patches/0010`在`ModulePrimary`中重写`populateConnectedDevicePort`，当目标端口`profiles`为空时直接复用模板端口声明的`profile`列表。

## 三、内置麦克风录音修复（`patches/0051`、`0052`、`0063`）

### 1. 混音器路由

内置双数字麦克风通过LPASS VA Macro采集至`MultiMedia4`（`hw:0,3`）：

```
MultiMedia4 Mixer VA_CODEC_DMA_TX_0 = 1
VA DEC0 MUX = MSM_DMIC, VA DEC1 MUX = MSM_DMIC
VA DMIC MUX0 = DMIC0, VA DMIC MUX1 = DMIC1
TX_DEC0 Volume = 102, TX_DEC1 Volume = 102
```

### 2. 三处用户态HAL修复

1. **ALSA设备地址解析（`patches/0051`）**：
   `XsdcConversion.cpp`在处理`AUDIO_DEVICE_IN_BUILTIN_MIC`时无条件将XML中的`address`覆写为`"bottom"`，抹掉了`audio_policy_configuration.xml`中配置的`"bottom CARD_0_DEV_3"`，导致HAL回退去打开并不存在采集流的`hw:0,0`。`patches/0051`保留带有`CARD_x_DEV_y`后缀的自定义地址。
2. **输入Profile约束与移除假MMAP输入**：
   - `hw:0,3`（`MultiMedia4`）的Audioreach拓扑仅支持`48000 Hz` + 双声道（`AUDIO_CHANNEL_IN_STEREO`）。若在XML中声明`16000`/`44100`或`AUDIO_CHANNEL_IN_MONO`，HAL会直接以不支持的参数调用`pcm_open`并失败。将其收敛为`48000` + `AUDIO_CHANNEL_IN_STEREO`后，`AudioFlinger`会自动为请求16 kHz单声道的应用插入重采样与声道转换。
   - 移除XML中的`mmap_no_irq_in`端口（AOSP `ModulePrimary`对该标志实例化的是只产生随机白噪声的`StreamInMmapStub`测试桩）。
3. **消除周期性丢块与启动静音（`patches/0052`、`0063`）**：
   - AOSP `StreamAlsa`的采集线程每10 ms从ALSA读取480帧写入容量为3840帧（8块）的`MonoPipe`，但其内部的`OffsetProvider`设置`setpoint = capacity / 2`，将写入速率钳制在平均每11.25 ms一块，导致每8块必然溢出丢弃1块。`patches/0052`将采集`MonoPipe`容量翻倍。
   - AOSP `StreamPrimary::transfer`原本对采集流也按墙钟休眠（`sleep`）定拍，而非按ALSA硬件就绪数据驱动，造成开头约171 ms全零静音与124 ms交付延迟。`patches/0063`将采集方向的`transfer`改为直接在`mAlsaDeviceProxy->read()`上阻塞等待硬件数据就绪，启动静音降至0 ms、单次交付耗时降至约1 ms。

## 四、低延迟音频播放与音游偏移修复（`patches/0008`、`0069`、`0215`）

1. **`tinyalsa`周期重算修复（`patches/0008`）**：
   AOSP `libaudiohal@aidl`请求打开`hw:0,1`时传入`period_size=1024, period_count=4`，而内核Audioreach拓扑要求30 ms周期对齐，在`SNDRV_PCM_IOCTL_HW_REFINE`中将`period_size`修正为`1440`（`buffer_size=5760`）。然而`external/tinyalsa_new/src/pcm.c`的`pcm_set_config()`仍用修改前的`1024`计算`sw_params.stop_threshold = 4096`。当环形缓冲区剩余帧数达到`5760 - 4096 = 1664`帧时，ALSA立即误判XRUN并中断流，引发持续爆音与断流。`patches/0008`修正`pcm_set_config()`以内核返回的实际`period_size`计算`buffer_size`与`stop_threshold`。
2. **移除假MMAP输出端口**：
   与输入侧同理，从`audio_policy_configuration.xml`中移除`mmap_no_irq_out`（对应的`StreamOutMmapStub`会把走AAudio MMAP路径的游戏音频静默丢弃）。
3. **播放方向取消墙钟跳帧（`patches/0069`）**：
   AOSP示例实现`StreamPrimary::transfer`在播放方向同样维护一条合成墙钟：一旦上层混音线程唤醒稍晚，HAL就打印`skipping transfer`并将整块音频直接丢弃，导致音乐游戏出现严重的节奏漂移与吞音。`patches/0069`在播放方向当ALSA proxy就绪时直接调用阻塞式ALSA写入，由声卡硬件时钟自然反压上层混音线程。
4. **播放缓冲只用3个period（`patches/0215`）**：
   alsa_utils给内置设备配ALSA缓冲时，period数统一取`ro.hardware.audio.tinyalsa.period_count`（默认4），播放和录音共用。`patches/0215`在`openProxyForAttachedDevice()`里只对输出设备改用`ro.vendor.audio.primary.out_period_count`（`device.mk`设3）：块大小不变，只是切成3个period，内核`q6apm-dai`再按480帧向上取整，得到3 × 480帧（30 ms）。录音方向保持4个，因为采集钩子在I/O线程里处理85 ms的块（通话降噪最长约51 ms才回退），这段时间新来的数据要靠录音缓冲接住。播放不能再减到2个：DSP缺数据时会扣着最后一个period不放，ALSA不会报XRUN，FastMixer与轨道的欠载计数也都是0，断音只能在游戏里听出来。

## 五、硬件视频解码（`qcom-iris` + `v4l2_codec2`）

### 1. 从`qcom-venus`迁移至`qcom-iris`

在Linux 6.18+/7.x主线内核中，高通为Gen2 HFI硬件（含SC8280XP / SM8450 / SM8550）引入了全新的`qcom-iris`驱动（`drivers/media/platform/qcom/iris/`），替代旧版`qcom-venus`。

- **内核配置与固件**：启用`CONFIG_VIDEO_QCOM_IRIS=y`（禁用`CONFIG_VIDEO_QCOM_VENUS`），设备树`video-codec@aa00000`指定固件`qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn`。
- **Android Codec2集成**：
  - 使用`android.hardware.media.c2@1.2-service-v4l2`（通过属性`media.c2.hal.selection=aidl`告知`MediaCodec`连接AIDL/HIDL服务）。
  - 注册解码器组件：`c2.v4l2.avc.decoder`（H.264）、`c2.v4l2.hevc.decoder`（HEVC / H.265）、`c2.v4l2.vp9.decoder`（VP9），最高支持4K60硬件解码。

### 2. 关键内核与用户态修复

| 补丁 | 位置 | 解决的问题 |
|---|---|---|
| `crdroid-tree-fixes.py` | `external/v4l2_codec2` | `V4L2Device::getDeviceInfos()`原本写死只扫描`/dev/video0..9`；启用相机`camss`后VFE占用了`video0..31`，解码器被推到`video32`，导致硬解静默消失。将扫描上界扩大至64 |
| `patches/0019` | `external/v4l2_codec2` | HEVC码流的首个配置缓冲（VPS/SPS/PPS）合并进第一帧DMA-BUF（≥8 MiB），避免固件因缺少`CODECCONFIG`标志在`SOURCE_CHANGE`前等待输出缓冲而死锁 |
| `patches/0056` | `drivers/media/platform/qcom/iris/` | 引入`core->hw_powered`标志，防止解码器下电后残余中断触发未供电寄存器读取导致内核崩溃 |
| `patches/0060` | `drivers/media/platform/qcom/iris/` | 允许`v4l2_codec2`在首个`V4L2_EVENT_SOURCE_CHANGE`事件到达前调用`VIDIOC_G_FMT(CAPTURE)`，兼容Android Codec2初始化顺序 |
| `patches/0065` | `drivers/media/platform/qcom/iris/` | 在非动态分辨率切换（非DRC）的`CAPTURE streamoff`路径发送`HFI_FLUSH_ALL`而非`HFI_FLUSH_OUTPUT`，修复视频拖动进度条（seek）或关闭播放器时固件等待`HFI_FLUSH_OUTPUT`响应超时导致的卡死 |

