# 内置麦克风阵列与通话回声消除库（`micarray/`）

Huawei MateBook E Go（gaokun3）配备四颗数字麦克风（`DMIC0`–`DMIC3`）。横屏握持时，机身顶部边框从左至右依次为`DMIC1`、`DMIC3`、`DMIC2`（`DMIC0`位于另一侧）。默认双声道录音仅使用`DMIC0`与`DMIC1`。

本目录实现的音频HAL采集钩子库（`libgaokunmicarray.so`）将内置麦克风切换为四路同步采集，利用顶部三颗麦克风（`DMIC1`、`DMIC3`、`DMIC2`）执行频域**固定波束成形（Fixed Beamforming）**，定向增强屏幕正前方人声并抑制环境背景声，同时在通话场景串联WebRTC **AEC3回声消除**与CDSP NPU **DeepFilterNet3降噪**。

- **开关与依赖**：默认启用（可通过`persist.vendor.gaokun3.micarray=0`关闭，下一次开流生效）。运行依赖两项外部条件：
  1. 预置波束成形系数文件（位于[`prebuilt/README.md`](prebuilt/README.md)，不纳入版本控制）；
  2. 支持四路采集的音频拓扑（`audioreach-tplg.bin`）与内核补丁`0203`、`0204`。
  若任一条件缺失，本库自动放弃接管并回退为原有的`DMIC0`/`DMIC1`双路采集。
- **音源过滤**：仅接管内置麦克风（ALSA `card 0` / `device 3`）上的`MIC`、`CAMCORDER`、`VOICE_RECOGNITION`、`VOICE_COMMUNICATION`与`DEFAULT`音源；要求原始信号的`UNPROCESSED`、`VOICE_PERFORMANCE`音源以及外接耳机麦克风均不经过本库处理。

## 一、设计背景：为何置于音频HAL采集层

固定波束成形需要采集**4声道原始PCM输入**并输出**2声道立体声（或单声道）**交给上层应用：
- Android框架的会话级预处理音效（Effect）拿到的数据已经是AudioFlinger按2声道打开的流，无法改变底层ALSA硬件声道数；
- 若仅在`tinyalsa`层做透明包装，则无法获知上层录音轨的`AudioSource`类型，会导致要求无处理的`UNPROCESSED`录音也被强制波束成形。

将钩子置于音频HAL的`StreamPrimary`采集入口，可以在打开ALSA PCM前根据音源决定是否以4声道打开硬件，并在运行期响应录音轨音源的动态增减。

## 二、模块组成

| 文件 / 补丁 | 职责说明 |
|---|---|
| `patches/android/hardware-interfaces/0010-audio-aidl-primary-capture-hook.patch`（及`crdroid-tree-fixes.py`） | 在`StreamPrimary`采集侧引入厂商钩子：按`ro.vendor.audio.primary.capture_hook`加载库，由钩子决定ALSA采集声道数、转换PCM数据块、接收音源变更通知并修正采集时间戳延迟 |
| `micarray.cpp`（`libgaokunmicarray.so`） | 钩子主入口：识别内置麦克风PCM、按音源切换处理模式、动态配置VA混音器、串联AEC3与NPU降噪客户端。安装于`/vendor/lib64/soundfx/` |
| `beamformer.{h,cpp}` | 48 kHz频域重叠相加（STFT）固定波束成形实现 |
| [`prebuilt/`](prebuilt/README.md) | 存放从华为Windows驱动提取的波束成形复数权重文件（不入库，安装至`/vendor/etc/micarray/`） |
| `tplg-four-channel-capture.py` | 将`audioreach-tplg.bin`中内置麦克风（`MultiMedia4 Capture`）的声道上限从`2`修改为`4` |
| `patches/kernel/audio/0011`、`0012`（内核） | `0011`使VA后端在前端请求4声道时保留4路；`0012`按源顺序标记采集声道映射，防止ADSP媒体格式转换模块（MFC）按扬声器声场重混四路麦克风信号 |
| `denoise_protocol.h`、[`../denoise/`](../denoise/README.md) | 采集钩子与NPU降噪守护进程之间的Unix域套接字（`SOCK_SEQPACKET`）协议与服务端实现 |
| [`../etc/denoise.rc`](../etc/denoise.rc)、`NoiseSuppressionSettingsActivity` | 将「设置 → 声音 → 麦克风降噪」的各场景开关镜像至`vendor.gaokun3.denoise.*` |

## 三、频域固定波束成形算法

采样率固定为48 kHz，帧移（hop size）为480帧（10 ms）：

1. **分帧加窗与FFT**：取`DMIC1`、`DMIC3`、`DMIC2`三路各自最近1440帧（30 ms）样本，乘以正弦分析窗$\sin(\pi(n+1)/1441)$，末尾补零后执行2048点实数FFT；
2. **频域复数加权求和**：在频点$k \in [0, 1023]$（Nyquist频点置零）上，将三路复数频谱与各自的复数权重矩阵相乘求和，得到左右两路输出频谱；
3. **逆FFT与重叠相加（OLA）**：执行2048点逆FFT，乘以满足完全重构条件（$\sum w_a^2 w_s = 1$）的合成窗，重叠相加后每跳输出480帧双声道样本。

算法特性：
- **权重来源**：取自华为Windows驱动录音组件（`hdrec_bf_data.bin`）中的第一组3输入2输出复数权重矩阵。
- **固定算法延迟**：分析窗需累积960帧历史样本，加上480帧分块，总延迟为**1440帧（30 ms）**；HAL据此向框架修正采集位置时间戳。

## 四、音源切换与混音器管理

### 1. 音源路由策略
- **开流判定**：当所有活动录音轨的`AudioSource`均属于受支持音源时，以4声道打开ALSA并启用波束成形；若存在任一要求原始信号的音源（如`UNPROCESSED`），则直接以原生2声道打开，本库不介入。
- **流内动态切换**：若4声道录音进行中途有`UNPROCESSED`音轨加入同一会话，输出通过10 ms交叉淡化平滑切换至延迟对齐（30 ms）的`DMIC0`/`DMIC1`原始双声道；该音轨退出后再经10 ms交叉淡化切回波束成形输出。
- **语音通话单声道合成**：当活动音轨全部为`VOICE_COMMUNICATION`时，将波束成形左右两路同相输出取平均并写入双声道，进一步压低两路间不相关的本底噪声，随后送入AEC3与NPU降噪。

### 2. VA混音器动态接管
开机时[`bin/audio-route.sh`](../bin/audio-route.sh)默认仅将VA抽取器`DEC0`、`DEC1`连接至`DMIC0`、`DMIC1`。
- **开启4声道采集前**：本库先核实`DEC0`/`DEC1`路由正常，随后将`DEC2`、`DEC3`路由至`DMIC2`、`DMIC3`（`DMIC3`时钟依赖`DMIC2`同时开启），匹配`DEC1`数字增益，并将`VA_AIF1_CAP Mixer DEC2/DEC3`置为`1`。
- **关闭最后一条阵列流时**：显式将`VA_AIF1_CAP Mixer DEC2/DEC3`置为`0`（并在普通2声道开流前主动清理可能因前次HAL异常退出而残留的`DEC2`/`DEC3`混音器开关），确保2声道录音行为与未加载本库时完全一致。

## 五、音频拓扑准备（构建机）

在构建机同步固件目录后，需对`audioreach-tplg.bin`执行一次性的4声道上限补丁：

```sh
python3 device/huawei/gaokun3/micarray/tplg-four-channel-capture.py \
    device/huawei/gaokun3/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin
```

该脚本校验原始拓扑的SHA-256（`ee911c74...`），将`MultiMedia4 Capture`的声道上限字节由`2`改为`4`并校验输出SHA-256（`8f5f040b...`），支持幂等执行。`scripts/sync-device-tree.sh`会在同步设备树时校验构建机上的拓扑是否已打好该补丁。

## 六、通话回声消除（AEC3）与NPU降噪串联

### 1. 通话回声消除（WebRTC AEC3）
由于本机音频HAL未实现`IStreamCommon::addEffect`接口，AOSP框架层挂载的软件预处理AEC无法收到音频流。为此，通话流（`VOICE_COMMUNICATION`）的回声消除直接集成在本库内：
- **处理管线**：`4路DMIC → 固定波束成形 → 单声道合成 → WebRTC AEC3 → DeepFilterNet3 NPU降噪`。必须先执行线性/自适应回声消除，再送入非线性的神经网络降噪。
- **远端参考信号获取**：扬声器处理库[`speakerfx/`](../speakerfx/README.md)在每次向ALSA写入最终播放块时，同步将数据写入无锁环形缓冲（`speakerfx/echo_reference.h`）。本库在通话开流时通过`dlopen`获取该环形缓冲指针，在每块采集数据送入AEC3前按10 ms帧长先行喂入同一时间窗内的扬声器参考信号。
- **开关与延迟**：可通过`persist.vendor.gaokun3.aec=0`关闭；AEC3分频滤波器组引入约**430帧（约9 ms）**额外延迟。

### 2. DeepFilterNet3 NPU降噪对接
波束成形（及通话AEC3）输出可按场景送入[`denoise/`](../denoise/README.md)守护进程在CDSP HTP上执行DeepFilterNet3降噪：

| 场景属性（`vendor.gaokun3.denoise.*`） | 对应`AudioSource` | 默认状态 | 信号流向 |
|---|---|---|---|
| `...denoise.call` | `VOICE_COMMUNICATION` | 开启（非`0`即开） | 波束成形 → 单声道 → AEC3 → NPU单路降噪（总延迟3310帧） |
| `...denoise.record` | `MIC`、`DEFAULT` | 关闭（`1`为开） | 波束成形 → NPU双路独立降噪（总延迟2880帧） |
| `...denoise.camcorder` | `CAMCORDER` | 关闭（`1`为开） | 波束成形 → NPU双路独立降噪（总延迟2880帧） |

- `VOICE_RECOGNITION`遵循Android CDD 5.4.2要求，始终不启用降噪。
- **超时与故障回退**：采集钩子通过`/dev/socket/gk3denoise`与降噪服务通信。若开流时服务未就绪，或者录音中途单次IPC往返超过当前音频块时长的60%（4096帧块下约51 ms），本库立即无缝切回波束成形（含AEC3）直出，绝不阻塞音频HAL的I/O线程。

## 七、日志验证与已知限制

```sh
adb logcat -d -s gaokun_micarray AHAL_StreamPrimary
adb shell su -c 'cat /proc/asound/card0/pcm3c/sub0/hw_params'
```

- **关键日志判据**：
  - `weights: ... loaded`：波束成形系数加载成功；
  - `open: sources 1 -- array (DMIC1,3,2), 1440 frames delay`：普通录音已启用三麦阵列；
  - `open: sources 7 -- array, mono, echo cancellation, noise suppression (1 ch), 3310 frames delay`：语音通话已完整启用「阵列 + AEC3 + NPU降噪」管线；
  - 录音期间`/proc/asound/card0/pcm3c/sub0/hw_params`显示`channels: 4`。
- **已知限制**：
  1. 波束成形权重针对屏幕正前方固定优化，未包含自适应旁瓣对消（GSC），且输出左右声道并非物理左右麦克风的直接对应；
  2. 在50–190 Hz低频段，白噪声增益（White Noise Gain）较单颗麦克风高7–12.5 dB，强风噪或手指直接摩擦顶部机身时的低频噪声会被放大；
  3. 仅处理48 kHz 16-bit采集流（其他采样率由AudioFlinger重采样后送入）。
