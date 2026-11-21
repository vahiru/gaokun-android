# Power HAL（lineage-libperfmgr）

本机使用Lineage的`android.hardware.power-service.lineage-libperfmgr`（`hardware/lineage/interfaces/power-libperfmgr`，与Pixel共用`hardware/google/pixel`的libperfmgr），配置为本目录的`powerhint.json`，安装到`/vendor/etc/powerhint.json`。接入见`device.mk`的Power HAL一节、`BoardConfig.mk`的sepolicy一节和`init.gaokun3.rc`的`sys.boot_completed`触发器。

## 配置

配置以Pixel 6 Pro的mainline配置（`powerhint-raven-mainline.json`，不依赖Pixel私有调度节点的那一份）为骨架，只使用主线内核的接口：cpufreq的频率上下限、`/dev/cpuctl/top-app`的`cpu.uclamp.min`、GPU devfreq的`min_freq`和`sched_util_clamp_min_rt_default`。各节点按`Values`的顺序决定优先级，多个提示同时生效时取序号最小的值。

| 提示 | 动作 | 依据 |
|---|---|---|
| `LAUNCH`、`GAME_LOADING` | top-app的`uclamp.min`为67.58%，解除CPU频率上限，最长5秒（启动完成时由框架结束） | Pixel 6 Pro的完整配置（`powerhint-raven.json`）在LAUNCH时把top-app的uclamp提到692/1024，mainline配置只解除频率上限 |
| `INTERACTION` | top-app的`uclamp.min`为30% | 触摸时由框架发出；本机无显示空闲节点（`vendor.powerhal.disp.idle_support=false`），HAL按固定时长处理（1.4–5.65秒） |
| `EXPENSIVE_RENDERING` | GPU最低频率410 MHz | 对应Pixel的GPU最低频率提示 |
| `FIXED_PERFORMANCE` | CPU与GPU频率下限都拉到最高 | 基准测试用 |
| `LOW_POWER`（省电模式） | 小核、大核频率上限1209.6 MHz、1286.4 MHz | Windows的LowPower配置档把两类核心的最高性能限为50% |

- ADPF沿用Pixel 6 Pro的120 Hz配置档（只保留一份，名为`Default`）。会话的uclamp以算力为单位，按比例生效，与具体频率无关。HWUI的会话需要`debug.hwui.use_hint_manager=true`（`device.mk`）。
- `sched_util_clamp_min_rt_default`在HAL启动节点写入线程时写为0。主线内核的默认值为1024：RT任务（SurfaceFlinger、输入、音频FastMixer、内核中断线程等）每次运行都把所在CPU簇拉到最高频率，这份提频也顺带惠及同簇的应用线程。改为0之后，由上面的INTERACTION与ADPF给需要的线程提频。HAL未启动时，这个值保持1024。
- 频率值只用`9999999`（不设上限或拉到最高）、`0`和不高于2688 MHz的频点，3.0 GHz版本的大核同样适用；省电模式的上限在3.0 GHz版本上约为最高频率的43%。

## 已知限制

- Chromium内核浏览器（Edge）的网页内容未建立ADPF会话，滚动时只靠触摸提示。
- RT任务不再把整簇CPU拉到最高频率后，空闲一段时间再突然滑动时，超过帧期限的帧约为原来的两倍。如需调整，可提高`INTERACTION`的top-app下限或ADPF的`UclampMin_Init`。
- HAL会用到AOSP的`task_profiles.json`中不存在的任务配置：启动时的`PreferIdleSet`（两条警告），以及ADPF会话增减线程时的`SCHED_QOS_*`（每次一条警告和一条错误日志）。这些调用只是不生效，不影响uclamp的调整。
