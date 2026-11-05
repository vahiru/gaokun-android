# 触摸手感的测量工具

本目录包含用于采集电容触摸网格、回放驱动触控跟踪算法以及对比A/B参数预设的工具。手掌防误触算法与参数说明见[`docs/dev/palm-rejection.md`](../../docs/dev/palm-rejection.md)。

| 文件 | 用途 |
|---|---|
| `dump-state.sh` | **一键取证**：一次抓取逐级计数器、触点出生记录、两张原始网格、54个算法参数、输入轴信息与IRQ计数 |
| `grid.py` | 将40×60电容网格渲染为可视化字符图；传入两个文件时并排对比 |
| `capture-ab.sh` | 在设备上运行：录制`/dev/input/event7`原始事件流，收满指定数据量后自动切换A/B预设 |
| `evdev-strokes.py` | 在宿主机运行：将原始事件解码为轨迹，统计轨迹数、每条帧数、速度分布与**相邻轨迹间隔** |
| `tracker-sim.c` | 在宿主机运行：`hx_track_contacts()`的逐行复刻，无需硬件即可验证跟踪器改动 |
| `gk3trec.c` | 在设备上运行：将驱动处理后的网格（加`-r`时含原始网格）以及触摸屏、手写笔evdev事件录制进同一文件，统一使用`CLOCK_MONOTONIC`时间戳 |
| `hxsim/` | 在宿主机运行：直接编译内核树中的`hx-algo.c`生成离线回放器，逐帧输出连通域、峰值、触点与跟踪结果 |
| `trec.py` | 读取录制文件；`summary`子命令列出标记与每段活动的起止时间 |
| `simcmp.py` | 核对离线回放结果与设备录制时的实际上报是否一致 |
| `outcomes.py` | 按时间段统计到达应用的触摸与被撤回的触摸，用于对比算法改动前后的效果 |

## 驱动侧可观测接口（`patches/0043`、`patches/0082`）

| 路径 | 内容 |
|---|---|
| `/sys/bus/spi/devices/spi0.0/algo/stats` | 30个逐级计数器；写入任意值清零 |
| `…/algo/contacts_log` | 最近16个触点出生时的`seq x y area signal edge` |
| `/sys/kernel/debug/himax-hx83121a/frame_raw` | 面板产出的40×60 `s16`原始网格（仅去基线） |
| `/sys/kernel/debug/himax-hx83121a/frame` | 经过CMF、边缘增强与IIR滤波后的40×60网格 |
| `…/algo/hand_*` | 手掌位置图参数（`patches/0082`），计数器位于`stats`中，详见[`docs/dev/palm-rejection.md`](../../docs/dev/palm-rejection.md) |

* 并排对比`frame_raw`与`frame`可区分信号噪声问题与后处理算法问题。
* 读取二进制网格帧时必须使用`adb exec-out`而**不可使用`adb shell`**（后者会将`\n`转换为`\r\n`破坏二进制数据）。`dump-state.sh`已使用`exec-out`并校验4800字节长度。

## 录制与回放（`patches/0082`）

修改触控或防误触算法时，使用同一份实机录制做离线回放对比，比每次人工重滑更可复现。`gk3trec`在设备上录下网格与输入事件，`hxsim`在宿主机上将网格喂给同一份`hx-algo.c`重跑算法。

编译：

```sh
aarch64-linux-gnu-gcc -O2 -static -o gk3trec scripts/touch/gk3trec.c   # 静态链接，不依赖设备libc
bash scripts/touch/hxsim/build.sh /path/to/linux                        # 打过本仓补丁的内核树
```

在设备上录制（需root，在一个shell中启动录制器，另一个shell中打标记并保存参数）：

```sh
/data/local/tmp/gk3trec -r -o /data/local/tmp/rec.bin

echo palm > /data/local/tmp/gk3trec.mark      # 写入标记，录制器读取后自动删除该文件
echo stop > /data/local/tmp/gk3trec.mark      # 结束录制
cd /sys/bus/spi/devices/spi0.0/algo &&
	for f in *; do case $f in stats|contacts_log) ;; *) echo "$f=$(cat $f)" ;; esac; done > /data/local/tmp/params.txt
```

在宿主机回放：

```sh
python3 scripts/touch/trec.py summary rec.bin               # 查看标记与活动时间段（秒）
./hxsim -p params.txt rec.bin > sim.txt
python3 scripts/touch/simcmp.py rec.bin sim.txt             # 确认回放与实机上报一致
./hxsim -p params.txt -s hand_enabled=0 rec.bin > off.txt
python3 scripts/touch/outcomes.py rec.bin "palm:30-60,writing:70-120" off.txt sim.txt
```

回放说明：

* 评估点击漂移时使用`-r -F`。录制文件不包含固件手指标志，不加`-F`时回放器按持续有手指处理，会多出设备上被清零的抬手末帧；`-F`将全零网格视为手指离开。
* 回放默认直接读取录下的处理后网格，跳过预处理（基线、CMF、边缘增强、IIR）。加`-r`时改从原始网格重跑预处理，每帧`F`行末列显示与设备网格不一致的格数。
* 参数与算法和录制时完全一致时，`simcmp.py`报告的一致率应接近100%；使用新算法回放旧录制时，应对比改动前后两次离线回放的`outcomes.py`输出。
* 录制器会跳过空闲无变化的网格，回放时超过30毫秒的间隔会自动补齐空帧，以保证按帧计数的跟踪器与手掌位置图状态一致。
* `-s 名字=值`可在`-p`参数文件之后覆盖单个参数；导出`params.txt`时需排除`stats`与`contacts_log`。

## 事件流采集与A/B对比

避免使用`timeout N getevent > 文件`（`getevent`重定向到文件时使用全缓冲，被`timeout`终止时会丢失缓冲数据），一律使用`cat /dev/input/eventX`录制原始二进制流后用`evdev-strokes.py`离线解码：

```sh
adb push scripts/touch/capture-ab.sh /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/capture-ab.sh
adb shell 'nohup /data/local/tmp/capture-ab.sh >/dev/null 2>&1 &'
# 正常滑动屏幕，收满指定字节数后脚本自动切换A/B预设并退出
adb pull /data/local/tmp/ts_A.bin . && adb pull /data/local/tmp/ts_B.bin .
python3 scripts/touch/evdev-strokes.py ts_A.bin ts_B.bin
```

运行前请先通过`grep -A9 -i himax /proc/bus/input/devices`核对触摸屏对应的`eventX`编号。

## 断触判据与阈值调优原则

* **使用相邻轨迹间隔而非短轨迹占比作为断触判据**：日常点按操作本身就会产生大量短轨迹，单纯统计短轨迹占比会产生误判。`evdev-strokes.py`默认计算相邻两条轨迹之间的时间间隔与**跨间隔隐含速度**：若手指真实抬起再按下，跨间隔隐含速度应接近0；若是一条连续滑动被算法截断，隐含速度会接近当时的手指滑行速度：

| 状态 | 间隔 <100 ms占比 | 跨间隔隐含速度 | 8秒连续快速甩动轨迹数 |
|---|---|---|---|
| 跳点检测修复前（`patches/0038`前） | 35% | 1.01 m/s（等于原限速阈值） | 87条 |
| 跳点检测修复后 | 0/45（最小间隔124 ms） | — | 6条 |

* **移动手指的信噪比低于静止手指**：快速划动的手指在单次扫描周期内被拖长，电容峰值显著低于静止按压（例如将`peak_threshold`从800抬高到1500时，峰值3000+的静止指尖不受影响，但快速甩动会被切成平均7帧一条的碎片，59%间隔小于100 ms、隐含速度2.61 m/s）。因此上调`macro_threshold`、`peak_threshold`或`iso_nbr_ratio_q8`等任何候选门槛后，**验收必须包含快速连续甩动**并用`evdev-strokes.py`检查间隔分布。
* **事件驱动采集**：采集脚本应按读取到的事件字节数触发切换与停止，而非按固定挂钟睡眠等待，避免空录空闲时段。
