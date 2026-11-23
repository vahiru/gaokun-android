# Adreno 690 GPU加速架构（Turnip + ANGLE + DPU）

本文档说明`gaokun3`在主线Linux内核（`msm` DRM驱动）与Android 16/17用户态下启用Adreno 690硬件加速的架构、关键补丁与SMMU故障处理机制。

## 一、图形栈整体架构

由于主线内核使用`drivers/gpu/drm/msm`（UAPI为`msm_drm.h`）而非高通下游KGSL驱动，用户态采用开源Mesa Turnip驱动配合Google ANGLE实现完整的Vulkan 1.3与OpenGL ES 3.2支持：

```
Android App / SystemUI / SurfaceFlinger (renderengine = skiagl / skiavkthreaded)
        │                              │
   OpenGL ES 3.2                  Vulkan 1.3
        ▼                              │
  ANGLE (libEGL_angle /                │
   libGLESv2_angle)                    │
        └──────────────┬───────────────┘
                       ▼
        Mesa Turnip (vulkan.freedreno.so)
         [freedreno-kmds = "msm"]
                       │
        ┌──────────────┴──────────────┐
        ▼                             ▼
 /dev/dri/renderD128           CrosGralloc4 (minigbm_msm)
 (msm_drm GEM / SUBMIT)        (DMA-BUF + QCOM_COMPRESSED / LINEAR)
        │                             │
        └──────────────┬──────────────┘
                       ▼
       Linux Kernel: msm_gpu (Adreno 690) + msm_dpu (DPU 8.1)
```

### 核心配置与权限

| 组件 | 路径或属性 | 说明 |
|---|---|---|
| GPU硬件ID | `0x06090000`（Adreno 690） | 8档OPP（270 MHz–690 MHz），由GMU（`qcom/a660_gmu.bin`）与SQE（`qcom/a660_sqe.fw`）驱动 |
| Zap Shader | `qcom/sc8280xp/HUAWEI/gaokun3/qcdxkmsuc8280.mbn` | 离开安全模式所需的签名微码，在设备树`gpu@3d00000`节点的`zap-shader`子节声明 |
| Vulkan HAL | `/vendor/lib64/hw/vulkan.freedreno.so` | `ro.hardware.vulkan=freedreno`，声明`android.hardware.vulkan.version=4198400`（1.3） |
| OpenGL ES实现 | ANGLE（`ro.hardware.egl=angle`） | 将GLES 2.0/3.x调用翻译为Turnip Vulkan调用，无需编译Gallium `libgallium_dri.so` |
| Gralloc HAL | `minigbm_msm` | 分配`MSM_SUBMIT`与DPU直通兼容的DMA-BUF，支持`DRM_FORMAT_MOD_QCOM_COMPRESSED`（UBWC）与线性布局 |
| 节点权限与SELinux | `/dev/dri/renderD128`、`/dev/dri/card*` `0666` | `vulkan.freedreno.so`必须标记为`same_process_hal_file`，以便普通应用进程在沙箱内直接打开渲染节点并加载同进程HAL |

## 二、Android Native Buffer（ANB）延迟绑定修复（`patches/mesa/0001`）

### 1. 问题现象与触发路径

在Android平台上，Vulkan Loader（`libvulkan.so`）创建swapchain图像时采用**两阶段延迟绑定**流程：

1. **创建阶段（`vkCreateImage`）**：Loader在`pNext`链中传入`VkNativeBufferANDROID`，但其内部的`handle`指针置为`NULL`（此时图像仅确定宽高与格式，尚未从Gralloc分配底层缓冲）。Turnip的`tu_CreateImage`检测到`anb->handle == NULL`后，仅初始化`tu_image`结构体并计算默认线性布局，提前返回`VK_SUCCESS`，此时`image->mem == NULL`且`image->iova == 0`。
2. **绑定阶段（`vkBindImageMemory2`）**：Loader将真正的Gralloc buffer传入`VkBindImageMemoryInfo::pNext`中的`VkNativeBufferANDROID`，并将`BindInfo::memory`设为`VK_NULL_HANDLE`。

上游Mesa 26的`tu_BindImageMemory2`（`src/freedreno/vulkan/tu_image.cc`）在`BindInfo::memory == VK_NULL_HANDLE`分支仅留有一条未实现的占位分支：

```cpp
} else {
   /* TODO handle VkNativeBufferANDROID */
   UNREACHABLE("binding VkNativeBufferANDROID is unsupported");
}
```

在release构建中`UNREACHABLE`被编译为空操作，`tu_BindImageMemory2`直接返回`VK_SUCCESS`，但未导入DMA-BUF、未分配`image->bo`、也未更新`image->iova`。其后果有两处：

- **用户态段错误（SIGSEGV）**：当渲染通道包含深度/模板等`LAZY`瞬态附件时，`tu_allocate_transient_attachments()`尝试解引用`image->mem->bo`（偏移`0xd0`），因`image->mem == NULL`立即崩溃。
- **GPU写入IOVA `0x0`引发SMMU页错误**：对于不经过`tu_allocate_transient_attachments()`的颜色附件，Turnip生成的命令流直接以`image->iova == 0x0`作为渲染目标基址提交给Adreno 690，触发`gpu_smmu`写故障（`iova=0x00000000`）。

### 2. 修复方案（`patches/mesa/0001` v3）

在`tu_BindImageMemory2`中识别`pNext`链上的`VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID`：

1. 调用`vk_android_get_anb_layout()`向Gralloc4查询该buffer的实际`drm_format_modifier`、plane偏移与行跨距（stride）。
2. 将查询到的modifier（如`DRM_FORMAT_MOD_QCOM_COMPRESSED`或`DRM_FORMAT_MOD_LINEAR`）写入`image->vk.drm_format_mod`，调用`tu_image_update_layout()`重新计算Turnip内部布局（避免`tu_CreateImage`阶段假定的线性布局与Gralloc实际分配的UBWC布局不一致导致渲染错位或越界写）。
3. 调用`vk_android_import_anb()`将DMA-BUF fd导入为`VkDeviceMemory`，再通过`image->ops->bind_memory()`绑定`image->bo`并映射正确的GPU IOVA。

## 三、GPU SMMU上下文故障中断（`kernel/dts/0027`）与失速（Stall-on-Fault）

### 1. 中断号错位与死锁链

SC8280XP的GPU SMMU（`3da0000.iommu`，`qcom,sc8280xp-smmu-500`）在`msm_iommu.c`初始化时开启上下文故障失速模式（`SCTLR.CFCFG = 1`）：当任一GPU事务发生地址翻译错误时，SMMU冻结该笔总线事务并拉高该Context Bank（CB）的中断线，等待内核的`adreno_fault_handler`处理后写`RESUME`终止该笔事务。

主线设备树为`gpu_smmu`声明的上下文中断列表有错位：前两项SPI 678/679其实是PMU中断，CB0发生故障时拉高的是SPI 680（GIC INTID 712）。驱动按CB序号取中断，故障处理函数因此挂在了错误的中断线上，内核收不到任何上下文故障中断。此时一旦发生GPU页错误：

1. SMMU置起`FSR.SS = 1`（Stalled）并挂起GPU的AXI/AHB读事务，但CPU侧收不到中断，`adreno_fault_handler`永不执行。
2. 挂起的事务阻塞GPU的CX主总线（`CP_AHB_BUSY_CX_MASTER`）。
3. 随后GPU进入空闲休眠时，GMU固件尝试抽空总线但永远等不到空闲，报`HFI_H2F_MSG_GX_BW_PERF_VOTE`超时（`-110`），`cx gdsc`无法塌缩。
4. 内核尝试调用`a6xx_recover`复位GPU，但复位路径写寄存器同样走已冻结的AHB路径，最终导致GPU恢复工作队列永久卡死。

`kernel/dts/0027`是上游的修正（qcom树`arm64-for-7.5`分支，`89083b1e9e31`），按CB顺序改为SPI 680–688与703。应用之后，GPU页错误会进入`adreno_fault_handler`：`dmesg`打印`*** gpu fault: ttbr0=... iova=... dir=... type=...`，`/proc/interrupts`中`arm-smmu-context-fault`（GIC INTID 712起）的计数随之增加。

### 2. 内核的故障处理

`kernel/dts/0027`之后故障完全由内核处理。在A690上用Vulkan计算着色器经buffer device address向未映射地址写入，实测如下：

- **少量故障**：一次提交向512页以内的未映射地址写入时，第一次故障冻结该笔事务并进入`adreno_fault_handler`。内核打印`*** gpu fault`，关闭失速，生成devcoredump（`/sys/class/devcoredump/devcd*`），再写`RESUME`终止该笔事务。作业正常结束，写入被丢弃，应用不受影响。失速至少关闭500 ms，并且要等devcoredump被读取或删除（5分钟后自动删除）、下一次提交时才重新开启，在此之前的故障直接终止，不再生成devcoredump。含`fault-info`一节的devcoredump说明事务确实被冻结过（`FSR.SS`）；GPU挂起复位时也会生成devcoredump，但没有这一节。
- **大量故障**：一次提交向768页及以上写入，或经错误指针连续写入256 KB（65535次写入）时，内核只报告前两次故障，约150 ms后硬件挂起检测触发`hangcheck recover`复位GPU。出错的进程收到`VK_ERROR_DEVICE_LOST`，其他进程不受影响。若一直清除`CFCFG`、只用终止模式，同样的负载不会使GPU挂起；但大量故障说明应用本身已经出错，出错的应用收到设备丢失错误是预期行为，本项目沿用上游内核的默认处理。
- **严禁访问CB7及以上**：驱动按`IDR1.NUMCB`报告7个CB（CB0–CB6，`dmesg`中的`7 context banks`），GPU与GMU只用CB0和CB1。CB7及以上超出`NUMCB`，访问其寄存器空间会立即触发同步外部中止（Synchronous External Abort），导致内核无日志死机。用`devmem`排查时只能访问CB0和CB1。

## 四、10位视频（P010）零拷贝合成与构建脚本

- **P010零拷贝显示合成**（`patches/mesa/0002`）：
  - Iris硬件解码器在解码10-bit HEVC/VP9视频时输出`HAL_PIXEL_FORMAT_YCbCr_P010`。
  - `minigbm_msm`为`DRM_FORMAT_P010`开放`BO_USE_SCANOUT`用法，DPU驱动在`msm_dpu`层直接接收P010平面，避免SurfaceFlinger回退到GPU Shader做每帧格式转换，显著降低4K 10-bit视频播放时的GPU负载与功耗。
- **Mesa构建工具链**：Android 17使用上游生成器`meson2hermetic`（`meson convert`）生成`Android.bp`，具体适配步骤见[mesa-a17.md](mesa-a17.md)。


## 五、GPU抢占（`kernel/drm/0016`、`mesa/0012`、`mesa/0013`）

内屏是竖屏，横屏应用的图层要旋转，HWC做不了，只能由SurfaceFlinger每帧用GPU合成。没有抢占时所有队列共用一个ring，SurfaceFlinger的合成要等正在执行的应用提交跑完；GPU满载的游戏里，这会让合成赶不上刷新，整屏跟着掉帧。

### 1. 各部分如何配合

- **内核**（`kernel/drm/0016`）：主线的抢占代码原本只接在A7xx上，这个补丁让A6xx也走同一套流程，并给A690打开`ADRENO_QUIRK_PREEMPTION`，于是有4个ring。每次提交末尾插入`CP_CONTEXT_SWITCH_YIELD`，在提交之间切换ring；队列创建时带了`MSM_SUBMITQUEUE_ALLOW_PREEMPT`的，内核还用IB1LIST标记包住它的IB列表，CP可以在IB内部的分块边界抢占。只在提交之间切换不够：游戏一帧的GPU工作本身就接近刷新周期，SurfaceFlinger等的正是那次还没跑完的提交。
- **队列优先级**：用户态优先级由`msm_gpu_convert_priority()`换算成ring号和调度优先级（`kernel/drm/0014`修正了同一ring内方向相反的换算）。有4个ring时内核报告12级（`MSM_PARAM_PRIORITIES`），Turnip（`mesa/0013`）在这种情况下才声明`VK_QUEUE_GLOBAL_PRIORITY_REALTIME`，REALTIME落在最高的ring 0，默认的MEDIUM落在ring 2。
- **ANGLE**：ROM里的ANGLE看到队列族支持REALTIME时，会给整个进程的设备都用REALTIME。所以SurfaceFlinger和所有通过ANGLE绘制的界面（HWUI的Skia OpenGL）都在ring 0，直接用Vulkan的应用（例如用Unity Vulkan后端的游戏）在ring 2，前者可以抢占后者。用OpenGL ES的游戏也经过ANGLE，与SurfaceFlinger同在ring 0，彼此不抢占。
- **Turnip的A6xx抢占路径**：Turnip在内核接受`ALLOW_PREEMPT`时（`has_preemption`）就按抢占会丢失状态来生成命令，例如每个分块重新写VSC地址（`tu6_emit_tile_select()`）。这条路径原本为KGSL后端编写，`mesa/0013`让msm上的A6xx队列也声明`ALLOW_PREEMPT`。

### 2. 必须同时带上的修正

`mesa/0012`是上游Mesa的`ebdcf841`（“tu: Always lazy_init_vsc for tiler rendering”）。内核一报告抢占，Turnip就会走上面那条路径；没有这个修正时，不用硬件分块的GMEM渲染会写出未初始化的VSC地址，CP报`CP_HW_FAULT 0x13c00000`并触发GPU恢复，Chromium的合成最容易碰到。因此`kernel/drm/0016`不能单独发布，必须和`mesa/0012`一起。

### 3. 已知限制

- 只给A690开启：抢占时写的GMU保活寄存器`GMU_PWR_COL_PREEMPT_KEEPALIVE`是A660系列的，其他A6xx要改用`GMU_AO_SPARE_CNTL`，也只有A690有寄存器恢复表；`msm.enable_preemption=1`不会在其他A6xx上打开抢占。
- 抢占记录定为5096K：A660固件从记录内600K处开始保存GMEM，4M的GMEM要用到4696K，超过主线默认的4192K；5096K是固件自己在大配置下使用的记录大小。
- CP在抢占前后恢复的静态寄存器表取自下游驱动的A6xx和A650系列列表，下游驱动没有A690专用的表；IFPC相关的部分没有放进去，因为本驱动在A690上不启用IFPC。
- 抢占时CP会保存和恢复GMEM，会占用一部分GPU时间。
- GPU每次运行时挂起都会把4个抢占记录清零（`kernel/drm/0015`，共约20 MB），会占用一些CPU时间。
- 抢占超时或抢占出错时，GPU会被复位，正在执行的应用可能因此丢失GPU上下文。主线的恢复流程原本在当前ring空闲时（找不到要怪罪的提交）直接跳过，抢占状态停在FAULTED，所有ring都不再执行；`kernel/drm/0017`让这种情况也照常复位并重放，`dmesg`里会多一行`no pending submit on the active ring, resetting anyway`。
- 如果抢占卡住期间被切走的ring又收到新提交，hangcheck大约1秒后会照常复位GPU，但会把那次提交当作元凶：它其实从没执行过（卡住期间内核不写WPTR）。用VM_BIND的Vulkan应用（Turnip）会因此收到设备丢失。抢占若在某种情况下反复失败，每次都是约10秒卡顿加一次复位，没有计数，也不会自动关闭抢占。
- 抢占跨整机挂起的情况尚未经实测；GPU运行时挂起（同一个`pm_suspend`回调）之后工作正常。
- 用OpenGL ES的游戏与SurfaceFlinger同在ring 0，不受益。
- 内核和Turnip都不检查调用者的权限，任何应用都能申请REALTIME，与SurfaceFlinger同在ring 0。

### 4. 排查方法

- `dmesg`里不应出现`preemption timed out`、`preemption faulted`或`CP | HW fault`。
- tracefs的`drm_msm_gpu/msm_gpu_preemption_trigger`和`msm_gpu_preemption_irq`记录每次切换，两者应一一对应；`msm_gpu_submit`的`ring=`字段显示每次提交进了哪个ring。
- GPU挂死时用Mesa的`crashdec -f`解码devcoredump（`/sys/class/devcoredump/devcd*/data`，5分钟后自动删除）；A690要先把dump里的`revision: 0 (06090000)`改成`revision: 690 (6.9.0.0)`。只需查看GMU HFI消息、GMU固件trace或`ring0`当前位置附近的PM4包时，可以用[`scripts/gmu-forensics/`](../../scripts/gmu-forensics/README.md)中的解析脚本。

## 六、GPU的DDR带宽投票（`kernel/dts/0028`）

GPU的每档OPP在`opp-peak-kBps`中声明对DDR（`ebi`）的峰值带宽投票，devfreq切换频率时由OPP框架经interconnect提交给RPMh。主线的取值漏乘了DDR接口宽度（4字节），实际填的是DDR频率（kHz），投票只有应有值的四分之一。`kernel/dts/0028`是已提交上游、尚待评审的修正，改为1804000、6220000、10944000 kB/s（对应DDR 451、1555、2736 MHz）。

修正之前，GPU即使在最高频率，投票（2.7 GB/s）也低于NVMe控制器（`1c20000.pcie`）常驻的3.94 GB/s，DDR档位实际由NVMe的投票决定，GPU可用的带宽与自身频率无关。修正之后，GPU在410 MHz及以上会提高DDR档位；270 MHz的投票（1.8 GB/s）仍低于NVMe的投票，界面合成等轻载下DDR档位不变。

排查：`/sys/kernel/debug/interconnect/interconnect_summary`中，`ebi@interconnect-mc-virt`下`3d00000.gpu`一行是GPU当前的峰值投票，节点首行是汇总后的结果。

## 七、微信对acquire信号量的重复等待（`mesa/0014`）

微信的Flutter引擎（自行维护的Impeller分支，Vulkan后端）每次调用`vkAcquireNextImageKHR`后，会在两次提交里等待同一个acquire信号量：一次在acquire之后紧接着的提交里，一次在present之前的提交里。第一次等待已经消耗了acquire时导入的临时载荷，第二次等待的是从未被signal的永久载荷，违反`VUID-vkQueueSubmit-pWaitSemaphores-03238`。高通驱动不检查这一点。Turnip在msm上按需启用线程化提交（`VK_QUEUE_SUBMIT_MODE_THREADED_ON_DEMAND`），遇到这次等待会在`vkQueueSubmit`的`vk_queue_submit_move_binary_waits_to_temps()`中无限期阻塞：Flutter的光栅线程卡住，平台线程在下一次Surface变化时等待光栅线程，整个微信无响应。用LiteApp绘制的页面（公众号、服务号、游戏中心等）一直不出内容，离开页面时微信被判定为ANR。改为立即提交也不可行：内核会拒绝没有fence的syncobj，结果是设备丢失。

处理方式：

- Vulkan运行时为每个二值信号量统计已提交、但还没被等待或SYNC_FD导出消耗的signal（`vk_semaphore::binary_signals_pending`）。用计数而不用标志，是因为跨队列等待时间线时，同一个二值信号量可以先后有两次signal都已提交。永久载荷被导入，或导出为可共享的句柄之后，其他信号量或进程也能signal它，此后不再统计（`binary_untracked`）。
- driconf选项`vk_drop_unsignaled_binary_waits`打开时，等待一个没有临时载荷、仍在统计、且计数为0的二值信号量，这次等待会被丢弃；每个进程第一次丢弃时在logcat中输出一条警告（`Dropping waits on binary semaphores without a submitted signal operation`）。第一次等待已经让前一个提交等待acquire栅栏，msm上同一队列的作业按提交顺序执行，所以丢弃第二次等待不改变GPU的执行顺序。
- present路径（`vkQueueSignalReleaseImageANDROID`）在msm上原本由CPU直接复制信号量载荷来生成释放栅栏。其中有等待会被丢弃时改为向队列提交一次空提交，释放栅栏因此仍排在之前的GPU工作之后，SurfaceFlinger不会合成尚未画完的帧。
- 提交失败时计数不回滚，与临时载荷在失败时已被消耗的现有行为一致。
- `00-mesa-defaults.conf`只对进程名匹配`^com[.]tencent[.]mm(:.*)?$`的进程打开这个选项。ROM的Mesa使用编译进库的静态driconf表，其他应用的行为不变。

排查：

- 微信进程的logcat中出现上面的警告，说明选项已生效，并且发生了丢弃。
- 其他应用出现同样的卡死时，ANR记录中光栅线程停在`vk_queue_submit_move_binary_waits_to_temps()` → `drmSyncobjTimelineWait`。确认是同类问题后，可以在`00-mesa-defaults.conf`中为它添加同样的条目。

## 八、无穷大的保留（`mesa/0015`、`mesa/0016`）

《无限暖暖》所用的虚幻引擎5移动端色调映射在计算抖动噪声的符号时使用`clamp(x * +Inf, -1, 1)`，`+Inf`在DXC生成的SPIR-V中是一个`OpConstant`（`0x7f800000`），而模块没有声明`SignedZeroInfNanPreserve`。按Vulkan规范，这种情况下实现可以假设浮点运算的操作数和结果都不是Inf或NaN。ROM所用的Mesa快照已包含MR !40291（`nir/search_helpers: assume float sources without preserve flag can't be inf/nan`），按取值范围匹配的代数优化因此使用这一假设：`x * +Inf`只可能是±Inf或NaN，在该假设下既算作“非负”也算作“非正”，`nir_opt_algebraic`把`fmax`和`fmin`都换成`fcanonicalize`，clamp被删去。噪声随之变为`-Inf * s + Inf = NaN`，并被加到全部颜色通道上，UNORM渲染目标把NaN存为0。其结果是《无限暖暖》（[issue #33](https://github.com/vahiru/gaokun-android/issues/33)）进入大世界后实时3D画面全黑，而之后绘制的UI正常。

上游MR !41661（`4df9d03`，已回移到26.1分支）让GLSL.std.450的`FMin`、`FMax`和`FClamp`生成的`fmin`、`fmax`总是带有`nir_fp_preserve_inf`，上述规则不再匹配，clamp得以保留；上游的说明指出Unity和虚幻引擎都依赖这一行为。`mesa/0015`是该提交的原样回移，基线升级到包含该提交的版本（26.1分支为26.1.1及以后）时删除。

同一假设也影响`nir_atan2()`：零输入时`frcp(0.0)`会产生中间的无穷大，而最终结果并不是无穷大，应用即使没有要求保留无穷大，结果也应当确定。上游MR !41953（`3a815a5`，26.1.3）在`nir_atan2()`的降级中保留无穷大与有符号零，`mesa/0016`是它的原样回移，基线包含该提交时删除。该提交的Tested-by来自Intel的开发者；在Turnip上，测试用的32位与mediump的`atan(y, x)`着色器生成的代码与回移前相同，尚未发现受影响的着色器。

排查：

- 全屏后处理在lavapipe等驱动上正常、在Turnip上输出为0或NaN时，先用`spirv-dis`检查其SPIR-V中是否有Inf或NaN常量，以及是否声明了`SignedZeroInfNanPreserve`。
- Turnip的反汇编（`IR3_SHADER_DEBUG=disasm`，Android上对应属性`debug.mesa.ir3.shader.debug`，输出到logcat）只在着色器重新编译时输出，命中磁盘缓存或管线缓存时不输出，且该属性在每个进程中只读取一次。Inf常量位于常量寄存器中，可在反汇编开头的立即数表里找到`0x7f800000`及其所在的寄存器（如`c13.w`），再检查与它相乘之后是否有对应的`max.f`和`min.f`。

## 九、原神的深度写回（`mesa/0017`）

原神在ANGLE上水面在部分帧中整片消失，灌木后方的物体在部分帧中显示到前方。`TU_DEBUG=sysmem`时画面正常，`TU_DEBUG=gmem`时水面每一帧都消失。由此推断：原神对深度缓冲调用`glInvalidateFramebuffer`之后，后面的渲染步骤仍要读取它（水面按场景深度计算透明度，物体之间的遮挡也依赖深度）；Mesa的OpenGL驱动也为原神忽略discard与invalidate（driconf选项`ignore_discard_framebuffer`，MR !21916）。按规范，invalidate之后的内容是未定义的。ANGLE把invalidate转换为`VK_ATTACHMENT_STORE_OP_DONT_CARE`，下一个使用这张图像的渲染步骤则为`VK_ATTACHMENT_LOAD_OP_DONT_CARE`。Turnip按分块渲染（GMEM，先在GPU片上缓存中绘制，再写回内存）执行时，深度不写回内存；按直接渲染（sysmem）执行时深度仍在内存中。两种方式由autotune按每个渲染步骤的历史统计选择，同一个步骤在不同帧中可能不同，所以画面时好时坏。

`mesa/0017`新增driconf选项`vk_dont_care_as_store`：应用传入的附件若使用`STORE_OP_DONT_CARE`，按`STORE_OP_STORE`处理。它与已有的`vk_dont_care_as_load`（`LOAD_OP_DONT_CARE`按`LOAD_OP_LOAD`处理）一起，只对原神的国际服`com.miHoYo.GenshinImpact`、国服`com.miHoYo.Yuanshen`与B服`com.miHoYo.ys.bilibili`打开（`00-mesa-defaults.conf`）。两个选项都只作用于应用传入的附件，多重采样渲染到纹理（MSRTSS）的resolve与unresolve判断也按转换后的值。Turnip自己设定读写方式的附件不受影响：MSRTSS内部附件，以及动态渲染中的resolve附件、fragment density map与shading rate附件和被MSRTSS替换的单采样附件。上游的`vk_dont_care_as_load`原本也会改写这些附件，而其中一些不在GMEM里，载入会落在GMEM偏移0处，覆盖GMEM中排在最前的附件（通常是第一个颜色附件）。

代价：原神中以`DONT_CARE`开始或结束的附件（深度与颜色）在分块渲染时也会载入与写回，多重采样附件会因此分配实际内存。未启用这两个选项的应用不受影响。

限制（未修正）：

- 经VkRenderPass的MSRTSS深度模板附件若只清除深度、模板为`LOAD_OP_DONT_CARE`，打开`vk_dont_care_as_load`后深度的清除会丢失。这是上游已有的问题；ANGLE在Turnip上使用动态渲染，不经过这条路径。
- 附件为`LOAD_OP_NONE`与`STORE_OP_DONT_CARE`时，打开`vk_dont_care_as_store`后，未初始化的GMEM内容会写回内存。ANGLE只把`LOAD_OP_NONE`与`STORE_OP_NONE`成对设置，不会出现这种组合。
- 动态渲染中只有深度的D24S8 MSRTSS附件可能被载入到GMEM偏移0处。这是上游已有的问题（应用本来就用`STORE_OP_STORE`时同样发生），`vk_dont_care_as_store`扩大了它的触发范围。原神是否使用MSRTSS（GLES的`EXT_multisampled_render_to_texture`）尚未确认。

排查：

- 画面某些元素时有时无、且`TU_DEBUG=sysmem`后消失时，先用`TU_DEBUG=gmem`确认是否每帧都出错；若是，问题多半在分块渲染下附件内容的载入与写回。
- driconf按进程名匹配。经ANGLE的游戏中Turnip运行在游戏进程里，进程名就是包名；其他游戏遇到同类问题时，可以在`00-mesa-defaults.conf`中照原神的条目添加。
