# Power HAL（lineage-libperfmgr）

本机使用Lineage的`android.hardware.power-service.lineage-libperfmgr`（`hardware/lineage/interfaces/power-libperfmgr`，与Pixel共用`hardware/google/pixel`的libperfmgr），配置为本目录的`powerhint.json`，安装到`/vendor/etc/powerhint.json`。接入见`device.mk`的Power HAL一节、`BoardConfig.mk`的sepolicy一节和`init.gaokun3.rc`的`sys.boot_completed`触发器。

## 配置

配置以Pixel 6 Pro的mainline配置（`powerhint-raven-mainline.json`，不依赖Pixel私有调度节点的那一份）为骨架，只使用主线内核的接口：cpufreq的频率上下限、`/dev/cpuctl/top-app`的`cpu.uclamp.min`、`/dev/cpuctl/background`与`foreground_window`的`cpu.uclamp.max`、GPU devfreq的`min_freq`和`sched_util_clamp_min_rt_default`。各节点按`Values`的顺序决定优先级，多个提示同时生效时取序号最小的值。

| 提示 | 动作 | 依据 |
|---|---|---|
| `LAUNCH`、`GAME_LOADING` | top-app的`uclamp.min`为67.58%，解除CPU频率上限，最长5秒（启动完成时由框架结束） | Pixel 6 Pro的完整配置（`powerhint-raven.json`）在LAUNCH时把top-app的uclamp提到692/1024，mainline配置只解除频率上限 |
| `INTERACTION` | top-app的`uclamp.min`为30% | 触摸时由框架发出；本机无显示空闲节点（`vendor.powerhal.disp.idle_support=false`），HAL按固定时长处理（1.4–5.65秒） |
| `EXPENSIVE_RENDERING` | GPU最低频率410 MHz | 对应Pixel的GPU最低频率提示 |
| `FIXED_PERFORMANCE` | CPU与GPU频率下限都拉到最高 | 基准测试用 |
| `GAME` | 把`vendor.gaokun3.game`设为1，结束后恢复为0（`GameActive`节点，`Property`类型） | 框架在声明为游戏类别或已加入游戏空间列表的应用位于前台时发出（`GameManagerService`），离开游戏、熄屏时结束，框架开机完成时也会结束一次；触摸模式（`etc/touchmode.rc`）与温控HAL（`thermal/`的`.rc`）据此在游戏期间自动关闭防误触、开启性能模式（Parts中的两个开关）。HAL启动节点写入线程之前，这个属性由`init.gaokun3.rc`先设为0 |
| `LOW_POWER`（省电模式） | 小核、大核频率上限1209.6 MHz、1286.4 MHz | Windows的LowPower配置档把两类核心的最高性能限为50% |
| `AC_POWER`（接电源） | 小核、大核频率下限1209.6 MHz、1056 MHz；解除`background`与`foreground_window`分组的频率上限 | Windows接电源时两类核心的最低性能为49%、39%（MinPerformance、MinPerformance1），这里按2688 MHz的大核取不低于它的频点 |

- ADPF沿用Pixel 6 Pro的120 Hz配置档（只保留一份，名为`Default`）。会话的uclamp以算力为单位，按比例生效，与具体频率无关。HWUI的会话需要`debug.hwui.use_hint_manager=true`（`device.mk`）。
- `sched_util_clamp_min_rt_default`在HAL启动节点写入线程时写为0。主线内核的默认值为1024：RT任务（SurfaceFlinger、输入、音频FastMixer、内核中断线程等）每次运行都把所在CPU簇拉到最高频率，这份提频也顺带惠及同簇的应用线程。改为0之后，由上面的INTERACTION与ADPF给需要的线程提频。HAL未启动时，这个值保持1024。
- HAL还会给线程套用Pixel调度器的任务配置：启动时的`PreferIdleSet`，以及ADPF会话增减线程时的`SCHED_QOS_NONE`、`SCHED_QOS_SENSITIVE_STANDARD`、`SCHED_QOS_SENSITIVE_EXTREME`。Pixel上这几项写入其内核私有调度模块的`/proc/vendor_sched/sched_qos/`，主线内核没有等价接口。本目录的`task_profiles.json`（安装到`/vendor/etc/task_profiles.json`）把这几项声明为不含动作的空配置，套用时直接成功，不再记录警告和错误日志。
- 用电池时（没有`AC_POWER`）的默认值同样按Windows的设置：
  - 小核的频率下限为691.2 MHz，大核不设下限：Windows用电池时两类核心的最低性能为25%、0%。
  - 后台分组`/dev/cpuctl/background`的`cpu.uclamp.max`为32.03%：后台进程本来就只用小核（`init.gaokun3.rc`的cpuset设置），由它们决定频率时小核最高为902.4 MHz。Windows用电池时把低QoS（后台）线程限制在小核902 MHz、大核825 MHz，并只用小核。
  - 多窗口中离顶层较远的可见自由窗口，以及以`BIND_IMPORTANT`绑定到它们的服务进程（框架把它们放进`/dev/cpuctl/foreground_window`；分屏的两侧与最近使用的自由窗口在top-app）的`cpu.uclamp.max`为80.66%，大核最高为2169.6 MHz：Windows用电池时把中等QoS（可见但不在焦点的窗口）线程的大核限制在2169 MHz。按小核的算力，同一数值也把小核限制在2342.4 MHz，Windows不限制这部分。这个值不高于小核的算力，单个线程负载升高时，调度器不会因小核容不下而把它迁到大核；Windows则让这类线程优先使用大核。两者的性能上限相近。
  - uclamp以算力为单位：32.03%（328/1024）与80.66%（826/1024）按小核与大核的算力换算为上面的频点。这是软上限：每一簇核心共用一个频率，同簇任一核心上不受限的线程需要更高频率时，受限的进程也以这一频率运行。同一核心上有不受限的任务与受限的进程同时可运行时，上限要按内核的20个uclamp桶（`UCLAMP_BUCKETS_COUNT`，见`scripts/kernel-config-android.sh`）才能在前者离开后恢复：5个桶时80.66%与默认的100%在同一个桶里，桶里还有可运行的任务时一直按100%计。3.0 GHz版本的小核算力较低，后台分组的上限对应1017.6 MHz；`foreground_window`的上限对应大核2496 MHz，不限制小核。
- Windows现代待机静默完成后的睡眠阶段（Standby配置档）还会进一步限制频率，这里没有采用：Android接电源时不进入Doze；用电池时框架在Doze的维护窗口里会撤销`DEVICE_IDLE`，限制只会落在维护窗口之间运行的推送、闹钟，以及熄屏播放与通话上（Doze不考虑音频），而且亮屏后要到解锁才退出Doze。
- `AC_POWER`不是框架发出的提示，由温控HAL（`thermal/Thermal.cpp`）按`gaokun-ec-adapter`的`online`通过Power HAL的扩展接口（`IPowerExt`）开关；接电脑的USB口时`online`同样为1。电源状态变化后2–4秒生效。熄屏后下限仍然生效：配置包没有熄屏阶段的ScreenOff配置档，Windows在熄屏后、进入睡眠阶段前沿用Default的设置。温控把频率上限压到这个下限以下时，内核以上限为准，过热降频不受影响。
- 频率值只用`9999999`（不设上限或拉到最高）、`0`和不高于2688 MHz的频点，3.0 GHz版本的大核同样适用；省电模式的上限在3.0 GHz版本上约为最高频率的43%，接电源时大核的下限约为35%。

## 已知限制

- Chromium内核浏览器（Edge）的网页内容未建立ADPF会话，滚动时只靠触摸提示。
- RT任务不再把整簇CPU拉到最高频率后，空闲一段时间再突然滑动时，触摸后的第一帧比示例HAL慢。接电源时由`AC_POWER`的频率下限弥补；用电池时的情况尚未实测，如需调整，可提高`INTERACTION`的top-app下限或ADPF的`UclampMin_Init`。
