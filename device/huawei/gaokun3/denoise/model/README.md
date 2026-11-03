# denoise/model/ —— 降噪服务跑在NPU上的DeepFilterNet3

`dfn3_v2_cal2_x8_a16w8_v68.bin`是QNN的上下文二进制：DeepFilterNet3的网络部分，为CDSP的HTP（Hexagon v68）编好，
降噪服务加载后直接在NPU上执行。分析、特征、掩码、深度滤波和合成在CPU上做（服务的源码），只有网络在这里。

| 项 | 值 |
|---|---|
| 大小 | 5939200字节 |
| sha256 | `2cc2c34ab86fe96cc4b17fe655596668d5270d026240167ad38824c76ba76b81` |
| 模型 | DeepFilterNet3官方权重（DeepFilterNet提交`d375b2d8`），48 kHz，每跳480帧 |
| 两张图 | `dfn3_step`每次一跳，`dfn3_block8`每次八跳，两张图的循环状态编码相同，可以交替用 |
| 量化 | 激活16位、权重8位按通道（a16w8），偏置32位 |
| 目标 | QAIRT 2.43.0.260128，SoC model 30（SM8350，和SC8280XP同为v68），unsigned PD |

## 怎么来的

1. 从DeepFilterNet3的权重导出ONNX，两处为HTP v68改写，数学上不变：深度可分的转置卷积在v68上结果不对，
   改成1×1卷积加1×2卷积再交错；GRU拆成显式的单元，矩阵乘改成1×1卷积，这样权重能按通道量化。
   8跳的图把逐帧的层成批算，GRU在图里逐跳推进。
2. 量化的校准数据：VoiceBank-DEMAND测试集说话人p232（CC BY 4.0，带噪0/−12/−24 dB和干净语音）加一段本机的录音，
   每条流另加25个峰值帧；按构造就有界的张量（GRU状态、增益、lsnr）直接按公式定范围。
   模型里只存各张量的范围，不含任何录音。
3. 用QAIRT 2.43的工具转换、量化，再用`qnn-context-binary-generator`编成这个上下文二进制。

转换和校准的脚本现在不在这个仓库里。

## 验证

- 留出的说话人p257（不在校准集里）：输出和DeepFilterNet3的浮点实现（libDF）的差异信噪比38–41 dB，
  SI-SDR和浮点只差0.05 dB以内（−12 dB和0 dB两种信噪比：1跳图16.31/16.76 dB，8跳图16.34/16.77 dB，
  浮点16.36/16.76 dB）。
- 本机上跑（内核带0205）：和高通x86 HTP模拟器的输出差异信噪比56–58 dB；8跳一次调用约12 ms（功耗档low），
  逐跳约1.2–1.4 ms，都远在实时预算以内。

## 许可

DeepFilterNet是MIT或Apache-2.0双许可（Copyright (c) 2021 Hendrik Schröter），许可文本在本目录的
`LICENSE-MIT`、`LICENSE-APACHE`。VoiceBank-DEMAND（Edinburgh DataShare 10283/2791）是CC BY 4.0，
只用来定量化范围。这个文件只能和高通的QNN运行库一起用（`../prebuilt/README.md`）。
