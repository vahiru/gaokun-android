# 游戏与应用环境兼容

本系统运行在主线Linux 7.2.9内核、UEFI解锁启动且内置KernelSU的环境中。部分手游和环境检测工具默认假定设备运行在Android通用内核（GKI 4.x–6.x）和手机的48位页表配置下。本页说明为游戏兼容性、低延迟和环境检测所做的跨层改动与取舍边界。

## 1. 改动与机制总览

| 方向 | 改动 | 解决的问题与取舍 | 位置 |
|---|---|---|---|
| 地址空间 | **39位用户虚拟地址窗口**（`patches/kernel/core/0006`） | 主线arm64内核默认给64位进程分配$2^{48}$地址空间，而部分Unity `il2cpp`游戏（如《英雄联盟手游》）假定用户态指针不超过$2^{39}$，启动即崩溃。补丁保持48位页表，将64位进程默认mmap窗口压到39位（`ARM64_USER_MMAP_WINDOW_39`、`ARCH_MMAP_RND_BITS_MAX=24`，与手机一致） | `kernel-apply-patches.sh`、`kernel-config-android.sh` |
| 内核版本 | **按应用UID伪装`uname`版本**（`patches/kernel/core/0008` + Parts设置） | 部分环境检测只接受4/5/6开头的Android通用内核版本，见到7.x直接判异常。只改变应用进程读到的版本号，系统组件仍读真实版本（见第2节） | `patches/kernel/core/0008`、`init.gaokun3.rc`、Parts `AppKernelSettingsActivity`、`crdroid-tree-fixes.py` |
| 着色器缓存 | **EGL多文件blob缓存**（`ro.egl.blobcache.multifile=true`） | ANGLE的程序与管线缓存容易超出默认的2 MiB单文件上限，导致大型3D游戏（如《鸣潮》）每次启动都重新编译着色器 | `device.mk` |
| 音频延迟 | **10 ms FastMixer低延迟通路**（`patches/android/hardware-interfaces/0008` + `crdroid-tree-fixes.py`） | 将应用输出延迟从约317 ms降到约114 ms（`ro.vendor.audio.primary.out_latency_ms=10`，`mixPeriod=10.00 ms`） | `device.mk`、`crdroid-tree-fixes.py` |
| 音频延迟 | **播放缓冲3个period**（`patches/android/hardware-interfaces/0011`） | ALSA播放缓冲由4个480帧的period减到3个，输出延迟再少约10 ms；录音方向仍为4个，不影响采集钩子与降噪（`ro.vendor.audio.primary.out_period_count=3`） | `device.mk`、`crdroid-tree-fixes.py` |
| 触摸响应 | **`game`触摸预设** | 关闭坐标平滑（减少约25 ms稳态滞后）、debounce设为1帧、关闭手掌位置图（避免贴屏幕边缘平放的拇指被当成手掌撤回）。可在"设置 → 显示 → 触摸模式"切换 | `bin/gaokun3-touch-mode.sh`，详见[`palm-rejection.md`](palm-rejection.md) |
| 调度 | **后台cpuset限在`0-3`** | 将`background`和`system-background`限制在4个能效核，避免后台任务与前台游戏争抢大核 | `device/huawei/gaokun3/` |

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
