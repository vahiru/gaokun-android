# gaokun3 扬声器音效（Histen 后处理链）

给 Huawei MateBook E Go（gaokun3 / SC8280XP）的扬声器加一条后处理链：
**Histen 引擎 → LR4 高通 → 5 段 EQ → makeup → 限幅器 → 软削波**。

这段是**纯软件**处理，不碰 DAC / 功放寄存器，也不需要内核补丁。
它解决的是「本机 Android 放音明显比 Windows 小且闷」——原因是 Windows 侧的音效在
Android 上是缺的，而不是硬件增益不同。

> ⚠️ **本目录含一份华为专有二进制**（`prebuilt/…/libhw_histen_processing.so`），
> 来源与许可见文末「来源与许可」。若你不想让它进自己的仓库/发行版，删掉
> `prebuilt/` 并移除 `device.mk` 里对应的那一行即可，**其余部分照常工作**
> （effect 会自动降级为逐比特直通，不会哑）。

---

## 一、目录内容

| 文件 | 性质 | 说明 |
|---|---|---|
| `gaokun_effect.cpp` | 自研 | AIDL effect 主体。继承 `EffectImpl`，只实现 `effectProcessImpl()` |
| `histen_chain.h` | 自研 | 与 Histen 引擎的适配层：dlopen/dlsym、参数下发、逐比特直通降级 |
| `speaker_chain.h` | 自研 | 自研扬声器链：LR4 高通、5 段 EQ、makeup、限幅器、软削波 |
| `histen_scenes.h` | 生成 | 15 个 `SWS_SPK_*` 场景的参数表，由 `gen_histen_scenes.py` 从 `sws_config.xml` 生成 |
| `gen_histen_scenes.py` | 自研 | 上述生成器（换机/换固件版本时重跑） |
| `audio_effects_config.xml` | 配置 | effects HAL 读取的**唯一**配置。= AOSP 默认内容 + 我们的 `gaokun_histen` 条目 |
| `prebuilt/lib64/soundfx/libhw_histen_processing.so` | **华为专有** | Histen 引擎（iMedia Audio 8.0 Histen 6.1.9） |

---

## 二、为什么是三件套

AOSP 的音频 effect 架构决定了「一个音效」要落在三个地方，缺一不可：

```
App / AudioPolicy
      │  按 audio_effects_config.xml 的 <postprocess> 找到 impl UUID
      ▼
audioserver ── dlopen ──▶ /vendor/lib64/soundfx/libgaokunhisteneffect.so   ← 自研 effect
                                │ 需要引擎算法时 dlopen
                                ▼
                          /vendor/lib64/soundfx/libhw_histen_processing.so ← 专有引擎
```

* **自研 effect 承担协议**：AIDL effect 接口、FMQ 环形缓冲、`EffectImpl` 生命周期。
  必须继承 `EffectImpl` 且只实现 `effectProcessImpl()` —— 自己建 FMQ 或线程会被
  `open()` 里的 `dupeFmq()` 覆盖，表现为「库被加载但永远拿不到数据」。
* **专有引擎承担算法**：Histen 的 255 值 p3 参数块（EQ / DRC / LMB 等）只由它解释。
* **配置承担注册**：`audio_effects_config.xml` 里 `<libraries>` 声明库，
  `<effects>` 声明 effect 的 **impl UUID 与 type UUID**，`<postprocess>` 把它挂到
  music stream。

> ⚠️ 两条踩过的坑，改配置前务必读：
> * **不要**把原厂某个 `<library path=...>` 改成我们的库。HAL 的
>   `Factory::queryEffects()` 按 library descriptor 枚举，改 path 会让原厂
>   LoudnessEnhancer 的 descriptor 从枚举结果里消失，而 LineageOS 的 AudioFX
>   每次播放都要按 type UUID 创建它；`AudioPolicyService::startOutput()` 里
>   `addOutputSessionEffects()` 遇到失败会**整体中断**，结果是我们的 effect
>   永远拿不到 `setEnabled(true)`。**必须新增独立槽位。**
> * AIDL effect HAL 机型上，`AudioPolicyEffects` **不再**自己解析
>   `audio_effects.xml`，而是问 HAL 要 `getProcessings()`，其来源是 HAL 自己的
>   `audio_effects_config.xml`。所以 postprocess 必须写进**这个**文件。

---

## 三、构建集成

`device.mk` 做了两件事：

```make
PRODUCT_PACKAGES += libgaokunhisteneffect          # Soong 编出的 effect 库

# 用我们的配置取代 AOSP 默认那份（soong config 只门控这一个 prebuilt_etc，
# 它不控制任何编译期行为，所以关掉是安全的）
$(call soong_config_set_bool,hardware_interfaces_audio,use_default_audio_effects_config,false)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/effects/audio_effects_config.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_effects_config.xml \
    $(LOCAL_PATH)/effects/prebuilt/lib64/soundfx/libhw_histen_processing.so:$(TARGET_COPY_OUT_VENDOR)/lib64/soundfx/libhw_histen_processing.so
```

`Android.bp` 的关键三点：

* `defaults: ["aidlaudioeffectservice_defaults"]` —— 这个 default 已带齐
  FMQ / libutils / libcutils / libbinder_ndk 与 AIDL effect 的 NDK 绑定。
  **不要改用 NDK 直接编**：NDK 根本不提供 `libfmq`。
* `srcs` 里的 `":effectCommonFile"` **不是可选项** —— 它是
  `hardware/interfaces/audio/aidl/default/` 下持有 `EffectContext.cpp` /
  `EffectThread.cpp` / `EffectImpl.cpp` 的 filegroup，也就是整个 effect 框架。
  AOSP 里每个 effect 库都各自编一份；`destroyEffect` 这个导出符号**只可能**
  来自 `EffectImpl.cpp`（这也是校验产物时最可靠的标志）。
* `relative_install_path: "soundfx"` + `vendor: true` ⇒ 落到
  `/vendor/lib64/soundfx/`，即配置里 `path="…"` 的查找目录。

---

## 四、运行时控制

全部走 `persist.*` 属性（重启保留）。effect **每秒重读一次**，所以改完约 1 秒生效，
不用重启、不用重放。

| 属性 | 取值 | 作用 |
|---|---|---|
| `persist.gaokun3.histen.enable` | `0` / `1`（默认 1） | `0` = **逐比特直通**（A/B 对照用，注意它同时保留扬声器保护链） |
| `persist.gaokun3.histen.scene` | `0`–`14` | 选 `SWS_SPK_*` 场景（索引同 `histen_scenes.h` 顺序） |
| `persist.gaokun3.histen.eq.N` | `N`=0–10，值 −128…255 | **绝对覆盖**场景表的第 N 个 EQ 槽。<br>⚠️ 空 = 回落场景基线，所以**切场景前要先清空 `eq.*`**，否则旧覆盖会盖住新场景 |
| `persist.gaokun3.histen.hpf` | Hz | 高通拐点（LR4） |
| `persist.gaokun3.histen.makeup` | dB | 链内补偿增益 |
| `persist.gaokun3.histen.limit` | dB | 限幅阈值 |
| `persist.gaokun3.histen.ceiling` | dB | 限幅后天花板 |
| `persist.gaokun3.histen.release` | ms | 限幅器释放时间 |
| `persist.gaokun3.histen.ben.{on,thr,gain,freq,a,b}` | 见源码 | 低音增强（BEN）分字段覆盖，便于逐项扫描 |
| `persist.gaokun3.histen.vol.{ana,dig}` | 见源码 | 引擎的模拟/数字音量字段 |

> ★ **整体增益要走功放（PA），不要走 `makeup`**：PA 在 DAC 之后、是纯线性的，
> 不消耗限幅器余量；`makeup` 走链内会直接顶限幅器，听感上是「不干净」。

清空某个覆盖：`setprop persist.gaokun3.histen.eq.3 ""`

---

## 五、验证

日志 tag 是 **`gaokun_effect`**：

```bash
adb shell su -c 'logcat -d -s gaokun_effect' | tail -40
```

按可靠性排序：

| 判据 | 含义 |
|---|---|
| 逐行 `meter: in=… out=… dBFS` | ★ **最硬**。出现 = 音频真的流过我们的 effect |
| `Histen chain up: scene=N of 15, block=480 frames` | 引擎链已建立 |
| `speaker chain up: sr=… hpf=…` | 自研扬声器链已就位 |
| `Histen engine loaded from <path>` | 引擎从哪个路径 dlopen 成功 |
| `Histen disabled by …enable=0 -- bit-exact passthrough` | 主动旁路 |

* **没有 `meter:` 行 = effect 没被加载**，此时"改了没感觉"与音质无关，先查注册与配置。
* `Histen engine not found in any known location -- passthrough` ⇒ 引擎库没到位，
  **不会哑**，只是没有音效。

---

## 六、安全设计：任何异常都退回逐比特直通

一条音效链最坏的失败形态不是"没效果"，而是**把扬声器搞哑或搞爆**。所以
`histen_chain.h` 的策略是：dlopen / dlsym / `GetSize` / `Init` / `SetParams` /
`Apply` 任一环节出错，或 `Apply` 连续失败，就**置回未就绪状态并原样透传**，
让音频继续以逐比特不变的方式通过。库缺失、版本不符、参数非法都不会导致无声。

这也是为什么引擎路径是**列表**而不是单个常量：

```cpp
constexpr const char* kHistenLibPaths[] = {
    "/vendor/lib64/soundfx/libhw_histen_processing.so",
    "/system/lib64/soundfx/libhw_histen_processing.so",
};
```

`dlopen()` 只在名字**不含 `/`** 时才走链接器搜索路径，所以这里必须列绝对路径；
（本项目的另一条产品线正好踩过反向的坑：某个组件内部用了一条写死的绝对路径，
`LD_LIBRARY_PATH` 对它完全无效。）先 `/vendor` 是因为构建产物落在那儿，`/system`
是给树外 overlay 部署方式留的兼容位。

---

## 七、来源与许可

* **自研部分**（`gaokun_effect.cpp`、`histen_chain.h`、`speaker_chain.h`、
  `gen_histen_scenes.py`）为本项目独立实现，不含下列二进制。
* **`prebuilt/lib64/soundfx/libhw_histen_processing.so`**：华为专有二进制
  （iMedia Audio 8.0 / Histen 6.1.9），从**麒麟 V10 SP1** 的华为音频栈中提取，
  **未获再分发授权**。它被放进本目录是为了让构建产物开箱即有音效；
  如果你要发布或再分发本仓库，**建议先移除它**（见文首说明），
  改为让使用者自备——本仓库的 `firmware/`、`hexagonrpcd-root/`、`prebuilt-boot/`
  已经是这套做法（整目录忽略 + 只放行 README）。
* **`histen_scenes.h`**：由设备自身的音频调音配置 `sws_config.xml` 机械生成
  （生成器见 `gen_histen_scenes.py`），是纯数值表；随仓库一并分发，便于任何人直接构建。

  > ⚠️ **关于它与 `sws_config.xml` 的可复现性** —— 实测发现两者**不是**逐字节对应的，
  > 原因是开发期那份 `sws_config.xml` 已被改动：场景 `SWS_SPK_LANDSCAPE_ONE` 的
  > p3 索引 **96–111** 被覆盖成了 `SWS_SPK_LANDSCAPE_TWO` 的同段值
  > （那是为证明"场景参数确实参与运算"做的 A/B 判别实验，做法正是把 ONE 改成 TWO）。
  > 证据：本目录这份表 ONE ≠ TWO（`0x1f 0x3e 0x7d …` vs `0x41 0x78 0x230 …`），
  > 而从那份配置重生成会得到 ONE == TWO。
  >
  > 用本目录的生成器从三份现存副本（md5 均为 `c354020b01db…`）重跑，与提交的这份
  > 逐行比对**只有两处不同**：① 文件头注释（本表出自更早的生成器版本）；
  > ② 数值上**唯一**的差异就是 ONE 的 p3 索引 96–111（两行）。把 15 个场景的
  > p3 数组逐个比对，只有 `spk_landscape_one_p3` 这一个不同 —— 也就是说差异确实
  > 只来自那次实验，不是生成器有问题。
  >
  > ⇒ **本目录提交的是未经改动的原始值**（ONE = `0x1f…`，即实验记录里的 "orig"）。
  > 用生成器重跑时请用**未改动过的** `sws_config.xml`，否则 ONE 会被换成 TWO 的参数——
  > 而 ONE 恰好是默认场景（`kDefScene = 0`），这个错误不会报错、只会听感变样。
  > 换固件版本要重新生成时，务必先确认场景 ONE 与 TWO 在该区间不相等。
  > 直接把生成的表里这两个场景的 p3 数组拉出来 diff 即可（下面这条命令在
  > 本目录这份表上实测输出 `13,14c13,14`，正好对应索引 96–111 的两行）：
  >
  > ```bash
  > one() { sed -n "/spk_landscape_${1}_p3\[255\]/,/^};/p" histen_scenes.h | sed '1d;$d'; }
  > diff <(one one) <(one two)     # 必须非空；若为空 = 用了被改过的配置，别提交
  > ```

---

## 八、已知限制

1. 只挂 **music stream 的扬声器**输出；耳机 / 蓝牙 / 通话不走这条链。
2. 场景 `SWS_SPK_*` 的**几何含义**（LANDSCAPE_ONE 对应哪种摆放）由原厂配置决定，
   本项目只做了参数搬运，未逐一实听确认。
3. `eq.N` 的**符号约定**来自实测扫描，不是文档：改动前先按第四节做小步扫描，
   不要凭直觉设值。
4. 树内没有 SELinux 策略。本 ROM 目前是 permissive；若将来转 Enforcing，
   需要为 effect 库与属性访问补 `sepolicy`。
5. 该链是在**本机扬声器（WSA 双单元）**上调的，参数不具备跨机型通用性。
