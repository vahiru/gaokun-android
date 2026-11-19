# GPU / GMU诊断与取证工具集（Adreno 690 / 主线`msm`）

本目录收录排查Adreno 690（`a690`）GMU HFI通信与GPU挂死问题的宿主机侧devcoredump解析工具，以及跨优先级队列提交的复现程序。Adreno 690架构与修复细节详见[`../../docs/dev/gpu-turnip.md`](../../docs/dev/gpu-turnip.md)。

## 一、工具

| 文件 | 用途 |
|---|---|
| `gmu-hfi-decode.py` | 解析devcoredump中的`gmu-hfi`段：打印HFI队列表与消息流水 |
| `decode-fatal.py` | 按`queue-history`提取最后8条H2F消息与F2H ACK的原始载荷 |
| `decode-gmulog.py` | 转储devcoredump中的`gmu-log`段（GMU固件trace环形缓冲）；记录格式未公开，按32位字逐行打印非零区域 |
| `decode-ring.py` | 解析`ring0`在`rptr`附近的PM4包（`TYPE4`/`TYPE7`） |
| `vk-prio.c` | 最小跨优先级Vulkan队列提交复现程序（模拟SurfaceFlinger与App双优先级并发） |

四个解析脚本的输入均为从`/sys/class/devcoredump/devcd*/data`读出的文本，用法见各文件开头。

## 二、调试注意事项

1. **GPU SMMU寄存器访问前提**：GPU SMMU的Context Bank（CB）寄存器仅在GPU处于上电状态时可读；另外，驱动按`IDR1.NUMCB`报告7个Context Bank，GPU与GMU只用CB0和CB1。**严禁扫描CB7及以上的寄存器**：它们超出`NUMCB`，访问会触发同步外部中止（external abort）导致整机死锁。
2. **`toybox devmem`输出格式**：Android内置`toybox devmem`默认以十进制输出结果（例如`00000487`代表`0x1E7`），读取时需注意进制转换。
3. **`crashdec`版本号识别**：使用Mesa `crashdec`解析主线内核产生的devcoredump时，若遇到`revision: 0 (06090000)`，可替换为`revision: 690 (6.9.0.0)`以便解析器识别Adreno 690寄存器定义。
4. **Mesa Android属性命名规则**：环境变量`TU_DEBUG`在Android属性系统中对应`debug.mesa.tu.debug`（`mesa.`前缀并将下划线转为点号）。
5. **切勿设置`msm.enable_preemption=0`**：主线`msm_submitqueue.c`中对该参数的判断逻辑相反，传入`0`反而会放行未完成的抢占路径。
6. **停用`/vendor/etc/init/`下的rc文件**：Android `init`会解析`/vendor/etc/init/`目录下的所有文件（不限于`.rc`扩展名），将文件重命名为`.rc.off`仍会被加载，停用时需移出该目录。
