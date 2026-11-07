# GPU / GMU诊断与取证工具集（Adreno 690 / 主线`msm`）

本目录收录排查Adreno 690（`a690`）GMU HFI通信、SMMU上下文故障与Mesa Turnip渲染问题的设备端脚本和宿主机侧devcoredump解析工具。Adreno 690架构与修复细节详见[`../../docs/dev/gpu-turnip.md`](../../docs/dev/gpu-turnip.md)。

## 一、设备端工具（Android）

| 文件 | 用途 |
|---|---|
| `smmu-nostall.sh` + `smmustall.rc` | 早期诊断辅助：轮询清除`SCTLR.CFCFG`使GPU SMMU fault立即terminate而非永久stall，并抓取`FSR`/`FAR`/`FSYNR`/`TTBR0`输出至logcat |
| `smmu-irq-probe.sh` | 验证SMMU context-fault中断是否送达CPU（挂载debugfs、开启dyndbg并对比前后`/proc/interrupts`） |
| `tudebug.rc` | 注入Turnip调试属性（属性名为`debug.mesa.tu.debug`） |
| `gpu-pin.sh` + `gpupin.rc` | 固定GPU `min_freq`（读取`/data/local/tmp/gpu_min_freq`），用于隔离动态调频引发的问题 |
| `capture-death.sh` | 宿主机侧异常现场捕获脚本：持续拉取内核日志、devcoredump与devfreq快照 |

## 二、宿主机与Linux对照分析工具

| 文件 | 用途 |
|---|---|
| `gmu-hfi-decode.py` | 解析devcoredump中的`gmu-hfi`段：打印HFI队列表与消息流水 |
| `decode-fatal.py` | 按`queue-history`提取最后8条H2F消息与F2H ACK的原始载荷 |
| `decode-gmulog.py` | 解析GMU固件trace环形缓冲（4字记录`{msg_hdr, event_code, timestamp, arg}`） |
| `decode-ring.py` | 解析`ring0`在`rptr`附近的PM4包（`TYPE4`/`TYPE7`） |
| `dtb-smmu-patch.py` | 历史实验脚本（请勿使用：移除`qcom,adreno-smmu`会导致`msm_iommu.c`空指针解引用panic） |
| `gmu-lab.sh` | Linux对照侧压力与电源循环脚本（`evidence` / `churn` / `stress` / `watch`） |
| `vk-prio.c` | 最小跨优先级Vulkan队列提交复现程序（模拟SurfaceFlinger与App双优先级并发） |
| `mesa-lab-setup.sh` | 在Linux环境编译Mesa `crashdec`等分析工具的辅助脚本 |

## 三、调试注意事项

1. **GPU SMMU寄存器访问前提**：GPU SMMU的Context Bank（CB）寄存器仅在GPU处于上电状态时可读；另外，本机仅实现2个Context Bank（`/proc/interrupts`中2条`arm-smmu-context-fault`，对应CB0 GPU与CB1 GMU）。**严禁扫描未实现的CB寄存器**，否则会触发同步外部中止（external abort）导致整机死锁。
2. **`toybox devmem`输出格式**：Android内置`toybox devmem`默认以十进制输出结果（例如`00000487`代表`0x1E7`），读取时需注意进制转换。
3. **`crashdec`版本号识别**：使用Mesa `crashdec`解析主线内核产生的devcoredump时，若遇到`revision: 0 (06090000)`，可替换为`revision: 690 (6.9.0.0)`以便解析器识别Adreno 690寄存器定义。
4. **Mesa Android属性命名规则**：环境变量`TU_DEBUG`在Android属性系统中对应`debug.mesa.tu.debug`（`mesa.`前缀并将下划线转为点号）。
5. **切勿设置`msm.enable_preemption=0`**：主线`msm_submitqueue.c`中对该参数的判断逻辑相反，传入`0`反而会放行未完成的抢占路径。
6. **停用`/vendor/etc/init/`下的rc文件**：Android `init`会解析`/vendor/etc/init/`目录下的所有文件（不限于`.rc`扩展名），将文件重命名为`.rc.off`仍会被加载，停用时需移出该目录。
