# gaokun3 扬声器低音处理（speakerfx，音频 HAL 里）

给 Huawei MateBook E Go（gaokun3）的**内置扬声器**补低音：按当前电平下剩下的余量加低音，
不让低音把人声和高频压下去。它跑在**音频 HAL 里**，处理的是 AudioFlinger 混好、马上要写进 ALSA
的那一块数据，所以扬声器放出来的所有声音都经过它：音乐、游戏的低延迟（FAST）音轨、铃声、通知、系统音。

> ★ **默认不开。** 打开方式：「设置 → 声音 → 扬声器增强」打开（`persist.sys.gaokun3.histen`）。
> 同一页的「处理方式」默认就是「低音增强」（`persist.sys.gaokun3.speakerfx.engine`，没设过 = `bass`）；
> 选「华为 Histen」时改由会话音效（effects/）处理，这个库直通。两边由 `etc/histen.rc` 的同一个镜像
> `vendor.gaokun3.speakerfx.engine` 决定谁动手，不会处理两遍。只认内置扬声器的 PCM（card 0 / device 1），
> 耳机、蓝牙、USB 不经过它。

---

## 一、为什么放在 HAL 里

会话音效（effects/gaokun_effect.cpp）覆盖不到游戏。rc.2 起（补丁 0083）主输出有 FastMixer，
AudioFlinger 不许在带 FAST 轨的会话上挂纯软件 effect。实机 Project SEKAI 的音乐和打击音全在 FAST 轨上，日志：

```
W AudioFlinger: checkEffectCompatibility_l: non HW effect Gaokun Histen on playback thread in fast mode
E AudioPolicyEffects: addOutputSessionEffects(): failed to create Fx  Gaokun Histen on session …
```

HAL 拿到的是 FAST 轨也已经混进去的最终结果。在这里处理还有一个好处：
低音处理有自己的 -1 dBFS 天花板，借高 WSA 数字音量 6 dB（见第三节）期间，扬声器放出的每一个声音都受它约束。
如果只有会话音效经过限幅，游戏、铃声这些就会直接响 6 dB。

## 二、组成

| 文件 / 改动 | 说明 |
|---|---|
| `patches/0117-audio-aidl-primary-playback-hook.patch`（`crdroid-tree-fixes.py`） | `StreamPrimary`：开流后按 `ro.vendor.audio.primary.playback_hook` dlopen 一个库，每块播放数据写进 ALSA 之前原地交给它；待机 / 关闭 / 换设备时释放。不设这个属性 = 和原来完全一样 |
| `speakerfx/speakerfx.cpp` → `libgaokunspeakerfx.so` | 钩子的实现：认扬声器 PCM、读开关和旋钮、借音量的补偿、日志。装在 `/vendor/lib64/soundfx`：HAL 在 APEX 里，隔离的链接器命名空间只放行这个 vendor 目录（补丁 0068） |
| `speakerfx/bass_chain.h` | 低音处理本体（DSP），做法与每一部分的理由写在文件头 |
| `speakerfx/bass_chain_coeffs.h` / `gen_bass_coeffs.py` | 谐波生成用的 90 度移相网络系数及其生成器（numpy + scipy；输出确定性，重跑后 diff 应为空） |
| `device.mk` | `PRODUCT_PACKAGES += libgaokunspeakerfx`，`ro.vendor.audio.primary.playback_hook=/vendor/lib64/soundfx/libgaokunspeakerfx.so` |
| `parts/`（`SpeakerFxSettingsActivity`）、`etc/histen.rc` | 「处理方式」：Parts 写 `persist.sys.gaokun3.speakerfx.engine`，init 镜像成 `vendor.gaokun3.speakerfx.engine`（HAL 读不了 `persist.sys.*`，理由同总开关） |
| `effects/gaokun_effect.cpp` | 处理方式不是 Histen 时会话音效保持逐比特直通。它每 16 块重读一次处理方式，speakerfx 每 100 ms 重读一次开关和处理方式：播放中途切换时，两边可能同时处理（或都不处理）不到一秒，响度仍受 speakerfx 的限幅器约束 |
| `bin/audio-route.sh`、`sepolicy/gaokun3_scripts.te` | 设好数字音量基准后写 `vendor.gaokun3.audioroute.spkdig`，speakerfx 借余量以它为准（第三节） |

处理链（48 kHz 立体声）：

```
输入 → 150 Hz LR4 分频
  高频 → 250 Hz 低架 +8 dB ─────────────────────┐
  低频（合成单声道）→ 2–4 次谐波（虚拟低音）─────┴→ 基本组 → 分频段动态 → 串扰消除 → −3 dB
  低架补到 +12 dB 的差值 + 低频（50–150 Hz +14 dB、50 Hz 以下 +10 dB）→ 低音组
输出 = (基本组 + gb × 低音组) × gf，gb ≤ 1 是低音组能拿到的余量，gf 是全频限幅，两者共用 3 ms 预读
```

**没有固定的音色曲线。** 试过 Histen 场景 0、场景 3 的静态曲线和 Windows 整条渲染链的响应
（effects/measurement 的 loopback 数据），分别在 3–5 kHz 或高频动了 5–9 dB，在这对喇叭上都不如不加：
刺耳、尖、或者人声发闷。

## 三、数字音量余量

低音处理的天花板是数字满幅（-1 dBFS），而带 0113–0115 的内核上 WSA 数字音量的基准是 78（-6 dB），
天花板离功放的上限还差 6 dB。所以 speakerfx 处理时自己把混音器借高到 84（单位增益），并在自己这边补偿：

* **什么时候借**：扬声器流打开时，如果低音处理是开着的（开关开、处理方式是低音增强），就把
  `WSA_RX0/RX1 Digital Volume` 设成 84；最后一个借了的流关闭（待机、关闭、换设备）时放回基准。
  开流时、关流时都还没有声音在放，混音器变化听不出来。
* **怎么补偿**：每 100 ms 回读一次混音器，高出基准多少就补偿多少 —— 处理时从 makeup 里减掉，
  不处理的块（播放中途关了开关、或换成 Histen）整体衰减这么多。任何时候响度都和基准一样，
  只是处理时天花板离功放上限近了 6 dB，这段余量给低音。回读失败时沿用上一次的值。
* **播放中途切换**：只改处理，不动混音器，混音器到下一次播放才跟着变。
* **开流时低音处理没开**：混音器留在基准，直通逐比特不变。
* **基准从哪来**：`audio-route.sh` 设好混音器后把基准写进 `vendor.gaokun3.audioroute.spkdig`。
  没有这个属性（脚本还没跑）时 speakerfx 不碰混音器、也不补偿。基准已经是 84 的旧内核（没有端口映射）不借。
* speakerfx 没加载上时，没有谁会去抬混音器。音频 HAL 在借着的时候崩溃，混音器会停在 84，
  下一次开流时 speakerfx 按当时的状态改回来；这期间不播放，所以没有哪个声音因此变响。
* 直接用 `tinyplay` 往扬声器 PCM 写（不经过 HAL）时，如果混音器正借着，会响 6 dB —— 测量请走 HAL。

## 四、旋钮

都在 `persist.vendor.gaokun3.speakerfx.bass.*` 下（`vendor_gaokun3_prop`，要 root），约 1 秒内生效；没设 = 默认值。

| 属性 | 默认（范围） | 作用 |
|---|---|---|
| `…bass.lowboost` | 14 dB（0–24） | 50–150 Hz 真实低频的提升（低音组，受余量控制） |
| `…bass.lowhp` | 50 Hz（`0` = 不分，20–120） | 低频再分一次的位置 |
| `…bass.lowsub` | 10 dB（−99–12，≤ −60 = 去掉） | 上面那个位置以下的提升。喇叭在那里几乎不出声，却会被推着走：数字 84、+18 dB 时 35 Hz 的音符让右喇叭破音，所以给得比上面少 |
| `…bass.trim` | −3 dB（−12–0） | 基本组的固定衰减，给低音组让余量。固定值，人声不会随低音起伏 |
| `…bass.harm` | −9 dB（−99–0，≤ −60 = 关） | 谐波（虚拟低音）电平 |
| `…bass.lc` | 0.15（0–1，`0` = 关） | 响度补偿：节目电平每低于 −15 dBFS 1 dB，真实低频多加多少 dB。按内容电平估计，游戏混音普遍比音乐小声，给多了低音会盖过人声和吉他 |
| `…bass.lcmax` | 12 dB（0–18） | 响度补偿的上限 |
| `…bass.mbc` | `1` | 分频段动态（500 Hz / 4 kHz 三段，1.6:1，±4 dB，相对每段自己 1.5 秒的平均电平） |
| `…bass.xtc` | 0.55（0–0.9，`0` = 关） | 串扰消除强度，只处理侧信号（L−R） |
| `…bass.xtcdist` | 30 cm（15–200） | 听音距离 |
| `…bass.xtclo` | 250 Hz（100–2000） | 串扰消除的下限。听到的"变宽"大多来自 300–600 Hz 的侧信号提升，那里也是左右分轨吉他的主体：调高（450–700 Hz）吉他更干净，变宽也就几乎听不出来 |
| `…bass.makeup` | +4 dB（−20–12） | 链内补偿增益（实际用的是它减去借的数字音量） |
| `vendor.gaokun3.speakerfx.ref` | 空 | `1` = A/B 参考：按低音处理的人声电平原样输出，同样经过限幅器、同样的延迟。不持久 |
| `vendor.gaokun3.audioroute.spkdig` | 开机时由 `audio-route.sh` 写 | 数字音量的基准（第三节）。**不要手动改** |

## 五、验证

```bash
adb shell su -c 'logcat -d -s gaokun_spkfx AHAL_StreamPrimary audioroute' | tail -20
adb shell su -c 'tinymix "WSA_RX0 Digital Volume"'
```

| 判据 | 含义 |
|---|---|
| `playback hook: using /vendor/lib64/soundfx/libgaokunspeakerfx.so`（AHAL_StreamPrimary） | HAL 加载了钩子库（第一次播放时） |
| `speaker stream open: format 0` | 扬声器流开了，库接管了它 |
| `mixer: WSA digital 78 -> 84 (base 78)` / `mixer: WSA digital back to 78` | 开流时借了余量 / 最后一个借了的流关闭、放回基准 |
| `processing (boost 6 dB)` 及其后的 `knobs: …` | 开始处理；`passthrough` = 开关关或处理方式是 Histen |
| `meter: peak in … out …, lc …, bass-gr …, gr …, boost …, ref …, … us/block …` | 每 ~2 秒一行：峰值、响度补偿、低音组 / 全频的最大衰减、借的音量、每块耗时 |
| `tinymix "WSA_RX0 Digital Volume"` | 低音处理正在播放时 84，其余时候 78（基准） |

运算量：电脑上约 160 ns/帧；平板空闲（CPU 低频）时每 10 ms 一块平均 0.54 ms、最多 0.66 ms，约占实时的 5.4%。

## 六、已知限制

1. 满音量播放低音很重的内容时，扬声器一侧有 2–3 dB 的瞬时压缩，处理链的限幅器看不到它。线圈发热不是问题
   （满音量 12 分钟约升温 10–35 K，不累积），所以没有做热保护。
2. 调音只在一台机器上听过。
3. 只在 48 kHz 立体声、S16 / S32 的扬声器流上工作。
4. 延迟多 3 ms（预读）。上报给应用的位置没算这 3 ms，音游里校准一次即可。
5. 开关切换时处理从头开始（状态清零、预读的 3 ms 插入或去掉），播放中切换会有一下轻微的咔声。
