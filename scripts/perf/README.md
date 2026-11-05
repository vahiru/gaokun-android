# 性能、待机与长稳采样工具

本目录下的脚本全部为**只读采样**：不修改系统属性、不改刷新率、不启停服务、不执行`--latency-clear`或`timestats -clear`。唯一的文件写入是待机采样器向设备`/data/local/tmp/gk3-standby/`目录追加采样日志。回归执行时机与通过判据见[`docs/dev/release-checklist.md`](../../docs/dev/release-checklist.md)。

| 脚本 | 运行位置 | 用途 |
|---|---|---|
| `game-perf.sh` | 宿主机（adb） | 游戏在前台运行时每0.5秒抓取SurfaceFlinger帧时间，每5秒记录一行CSV（CPU/GPU频率驻留、温度、温控压频、电池），结束时汇总平均fps、1% low与最高温度 |
| `standby.sh` | 宿主机（adb） | 控制设备端待机采样器的`once` / `start` / `status` / `pull` / `stop` |
| `standby-sampler.sh` | 设备（root，常驻） | 每次唤醒（`suspend_stats/success`增加）追加一行时间、`suspend_stats`、电池、`qcom_stats`各项与`boot.reason.history`；每30分钟记录一行关键进程PID/RSS/fd与dropbox计数心跳 |

## 游戏性能采样

```bash
# 解锁并进入游戏固定场景后执行：
SER=gaokun3 bash scripts/perf/game-perf.sh -p com.tencent.tmgp.dfm -t 1200     # 三角洲行动20分钟
SER=gaokun3 bash scripts/perf/game-perf.sh -p com.idreamsky.klbqm -t 600        # 卡拉彼丘10分钟
SER=gaokun3 bash scripts/perf/game-perf.sh -p com.hypergryph.arknights -t 1800  # 明日方舟30分钟
```

产物保存在`out/perf/<包名>-<时间>/`，包含`header.txt`（构建戳、刷新率、帧率上限相关属性、图层frameRate投票）、`samples.csv`、`frames.txt`与`summary.txt`。

* **fps计算**：`dumpsys SurfaceFlinger --latency <图层>`仅保留FrameTimeline最近64个合成帧（crDroid启用了`deprecate_frame_tracker`；旧路径为128帧），在120 Hz下约合0.53秒。脚本以`sleep 0.2`轮询并按上屏时间戳去重拼接；若两次采样间无重叠帧，则在`frames.txt`记`GAP`、CSV的`gap`列记1，跨GAP间隔不计入帧时间分布。结束时另用`--timestats -dump`的`totalFrames`增量交叉核对整段平均帧率。
* **1% low**：取最慢1%帧的平均帧时间换算为fps，同时输出p99帧时间与最大单帧耗时。
* **GPU频率驻留**：msm驱动不导出GPU忙闲比，脚本统计`trans_stat`各频点累计时长增量占比，需结合fps一起解读。
* **CPU频率驻留**：取自`policyN/stats/time_in_state`（含idle时间），代表频点驻留比例而非CPU利用率。
* **帧率上限属性**：解读fps前需先核对`header.txt`中`ro.surface_flinger.game_default_frame_rate_override=60`与`debug.graphics.game_default_frame_rate.disabled=true`的实际生效情况。

## 待机与长稳采样

```bash
SER=gaokun3 bash scripts/perf/standby.sh once     # 单次只读查看采样字段（不写设备）
SER=gaokun3 bash scripts/perf/standby.sh start    # 插线启动采样器；会提示当前allow_suspend值
# 拔除USB线息屏待机（8小时）或日常使用（72小时）
SER=gaokun3 bash scripts/perf/standby.sh pull     # 插回USB后取回日志并生成摘要
SER=gaokun3 bash scripts/perf/standby.sh stop
```

* 采样器依赖`sleep`（`CLOCK_MONOTONIC`，系统挂起时暂停计时）轮询`suspend_stats/success`，**自身不会唤醒设备**；唤醒不足5秒即再次休眠的短唤醒会合并至下一条记录（`ss_ok`为累计成功挂起次数）。
* 每行写入后立即`sync`，确保断电或异常复位前的数据已写入磁盘。采样器不跨重启存活；`pull`会一并读取当前`persist.sys.boot.reason.history`以区分异常复位与正常关机。
* **连接USB主机时SoC无法进入CX塌缩**，`qcom_stats`的`cxsd` / `aosd` / `ddr`计数为0属预期行为，待机功耗测试必须拔除USB线。
* 开发机若将`persist.vendor.gaokun3.allow_suspend`设为0则不会挂起；脚本不会自动修改该属性。
* 日志中的`screen=`字段取自`debug.tracing.screen_state`（`Display.state`原始值）：**`2`为亮屏，`1`为息屏**。
* **采样期间请勿切换USB调试开关或执行`adb root` / `adb unroot`**：采样器由`setsid nohup`启动，可耐受拔除USB线，但仍位于`init`为`adbd`创建的进程组cgroup中；`adbd`重启时同cgroup进程会被一并终止且不留`stop`记录。
* 亮屏硬解1小时测试可通过`HB=300 SER=gaokun3 bash scripts/perf/standby.sh start`缩短心跳间隔。
* 唤醒源与持锁排查：每条记录包含`wl=`（`/sys/power/wake_lock`中的用户态锁，如`gaokun3_nosuspend`、`gaokun3_usbrole`）、`ws_act=`（当前活动的内核唤醒源）、`ws_top=`（累计`wakeup_count`前5名，首尾相减即为期间唤醒源）以及`lpass=`（`LPASS_HW_MACRO` / `DCODEC`投票计数）。解读`qcom_stats`时应同时关注睡眠累计时长占比与进入次数。
* 读取debugfs下的`qcom_stats`需要root权限；在发布构建上通过`su -c`执行，需预先在KernelSU管理器中为Shell授权。
