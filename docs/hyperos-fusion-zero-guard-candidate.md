# HyperOS Fusion 零输入保护候选

本候选只拒绝不能归一化的加速度输入，不替换整套OEM算法、不伪造姿态/重力值，
也不把已有NaN强行改成单位四元数。当前仅完成离线静态与真实OEM函数执行验证。
设备临时绑定及框架重启对照由父线程另行记录。

## 实际触发证据

旧R17 EFI的R18系统同样复现，已排除“仅R19FP内核才有”的推断。
后续基线冷启动探针证据 `~/.cache/hos-r19-fusion-probe/cold-results.txt`：
启动前q为零，前两轮原始accel/gyro全零，第三轮原始数据恢复后融合仍NaN。
这与已验证的零输入污染顺序相符。producer为何先交付零值另行调查；本保护不能
代替修复生产端有效性，也不能证明全部NaN来源已经解决。

## 文件与修改范围

- 原库SHA256：`a4ba29620bc241c8cc4ab07b9dee05fd3e29e170855a94f0bddbd8ab8594dcaf`
- 候选SHA256：`3c54d8c74b94fa69e96f4c1dd5862983e7658e78ce81c2b3d234bf987dda2e21`
- guard SHA256：`4ad673e9c6bd69b5a8f0857b18f78feac26648d1654869df2626d72c65a8f8a7`
- 候选目录：`~/.cache/hos-r19-fusion-guard/candidate/`
- patcher：`tools/hos/patch-fusion-zero-guard.py`

只允许以下文件字节范围变化（末地址不含）：

| 范围 | 内容 |
|---|---|
| 0xd0–0xe0 | RX PT_LOAD的p_filesz/p_memsz各增加72 |
| 0x567d8–0x567dc | 原ldr替换为单条直接B |
| 0x809c0–0x80a08 | 72字节guard |

RX段原本为offset/VA0x3c000，filesz=memsz0x449c0，flags=R+X、alignment16KiB。
guard放在原RX末尾全零对齐空隙，显式扩展声明到0x80a08；仍在原最后映射页内，
不接触下一段0x84000。文件总长度不变。section表、dynsym、压缩debug符号、依赖、
所有重定位字节和其他所有字节不变；静态检查确认guard不与既有allocated section
或动态/mini-debug非零大小符号重叠。

## 控制流与ABI

函数入口0x567d4的BTI保留。被替换指令在0x567d8，早于0x567ec PACIASP及建栈。
guard只使用caller-saved w16、v16–v18及NZCV，保留this/what/data/dT和callee-saved寄存器。

- 非ACC输入：重放原 `ldr w10,[x0,#0x1f4]`，直接跳回0x567dc。
- ACC输入：准确读取12字节，检查平方和为正且有限。拒绝零、NaN、±Inf，以及
  平方和下溢为零/溢出为Inf的输入。
- 有效输入：仍重放原ldr并回到原OEM逻辑，原状态逐字节保持一致。
- 无效输入：`mov w0,0; ret`，没有修改对象、计数、协方差、四元数或偏置。
  因为尚未签名LR/建栈，不需要伪造PAC恢复或栈展开。原函数自身亦存在PAC前早退路径。

handleAcc看到false即返回BAD_VALUE；后续正常真实样本可以完成初始化。
对于已污染的进程需重新初始化/重启框架；本patch不会自行修正旧NaN。
它也不涵盖若干有效向量累积后相消为零等其他退化路径。

## 已完成验证

1. `test-fusion-guard-patcher.py`：错误原hash、RX布局、BTI/入口、非零空隙、越范围改字节、
   错误B/guard/段长度均拒绝；Capstone核对全部18条指令、分支目标与无SP修改。
2. `fusion-guard-study.py --candidate ...`：先验证候选精确等于允许的变更，再从候选
   取入口及guard字节执行。独立macOS arm64子进程仅映射审过的OEM初始化函数/常量，
   两个内部PLT解析到本地函数，无OEM服务入口、constructor或外部imports。
3. 原OEM实际函数在零acc之后产生NaN；guard拒绝六种非法输入且对象完全不变。
   所有三种fusion mode的有效输入和原版状态逐字节一致；非法后正常样本能初始化。
4. `file`识别候选仍为ARM64 ELF共享库；原库文件未修改。候选没有执行设备操作。

```sh
python3 tools/hos/test-fusion-guard-patcher.py ~/.cache/hos-r19-fusion/libsensorservice.so
python3 tools/hos/fusion-guard-study.py ~/.cache/hos-r19-fusion/libsensorservice.so \
  --candidate ~/.cache/hos-r19-fusion-guard/candidate/libsensorservice.so
```

## 调试身份与局限

为保持其余字节不变，GNU buildID保留 `951bf6f987a63129e476bf95681eaed5`。
它现在不能区分原库与候选，必须用整文件SHA256；不能把buildID相同当作未修改。
72字节leaf guard没有原DWARF FDE覆盖，异步采样恰好落在那里时回溯可能截断。
没有扩大SELinux权限、改动态依赖、替换HAL、改磁力计能力或输出补零。
完整运行时性能、加载器及冷启动回归仍需实测；上面验证只覆盖本保护的明确输入范围。
