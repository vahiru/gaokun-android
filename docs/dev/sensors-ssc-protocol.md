# SSC传感器协议与SLPI通路规格

本机没有任何直接挂在AP侧总线的传感器芯片驱动：六轴IMU（SH3001）、环境光（TCS3701）、霍尔与铰链传感器全部连接在SLPI DSP上。Android侧通过两条并行通路驱动传感器：
1. **FastRPC文件服务**：`hexagonrpcd`向SLPI提供只读虚拟文件系统（`/vendor/etc/hexagonrpcd-root/`），供DSP加载传感器配置与注册表并启动SSC（Snapdragon Sensor Core）。
2. **QRTR + QMI + Protobuf**：SSC启动后在QRTR上暴露服务400，[`device/huawei/gaokun3/ssc/`](../../device/huawei/gaokun3/ssc/README.md)（`libgaokun3ssc`）与[`device/huawei/gaokun3/sensors-hal/`](../../device/huawei/gaokun3/sensors-hal/README.md)按本规格通过QMI报文收发protobuf消息。

## 1. 为什么重写客户端而不移植`libssc`

Linux桌面端常用的`libssc`依赖`glib-2.0`、`gio`、`gobject`、`qmi-glib`（`libqmi`）与`libprotobuf-c`（`libssc/meson.build:74-87`），整套GLib依赖栈不适合引入Android vendor分区。而SSC协议本身的逻辑很薄（`.proto`共8个文件389行），AOSP自带`protoc`与`libprotobuf-cpp-lite`，在`Android.bp`中用`proto: { type: "lite", canonical_path_from_root: false }`直接编译`.proto`并静态链接`libprotobuf-cpp-lite`（同时补`shared_libs: ["liblog"]`满足`__android_log_write`符号）即可。

## 2. 协议分层结构

```
Android SensorService / 应用
  ↓  AIDL android.hardware.sensors（device/huawei/gaokun3/sensors-hal/）
SscHub / libgaokun3ssc（device/huawei/gaokun3/ssc/）
  ↓  protobuf（SscClientRequest / SscClientResponse …）
QMI SSC服务（service 400）
  ↓  QMI SDU：7字节service_header + TLV（无QMUX头）
AF_QIPCRTR socket（QRTR）
  ↓
内核QRTR → SLPI DSP上的SSC
```

### 2.1 第一层：QRTR服务发现

* `AF_QIPCRTR = 42`（`bionic/libc/include/sys/socket.h:169`）
* `QRTR_PORT_CTRL = 0xfffffffe`、`QRTR_TYPE_NEW_LOOKUP = 10`、`QRTR_TYPE_NEW_SERVER = 4`、`struct qrtr_ctrl_pkt`（`bionic/libc/kernel/uapi/linux/qrtr.h`）
* 服务号**400**对应`QMI_SERVICE_SSC`（本机实测位于`instance 1, node 9, port 13`）。

发现流程：`socket(AF_QIPCRTR, SOCK_DGRAM, 0)` → `getsockname()`取本节点号 → 向`{本节点, QRTR_PORT_CTRL}`发送`NEW_LOOKUP` → 接收`NEW_SERVER`包（参考[`device/huawei/gaokun3/tools/qrtr-lookup/qrtr_lookup.c`](../../device/huawei/gaokun3/tools/qrtr-lookup/qrtr_lookup.c)）。当服务表为空时不会立等收到全零终止包，接收循环必须设置超时。

### 2.2 第二层：QMI over QRTR线格式

QRTR上的QMI报文**不包含**QMUX头或`libqmi`内部marker字节（见`qmi-endpoint-qrtr.c:547-548`与`qmi-message.c:664-678`），socket直接收发7字节`service_header`加TLV序列：

```c
struct service_header {      /* PACKED，共7字节 */
    uint8_t  flags;          /* 0=请求, 1=COMPOUND, 2=响应, 4=指示(indication) */
    uint16_t transaction;    /* 小端 */
    uint16_t message;        /* 小端，消息ID */
    uint16_t tlv_length;     /* 小端，后续所有TLV字节数总和 */
};
struct tlv {
    uint8_t  type;
    uint16_t length;         /* 小端 */
    uint8_t  value[];
};
```

### 2.3 第三层：SSC服务的QMI消息（`libqmi/data/qmi-service-ssc.json`）

* **请求：Control（消息ID `0x0020`）**
  * TLV `0x01`（Data）：字节数组，**内部带`uint16`小端长度前缀**，紧跟序列化的`SscClientRequest`。
  * TLV `0x10`（Report Type）：`uint8`，`0x00` = SMALL，`0x01` = LARGE。
  * 响应TLV：`0x02` = 标准Operation Result，`0x10` = Client ID（`uint64`），`0x11` = Response（`uint32`）。
* **指示：Report Small（`0x0021`）/ Report Large（`0x0022`）**
  * TLV `0x01`（Client ID）：`uint64`。
  * TLV `0x02`（Data）：带`uint16`长度前缀的字节数组，内容为序列化的`SscClientResponse`。

### 2.4 第四层：Protobuf消息（proto2，`libssc/data/*.proto`）

```protobuf
message SscUid { required fixed64 low = 1; required fixed64 high = 2; }

message SscClientConfig {
    required int32 processor    = 1 [default = 1];   // APPS
    required int32 suspend_mode = 2 [default = 0];   // WAKEUP
}
message SscClientRequestBody {
    optional bytes unknown    = 1;
    optional bytes msg        = 2;   // 传感器专属请求
    optional bool  is_passive = 3 [default = false];
}
message SscClientRequest {
    required SscUid               uid     = 1;
    required fixed32              msg_id  = 2;
    required SscClientConfig      config  = 3;
    required SscClientRequestBody request = 4;
}
message SscClientResponseBody {
    required fixed32 msg_id    = 1;
    required fixed64 timestamp = 2;   // DSP内部计时器，19.2 MHz
    required bytes   msg       = 3;
}
message SscClientResponse {
    required SscUid uid = 1;
    repeated SscClientResponseBody response = 2;
}
```

Protobuf层常用`msg_id`（`libssc/src/libssc-sensor*-private.h`）：

| 常量 | 值 | 用途 |
| --- | --- | --- |
| `SSC_MSG_REQUEST_GET_ATTRIBUTES` | 1 | 请求传感器属性（量程、分辨率、流类型、安装矩阵等） |
| `SSC_MSG_REQUEST_DISABLE_REPORT` | 10 | 停止上报 |
| `SSC_MSG_RESPONSE_GET_ATTRIBUTES` | 128 | 属性响应 |
| `SSC_MSG_REQUEST_SUID` | 512 | 向SUID哨兵（`low = high = 0xABABABABABABABAB`）查询某`data_type`的传感器UID |
| `SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS` | 513 | 开启连续采样上报（`SscEnableConfigRequest{sample_rate}`） |
| `SSC_MSG_REQUEST_ENABLE_REPORT_ON_CHANGE` | 514 | 开启变化时上报 |
| `SSC_MSG_RESPONSE_SUID` / `ENABLE_REPORT` | 768 | SUID查询响应 |
| `SSC_MSG_REPORT_MEASUREMENT_PROXIMITY` | 769 | 接近传感器事件 |
| `SSC_MSG_REPORT_MEASUREMENT` | 1025 | 连续采样测量值（如`SscAccelerometerResponse`） |

## 3. 本机传感器状态与运行约束

### 3.1 各`data_type`实机表现

| `data_type` | 物理芯片 | 状态与注意事项 |
| --- | --- | --- |
| `accel` | SH3001 | 正常，平放Z轴约9.88 m/s²，`accuracy=3`。出厂安装矩阵随Windows校准分区丢失而全零，退回单位矩阵后方向与本机面板天然一致 |
| `gyro` | SH3001 | 正常，静止各轴约0 rad/s，`accuracy=3`。配合`accel`由Android框架融合出Game Rotation Vector、Gravity、Linear Acceleration |
| `ambient_light` | TCS3701 | 注册了两个SUID：流类型为1（DRI变化上报，`57069a9c48dd76bdef49dfb91c177728`，默认返回项）是驱动内部主实例，向其发送使能请求会触发SEE框架`SNS_ASSERT(req_found)`导致SLPI崩溃重启；**只能使能流类型为0（连续上报，`57069a9c48dd768def497fb916377829`）的提供者**（查询SUID时必须显式设`only_default_values=false`并按属性16筛选） |
| `mag` / `rotv` / `proximity` | — | 本机无磁力计硬件，SSC不提供`mag`、`rotv`及`proximity`（查询失败不会污染会话） |

### 3.2 `hexagonrpcd`与会话生命周期约束

1. **VFS根目录与CRLF路径处理**：DSP固件在Windows环境下编译，通过FastRPC请求的文件路径每段末尾均带有`\r`（CR）。本仓使用的`hexagonrpcd`在通用分段解析函数`copy_segment_and_advance()`（`hexagonfs.c:179`）中统一剥离末尾`\r`，因此物理目录`/vendor/etc/hexagonrpcd-root/`下无需创建带`\r`后缀的符号链接即可由`PRODUCT_COPY_FILES`正常部署（详见[`device/huawei/gaokun3/hexagonrpcd-root/README.md`](../../device/huawei/gaokun3/hexagonrpcd-root/README.md)）。
2. **`sensors/registry/registry`必须为空文件**：0字节文件受控于`device/huawei/gaokun3/etc/hexagonrpcd-empty-registry`。若使用`sscregistrygen`预生成142个注册表文件，会导致加速度计等全部失效；保持空文件可让DSP回退至默认启用全部传感器。此外，`sensors/sns_reg.conf`必须使用约400字节的`version=1`文本格式，不能使用2423字节的DriverData JSON格式。
3. **启动后沉降窗口**：`registry`服务比物理传感器更早完成注册。`hexagonrpcd`启动后约需20秒物理传感器才可被查到，客户端初始化轮询窗口需留足40–60秒。
4. **单客户端长会话与按订阅启停**：每次在SSC上新建并丢弃`SscClient`都会残留会话状态，反复重建会话会破坏SSC的传感器枚举，直至重启`hexagonrpcd`才恢复。因此[`SscHub`](../../device/huawei/gaokun3/sensors-hal/SscHub.cpp)由单一线程独占一个`SscClient`，无订阅者时在同一会话内发送`msg_id=10`（Disable）停流以允许SLPI在息屏期间进入休眠，有订阅者时在同一会话内重新`EnableContinuous`。
5. **SLPI崩溃自愈后的两阶段恢复**：SLPI发生SSR重启后，`hexagonrpcd`必须在SLPI启动完成后再重启一次，SEE才会重新注册传感器。HAL检测到超时无读数或找不到传感器时不自行重建会话，而是置位`vendor.gaokun3.sscrecover.req`，由init触发`bin/gaokun3-ssc-recover.sh`按顺序重启`hexagonrpcd`与`sensors-hal`。
6. **良性日志噪声**：DSP运行期间会持续尝试写入`/persist/sensors/registry/registry/../temp.json`，被只读的`hexagonfs`拒绝，属正常现象；内核日志约1 Hz的`Handover signaled, but it already happened`同样伴随正常传感器会话出现，不影响读数。
