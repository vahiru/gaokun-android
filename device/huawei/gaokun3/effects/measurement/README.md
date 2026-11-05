# `audio-measurement`：与Windows的频响对照测量（脚本与数据）

本目录包含扬声器处理链的测量脚本与Windows对照测量数据（不含华为闭源二进制与`sws_config.xml`），用于复查和复现「Windows侧响度更高且低频更足」「整体增益差约12 dB」「116–145 Hz凹陷与185 Hz峰值」等测量结论。音频效果链实现详见[`../README.md`](../README.md)与[`../../../../../docs/dev/audio-video.md`](../../../../../docs/dev/audio-video.md)。

---

## 一、两种测量域

| 测量域 | 测量对象 | Windows侧 | Android侧 |
|---|---|---|---|
| **数字域**（主力） | 输入wav→输出wav的传递函数`H(f)`，不含房间、麦克风与扬声器物理声学特性 | `windows/loopback_rec.py`：WASAPI loopback抓取渲染端点（全部APO处理后的数字信号）；`windows/digital_response.py`：噪声激励+Welch平均生成1/12倍频程曲线 | `android/eqscan.c`：设备端离线运行Histen引擎，对数扫频20 Hz–20 kHz逐个EQ配置输出wav；`android/eqscan_report.py`：STFT对角采样提取曲线 |
| **声学域** | 扬声器播放+麦克风回采的声压响应 | 外录双机位或内置麦克风回采 | `android/audio_goertzel.py`：内置麦克风回采440 Hz单音，Goertzel提取基波（含PA爆音掐头去尾、削顶检查与信噪比诊断） |

主力采用数字域的原因：两侧均消除了房间反射、麦克风频响与扬声器物理差异，差值直接对应数字信号处理链的差异；声学域用于测定绝对响度差并与数字域结果交叉印证。

---

## 二、目录结构

```text
measurement/
├── windows/loopback_rec.py            # WASAPI loopback采集（soundcard库，48 kHz / 2 ch）
├── windows/digital_response.py        # 噪声 + Welch -> 1/12 oct传递函数（--csv）
├── android/eqscan.c                   # 设备端离线扫频Histen引擎（逐EQ配置输出wav）
├── android/eqscan_report.py           # eqscan输出 -> 槽/频段/增益斜率表 + 曲线CSV
├── android/audio_goertzel.py          # 声学域单点A/B测量（含--make-tone / --selftest）
├── android/analyze-noise-response.py  # 噪声宽带频段表（声学/离线通用）
├── analysis/win_vs_android.py         # 两侧曲线对比与带约束的EQ求解器
└── data/                              # 实测基线数据（scene 3 = SWS_SPK_LANDSCAPE_TWO）
    ├── win-digital-response.csv       # Windows APO链数字域曲线
    ├── eqscan-curves.csv              # Android Histen数字域：基线 + 每槽0/64/128/192差值曲线
    ├── base_eq.txt                    # eqscan输出的基线槽值与实测频带中心
    └── win-freq-response.csv          # Windows扬声器端声学频响（92个1/12 oct采样点）
```

---

## 三、复现步骤

### Windows侧（数字域）

```powershell
pip install soundcard numpy
# 1) 播放48 kHz平谱噪声激励并由loopback抓取：
python windows/loopback_rec.py noise-flat.wav lb-noise-win.wav 20 3
# 2) 计算传递函数曲线：
python windows/digital_response.py noise-flat.wav lb-noise-win.wav --csv win-digital-response.csv
```

### Android侧（数字域）

```bash
# 使用NDK clang交叉编译eqscan.c：
$NDK/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android34-clang -O2 android/eqscan.c -I.. -o eqscan -ldl
adb push eqscan /data/local/tmp/
adb shell /data/local/tmp/eqscan /data/local/tmp/eqscan-out 3   # 场景3 (SWS_SPK_LANDSCAPE_TWO)
adb pull /data/local/tmp/eqscan-out ./eqscan-out
python3 android/eqscan_report.py eqscan-out                     # 生成eqscan_curves.csv
```

### 曲线对比与EQ求解

```bash
# 部署配置对比（HPF 150 Hz）：
python3 analysis/win_vs_android.py --win data/win-digital-response.csv \
    --and data/eqscan-curves.csv --base data/base_eq.txt --hpf 150

# 调整HPF时检查低频净抬升（评估扬声器振膜行程余量）：
python3 analysis/win_vs_android.py --win data/win-digital-response.csv \
    --and data/eqscan-curves.csv --base data/base_eq.txt --hpf 90 --hpf-scan 90 --g-scan 0 \
    --low-lift "1=180 2=50 3=60 4=75 5=50 6=70 7=160 8=100" --low-lift-hpf 150
```

依赖：`python3`、`numpy`（求解器另需`scipy`，Windows侧采集需`soundcard`）。

---

## 四、关键测量结论

1. **整体增益差约`+12 dB`**：数字域实测`@1 kHz`相差`+12.27 dB`，声学域双机位外录实测相差`+11.6 dB`，两者吻合，证实未加载音效插件时Android侧响度偏低源于缺少Windows侧软件音效链与功放增益配置。
2. **频响形状差异**：Windows APO链在116–145 Hz存在凹陷、185 Hz存在峰值、590 Hz与3300 Hz存在窄谷；而Histen引擎默认场景表在110–230 Hz为`+13`至`+15 dB`平台，需配合后置LR4高通滤波器（HPF）与EQ槽位联合整形。
3. **EQ槽位标定（`eqscan`，`R² >= 0.999`）**：场景3下槽`1..8`有效，中心频率依次为`120 / 220 / 560 / 1100 / 2200 / 3700 / 4600 / 14000 Hz`，槽`0/9/10`无响应；参数按无符号整数解析，`1`单位约等于`0.0801 dB`，`>= 204`会被`Init`拒绝（返回`-145`）。
4. **HPF截止频率与失真保护**：
   - `win_vs_android.py`求解器仅评估`--grid-lo`（默认90 Hz）以上的频响残差，不包含振膜非线性失真（THD）约束。
   - 实测表明，若将HPF从150 Hz降至90 Hz，40–60 Hz频段净抬升达`+12.6`至`+15.5 dB`，显著增加微型扬声器冲程与破音风险；而在150 Hz LR4高通下，150 Hz处的THD从`-2.7 dB`降至`-26.8 dB`。因此ROM默认部署保持`HPF = 150 Hz`、`makeup = +4 dB`、WSA883x `PA Volume = 21`（内核驱动上限为`23`，即`+9 dB`）。

---

## 五、测量注意事项

- **`SpeakerChain`高通叠加**：`eqscan`仅测量Histen引擎本身，而实际运行链路为`SpeakerChain(Histen(...))`，LR4高通位于Histen之后，对比时需通过`win_vs_android.py --hpf`将该高通计入Android侧曲线。
- **扫频激励幅度**：`eqscan`使用`0.10`（`-20 dBFS`）扫频幅度，避免触发引擎内部限幅器/DRC非线性区；运行结束时需确认钳位样本数为`0`。
- **样本格式**：Android侧Histen引擎输入为`int32`高半字（Q1.31高16位有效），不可直接传入紧凑`int16`样本流。
- **场景号绑定**：`data/`下的标定数据对应场景3（`SWS_SPK_LANDSCAPE_TWO`，与`histen_chain.h`的`kDefScene`一致）。不同场景的EQ槽中心频率定义不同，预设值不可跨场景混用。
