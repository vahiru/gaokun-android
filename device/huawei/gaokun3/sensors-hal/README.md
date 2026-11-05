# gaokun3传感器HAL（`android.hardware.sensors` AIDL）

本模块通过[`libgaokun3ssc`](../ssc/README.md)订阅SLPI DSP上的加速度计、陀螺仪与环境光传感器数据，向Android `SensorService`提供标准AIDL传感器接口。底层协议规格见[`docs/dev/sensors-ssc-protocol.md`](../../../../docs/dev/sensors-ssc-protocol.md)。

```
SLPI DSP → QMI/QRTR → libgaokun3ssc → SscHub → android.hardware.sensors-service.gaokun3 → SensorService
```

## 一、实现结构

本模块基于AOSP `hardware/interfaces/sensors/aidl/default/`（Apache-2.0）改造：

1. 将默认示例中的9个模拟传感器裁剪为本机实际具备的3个物理传感器：`AccelSensor`（`SH3001 Accelerometer`）、`GyroSensor`（`SH3001 Gyroscope`）与`LightSensor`（`TCS3701 Ambient Light`）。Android框架基于`accel` + `gyro`自动融合生成`Game Rotation Vector`、`Gravity`与`Linear Acceleration`。
2. `Sensor::readEventPayload`从`SscHub`读取最新实测样本；在首个有效样本到达或数据过期（超过2秒未刷新）时上报`SensorStatus::UNRELIABLE`。
3. `Sensor::activate`将订阅状态同步至`SscHub`，实现按需启停。

## 二、`SscHub`单会话线程模型与按需启停

1. **单线程独占持久会话**：
   - `SscClient`内部维护事务计数与socket读取循环，不可跨线程并发调用。
   - 此外，在SSC上频繁关闭并新建`SscClient`会在DSP侧累积残留客户端状态并破坏传感器枚举。因此`SscHub`由单一工作线程持久持有一个`SscClient`，初始化阶段在同一会话内轮询等待`FindSensor`（`registry`服务比物理传感器早约20秒上线），绝不因暂未查到传感器而销毁重建会话。
2. **按订阅状态启停（`EnableContinuous` / `Disable`）**：
   - 当上层无任何订阅者时（如息屏期间），工作线程在同一会话上发送`Disable`（`msg_id = 10`）停止采样上报，排空在途数据包后进入条件变量等待，使SLPI得以进入低功耗休眠。
   - 当有订阅者激活传感器时，工作线程唤醒并在同一会话上重新发送`EnableContinuous`。
3. **SLPI崩溃自愈后的恢复协同**：
   - 若SLPI发生SSR重启，`hexagonrpcd`必须在SLPI恢复运行后重启一次，SEE才会重新注册传感器。
   - 当`SscHub`检测到超时未收到数据或未注册传感器时，置位属性`vendor.gaokun3.sscrecover.req`，由`etc/sscrecover.rc`调用`bin/gaokun3-ssc-recover.sh`按顺序重启`hexagonrpcd`与本HAL服务。

## 三、环境光传感器（`ambient_light`）流类型筛选

SLPI固件中的`tcs3701`驱动为`ambient_light`注册了两个SUID：

| SUID | 流类型（属性16） | 状态与行为 |
|---|---|---|
| `57069a9c48dd76bdef49dfb91c177728` | `1`（DRI变化上报，默认项） | 驱动内部主传感器实例，不受理外部客户端请求；向其发送使能请求会触发SEE框架`SNS_ASSERT(req_found)`并导致SLPI崩溃重启 |
| `57069a9c48dd768def497fb916377829` | `0`（连续采样上报，1/2/5/10 Hz） | 正常输出光照度（lux）与色温 |

由于SEE在`only_default_values=true`时默认返回流类型为1的SUID，`SscHub::FindStreamingLight`显式以`only_default_values=false`查询全部提供者，并按属性`16`（`SNS_STD_SENSOR_ATTRID_STREAM_TYPE`）筛选出流类型为`0`的SUID进行订阅。自动亮度映射曲线配置于`overlay/frameworks/base/core/res/res/values/config.xml`。
