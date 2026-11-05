# 扬声器Histen音效后处理模块（`effects/`）

本目录实现针对Huawei MateBook E Go（gaokun3）内置扬声器的AIDL音频effect模块（`libgaokunhisteneffect.so`），串联华为Histen专有音效引擎与自研高通/限幅保护链路：

```text
Histen引擎（内置多段EQ与低音增强） ──► LR4高通滤波 ──► Makeup补偿增益 ──► 限幅器 ──► 软削波
```

- **开关与分流机制**：由「设置 → 声音 → 扬声器增强」（`persist.sys.gaokun3.histen`，默认关闭）总控。当同一页面的「处理方式」（`persist.sys.gaokun3.speakerfx.engine`）处于默认的「低音增强」（`bass`）时，音频由音频HAL内的[`speakerfx/`](../speakerfx/README.md)处理，本effect保持逐比特直通；仅当用户选择「华为Histen」（`histen`）且总开关打开时，才由本effect处理内置扬声器上的媒体流（耳机、蓝牙与USB音频一律直通）。
- **专有二进制依赖**：Histen算法引擎（`libhw_histen_processing.so`）不纳入版本控制，需放置于[`prebuilt/`](prebuilt/README.md)。缺失该文件时不影响编译，运行时自动退化为仅执行自研高通与限幅链路。

---

## 一、目录结构

| 文件 / 目录 | 说明 |
|---|---|
| `gaokun_effect.cpp` | AIDL effect主体实现（继承`EffectImpl`），负责属性轮询、输出设备判定与生命周期管理 |
| `histen_chain.h` | Histen专有引擎适配层（`dlopen`/`dlsym`、480帧重分块、参数下发与异常回退） |
| `speaker_chain.h` | 自研扬声器保护链（LR4高通、Makeup增益、前瞻限幅器与软削波） |
| `histen_scenes.h`、`gen_histen_scenes.py` | 从华为`sws_config.xml`提取的15组`SWS_SPK_*`场景参数表及其生成脚本 |
| `audio_effects_config.xml` | 替代上游默认配置的effect注册表，在`music`流`<postprocess>`中挂载`gaokun_histen` |
| [`measurement/`](measurement/README.md) | 离线频响扫描工具（`eqscan`）、Windows WASAPI回环采集与频响对比分析脚本 |
| [`prebuilt/`](prebuilt/README.md) | 存放不入库的`libhw_histen_processing.so`专有二进制 |

---

## 二、架构与构建集成

### 1. AIDL Effect三层结构
```text
AudioPolicyEffects（读取HAL getProcessings()公布的audio_effects_config.xml <postprocess>）
      │
      ▼
effects HAL服务（APEX com.android.hardware.audio，SELinux域 hal_audio_default）
      │ dlopen
      ▼
/vendor/lib64/soundfx/libgaokunhisteneffect.so（自研AIDL effect包装）
      │ 启用Histen模式时按需dlopen
      ▼
/vendor/lib64/soundfx/libhw_histen_processing.so（专有算法引擎，可选）
```

配置与实现约束：
- **独立UUID槽位**：必须在`audio_effects_config.xml`中注册独立的`<library>`与`<effect>`条目，不可覆盖上游`LoudnessEnhancer`等原有库路径（否则LineageOS AudioFX按type UUID创建音效失败会导致`AudioPolicyService::startOutput()`中断整条会话的音效挂载）。
- **继承`EffectImpl`**：直接复用AOSP `libaudioeffectaidlcommon`提供的`EffectImpl`与FMQ线程管理，仅重写纯虚函数。
- **APEX链接器命名空间放行（`patches/0068`）**：effects HAL运行在vendor APEX `com.android.hardware.audio`中，默认隔离命名空间不放行`/vendor/lib64/soundfx/`。`patches/0068`在该APEX的`linker.config.pb`中追加了`/vendor/${LIB}/soundfx`许可路径。
- **SELinux属性转发**：由于`hal_audio_default`受`neverallow`限制不可直接读取`system_prop`（`persist.sys.*`），[`etc/histen.rc`](../etc/histen.rc)将其镜像为`vendor.gaokun3.histen.on`与`vendor.gaokun3.speakerfx.engine`（`vendor_gaokun3_prop`）。

---

## 三、生效条件与低延迟音轨限制

`music`流上的`gaokun_histeneffect`实例仅在以下条件**同时满足**时执行信号处理，否则逐比特直通：

1. **总开关开启**：`vendor.gaokun3.histen.on=1`；
2. **处理方式为Histen**：`vendor.gaokun3.speakerfx.engine=histen`；
3. **当前流已完成引擎装载（armed）**：引擎`dlopen`不在实时音频线程内执行，仅在开流或`START`命令时装载；
4. **输出设备为内置扬声器**：框架下发的设备类型为`OUT_SPEAKER`且不含`bt-a2dp`连接标识；
5. **双声道立体声格式**：Histen引擎仅在48 kHz采样率下接入（其他采样率仅运行自研扬声器保护链）。

**低延迟（`FAST`）音轨限制**：主输出开启`FastMixer`后（`patches/0083`），AudioFlinger拒绝在含`FAST`音轨的会话上挂载非硬件加速effect。使用低延迟通路的游戏音轨不会经过本模块；如需覆盖游戏音轨，请使用默认的音频HAL层「低音增强」模式（[`../speakerfx/README.md`](../speakerfx/README.md)）。

---

## 四、调试属性（root）

以下属性位于`persist.vendor.gaokun3.histen.*`命名空间下，仅在总开关开启且选择Histen模式时生效：

| 属性名 | 取值（默认） | 作用说明 | 生效时机 |
|---|---|---|---|
| `...engine` | `0` / `1`（默认`1`） | 设为`0`时跳过Histen引擎、仅运行自研高通与限幅链（用于A/B对照） | 下一次开始播放 |
| `...scene` | `0`–`14`（默认`3`） | 选择`histen_scenes.h`中的`SWS_SPK_*`场景（默认`3`=`SWS_SPK_LANDSCAPE_TWO`；请勿使用未标定且左右不对称的场景`0`） | 下一次开始播放 |
| `...eq.N` | `N`=`0`–`10`，值`0`–`203` | 无符号绝对覆盖当前场景的第`N`个EQ频段（约`0.0801 dB/单位`，仅槽`1`–`8`有效，中心频率依次为120 / 560 / 2200 / 3700 / 4600 / 1100 / 220 / 14000 Hz；设为空串恢复场景默认值） | 约1秒 |
| `...ben.{on,thr,gain,freq,a,b}` | 整数 | 覆盖Histen内部低音增强（BEN）对应字段 | 约1秒 |
| `...vol.{ana,dig}` | 整数 | 覆盖Histen内部模拟/数字音量字段 | 约1秒 |
| `...hpf` | Hz（默认`150`，`0`=关闭） | 自研链LR4高通滤波器截止频率 | 约1秒 |
| `...makeup` | dB（默认`+4`） | 自研链补偿增益 | 约1秒 |
| `...limit` | `0` / `1`（默认`1`） | 自研链限幅器开关 | 约1秒 |
| `...ceiling` | dBFS（默认`-1`） | 限幅器输出天花板 | 约1秒 |
| `...release` | ms（默认`120`） | 限幅器释放时间 | 约1秒 |

---

## 五、日志验证与故障回退

```sh
adb shell su -c 'logcat -d -s gaokun_effect' | tail -40
```

- **关键日志**：
  - `open: Histen chain up: scene=3 of 15, block=480 frames`：Histen引擎加载并初始化成功；
  - `processing: built-in speaker, Histen + speaker chain`及周期性`meter: in=... out=... dBFS`：音频流正在通过处理链；
  - `bypass: ...`：当前流处于直通状态及具体原因。
- **故障安全机制**：若`dlopen`、符号解析、`Init`或`SetParams`任一步失败，或者运行期`Apply`连续失败达64次，适配层会自动卸载Histen引擎并回退为自研高通+限幅链路，确保扬声器不会静音或输出失控信号。
