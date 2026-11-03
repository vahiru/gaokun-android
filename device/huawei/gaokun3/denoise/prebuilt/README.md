# denoise/prebuilt/ —— 麦克风降噪服务要的高通文件（不入版本库，构建时获取）

这个目录里只有本README受版本控制。其余文件由`scripts/fetch-denoise-blobs.py`从公开来源下载、按sha256核对后放进来：

```
qairt/lib/aarch64-android/libQnnHtp.so          QNN的HTP后端（Android侧）
qairt/lib/aarch64-android/libQnnHtpV68Stub.so   v68的stub
qairt/lib/aarch64-android/libQnnSystem.so       读上下文二进制用的System接口
qairt/lib/hexagon-v68/unsigned/libQnnHtpV68Skel.so   DSP侧的skel，跑在CDSP的unsigned PD里
qairt/LICENSE.pdf、qairt/NOTICE.txt             高通的许可和第三方声明，跟库一起装进ROM
qairt/include/QNN/**                            编译降噪服务用的头文件（不装进ROM）
dsp/fastrpc_shell_unsigned_3                    CDSP的unsigned PD外壳
dsp/libc++.so.1、dsp/libc++abi.so.1           DSP侧的C++运行库，skel要用
fastrpc/src/**、fastrpc/inc/**、fastrpc/LICENSE.txt   FastRPC用户库的源码（BSD-3-Clause），编libcdsprpc_gk3denoise
SHA256SUMS                                      脚本写的清单
```

**正式构建一定带着它们**：`scripts/sync-device-tree.sh`在同步前检查这个目录，缺了或对不上就先跑获取脚本，
再把目录带到构建机并核对sha256，还不对就停下来报错。没有跳过的开关：ROM里一定要有降噪服务。

## 为什么不入库

- **QNN运行库**：高通的AI Stack License（`qairt/LICENSE.pdf`）只允许以目标代码形式、作为应用的一部分分发，
  明确不许单独分发，SDK的头文件也只许自己开发时用。所以库和头文件都不能进公开仓库，也不能由谁转手给构建的人：
  每次构建由构建的人自己从高通的公开下载地址取（下载即接受许可），和meta-qcom的做法一样。
  库装在只给降噪服务用的私有目录里，许可和声明文件一起装。
- **CDSP外壳和DSP侧的C++运行库**：取自华为这台机器的Windows驱动包，和`firmware/`里的CDSP固件同一个cab，**未获再分发授权**。
- **FastRPC源码**：许可允许入库；为了不在仓库里多放一份第三方源码，按钉住的提交在构建时取。

## 从哪来

| 文件 | 来源 |
|---|---|
| `qairt/**` | Qualcomm AI Runtime（QAIRT）2.43.0.260128的Community下载（不需要账号）：`https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/2.43.0.260128/v2.43.0.260128.zip`。zip有1.5 GB，脚本用HTTP range只读需要的成员，约10 MB |
| `dsp/*` | `firmware/README.md`第三种来源的uup-drivers-sc8280xp 200.0.10.0 release里的`qcsubsys_ext_cdsp8280.cab`（只读这一个成员），`cabextract`解出`CDSP/`下的这三个文件；会提示cab尾部有多余数据，那是签名，不影响 |
| `fastrpc/**` | `github.com/qualcomm/fastrpc`的固定提交（`FASTRPC_COMMIT`），只取`src/`、`inc/`和`LICENSE.txt` |

各文件的sha256写在脚本里（`QAIRT_FILES`、`DSP_FILES`、`FASTRPC_TREES`、`FASTRPC_LICENSE_SHA`），头文件和FastRPC源码按整棵树核对哈希。换QAIRT版本时，
降噪服务的模型（上下文二进制）要用同一版本重新生成并重新验证。
