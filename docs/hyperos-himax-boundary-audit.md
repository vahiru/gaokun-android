# Himax 边界越界：独立修复候选

当前确认的是边缘 off-by-one，**尚不能解释用户所有界面中央滑动不灵敏**。

## 证据链

`/tmp/hos-touch-contacts-log.txt` 的新轨迹 seq5484、5487 显示算法 x=2560 且 edge=1。该记录是新轨迹出生记录，不是连续帧轨迹。

`hx_edge_compensate()` 允许右边缘 Q8.8 坐标达到 `HX_COLS * 256 = 15360`，下边缘达到 `HX_ROWS * 256 = 10240`。`hx_expand_and_resolve()` 分别除以 6、乘 5/32，因此输出上界为 2560 和 1600；仅钳到 65535，没有限制在面板有效坐标内。

DT 定义原始 size-x=2560、size-y=1600，先 invert-x 后 swap-x-y。touchscreen properties 保存的 inclusive max 分别为 2559/1599。内核 helper 不自动钳位，因此原始 x=2560 经 `2559 - 2560` 在 unsigned 运算下回绕，最终 input 整数值表现为 -1，交换后进入 ABS_Y。原始 y=1600 同样会变成超过 ABS_X 最大1599的值。这与现有 ABS_Y=-1 读回一致。

## 候选内容

独立补丁：`tools/hos/patches/himax-clamp-before-coordinate-transform.patch`。

目标 `drivers/input/touchscreen/himax-spi-core.c` 的 `himax_report_tracked_state()`。只在 `touchscreen_report_pos()` 的参数处用 `clamp_t(s32, ..., 0, ts->props.max_x/max_y)` 限制**尚未旋转的有符号坐标**；继续使用原 helper 处理反转和交换。读取 properties 的真实轴最大值，不写死面板尺寸，不更改全局 touchscreen helper。跟踪、压力、掌压过滤和内部 contacts_log 均保持原样，因此 contacts_log 仍可能出现2560，而最终 input 上报应合法。

补丁未加入生产内核补丁列表、未构建设备内核、未刷机或操作 VM。

## 离线验证

`python3 -m unittest discover -s tools/hos -p test_himax_bounds.py` 通过。

测试从本地 buildbot 的原始 Himax 导入补丁恢复三个完整源文件，读取实际 rc2d 发布清单的78个补丁并检查每个SHA，按清单顺序重放其中全部12个修改这三个文件的补丁，再应用候选，严格 `git apply` 无 fuzz。验证仅报告调用及注释改变，其他完整源内容不变。这是完整**相关驱动文件**的发布补丁链投影，不是声称在本地重建了整个7.2.9内核。

从补丁后的实际调用提取 C 代码，并使用本地内核参考的坐标转换函数编译运行，开启 undefined-behavior sanitizer：

- (2560,1600) → (1599,0)，符合当前反转与交换顺序。
- 负坐标与 int32 极值先按有符号钳位，不会变成巨大 unsigned 坐标。
- 穷举1600×2560全部有效坐标，与原合法映射逐一一致。
- 使用另一组轴最大值并测试全部反转/交换组合，确认没有写死面板参数或改变旋转顺序。

## 中央滑动需另行诊断

现有 game 参数实际为 smoothing=0、debounce=1、jump=0、hand_enabled=0、pressure_enabled=1。hand map 关闭不等于早期掌压过滤关闭，palm_enabled 仍为1。累计 palm_aspect=3632、rej_minarea=16277、guard_kill=350 不能单独定因，应在同一次真实短滑前后读计数差值，与 evdev tracking ID 和应用 MotionEvent 的 DOWN/MOVE/UP/CANCEL 同步比较。

active guard 仅在尚未建立 touch_active 时丢弃未匹配候选；稳定轨迹持续缺失超过3帧才释放（第4帧，120Hz时约33ms）。如果一次连续单指滑动产生多个新轨迹/UP-DOWN，优先关联面积/掌压/信号门限与失配；如果 raw 连贯而应用收到 CANCEL，查输入策略抢流。不能根据本边缘补丁宣称这些问题已修复。
