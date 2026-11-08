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

## 三、GPU SMMU上下文故障失速（Stall-on-Fault）死锁

### 1. 硬件中断号失配与死锁链

SC8280XP的GPU SMMU（`3da0000.iommu`，`qcom,sc8280xp-smmu-500`）在`msm_iommu.c`初始化时默认开启上下文故障失速模式（`SCTLR.CFCFG = 1`）：当任一GPU事务发生地址翻译错误时，SMMU冻结该笔总线事务并拉高上下文中断线，等待内核的`adreno_smmu_fault_handler`执行`RESUME`（终止事务）并清中断。

然而在SC8280XP上，设备树为`gpu_smmu`声明的上下文中断号（GIC SPI 678/679）与硬件在故障时实际拉高的中断线（SPI 675/680，对应GIC INTID 707/712）不一致。因此一旦发生任何一次GPU页错误：

1. SMMU置起`FSR.SS = 1`（Stalled）并挂起GPU的AXI/AHB读事务，但CPU侧收不到中断，`adreno_smmu_fault_handler`永不执行。
2. 挂起的事务阻塞GPU的CX主总线（`CP_AHB_BUSY_CX_MASTER`）。
3. 随后GPU进入空闲休眠时，GMU固件尝试抽空总线但永远等不到空闲，报`HFI_H2F_MSG_GX_BW_PERF_VOTE`超时（`-110`），`cx gdsc`无法塌缩。
4. 内核尝试调用`a6xx_recover`复位GPU，但复位路径写寄存器同样走已冻结的AHB路径，最终导致GPU恢复工作队列永久卡死。

### 2. 处置与注意事项

- **关闭`SCTLR.CFCFG`**：清除已分配上下文银行（Context Bank）`SCTLR`寄存器的bit 7（`CFCFG`）并置位bit 8（`HUPCF`，Hit Under Previous Context Fault）。这样当发生非法地址访问时，SMMU直接向GPU返回总线错误（Bus Error）而非挂起流水线，GPU可正常完成硬件复位。
- **严禁遍历未实现的Context Bank**：`gpu_smmu`的`IDR1.NUMCB`（架构上限）虽为8或16，但硬件仅实现了2个上下文银行（`NCB = 2`，CB0与CB1）。访问CB2及以上的寄存器空间会立即触发同步外部中止（Synchronous External Abort），导致内核无日志死机。

## 四、10位视频（P010）零拷贝合成与构建脚本

- **P010零拷贝显示合成**（`patches/mesa/0002`）：
  - Iris硬件解码器在解码10-bit HEVC/VP9视频时输出`HAL_PIXEL_FORMAT_YCbCr_P010`。
  - `minigbm_msm`为`DRM_FORMAT_P010`开放`BO_USE_SCANOUT`用法，DPU驱动在`msm_dpu`层直接接收P010平面，避免SurfaceFlinger回退到GPU Shader做每帧格式转换，显著降低4K 10-bit视频播放时的GPU负载与功耗。
- **Mesa构建工具链**：
  - Android 16使用Mesa树内`meson_to_hermetic`生成`Android.bp`，由`scripts/mesa-tool-fixes.py`与`scripts/mesa-bp-merge.py`修补并合入；同时为`external/deqp-deps/glslang`补充Turnip编译器所需的宿主机`glslangValidator`目标。
  - Android 17切换至重写后的`meson2hermetic`（`build_android_bp.py`），具体适配步骤见[mesa-a17.md](mesa-a17.md)。


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
- GPU挂死时用Mesa的`crashdec -f`解码devcoredump（`/sys/class/devcoredump/devcd*/data`，5分钟后自动删除）；A690要先把dump里的`revision: 0 (06090000)`改成`revision: 690 (6.9.0.0)`。
