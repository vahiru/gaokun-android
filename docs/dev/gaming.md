# 游戏与应用环境兼容

本系统运行在主线Linux 7.2.9内核、UEFI解锁启动且内置KernelSU的环境中。部分手游和环境检测工具默认假定设备运行在Android通用内核（GKI 4.x–6.x）和手机的48位页表配置下。本页说明为游戏兼容性、低延迟和环境检测所做的跨层改动与取舍边界。

## 1. 改动与机制总览

| 方向 | 改动 | 解决的问题与取舍 | 位置 |
|---|---|---|---|
| 地址空间 | **39位用户虚拟地址窗口**（`patches/kernel/core/0006`） | 主线arm64内核默认给64位进程分配$2^{48}$地址空间，而部分Unity `il2cpp`游戏（如《英雄联盟手游》）假定用户态指针不超过$2^{39}$，启动即崩溃。补丁保持48位页表，将64位进程默认mmap窗口压到39位（`ARM64_USER_MMAP_WINDOW_39`、`ARCH_MMAP_RND_BITS_MAX=24`，与手机一致） | `kernel-apply-patches.sh`、`kernel-config-android.sh` |
| 内核版本 | **按应用UID伪装`uname`版本**（`patches/kernel/core/0008` + Parts设置） | 部分环境检测只接受4/5/6开头的Android通用内核版本，见到7.x直接判异常。只改变应用进程读到的版本号，系统组件仍读真实版本（见第2节） | `patches/kernel/core/0008`、`init.gaokun3.rc`、Parts `AppKernelSettingsActivity`、`crdroid-tree-fixes.py` |
| 图形接口 | **按应用在GL_VERSION中报告高通驱动版本**（`patches/android/frameworks-base/0003` + Parts） | 虚幻引擎只在GL_VERSION里读到不低于下限的高通驱动版本（`V@<版本>`）时，才给Adreno 6xx选择开启Vulkan的设备档案；ANGLE的GL_VERSION不含驱动版本，只带Vulkan着色器的游戏（如《无限暖暖》）因此启动即退出。在"设置 → 系统 → GPU驱动兼容"中选定的应用启动时，`GraphicsEnvironment`设置环境变量`ANGLE_GL_VERSION`，使其读到`OpenGL ES 3.2 V@0762.10 (ANGLE 2.1)`；名单存于`Settings.Global`的`gaokun3_angle_gl_version_pkgs`，经`CoreSettingsObserver`传给应用，默认为空。未采用全局属性`debug.angle.gl_version`：Edge等Chromium浏览器自带的ANGLE也会读到它，把WebGL 1的ES 2.0上下文报告为3.2，在Edge上实测WebGL 1因此无法使用 | `crdroid-tree-fixes.py`、Parts `GlDriverVersionSettingsActivity` |
| 图形接口 | **SPIR-V的`FMin`、`FMax`与`FClamp`保留无穷大**（`patches/mesa/0015`，上游回移） | 《无限暖暖》所用的虚幻引擎5移动端色调映射用`clamp(x * +Inf, -1, 1)`求符号，着色器没有声明`SignedZeroInfNanPreserve`；Mesa依照规范允许的做法假设不会出现无穷大，删去了这个clamp，输出变为NaN，3D画面全黑。上游的修正让这三种运算总是保留无穷大（详见[`gpu-turnip.md`](gpu-turnip.md)） | `patches/mesa/0015`、`scripts/mesa-a17/regen.sh` |
| 图形接口 | **原神的深度数据在分块渲染下也写回**（`patches/mesa/0017`） | 原神在深度缓冲被invalidate之后仍要读取它（由`TU_DEBUG=sysmem`与`gmem`的对比推断）。经ANGLE时这变成`STORE_OP_DONT_CARE`，Turnip按分块渲染执行的步骤不把深度写回内存，水面在部分帧中整片消失，灌木后方的物体在部分帧中显示到前方。补丁新增driconf选项`vk_dont_care_as_store`，与`vk_dont_care_as_load`一起只对原神打开（详见[`gpu-turnip.md`](gpu-turnip.md)） | `patches/mesa/0017`、`scripts/mesa-a17/regen.sh` |
| 图形接口 | **ANGLE初始化着色器变量**（`debug.angle.feature_overrides_enabled=forceInitShaderVariables`） | 部分应用的着色器读取未赋值的变量，或在某些路径上不写输出就结束，按GLSL ES规范得到的值未定义；在ANGLE + Turnip上得到的是寄存器中的残留值，例如小米天气的背景整片为黑色。ANGLE转换着色器时把输出变量、未赋值的局部变量和函数的out参数设为0；正常着色器中这些初始化通常会被随后的写入覆盖并由编译器删除。应用用`glProgramBinary`保存的旧程序不会自动重新转换（详见[`gpu-turnip.md`](gpu-turnip.md)） | `device.mk` |
| 着色器缓存 | **EGL多文件blob缓存**（`ro.egl.blobcache.multifile=true`） | ANGLE的程序与管线缓存容易超出默认的2 MiB单文件上限，导致大型3D游戏（如《鸣潮》）每次启动都重新编译着色器 | `device.mk` |
| 音频延迟 | **5 ms FastMixer低延迟通路**（`patches/android/hardware-interfaces/0008` + `patches/kernel/audio/0013` + `crdroid-tree-fixes.py`） | 原来85 ms一块时没有FastMixer，游戏的低延迟请求批不下来，应用输出延迟约317 ms。播放块现为5 ms（`ro.vendor.audio.primary.out_latency_ms=5`，`mixPeriod=5.00 ms`），按块数申请缓冲的游戏（如PJSK申请两个块），轨道缓冲也随之缩小；内核`q6apm-dai`的播放period步长同时改为240帧，否则ALSA缓冲不会跟着缩短 | `device.mk`、`kernel-apply-patches.sh`、`crdroid-tree-fixes.py` |
| 音频延迟 | **播放缓冲3个period**（`patches/android/hardware-interfaces/0011`） | ALSA播放缓冲切成3个period（3 × 240帧 = 15 ms）；录音方向仍为4个，不影响采集钩子与降噪（`ro.vendor.audio.primary.out_period_count=3`）。与上一行合计：按PJSK的申请方式，应用按时间戳算出的输出延迟中位约21.5 ms（10 ms块、3 × 480帧时为43.7 ms），扬声器和有线耳机相同 | `device.mk`、`crdroid-tree-fixes.py` |
| 触摸响应 | **`game`触摸预设** | 关闭坐标平滑（减少约25 ms稳态滞后）、debounce设为1帧、关闭手掌位置图（避免贴屏幕边缘平放的拇指被当成手掌撤回）。可在"设置 → 显示 → 触摸模式"切换 | `bin/gaokun3-touch-mode.sh`，详见[`palm-rejection.md`](palm-rejection.md) |
| 内存 | **zram交换空间**（`mmd.zram.enabled=true`） | 没有交换空间时，内存压缩只能回收文件页，进程正在执行的代码页也会被逐出；压缩全部系统进程时会反复逐出SurfaceFlinger、audioserver、音频HAL的代码页，造成约1秒的画面剧烈掉帧和扬声器爆音。现由mmd在开机完成时配置zram（lz4，大小为内存的一半） | `device.mk` |
| 调度 | **后台cpuset限在`0-3`** | 将`background`和`system-background`限制在4个能效核，避免后台任务与前台游戏争抢大核 | `device/huawei/gaokun3/` |
| 调度 | **Power HAL（lineage-libperfmgr）** | 触摸、启动应用、游戏加载时提高top-app的uclamp下限，HWUI等的ADPF会话按帧耗时调整线程的uclamp；RT任务不再一运行就把整簇CPU拉到最高频率 | `device/huawei/gaokun3/power/`，详见[`power/README.md`](../../device/huawei/gaokun3/power/README.md) |
| 内存 | **开机后不自动压缩全部系统进程**（`patches/android/frameworks-base/0002`） | crDroid（取自CAF）原本在开机20分钟后把全部系统进程的内存回收一遍，SurfaceFlinger、音频服务正在使用的页面被逐出，正在玩游戏时会出现约1秒的剧烈掉帧和爆音。AOSP和LineageOS的正式版本不会自动压缩系统进程，补丁与之一致 | `crdroid-tree-fixes.py` |

## 2. 按应用UID伪装内核版本（`patches/kernel/core/0008`）

在1.0.0-rc.2和rc.3中，旧版`patches/kernel/core/0008`直接覆写全局`init_uts_ns`，导致全系统都读到`6.6.30-android15-8-g0a4d3c12b3`。但这会让系统组件也误以为运行在6.6内核上（例如crDroid 17的`netd`在读到6.6版本时会启用未在主线7.2上验证过的L4S BPF路径，`BpfHandler.cpp:174-178`），在`adb shell`和系统设置中也看不到真实内核版本。

因此现行方案改为**仅对应用UID返回设定的版本串**，可在"设置 → 系统 → 内核版本伪装"中开关或修改，重启后生效：

* **内核作用域**：`override_release()`仅对`uid % 100000 ≥ 10000`（含多用户与隔离进程，不含overflow UID 65534）返回`kernel.app_osrelease`；其他进程仍读真实`UTS_RELEASE`。`/proc/version`和`/proc/sys/kernel/osrelease`由SELinux拦截，应用只能通过`uname()`读取。
* **Zygote `os.version`刷新**：由于zygote以root（UID 0）启动并在预加载时缓存Java的`os.version`系统属性，直接fork出的应用进程如果读`System.getProperty("os.version")`仍会拿到真实版本。`crdroid-tree-fixes.py`在应用进程专用化（specialize）后重新调用`uname()`刷新`os.version`，且不再设置可读的`ro.kernel.version`。
* **属性与模块边界**：`persist.sys.gaokun3.app_kernel_*`使用独立的SELinux类型`gaokun3_app_kernel_prop`，由Parts写入、`vendor_init`在`zygote-start`前写入sysctl，普通应用不可读。内核编译期的`LINUX_VERSION_CODE`、模块`vermagic`、`linux_banner`（`dmesg`首行）均保持真实版本不变。

## 3. 环境检测的取舍边界与会话脚本

部分游戏和反作弊框架会做环境检测。我们的边界原则是**只修正移植遗留的不一致与主线内核缺失的AOSP隐私边界，不伪造硬件证明链，也不单独维护两套发版内核**：

* **纳入镜像的如实修正**：
  * 补齐主线内核缺失的AOSP netlink隐私边界（`patches/kernel/core/0007`：阻止普通应用通过`RTM_GETLINK` / `RTM_GETNEIGH`读取物理MAC与局域网邻居表）；
  * 对齐`Build.FINGERPRINT`尾部的变体段与`ro.build.type=user`（`crdroid-tree-fixes.py`）；
  * 移除不再使用的SwiftShader `execmem` SELinux规则（硬件Vulkan已由turnip提供）。
* **不做的事**：
  * **不伪造硬件证明链（Bootloader / TEE）**：本机依靠开放的UEFI启动链运行，`/proc/cmdline`中的`verifiedbootstate=orange`、`flash.locked=0`和`ro.crypto.state=unsupported`均为客观事实，不在系统或内核中篡改。
  * **不拆分"无root游戏内核"**：之前曾评估过拆分默认内核与游戏内核，但维护两套内核与启动入口选核机制复杂度过高，因此发布版本只维护一套内核。
* **会话级调试脚本（`scripts/game-mode.sh`）**：
  * 在开发机上测试对adb、开发者选项、自定义ROM属性或root管理器敏感的游戏时，可在宿主机运行`bash scripts/game-mode.sh on [分钟]`、`hide`、`noforge`做可逆的会话级清理与自动恢复。子命令说明与cgroup / `adb_enabled`等实现约束见[`scripts/game-mode.sh`](../../scripts/game-mode.sh)与[`scripts/game-mode-device.sh`](../../scripts/game-mode-device.sh)文件头注释。
