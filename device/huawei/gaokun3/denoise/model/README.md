# DeepFilterNet3 QNN上下文模型（`denoise/model/`）

本目录存放面向CDSP HTP（Hexagon v68）预编译的QNN上下文二进制文件`dfn3_v2_cal2_x8_a16w8_v68.bin`，包含DeepFilterNet3的神经网络推理图，由[`gk3denoised`](../README.md)加载至NPU执行；STFT分析、特征提取、ERB掩码应用、深度滤波（DF）与ISTFT合成则在CPU侧由服务源码完成。

| 属性 | 参数值 |
|---|---|
| 文件大小 | `5,939,200`字节 |
| SHA-256 | `2cc2c34ab86fe96cc4b17fe655596668d5270d026240167ad38824c76ba76b81` |
| 基础模型 | DeepFilterNet3官方预训练权重（上游提交`d375b2d8`），48 kHz采样率，每跳480帧 |
| 包含计算图 | `dfn3_step`（单跳推理）与`dfn3_block8`（8跳批处理推理），两图共享相同的循环状态编码，可交替调用 |
| 量化精度 | 16位激活、8位逐通道权重（`a16w8`）、32位偏置 |
| 编译目标 | QAIRT `2.43.0.260128`，SoC Model `30`（SM8350，与SC8280XP同为Hexagon v68），unsigned PD |

---

## 一、模型导出与量化流程

1. **计算图等价改写（适配HTP v68算子限制）**：
   从DeepFilterNet3官方权重导出ONNX模型，并针对HTP v68完成两处数学等价改写：
   - 将深度可分离转置卷积（在v68后端存在数值异常）拆解为`1×1`卷积加`1×2`卷积并交错重排。
   - 将GRU展开为显式时间步单元，并将矩阵乘法改写为`1×1`卷积以支持权重逐通道（per-channel）量化。
   - `dfn3_block8`图将非循环层按8跳合并批处理，GRU状态在图内逐跳递推。
2. **量化校准数据集**：
   使用VoiceBank-DEMAND测试集说话人`p232`（CC BY 4.0，含`0 / -12 / -24 dB`带噪语音与干净语音）叠加本机实录背景噪声样本，每条音频流附加25个峰值帧；对数学上严格有界的张量（GRU隐藏状态、增益掩码、`lsnr`）直接按解析上下界设定量化范围。最终模型二进制仅包含各层张量的量化缩放系数与零点参数，不含任何音频样本数据。
3. **上下文二进制生成**：
   使用QAIRT `2.43.0.260128`工具链完成模型转换与量化校准，并通过`qnn-context-binary-generator`编译生成目标上下文二进制文件。

---

## 二、精度与性能验证

- **离线语音质量对比（留出测试集说话人`p257`）**：
  与DeepFilterNet3官方浮点实现（`libDF`）相比，输出波形差异信噪比为`38–41 dB`，SI-SDR偏差在`0.05 dB`以内（在`-12 dB`与`0 dB`输入信噪比下：`dfn3_step`为`16.31 / 16.76 dB`，`dfn3_block8`为`16.34 / 16.77 dB`，浮点基准为`16.36 / 16.76 dB`）。
- **实机NPU推理验证（含内核补丁`0205`）**：
  实机HTP输出与高通x86 HTP模拟器输出差异信噪比为`56–58 dB`；在HTP `low`功耗档位下，`dfn3_block8`单次调用（处理80 ms音频）耗时约`12 ms`，`dfn3_step`单跳调用（处理10 ms音频）耗时约`1.2–1.4 ms`，均留有充足的实时余量。

---

## 三、开源许可说明

- DeepFilterNet采用MIT与Apache-2.0双重许可（`Copyright (c) 2021 Hendrik Schröter`），许可证全文保存在本目录的[`LICENSE-MIT`](LICENSE-MIT)与[`LICENSE-APACHE`](LICENSE-APACHE)中。
- VoiceBank-DEMAND数据集（Edinburgh DataShare `10283/2791`，CC BY 4.0）仅在离线量化阶段用于统计激活张量动态范围。
- 本模型二进制需配合高通QNN运行时库加载执行（详见[`../prebuilt/README.md`](../prebuilt/README.md)）。
