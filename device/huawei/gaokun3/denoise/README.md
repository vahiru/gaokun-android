# 麦克风AI降噪服务（`denoise/` / `gk3denoised`）

本模块实现后台守护进程`gk3denoised`，在CDSP NPU（Hexagon HTP v68）上运行DeepFilterNet3神经网络（48 kHz采样率，每跳480帧 / 10 ms），为音频HAL采集处理链（[`../micarray/`](../micarray/README.md)）提供实时语音降噪：
- **语音通话（`VOICE_COMMUNICATION`）**：在`micarray`回声消除（AEC）之后按单声道处理，默认开启。
- **普通录音（`MIC`、`DEFAULT`）与录像（`CAMCORDER`）**：对应系统属性开关开启时按双声道独立处理。

采集钩子通过Unix域套接字`/dev/socket/gk3denoise`（`SOCK_SEQPACKET`，协议定义见[`../micarray/denoise_protocol.h`](../micarray/denoise_protocol.h)）按块发送音频跳（hop），服务端按连接与声道维护滤波器状态并返回等长帧数据，固定引入3跳（30 ms）算法延迟。

## 一、目录组成与相关组件

| 路径 | 说明 |
|---|---|
| `service/` | 降噪服务源码（Apache-2.0）：含信号处理引擎（CPU侧执行STFT分析、特征提取、ERB掩码、深度滤波与ISTFT合成，神经网络推理交由QNN执行）、套接字服务端与`socid_override.c` |
| [`model/`](model/README.md) | 预编译QNN上下文二进制模型（纳入版本控制），导出与验证流程见[`model/README.md`](model/README.md) |
| [`prebuilt/`](prebuilt/README.md) | 构建前由[`scripts/fetch-denoise-blobs.py`](../../../../scripts/fetch-denoise-blobs.py)拉取的外部组件（不纳入版本控制）：QNN运行时库与头文件、CDSP unsigned PD外壳、DSP侧C++运行库及FastRPC用户态源码 |
| `Android.bp` | 编译`gk3denoised`可执行文件及由FastRPC源码构建的私有`libcdsprpc`（安装于`/vendor/lib64/gk3denoise/`） |
| `gk3denoise.rc` | `init`服务定义`vendor.gk3denoise`（运行用户`system`，套接字权限`0660 system:audio`） |
| `../sepolicy/gk3denoise.te` | `gk3denoise`独立SELinux域及音频HAL连接权限规则 |
| `../ueventd.gaokun3.rc` | 将`/dev/fastrpc-cdsp`节点所有者设为`system` |
| `../device.mk` | 打包服务二进制、私有运行库、DSP侧skel组件、上下文模型与许可证文件 |
| 内核补丁`patches/0205` | 将CDSP的FastRPC上下文标记为`dma-coherent`，修复CDSP加载skel时因缓存一致性时序触发的`AEE_EUNABLETOLOAD`错误 |

## 二、关键实现机制

- **SoC ID兼容覆写（`socid_override.c`）**：
  QNN Android运行库在初始化时通过libc的`open()`读取`/sys/devices/soc0/soc_id`并校验内置白名单；本机SC8280XP的`soc_id`为`449`，不在QNN硬编码列表内，直接加载会报错退出。`gk3denoised`在自身可执行文件内定义并导出`open`系列符号（通过`Android.bp`的`--export-dynamic-symbol`），仅在目标路径为`/sys/devices/soc0/soc_id`时返回`415`（与SC8280XP同为Hexagon v68架构的SM8350），其余路径透传系统调用。该机制仅作用于`gk3denoised`自身进程，无需全局挂载覆写。
- **网络前预增益补偿（`--pre-gain-db`）**：
  送入神经网络前固定施加`+18 dB`增益，输出后再等量衰减还原。原因在于a16w8量化模型对低于约`-40 dBFS`的微弱人声会产生过度抑制，而平板内置麦克风采集的正常语音电平分布于`-55`至`-15 dBFS`之间；`+18 dB`预增益可将语音信号平移至量化模型的最佳工作区间。
- **CDSP外壳搜索路径**：
  `libcdsprpc`仅在编译期固定的搜索路径（`DEFAULT_DSP_SEARCH_PATHS`及`/vendor/dsp`）下查找DSP外壳文件，忽略环境变量`ADSP_LIBRARY_PATH`，因此`Android.bp`将其编译期路径指定为`/vendor/dsp/gk3denoise`。初始化时库会先尝试打开`/dev/fastrpc-cdsp-secure`（权限`root 0600`，SELinux策略配置为`dontaudit`），失败后自动回退至`/dev/fastrpc-cdsp`。
- **故障降级与超时自愈**：
  若QNN或CDSP模型加载失败，服务仍保持套接字监听并对建流请求返回不可用状态，由采集钩子自动旁路降噪；后台每30秒重试加载（`--retry-s`），若单次加载阻塞超过60秒则主动退出，交由`init`重启进程（`--load-timeout-s`）。
- **HTP功耗档位投票**：
  存在活动客户端连接时向HTP投票`low`性能档位，最后一个客户端连接断开时立即撤销投票以节省待机功耗。

## 三、验证现状

- **实机音频链路验证**：相同源码使用NDK编译并通过KernelSU模块部署后，在SELinux Enforcing模式下运行于独立域且无AVC拒绝记录；实机NPU输出与离线参考实现逐位一致，音频HAL每85 ms音频块的IPC往返耗时约14 ms。
- **SELinux编译期校验**：策略通过`lineage_gaokun3-cp2a-userdebug`完整编译检查（含`neverallow`断言、平台公开类型约束、`file_contexts`与`sepolicy_tests`）。
