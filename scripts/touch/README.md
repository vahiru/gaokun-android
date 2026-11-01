# 触摸手感的测量工具

★ **先读 [`docs/stage4-findings.md` #114](../../docs/stage4-findings.md)。**
这三个工具是那一仗留下的，解决的是同一个问题：**"手感不好"没法调试，除非先变成数字。**

| 文件 | 干什么 |
|---|---|
| `dump-state.sh` | **一键取证**。把计数器、触点出生记录、两张原始网格、54 个旋钮、轴信息、IRQ 计数一次抓全 |
| `grid.py` | 把 40×60 电容网格画成看得懂的图；给两个文件就并排比较 |
| `capture-ab.sh` | 在设备上跑。录 `/dev/input/event7` 原始流，**按收到多少数据**切换 A/B 预设 |
| `evdev-strokes.py` | 在本机跑。解码成轨迹：轨迹数 / 每条帧数 / 速度分布 / **相邻轨迹间隔**（见下） |
| `tracker-sim.c` | 在本机跑。`hx_track_contacts()` 的逐行复刻，不用硬件就能验证跟踪器的改动 |
| `gk3trec.c` | 在设备上跑。把驱动处理后的网格（加`-r`时还有原始网格）和触摸屏、笔的evdev事件录进同一个文件，时间戳统一用CLOCK_MONOTONIC |
| `hxsim/` | 在本机跑。用内核树里的`hx-algo.c`原样编译出回放器，把录下的网格重新跑一遍算法，逐帧输出连通域、峰值、触点和跟踪 |
| `trec.py` | 读录制文件；`summary`列出标记和每段活动的起止时间 |
| `simcmp.py` | 核对回放和设备当时的实际上报是否一致 |
| `outcomes.py` | 按时间段统计到达应用的触摸和被撤回的触摸，用来比较改动前后 |

## 驱动侧的可观测接口（内核 **#23** 起，`patches/0043`）

| 路径 | 内容 |
|---|---|
| `/sys/bus/spi/devices/spi0.0/algo/stats` | 30 个逐级计数器。**写任意值清零** |
| `…/algo/contacts_log` | 最近 16 个触点**出生时**的 `seq x y area signal edge` |
| `/sys/kernel/debug/himax-hx83121a/frame_raw` | 面板**产出**的 40×60 s16 网格（仅去基线）|
| `/sys/kernel/debug/himax-hx83121a/frame` | 流水线**判定**的同一张网格（CMF/边缘增强/IIR 之后）|
| `…/algo/hand_*` | 手的位置图的参数（`patches/0082`），计数器在`stats`里，见[`docs/palm-rejection.md`](../../docs/palm-rejection.md) |

★ 并排看两张网格是**区分"信号问题"与"算法问题"**的唯一办法。
★ 空载读 `frame_raw` 是回答**"固件到底做不做逐像素基线跟踪"**的唯一办法。

⚠️ 整帧要用 `adb exec-out`，**不能用 `adb shell`** —— 后者会把 `\n` 变成 `\r\n`，
把二进制帧毁掉。`dump-state.sh` 已经处理了，并且会校验是不是 4800 字节。

## 录制与回放（`patches/0082`起）

改算法时，用同一份录制比较改动前后，比每次重新上手试更可靠，也能量出差别。`gk3trec`在设备上录下网格和事件，
`hxsim`在本机把网格重新喂给同一份`hx-algo.c`，输出和驱动当时一样的触点与跟踪。

编译：

```sh
aarch64-linux-gnu-gcc -O2 -static -o gk3trec scripts/touch/gk3trec.c   # 静态链接，不依赖设备上的libc
bash scripts/touch/hxsim/build.sh /path/to/linux                        # 打过patches/的内核树
```

在设备上录制，需要root。在一个adb shell里运行录制器，另开一个shell打标记、结束录制，再把录制时的参数存下来：

```sh
/data/local/tmp/gk3trec -r -o /data/local/tmp/rec.bin

echo palm > /data/local/tmp/gk3trec.mark      # 记一个标记，录制器读到后删掉这个文件
echo stop > /data/local/tmp/gk3trec.mark      # 结束录制
cd /sys/bus/spi/devices/spi0.0/algo &&
	for f in *; do case $f in stats|contacts_log) ;; *) echo "$f=$(cat $f)" ;; esac; done > /data/local/tmp/params.txt
```

在本机回放：

```sh
python3 scripts/touch/trec.py summary rec.bin               # 标记和每段活动的时间（秒）
./hxsim -p params.txt rec.bin > sim.txt
python3 scripts/touch/simcmp.py rec.bin sim.txt             # 先确认回放和实机一致
./hxsim -p params.txt -s hand_enabled=0 rec.bin > off.txt
python3 scripts/touch/outcomes.py rec.bin "palm:30-60,writing:70-120" off.txt sim.txt
```

几点说明：

- 比较点击漂移时用`-r -F`。录制里没有固件的手指标志，不加`-F`时回放按“一直有手指”算，多出设备上被清零的抬手最后一帧，只看距离的规则漂移会被高估。`-F`把全零的网格当作标志消失。
- 回放默认直接用录下的处理后网格，跳过预处理（基线、CMF、边缘增强、IIR）。`-r`改用原始网格，把预处理也重跑一遍，
  这时每帧`F`行的最后一列是与设备网格不同的格数。只改了预处理之后的步骤时，用默认方式就够了。
- `simcmp.py`把每一帧回放对到设备当时的那次上报。参数和算法都与录制时设备上的相同时，一致率应当接近100%，不一致的
  多半是录制器漏掉了网格的帧。一致率明显偏低时，先检查参数文件，以及编译回放器的内核树是不是录制时设备上跑的那一版。
  拿新算法回放旧录制时，一致率下降是正常的，这时要比较的是改动前后两次回放。
- 录制器跳过空闲的网格，回放时超过30毫秒的空隙按空帧补上，因为跟踪器和手的位置图都按帧计数。
- `-s 名字=值`覆盖单个参数（在`-p`的参数文件之后应用），可以拿同一份录制比较不同的取值。参数文件里有回放器不认识的名字时，
  它会报错退出，所以要排除`stats`和`contacts_log`。
- 笔的事件来自手写笔守护进程创建的`gk3 M-Pencil`设备；没有这个设备时只录触摸。

## 为什么不用 `getevent`

`timeout N getevent > 文件` **会因块缓冲丢光全部输出**（本仓 #26 的老坑）。
一律 `cat /dev/input/eventX` 录二进制、离线解码。

## 典型用法

```sh
adb push scripts/touch/capture-ab.sh /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/capture-ab.sh
adb shell 'nohup /data/local/tmp/capture-ab.sh >/dev/null 2>&1 &'
# ……让用户照常滑动，收够自动切换、自动结束……
adb pull /data/local/tmp/ts_A.bin . && adb pull /data/local/tmp/ts_B.bin .
python3 scripts/touch/evdev-strokes.py ts_A.bin ts_B.bin
```

⚠️ `capture-ab.sh` 里的 `event7` 与算法路径 `/sys/bus/spi/devices/spi0.0/algo/` 都可能变 ——
先 `grep -A9 -i himax /proc/bus/input/devices` 确认。

## 判据

**别用"短轨迹的比例"。** 它只在用户【连续滑动】时才有意义 —— 正常使用里点按本身就是
短轨迹，会得出假的回归结论（2026-09-14 我栽过一次，#114 §7bis）。

要用**相邻轨迹之间的间隔**（`evdev-strokes.py` 默认就跟着算）：手指若真抬起再按下，
跨过间隔的**隐含速度应当接近 0**；若是一条滑动被切开，手指在"隐形"期间仍在移动，
隐含速度就等于它当时的滑行速度。

| | 间隔 <100 ms 的占比 | 跨间隔隐含速度 |
|---|---|---|
| #114 修复前 | 35% | **1.01 m/s**（= 那条限速线本身）|
| 修复后 | 0/45，最小间隔 124 ms | — |

另一个可用的量级参考：同样的甩动，修复前 8 秒产生 **87 条**轨迹，修复后 **6 条**。

## ⛔ 调任何检测阈值之前先读这条（#116 §14）

**静止手指的信噪比不是约束，移动手指的才是。** 快速移动的手指在一次扫描里被拖糊，
峰值比静止时**低得多**。2026-09-16 把 `peak_threshold` 从 800 抬到 1500：静止指尖（峰 3000+）
毫无影响，但快速甩动被切成 7 帧一条的碎片（59% 间隔 <100 ms、隐含 2.61 m/s）——
和 #114 那个跳点检测缺陷一模一样的指纹。

⇒ 抬高 `macro_threshold` / `peak_threshold` / `iso_nbr_ratio_q8` 之类任何"让候选更难通过"的旋钮，
**验收必须包含快速甩动**，用 `evdev-strokes.py` 看间隔分布。只用点击验收会得出"没问题"的假结论。

## 事件驱动地等，不要按钟等

`capture-ab.sh` 与本目录的其他采集脚本都是"收够数据再切换/停止"。按钟等的版本今晚两次录到全零
—— 用户在看消息，还没碰屏幕。**实验的触发条件不该依赖人在特定时刻就位。**
