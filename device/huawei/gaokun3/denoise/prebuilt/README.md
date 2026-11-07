# 降噪服务外部预置组件目录（`denoise/prebuilt/`）

本目录仅将本说明文件纳入Git版本控制。其余编译与运行所需的QNN SDK、DSP运行库及FastRPC源码由[`scripts/fetch-denoise-blobs.py`](../../../../../scripts/fetch-denoise-blobs.py)在构建前从公开渠道自动获取并校验SHA-256：

```text
qairt/lib/aarch64-android/libQnnHtp.so               # QNN HTP后端库（Android用户态）
qairt/lib/aarch64-android/libQnnHtpV68Stub.so        # Hexagon v68 RPC Stub库
qairt/lib/aarch64-android/libQnnSystem.so            # 加载QNN上下文二进制所需的System接口库
qairt/lib/hexagon-v68/unsigned/libQnnHtpV68Skel.so   # DSP侧Skel库（运行于CDSP unsigned PD）
qairt/LICENSE.pdf, qairt/NOTICE.txt                  # 高通许可证与第三方声明（随库一同打包入ROM）
qairt/include/QNN/**                                 # 编译gk3denoised所需的C头文件（不装入ROM）
dsp/fastrpc_shell_unsigned_3                         # CDSP unsigned PD运行外壳
dsp/libc++.so.1, dsp/libc++abi.so.1                  # DSP侧C++运行时库（供Skel加载依赖）
fastrpc/src/**, fastrpc/inc/**, fastrpc/LICENSE.txt  # FastRPC用户态库源码（BSD-3-Clause，用于编译私有libcdsprpc）
SHA256SUMS                                           # 拉取脚本生成的校验清单
```

[`scripts/sync-device-tree.sh`](../../../../../scripts/sync-device-tree.sh)在同步设备树前会自动检查本目录：若文件缺失或校验不匹配则自动调用`fetch-denoise-blobs.py`拉取，随后同步至构建机并二次校验SHA-256，校验失败则立即终止构建。

## 一、不纳入版本控制的原因

- **QNN运行时库与头文件**：高通AI Stack许可证（`qairt/LICENSE.pdf`）仅允许将运行时库以目标代码形式随应用程序一并分发，禁止单独再分发或将SDK头文件直接提交至公开源码仓库。因此构建流程遵循与`meta-qcom`相同的规范，由构建者在本地直接从高通官方公开地址下载并随ROM镜像打包，同时附带许可证与声明文件。
- **CDSP外壳与DSP侧C++运行库**：提取自华为MateBook E Go官方Windows驱动包（与[`../../firmware/`](../../firmware/README.md)中的CDSP固件同源），未获单独再分发授权。
- **FastRPC用户态源码**：上游采用BSD-3-Clause许可证，为避免在设备树中重复维护第三方源码副本，构建时按固定提交哈希直接拉取。

## 二、上游来源与校验方式

| 目录项 | 上游来源与获取方式 |
|---|---|
| `qairt/**` | Qualcomm AI Runtime（QAIRT）`2.43.0.260128`公开下载包（无需账号）：`https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/2.43.0.260128/v2.43.0.260128.zip`。完整压缩包约1.5 GB，脚本通过HTTP Range请求按需拉取目标条目（约10 MB） |
| `dsp/*` | [`matebook-e-go/uup-drivers-sc8280xp`](https://github.com/matebook-e-go/uup-drivers-sc8280xp/releases/tag/200.0.10.0)发布包中的`qcsubsys_ext_cdsp8280.cab`（仅按Range读取该成员），通过`cabextract`提取`CDSP/`目录下的3个文件 |
| `fastrpc/**` | [`qualcomm/fastrpc`](https://github.com/qualcomm/fastrpc)固定提交（`FASTRPC_COMMIT`），仅提取`src/`、`inc/`与`LICENSE.txt` |

所有二进制文件与源码目录树的期望SHA-256均固定在[`scripts/fetch-denoise-blobs.py`](../../../../../scripts/fetch-denoise-blobs.py)中（`QAIRT_FILES`、`DSP_FILES`、`FASTRPC_TREES`、`FASTRPC_LICENSE_SHA`）。若升级QAIRT版本，需同步使用对应版本的工具链重新生成并验证[`../model/`](../model/README.md)下的QNN上下文模型。
