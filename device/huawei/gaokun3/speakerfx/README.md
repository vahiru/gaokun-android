# 内置扬声器低音增强处理库（`speakerfx/`）

本目录实现运行在音频HAL播放钩子中的内置扬声器DSP处理库（`libgaokunspeakerfx.so`）。它在AudioFlinger完成所有音轨（含媒体、系统音及低延迟`FAST`游戏音轨）混音后、写入ALSA PCM之前对48 kHz立体声缓冲做实时低音增强、谐波合成、分频段动态控制与串扰消除。

- **开关与模式协同**：由「设置 → 声音 → 扬声器增强」（`persist.sys.gaokun3.histen`，默认关闭）总控。同一页面的「处理方式」（`persist.sys.gaokun3.speakerfx.engine`）默认为「低音增强」（`bass`），由本库处理；若切换为「华为Histen」（`histen`），则本库保持逐比特直通，改由会话级音效（[`../effects/README.md`](../effects/README.md)）接管。两个属性均由[`etc/histen.rc`](../etc/histen.rc)镜像至`vendor.gaokun3.*`供HAL读取。
- **作用范围**：仅处理内置扬声器（ALSA `card 0` / `device 1`），有线耳机、蓝牙与USB音频一律直通。

## 一、设计背景：为何置于音频HAL层

主输出流开启`FastMixer`后（`patches/android/hardware-interfaces/0008`），AudioFlinger禁止在挂载低延迟`FAST`音轨的会话上启用非硬件加速的软件effect（`checkEffectCompatibility_l`）。使用AAudio/OpenSL ES低延迟通路的音乐游戏（如Project SEKAI）音频均走`FAST`轨，会话级effect无法覆盖。

将处理钩子置于音频HAL的`StreamPrimary`写盘出口有两个优势：
1. 能够覆盖包括`FAST`音轨在内的全部扬声器输出；
2. 借用WSA数字混音器6 dB余量（见第三节）期间，所有发声通道均受同一个`-1 dBFS`前瞻限幅器保护，不会出现系统提示音或游戏音效因未过限幅器而意外暴涨6 dB的问题。

## 二、模块组成与信号处理链

| 文件 / 补丁 | 职责说明 |
|---|---|
| `patches/android/hardware-interfaces/0009-audio-aidl-primary-playback-hook.patch`（及`crdroid-tree-fixes.py`） | 在`StreamPrimary`开流时按`ro.vendor.audio.primary.playback_hook`动态加载处理库，在每块PCM数据写入ALSA前原地调用处理回调 |
| `speakerfx.cpp`（`libgaokunspeakerfx.so`） | 钩子入口：识别内置扬声器PCM、轮询属性开关与调参旋钮、管理WSA数字音量余量借还与增益补偿。安装于`/vendor/lib64/soundfx/`（由`patches/android/hardware-interfaces/0005`向音频HAL APEX链接器命名空间放行） |
| `bass_chain.h` | DSP信号处理核心实现 |
| `bass_chain_coeffs.h`、`gen_bass_coeffs.py` | 虚拟低音谐波生成所用的90°全通移相网络系数及其确定性生成脚本（依赖`numpy`与`scipy`） |
| [`bin/audio-route.sh`](../bin/audio-route.sh) | 开机设置混音器后将扬声器数字音量基准值写入`vendor.gaokun3.audioroute.spkdig` |

48 kHz立体声信号处理拓扑：

```text
输入 ──► 150 Hz LR4分频
          ├─► 高频支路 ──► 250 Hz低架(+8 dB) ─────────────────────────┐
          ├─► 低频支路(单声道) ──► 2–4次谐波合成(虚拟低音) ───────────┴─► 基本组 ──► 三频段动态(MBC) ──► 串扰消除(XTC) ──► -3 dB衰减
          └─► 低架差值(补至+12 dB) + 真实低频(50–150 Hz +14 dB, <50 Hz +10 dB) ──► 低音组
输出 = (基本组 + gb × 低音组) × gf
（gb ≤ 1为低音组可用余量增益，gf为全频段-1 dBFS限幅增益，两者共用3 ms前瞻缓冲）
```

## 三、WSA数字混音器余量借用机制

启用WSA端口映射补丁（`0113`–`0115`）后，内置扬声器的`WSA_RX0/RX1 Digital Volume`基准值为`78`（`-6 dB`）。为给低频瞬态留出充足动态范围，本库在工作期间会临时借用这6 dB余量：

- **借还时机**：扬声器流打开且低音增强处于启用状态时，将`WSA_RX0/RX1 Digital Volume`提升至`84`（`0 dB`单位增益）；当最后一个借用余量的流进入待机或关闭时，恢复为`vendor.gaokun3.audioroute.spkdig`记录的基准值（`78`）。
- **链内增益补偿**：处理库每100 ms回读一次混音器实际值，将超出基准值的增益从链内`makeup`中扣除（若播放中途关闭开关或切换至Histen，则对直通样本施加等量衰减）。因此人声与中高频的主观响度始终与基准状态保持一致，多出的6 dB物理余量仅分配给受限幅器约束的低频峰值。
- **直接调用`tinyplay`时的注意**：若在音频HAL持流借用余量（混音器为`84`）期间绕过HAL直接使用`tinyplay`写PCM，输出电平会比基准高6 dB；声学测量应统一通过Android音频框架播放。

## 四、调试属性（root）

以下属性位于`persist.vendor.gaokun3.speakerfx.bass.*`命名空间下，修改后约1秒内生效（设为空串恢复默认值）：

| 属性名 | 默认值（范围） | 作用说明 |
|---|---|---|
| `...bass.lowboost` | `14` dB（`0`–`24`） | 50–150 Hz真实低频提升量（受余量控制） |
| `...bass.lowhp` | `50` Hz（`20`–`120`，`0`=不分频） | 极低频二次分频拐点 |
| `...bass.lowsub` | `10` dB（`-99`–`12`，`≤-60`=切除） | 二次分频拐点（50 Hz）以下的提升量（限制超低频冲程以防右声道破音） |
| `...bass.trim` | `-3` dB（`-12`–`0`） | 基本组固定衰减量，为低音组预留固定动态余量 |
| `...bass.harm` | `-9` dB（`-99`–`0`，`≤-60`=关闭） | 2–4次谐波（虚拟低音）混入电平 |
| `...bass.lc` | `0.15`（`0`–`1`，`0`=关闭） | 等响度补偿斜率：节目电平每低于`-15 dBFS` 1 dB时额外增加的低频增益（dB） |
| `...bass.lcmax` | `12` dB（`0`–`18`） | 等响度补偿增益上限 |
| `...bass.mbc` | `1`（`0` / `1`） | 三频段动态均衡（500 Hz / 4 kHz分频，压缩比1.6:1，范围±4 dB，时间常数1.5 s） |
| `...bass.xtc` | `0.55`（`0`–`0.9`，`0`=关闭） | 侧信号（L−R）跨声道串扰消除强度 |
| `...bass.xtcdist` | `30` cm（`15`–`200`） | 串扰消除几何模型的听音距离 |
| `...bass.xtclo` | `250` Hz（`100`–`2000`） | 串扰消除作用频段下限 |
| `...bass.makeup` | `+4` dB（`-20`–`12`） | 链内补偿增益（实际生效值会减去借用的混音器数字增益） |
| `vendor.gaokun3.speakerfx.ref` | 空（非持久） | 设为`1`时进入盲测参考模式：输出同响度、同延迟（3 ms）且经同一限幅器的未增强信号 |

## 五、验证与性能开销

```sh
adb shell su -c 'logcat -d -s gaokun_spkfx AHAL_StreamPrimary audioroute' | tail -20
adb shell su -c 'tinymix "WSA_RX0 Digital Volume"'
```

- **关键日志判据**：
  - `playback hook: using /vendor/lib64/soundfx/libgaokunspeakerfx.so`：HAL成功挂载播放钩子；
  - `mixer: WSA digital 78 -> 84 (base 78)` / `back to 78`：开流借用余量与关流归还正常；
  - `processing (boost 6 dB)`及周期性`meter: peak in ... out ...`：处理链正在运行。
- **开销与已知限制**：
  - 处理耗时大致与块长成正比。10 ms块（480帧）、CPU处于低频频点时，每块平均约0.54 ms（峰值约0.66 ms，占实时预算约5.4%）；另一次测量中，5 ms块（240帧）每块平均约0.21 ms（约4.2%），当时未限定CPU频点，两个数字不宜直接比较。
  - 仅作用于48 kHz立体声S16/S32扬声器流，引入3 ms前瞻延迟；播放途中切换开关或处理方式时会有一次性的缓冲重置微弱咔哒声。
