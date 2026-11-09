# 音频子系统与硬件视频编解码（Audioreach + `qcom-iris`）

本文档说明`gaokun3`的LPASS/SoundWire音频拓扑、扬声器/耳机/麦克风通路配置、低延迟播放修复以及基于`qcom-iris`与`v4l2_codec2`的硬件视频编解码架构。

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

## 四、低延迟音频播放与音游偏移修复（`patches/android/tinyalsa/0001`、`patches/android/hardware-interfaces/0006`、`0011`）

1. **`tinyalsa`周期重算修复（`patches/android/tinyalsa/0001`）**：
   AOSP `libaudiohal@aidl`请求打开`hw:0,1`时传入`period_size=1024, period_count=4`，而内核Audioreach拓扑要求30 ms周期对齐，在`SNDRV_PCM_IOCTL_HW_REFINE`中将`period_size`修正为`1440`（`buffer_size=5760`）。然而`external/tinyalsa_new/src/pcm.c`的`pcm_set_config()`仍用修改前的`1024`计算`sw_params.stop_threshold = 4096`。当环形缓冲区剩余帧数达到`5760 - 4096 = 1664`帧时，ALSA立即误判XRUN并中断流，引发持续爆音与断流。`patches/android/tinyalsa/0001`修正`pcm_set_config()`以内核返回的实际`period_size`计算`buffer_size`与`stop_threshold`。
2. **移除假MMAP输出端口**：
   与输入侧同理，从`audio_policy_configuration.xml`中移除`mmap_no_irq_out`（对应的`StreamOutMmapStub`会把走AAudio MMAP路径的游戏音频静默丢弃）。
3. **播放方向取消墙钟跳帧（`patches/android/hardware-interfaces/0006`）**：
   AOSP示例实现`StreamPrimary::transfer`在播放方向同样维护一条合成墙钟：一旦上层混音线程唤醒稍晚，HAL就打印`skipping transfer`并将整块音频直接丢弃，导致音乐游戏出现严重的节奏漂移与吞音。`patches/android/hardware-interfaces/0006`在播放方向当ALSA proxy就绪时直接调用阻塞式ALSA写入，由声卡硬件时钟自然反压上层混音线程。
4. **播放缓冲只用3个period（`patches/android/hardware-interfaces/0011`）**：
   alsa_utils给内置设备配ALSA缓冲时，period数统一取`ro.hardware.audio.tinyalsa.period_count`（默认4），播放和录音共用。`patches/android/hardware-interfaces/0011`在`openProxyForAttachedDevice()`里只对输出设备改用`ro.vendor.audio.primary.out_period_count`（`device.mk`设3）：块大小不变，只是切成3个period，内核`q6apm-dai`再按480帧向上取整，得到3 × 480帧（30 ms）。录音方向保持4个，因为采集钩子在I/O线程里处理85 ms的块（通话降噪最长约51 ms才回退），这段时间新来的数据要靠录音缓冲接住。播放不能再减到2个：DSP缺数据时会扣着最后一个period不放，ALSA不会报XRUN，FastMixer与轨道的欠载计数也都是0，断音只能在游戏里听出来。

## 五、硬件视频编解码（`qcom-iris` + `v4l2_codec2`）

### 1. 从`qcom-venus`迁移至`qcom-iris`

在Linux 6.18+/7.x主线内核中，高通为Gen2 HFI硬件（含SC8280XP / SM8450 / SM8550）引入了全新的`qcom-iris`驱动（`drivers/media/platform/qcom/iris/`），替代旧版`qcom-venus`。

- **内核配置与固件**：启用`CONFIG_VIDEO_QCOM_IRIS=y`（禁用`CONFIG_VIDEO_QCOM_VENUS`），设备树`video-codec@aa00000`指定固件`qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn`。
- **Android Codec2集成**：
  - 使用`android.hardware.media.c2@1.2-service-v4l2`（通过属性`media.c2.hal.selection=aidl`告知`MediaCodec`连接AIDL/HIDL服务）。
  - 注册解码器组件：`c2.v4l2.avc.decoder`（H.264）、`c2.v4l2.hevc.decoder`（HEVC / H.265）、`c2.v4l2.vp9.decoder`（VP9），最高支持4K60硬件解码。
  - 注册编码器组件：`c2.v4l2.avc.encoder`（H.264）、`c2.v4l2.hevc.encoder`（HEVC Main），见本节第3小节。

### 2. 关键内核与用户态修复

| 补丁 | 位置 | 解决的问题 |
|---|---|---|
| `crdroid-tree-fixes.py` | `external/v4l2_codec2` | `V4L2Device::getDeviceInfos()`原本写死只扫描`/dev/video0..9`；启用相机`camss`后VFE占用了`video0..31`，解码器被推到`video32`，导致硬解静默消失。将扫描上界扩大至64 |
| `patches/android/v4l2-codec2/0001` | `external/v4l2_codec2` | HEVC码流的首个配置缓冲（VPS/SPS/PPS）合并进第一帧DMA-BUF（≥8 MiB），避免固件因缺少`CODECCONFIG`标志在`SOURCE_CHANGE`前等待输出缓冲而死锁 |
| `patches/kernel/iris/0001` | `drivers/media/platform/qcom/iris/` | 引入`core->hw_powered`标志，防止解码器下电后残余中断触发未供电寄存器读取导致内核崩溃 |
| `patches/kernel/iris/0003` | `drivers/media/platform/qcom/iris/` | 允许`v4l2_codec2`在首个`V4L2_EVENT_SOURCE_CHANGE`事件到达前调用`VIDIOC_G_FMT(CAPTURE)`，兼容Android Codec2初始化顺序 |
| `patches/kernel/iris/0005` | `drivers/media/platform/qcom/iris/` | 在非动态分辨率切换（非DRC）的`CAPTURE streamoff`路径发送`HFI_FLUSH_ALL`而非`HFI_FLUSH_OUTPUT`，修复视频拖动进度条（seek）或关闭播放器时固件等待`HFI_FLUSH_OUTPUT`响应超时导致的卡死 |

### 3. 硬件视频编码

硬件编码要内核、`v4l2_codec2`和设备配置三处一起改。只改设备配置、内核却没有CP非像素上下文时，第一次编码就会让整板复位，所以`device.mk`里的编码器属性只能配合本仓的内核补丁使用。

#### 内核侧

`qcom-iris`的gen1编码路径在SC8280XP上要额外满足一条硬件约束：编码器固件通过内容保护非像素流SID `0x2a04`访问它的ARP（auto-register programming）PERSIST缓冲，而这条流只能经由`apps_smmu`上CBAR VMID为`0xb`（CP_NON_PIXEL）的stage-1上下文bank翻译。ARP若留在`iris`主域（SID `0x2a00`）上，第一次编码就会让整板复位。Windows的`qcsmmu8280.sys`从host侧配置这块bank，主线`arm-smmu`没有secure-VMID支持，所以由下游补丁完成：

- **`patches/kernel/iommu/0001`（`arm-smmu-qcom`）**：`cfg_probe`时预留CB15、写CBAR（VMID `0xb`）、加`0x2a04`的SME，成功时打印`cp-nonpixel: CB15 ready`。bank号固定为15，与Windows相同：把`0x2a04`绑到另一个VMID相同的bank时启动挂住。页表经io-pgtable的自定义分配器分配，每个表页用`qcom_scm_assign_mem`赋给`{HLOS RW, 0xb R}`。系统resume的reset会清掉这块bank，SoC的reset钩子随后重新编程它。`0x2a04`不能写进`iris`主节点的`iommus`：那样它会被绑到`iris`的HLOS bank，启动时挂住。
- **`patches/kernel/iris/0013`**：编码器的`BUF_ARP`分配为连续内存，页独占赋给VMID `0xb`（HLOS与`0xb`共享的授予会被hypervisor拒绝），映射进CB15在窗口`0x01000000..0x25800000`内的固定IOVA `0x01000000`，并把该IOVA交给固件；拆除时反向还原，页无法交还HLOS时宁可泄漏也不释放。
- **`patches/kernel/iris/0014`**：gen1编码器的档次/级别改发HFI位掩码值（原来发V4L2枚举，HEVC Main会被固件当成Main Still Picture）；级别按分辨率和帧率推导。同时修正该属性的payload大小（原来少分配4字节）。
- **`patches/kernel/iris/0015`**：编码器EMPTY_BUFFER时间戳按微秒发（固件码率控制按微秒读，发纳秒会让码率严重超标），响应侧转回纳秒。
- **`patches/kernel/iris/0016`**：回移主线v7.3的强制关键帧控制`V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`。`v4l2_codec2`每到关键帧间隔都设这个控件，设不上就判编码失败。
- **`patches/kernel/iris/0017`**：gen1编码器的输入帧尺寸改发原始尺寸、输出帧尺寸改发请求的输出尺寸（`enc_scale`）。原来输入发的是按128x32对齐的缓冲尺寸，宽度不是128的倍数时（竖拍1080x1920），固件把右侧填充当成画面读进来再缩放，画面右侧出现绿边、整体被压窄。步长和行数另由`UNCOMPRESSED_PLANE_ACTUAL_INFO`发给固件；固件把编码图像补齐到整宏块，并在SPS里写明裁剪。gen1没有输入裁剪偏移，比原始尺寸小的裁剪仍会被缩放而不是裁掉，`v4l2_codec2`不会这样设置。
- **`patches/kernel/iris/0018`**：EMPTY_BUFFER包清零。没有开启`CONFIG_INIT_STACK_ALL_ZERO`的内核会把`extradata_buffer`与`data`两个字段的栈上旧值发给固件；本仓内核开启了这个选项，实际不受影响。

#### `v4l2_codec2`侧

- **`patches/android/v4l2-codec2/0004`**：编码器输入块的用途加上`C2MemoryUsage::CPU_READ`。组件要在CPU上映射第一块输入来读平面布局，录屏送来的RGB还要逐帧在CPU上转成NV12；输入块没有CPU读用途时gralloc拒绝lock，相机录像和录屏在第一帧就失败。`C2StreamUsageTuning`里放的是C2的用途位，不能填gralloc常量：gralloc的`SW_READ_OFTEN`（`0x3`）按C2的位读是`READ_PROTECTED`，录屏会一帧都拿不到。
- **`patches/android/v4l2-codec2/0005`**：新增`c2.v4l2.hevc.encoder`，由`ro.vendor.v4l2_codec2.encoder.supported.hevc`把守。只提供Main档次：输入是8位NV12，Main Still Picture一个码流只能有一张图，Main 10要10位输入。级别1到5.2，默认4.1，按尺寸、帧率和码率自动抬到够用的最低一级（H.265表A.8、A.9，以及A.4.1对单边长度的限制）。iris gen1没有`V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR`，参数集由组件缓存后插到每个IRAP图像前：H.264是SPS/PPS（上游已有），HEVC是VPS/SPS/PPS。

#### 设备配置

- `device.mk`：`ro.vendor.v4l2_codec2.encoder.supported.h264`、`.hevc`设为`true`，`ro.vendor.v4l2_codec2.encode_concurrent_instances=1`。VP8/VP9编码iris没有，保持关闭。
- `media_codecs_c2.xml`：两个编码器的`<Encoders>`条目，`concurrent-instances`为1。尺寸和吞吐量上限按4K30申报，实测只做到1080p 30 fps。
- 组件rank是0x80，排在软编（0x200）前面。第二个编码请求在分配组件时被拒（`maximum number of instances reached`，返回资源不足）。`MediaCodec`遇到资源不足会先请资源管理器从优先级更低的进程收回一个硬件编解码器再重试，收不回才试下一个编码器，落到`c2.android.*`软编。因此按框架代码，优先级更高的一方会让正在编码的应用失去编码器（`ERROR_RECLAIMED`），例如前台相机录像时从快捷设置启动系统录屏，尚未经实测；优先级不高于对方时，第二路才会直接换软编。原生进程（例如shell里的`screenrecord`）既不会收回别人的编码器，也不会被收回。硬件编码器配置失败时，`MediaRecorder`同样会换下一个编码器；自己调用`configure`的应用拿到的是错误。
- 验证：`scripts/verify-hw-codec2.sh`检查属性、编码节点属主、`cp-nonpixel: CB15 ready`和组件列表，亮屏时用`screenrecord --codec-name c2.v4l2.avc.encoder`真编一段。

#### 已知限制

- **熄屏时视频硬件会挂起**：屏幕关闭而系统没有休眠时（例如熄屏后继续录像或导出），APSS进入cluster-sleep-0期间视频硬件（VPP/VSP）会在一帧中途挂起，固件看门狗复位硬件。编码表现为丢帧、多出关键帧、提前结束，随后可能出现SMMU context fault；固件因此重启后，之后的编码会话可能一直失败，直到重启系统。解码也会挂起，但只丢一帧，播放时通常察觉不到。亮屏使用（包括相机预览和录像）时cluster sleep很少，没有出现挂起。原因尚未查明，供电档位、时钟、LLCC、页表遍历和固件版本都已排除；用CPU延迟QoS挡住cluster-sleep-0可以避免，但下游驱动没有这样做，目前不采用。
- **一次只有一个编码会话**：ARP在固定IOVA。窗口内的另一处IOVA（`0x10000000`）实测可用，支持并发需要在窗口上做IOVA分配。
- **两个IDR之间还有周期性的I帧**：iris gen1没有`V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE`，`v4l2_codec2`把它和`GOP_SIZE 0`放在同一次`S_EXT_CTRLS`里，整次调用被拒，`GOP_SIZE`停在驱动默认的30。驱动每次开流都把它作为帧内间隔发给固件，于是在`v4l2_codec2`请求的IDR之间每30帧还有一个I帧。码流可以正常解码。
- **没有HEVC Main 10和HDR编码**：输入只有8位NV12。
- **系统录屏会换用软编**：系统录屏按解码器的能力和120 Hz刷新率选尺寸和帧率（横屏约2560x1600@117，竖屏约1360x2176@120），码率约70–96 Mbps，超出H.264 5.1级和组件50 Mbps的码率上限，硬件编码器配置失败，按框架代码会换用软编，尚未经实测。`adb shell screenrecord`不带`--size`时会改用1280x720重试。
- **很小的尺寸**：宽或高按驱动的对齐（128x32）之后仍小于128时（例如160x90），内核在申请缓冲时拒绝（`current session not supported`），应用在开始编码时拿到错误。原始尺寸小于128、对齐后不小于128的（例如160x120、100x100），固件能正常编码，并在SPS里写明裁剪。
- **120x160时不出帧**：`v4l2_codec2`的编码组件在这个尺寸下申请完缓冲就停住，既不开流也不报错，换回ROM原版的库也一样，原因不明；相邻的120x120、120x176、136x160都正常，下一次编码不受影响。
- **唤醒后偶发一次无输出**：休眠唤醒后紧接着编码，偶尔会有一次两个编码器都一帧不出，内核没有任何报错，随后的编码自行恢复。原因不明。
