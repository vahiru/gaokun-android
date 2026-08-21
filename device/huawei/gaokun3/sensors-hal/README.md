# gaokun3 的 sensors HAL

把 SLPI DSP 上的加速度计与陀螺仪喂给 Android 的 SensorService。
**实机验证通过（2026-08-20）**：框架里能看到真实读数，自动旋转的
`WindowOrientationListener` 正在消费它们。

```
Active sensors: SH3001 Accelerometer (handle=0x00000001, connections=2)
Recent Sensor events:
SH3001 Accelerometer: last 50 events
     1 (ts=612.371743828, wall=17:42:46.858) -0.04, 0.05, 9.88,
```

数据通路：`SLPI → QMI/QRTR → libgaokun3ssc → SscHub → 本 HAL → SensorService`。
再往下（AP 怎么给 DSP 当文件服务器）见 `docs/stage4-findings.md` #37，
协议规格见 `docs/sensors-ssc-protocol.md`。

## 这是 AOSP 默认实现的改造副本，不是从零写的

源自 `hardware/interfaces/sensors/aidl/default/`（Apache-2.0，版权头原样保留）。
**为什么必须复制而不能继承**：
* 那个 `Sensors` 类的传感器列表在**构造函数体里**硬加，而 `mSensors` 是 `private`；
* `libsensorsexampleimpl` 的 `visibility` 只放行 `hardware/interfaces` 的子包，
  设备树链不了它。

改动只有三处，其余原样：
1. `Sensors.h` 的传感器列表从 9 个假传感器收敛为 `AccelSensor` + `GyroSensor`。
   ★**必须删掉那 7 个**，留着会让框架以为本机有气压计/湿度计。
2. `Sensor.cpp` 里两个 `readEventPayload` 从硬编码的 `{0,0,9.8}` 换成
   从 `SscHub` 取真数据；数据没到时报 `UNRELIABLE` 而**不是**编一个 9.8 出来
   —— 那会让上层以为机器平放着。
3. `SensorInfo` 写上真名字（`SH3001 Accelerometer` 等）。

新增的只有 `SscHub.{h,cpp}`。

## ★ SscHub 的线程模型：一个坑换来的设计

**整个 SSC 会话由一个线程独占**，各传感器的 `readEventPayload` 只读缓存。
两个原因：
1. `SscClient` 有 txn 计数器，`FindSensor` 内部自己也在 recv —— 多线程共用
   一个 client 必然互抢数据包。
2. ★★**更要紧的**：最初的实现在查不到传感器时会**重建整个会话**
   （每 10 秒 `Open` 一个新 client）。实测这会把**传感器枚举彻底弄坏**：
   之后连独立命令行客户端都报 `SSC 说没有传感器提供 data_type=accel`，
   必须重启 `hexagonrpcd` 才恢复。**干净的 A/B**：停掉 HAL、重建会话，
   独立客户端立刻又能读到 Z≈9.88。
   > 根因是每次重建都在 SSC 上留下一个**被丢弃的客户端**。
   > 这与"使能光感会污染会话"是同一类现象。

所以现在的逻辑是：**在同一个 client 上重试 `FindSensor`**，绝不为此重建会话。
—— ★ 这也解释了一个必知的时序事实：**`registry` 服务比物理传感器【先】注册**，
所以 `WaitForService` 成功之后立刻查 `accel` 会得到"没有传感器提供"，
必须再等（约 20 秒）。

## 本机能提供什么

| Android 传感器 | 来源 |
|---|---|
| `TYPE_ACCELEROMETER` | SSC `accel`（sh3001 六轴）|
| `TYPE_GYROSCOPE` | SSC `gyro`（同一颗）|
| Game Rotation Vector / Gravity / Linear Acceleration | ★**框架自动融合出来的**，有 accel+gyro 就有 |

没有的：磁力计（本机无硬件 → 没有指南针、没有 9 轴融合）、
环境光（一使能就污染 SSC 会话）、接近（同一颗 tcs3701，没试，理由同上）。

## 还欠的

* ⬜ **sepolicy**：现在跑在 permissive 下，logcat 里有一串 `avc: denied`
  （`execute_no_trans` / `add` 到 `hal_sensors_service` 等）。转 enforcing 前必须补。
* ✅ **轴向：不用改。** SSC 报的安装矩阵**全零**（出厂校准随 Windows 永久丢失，
  见 #37），所以我们直接用了传感器自身坐标系 —— 2026-08-20 用户实机确认
  **自动旋转方向正确**。运气好：单位矩阵恰好与本机面板方向一致。
  ⚠️ 别据此以为所有 gaokun3 都一样；换面板批次的机器若发现方向反了，
  纠正点就在 `Sensor.cpp` 的两个 `readEventPayload` 里。
* ✅ **`CONFIG_QCOM_FASTRPC=y`**：M13 起已是 `=y` 并实机验证 —— `#18` 内核起来后
  四个 `/dev/fastrpc-*` 都在而 `/proc/modules` 是 **0 行**，
  `scripts/sensors-up-android.sh` 那套手动步骤不再需要（这是本项目第 13 个「=m 坑」）。
* 采样率/batching/flush 的语义映射仍用上游默认（`no batching`）。
