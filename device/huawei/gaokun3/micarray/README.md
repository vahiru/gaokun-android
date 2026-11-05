# gaokun3 内置麦克风阵列（micarray，音频HAL里）

Huawei MateBook E Go（gaokun3）有四颗数字麦克风（DMIC0–3）。横屏、屏幕朝向自己时，顶部从左到右是
DMIC1、DMIC3、DMIC2，DMIC0在别处。原来的录音只用DMIC0和DMIC1两颗，各占一个声道。

这个库让内置麦改成四路采集，用顶部三颗做**固定波束成形**（按频率给每颗麦克风一组固定的复数权重再相加，
让前方的声音叠加、其他方向的声音相互抵消一部分），输出双声道交给应用。人声更集中，房间里的背景声更小。
代价是**30毫秒延迟**和一些CPU时间：在音频HAL里实际录音时，每85毫秒的一块要处理7–8毫秒（CPU此时多在低频）；连续跑满时每块约1.4毫秒。

> ★ **默认打开**。系数文件不在版本库里，见[`prebuilt/README.md`](prebuilt/README.md)；拓扑要在构建机上改成允许四路，见第六节。
> 正式构建一定带着这两样（`sync-device-tree.sh`会核对），缺一样这个库就不接管录音。
> 关掉：`persist.vendor.gaokun3.micarray=0`，从下一次开始录音起生效。
> 只处理内置麦（card 0 / device 3）的MIC、CAMCORDER、VOICE_RECOGNITION、VOICE_COMMUNICATION和DEFAULT音源；
> UNPROCESSED、VOICE_PERFORMANCE等仍是原来的DMIC0/DMIC1，耳机麦不经过它。
> 通话默认还会经过降噪（DeepFilterNet3，在NPU上，排在本库的回声消除之后），录音、录像可以在「设置 → 声音 → 麦克风降噪」里打开，见第七节。

---

## 一、为什么放在HAL里

阵列要**四路原始输入**、交给应用**两路**。会话音效拿到的已经是两路，改不了ALSA开几路；
包装tinyalsa能改声道数，却看不到录音轨的音源，`UNPROCESSED`（应用要求原始录音）也只能跟着一起处理。
HAL两样都有：开ALSA前知道这条流的音源，开几路由它定。

## 二、组成

| 文件/改动 | 说明 |
|---|---|
| `patches/0206-audio-aidl-primary-capture-hook.patch`（`crdroid-tree-fixes.py`） | `StreamPrimary`采集方向的厂商钩子：开ALSA前按`ro.vendor.audio.primary.capture_hook` dlopen一个库，库决定硬件声道数；读到的每块进管道之前交给它转换；音源（录音轨的AudioSource）变化时通知它；采集位置的时间扣掉它报的延迟。不设这个属性 = 和原来完全一样 |
| `micarray/micarray.cpp` → `libgaokunmicarray.so` | 钩子的实现：认内置麦、按音源决定处理还是原样、设混音器、日志。装在`/vendor/lib64/soundfx`：HAL在APEX里，隔离的链接器命名空间只放行这个vendor目录（补丁0068） |
| `micarray/beamformer.{h,cpp}` | 波束成形本体（第三节） |
| `micarray/prebuilt/` | 系数文件（不入库），`device.mk`有就装到`/vendor/etc/micarray/` |
| `patches/0203`、`0204`（内核） | 0203：VA后端在前端要四路时保留四路（原来一律压成两路）。0204：采集按源的顺序标声道，否则DSP的声道转换模块（MFC）会把四路按音箱布局重混 |
| `micarray/tplg-four-channel-capture.py` | 音频拓扑里内置麦PCM（MultiMedia4 Capture）的声道上限从2改成4，一个字节（第六节） |
| `device.mk` | `PRODUCT_PACKAGES += libgaokunmicarray`，`ro.vendor.audio.primary.capture_hook=/vendor/lib64/soundfx/libgaokunmicarray.so` |
| `micarray/denoise_protocol.h` | 钩子和降噪服务之间的协议（第七节） |
| `denoise/`、`denoise/prebuilt/` | 降噪服务：在CDSP的NPU上跑DeepFilterNet3；它要的高通文件不入库，构建时由`scripts/fetch-denoise-blobs.py`取 |
| `patches/0205`（内核） | CDSP的FastRPC上下文标成dma-coherent，否则降噪服务在CDSP上加载几乎每次都失败（与时序有关） |
| `etc/denoise.rc`、Parts的`NoiseSuppressionSettingsActivity` | 降噪的场景开关：Parts写`persist.sys.gaokun3.denoise.*`，rc镜像成钩子读的`vendor.gaokun3.denoise.*` |

## 三、处理

48 kHz，每480帧（10毫秒）一跳：

1. 三路输入各取最近1440帧，乘分析窗，补零做2048点FFT；
2. 每个频点（0到1023，Nyquist置零）把三路复数谱乘各自的权重相加，得到两路输出谱；
3. 逆FFT，乘合成窗，重叠相加，每跳交出480帧。

- 权重来自华为Windows驱动的录音音效（`hdrec_bf_data.bin`）。文件里有两组3输入2输出的矩阵，用的是第一组。
  文件格式和Windows侧的处理顺序是从驱动静态分析恢复的，华为没有公开的说明。
- 分析窗是`sin(π(n+1)/1441)`，合成窗是它除以在同一点重叠的三个分析窗的平方和，两者相乘、重叠后加起来恒为1。
  这两个窗和Windows驱动里的表一致（误差在float精度内），这里按公式算，不带华为的表。
- 延迟1440帧：分析窗要攒满960帧历史，再加上按跳分块的480帧。HAL按这个数把采集位置的时间往前挪。
- 阵列本身不做自动增益、降噪、回声消除，也不做Windows那条链里的自适应部分；降噪是后面另一步（第七节）。

## 四、音源与切换

- **开流时**（HAL每次开始录音）：音源全是第一段列出的那几种，库才接管；有一条要原始录音的音源就不接管，这次录音按原来的两路开。
  这次录音的声道数和延迟在开流时就定了，所以以原始方式开的流，直到下一次开始录音前都保持原样。
- **录音中途**有要原始录音的轨加进来：输出改成DMIC0/DMIC1，
  也就是原来两路录音的内容，同样延迟30毫秒，免得时间轴跳动；用10毫秒交叉淡化，不出咔哒声。那条轨走了再换回阵列。
  波束成形一直在算，换回时没有重新起步的过程。AudioPolicy只把同一会话的轨放进已经开着的输入，别的应用用UNPROCESSED
  录音时拿到的是另一条输入流（本机只有一路采集，那条流起不来，没有这个库时也一样），所以这只在一个应用自己的会话里发生。
- **只有通话**（录音轨全是VOICE_COMMUNICATION）：输出两路的平均，左右写同一份。通话本来就是单声道，
  多数通话应用录单声道时，AudioFlinger做的也是这个平均。对正前方的声音两路几乎同相（16 kHz以下相差15°以内），
  平均不会抵消；两路之间不相关的噪声反而降低。通话和其他录音混在一起时保持双声道；中途切换同样淡化10毫秒。
- 轨全走了（列表为空）：保持当前状态，马上就要停了。

## 五、混音器

内置麦的路由由`bin/audio-route.sh`在开机时设好：VA解码器0、1分别接DMIC0、DMIC1。阵列接管一条流时，库先确认这两路确实这样接着
（否则不接管），再在开ALSA之前把解码器2、3接上DMIC2、DMIC3并接进采集，增益跟解码器1一样；最后一条阵列流关闭时把2、3从采集里拿掉。
这样其他录音和没有这个库时完全一样。DMIC3只有和DMIC2一起开时才有时钟，所以两者总是一起开。

拿掉是直接把`VA_AIF1_CAP Mixer DEC2/DEC3`写成0，不是恢复打开前存下的值：混音器的状态比HAL进程活得久，
HAL在阵列录音中途崩溃重启后，存下的“原值”就成了“接着”。同样的原因，没有阵列流时每次打开内置麦，库都会先检查2、3有没有残留，
有就拿掉。解码器2、3的路由和增益在没接进采集时不起作用，关闭时按打开前的值放回。

## 六、内核与拓扑

四路要三样都在：

1. 内核0203、0204；
2. 拓扑里MultiMedia4 Capture的声道上限是4。拓扑文件`firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin`
   和audioreach-topology（fe7d4c4）里ThinkPad X13s的预编译拓扑`prebuilt/qcom/sc8280xp/LENOVO/21BX/audioreach-tplg.bin`逐字节相同，
   对应源码是同一目录的`audioreach.conf`。`firmware/`不入库、`sync-device-tree.sh`也不同步它，所以要在**构建机**的树里
   改（换机器或更新固件后也一样），跑一次：

   ```bash
   python3 device/huawei/gaokun3/micarray/tplg-four-channel-capture.py \
       device/huawei/gaokun3/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin
   ```

   脚本只认原来那份文件（sha256 `ee911c74…`），改完核对结果（`8f5f040b…`），已经改过的不再动。
   `sync-device-tree.sh`第3b1b步核对构建机上的这个文件，不是四路那份就停下（`GK3_ALLOW_NO_MICARRAY=1`时只提示）。
   `device.mk`装进vendor和ramdisk的几个文件名都来自这一个源文件，改一次就够；
3. 系数文件。

拓扑没改时四路打不开，HAL会关掉钩子、按原来的两路重开，录音照常，`logcat`里有一行
`AHAL_StreamPrimary: start: the device does not open with 4 channels, capturing without the hook`。
**内核缺0203或0204时四路照样能打开**，只是声道已经被DSP重混过（缺0204时第三路混进前两路、第四路是零），
钩子发现不了，处理出来的声音不对。所以改过的拓扑必须和这两个内核补丁一起发布。

## 七、降噪

阵列后面可以再接一步降噪：DeepFilterNet3（MIT/Apache-2.0），由降噪服务（`denoise/`）在CDSP的NPU上跑。场景和开关
（`vendor.gaokun3.denoise.<场景>`，由Parts的设置经`etc/denoise.rc`镜像过来）：

| 场景 | 音源 | 默认 | 在哪里降噪 |
|---|---|---|---|
| `call` | VOICE_COMMUNICATION | 开（只有`0`算关） | 本库，在本库的回声消除之后（第八节） |
| `record` | MIC、DEFAULT | 关（只有`1`算开） | 本库（HAL里），开流时读开关 |
| `camcorder` | CAMCORDER | 关（只有`1`算开） | 本库（HAL里），开流时读开关 |

- **通话先消回声再降噪**：DeepFilterNet3是非线性的，放在回声消除前面会让它跟不上回声路径。系统给VOICE_COMMUNICATION
  接的回声消除在这个HAL上根本拿不到音频（第八节），所以回声消除也在本库里做，顺序是阵列 → 单声道 → AEC3 → DeepFilterNet3。
- VOICE_RECOGNITION永远不降噪：CDD 5.4.2 [C-1-2]要求这个音源默认关掉一切降噪处理。录音和录像默认关，
  因为降噪会把人声以外的声音（音乐、环境声）一起压低。
- **哪些录音降噪**：开流时所有音源都属于打开了开关的场景，而且降噪服务在开流时应答。
- **怎么接**：钩子自己按跳（480帧）收集输入，每跳过波束成形，一块里新出来的跳（4096帧一块时8或9跳）一次发给服务，
  拿回同样多跳的降噪结果，再按跳交出，和不降噪时同样的分帧。通话流发一路（回声消除后的单声道），其余两路各自降噪。
- **延迟**：再加1440帧（30毫秒，正好3跳），整条流2880帧（通话流还有回声消除的430帧，共3310帧），开流时定下，
  HAL按它挪采集时间。
- **中途切换**：有音源不在打开的场景里了（例如录音时来了通话），10毫秒淡到阵列的输出；
  阵列的输出这时也晚3跳交出，时间轴不跳。回来时再淡回。
- **服务出问题**：开流时服务不在或不应答，这条流不降噪，延迟按1440帧报（通话流1870帧）。录音中途服务报错、断开、或者在这一块时长的
  60%内没回（4096帧一块时51 ms），这条流余下的部分都用阵列的输出；淡化没走完就到了缺结果的那一跳时，在那里直接切过去。
  钩子只在I/O线程里等这么久，HAL不会因为服务卡住而停住。
- **协议**：`denoise_protocol.h`，每条流一条`SOCK_SEQPACKET`连接（`/dev/socket/gk3denoise`）。

## 八、回声消除（通话）

**为什么在这里做**：AudioFlinger不在录音线程上跑预处理音效，而是交给HAL（`IStreamCommon::addEffect`），我们的HAL不支持，
所以系统配的AEC/NS只会被创建、拿不到任何音频（rc.3上核实：`E EffectChain: Error when adding effect: -38`，
两个音效线程的CPU时间为0）。AOSP那份AIDL的AEC还拿采集信号自己当参考，即使拿到音频也消不掉扬声器的回声。
应用却会看到“有AEC”而关掉自己的。所以通话的回声消除在本库里做。

- **算法**：WebRTC的AEC3（`echo_canceller.cpp`，AudioProcessing只开回声消除，`mobile_mode`关，也就是不用AECM），
  用ROM树里AOSP自带的`webrtc_audio_processing`（external/webrtc，android-17.0.0_r1；设备树能链它靠`crdroid-tree-fixes.py`）。
- **远端参考**：扬声器的处理库speakerfx把每一块交回HAL的数据（它自己处理完、ALSA拿去播的那份）抄进一个无锁环形缓冲
  （`speakerfx/echo_reference.h`），不加锁、不等待。本库在通话流开流时（HAL的工作线程上）用同一个路径`dlopen`speakerfx拿到这个缓冲，HAL还没加载过它时就由这里先加载，
  句柄一直留着；I/O线程上不查找。找不到时这条流不做回声消除。
  每块采集处理之前，先把这段时间扬声器播过的都按10毫秒一帧送进AEC3，再处理这块的采集，所以参考总在回声之前。
  耳机、蓝牙时扬声器流不动，AEC3收不到参考，就不做事。
- **哪些流**：开流时全是通话的流。它从阵列两路的平均（单声道）进AEC3，之后整条流都是消过回声的单声道，中途有别的音源
  加入也一样；只有要原始录音的轨加入时，像别的流一样换成DMIC0/DMIC1（第四节）。`persist.vendor.gaokun3.aec=0`关掉，下次开始录音起生效。
- **延迟**：AEC3的分频滤波器让采集晚430帧（约9毫秒，冲激测得），开流时加到流的延迟里。
- **电脑上的测试**（gaokun-workspace的experiments/denoise-hal-20261103/hook-host）：远端经过60毫秒、T60 150毫秒的响应
  回到四颗麦克风，过阵列再进AEC3：只有远端时回声压低43 dB，只有近端时人声只低0.9 dB。
- **实机验证**：媒体音量13/15时回声比底噪高约20 dB，开AEC3后压到底噪（约19.5 dB）；
  QQ外放通话盲测中，开启回声消除后对方听到的回声明显减少。双讲场景尚未单独实测。

## 九、日志与排查

`logcat -s gaokun_micarray AHAL_StreamPrimary`：

| 日志 | 意思 |
|---|---|
| `weights: … loaded` | 第一次接管时读到了系数文件 |
| `weights: …: <原因> -- array off` | 系数文件没有或校验不过，以后都不接管（HAL重启后再读） |
| `mixer: decimators 2 and 3 = DMIC2 and DMIC3 at gain N, added` | 开流时接上了解码器2、3 |
| `mixer: decimators 0 and 1 are not routed from DMIC0 and DMIC1 -- not using the array` | `audio-route.sh`的路由不在（还没跑完或被改过），这次不接管 |
| `open: sources 1 -- array (DMIC1,3,2), 1440 frames delay` | 接管了这条流（音源编号是AudioSource的值：1 MIC、5 CAMCORDER、6 VOICE_RECOGNITION、7 VOICE_COMMUNICATION、9 UNPROCESSED、10 VOICE_PERFORMANCE） |
| `open: sources 9 -- raw` | 有要原始录音的音源，这次不接管 |
| `sources 1,9 -- raw pair, delayed` / `sources 7 -- array, mono` / `sources 1 -- array` | 录音中途切换 |
| `close: N frames in M blocks, … us a block on average, … us at most` | 一次录音结束，附处理耗时 |
| `mixer: decimators 2 and 3 taken out` | 最后一条阵列流关闭，混音器恢复 |
| `mixer: decimators 2 and 3 were left in (HAL restarted?) -- taken out` | 上一个HAL进程在阵列录音中途退出，残留已拿掉 |
| `open: … , noise suppression (2 ch), 2880 frames delay` | 这条流降噪（通话是`echo cancellation, noise suppression (1 ch), 3310 frames delay`） |
| `open: sources 7 -- … , echo cancellation, …` / `no echo cancellation: <原因>` | 通话流的回声消除开了 / 没开 |
| `close: echo cancellation, N speaker frames (M skips), delay estimate D ms` | 一次通话结束：收到多少参考、AEC3估计的回声延迟 |
| `open: … , no noise suppression: <原因>, 1440 frames delay` | 场景开着，但服务不在或不应答（通话流1870 frames） |
| `denoise: <原因> after N ms -- the array's output for the rest of the stream` | 录音中途服务出了问题，改用阵列的输出 |
| `close: noise suppression to the end / dropped, N messages, … ms a round trip …` | 一次降噪录音结束，附往返耗时 |

录音时`cat /proc/asound/card0/pcm3c/sub0/hw_params`里`channels: 4`说明阵列在用，`2`说明没接管。`channels: 4`不能说明声道顺序对，那要看内核有没有0204。

## 十、限制

- 权重是固定的，按华为的设计针对屏幕前方的人；从别的方向来的声音变成什么样，没有逐个方向测过。
- DMIC0不参与阵列；阵列输出的左右声道不是左右两颗麦克风，立体声定位和原始录音不同。
- 30毫秒延迟对通话、语音识别有没有影响没有专门测过；对延迟敏感的用途（`VOICE_PERFORMANCE`）不经过它。
- 低频代价：约50–190 Hz，各颗麦克风互不相关的噪声（本底噪声、风声、手碰机身）经过权重后比单颗麦克风大7–12.5 dB，而同相的声音在这一段低2–3 dB（按权重计算）。风大或手持摩擦时低频会比原始录音明显，也更容易削波；实际录音里没有专门测过。
- HAL只接受48 kHz、双声道、16位这一种内置麦流（`primary_audio_policy_configuration.xml`），其余采样率和声道数由AudioFlinger转换，库只认这一种。
