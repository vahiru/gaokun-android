# SSC 传感器协议规格（Android sensors HAL 的实现依据）

> 这份文档的目的：把"逆向 libqmi + libssc"变成"照规格实现"。
> 每条事实都注明来源文件与行号，**没有一条来自记忆**。
>
> 状态（2026-08-20）：底层管道已在 Android 上实测打通（QRTR 服务 400 上线）；
> 本文描述的 QMI/protobuf 客户端**尚未实现**，这是剩下的工程量。
> 背景与实测数据见 `stage4-findings.md` #37。

## 为什么必须重写，不能移植 libssc

`libssc`（读数那一层）依赖 **glib-2.0 / gio / gobject / qmi-glib(libqmi) /
libprotobuf-c**（`libssc/meson.build:74-87`）。这一整套 GLib 栈搬进 Android
不现实。但它的**协议逻辑很薄**（`src/` 全部 4361 行，含头文件），
而 `.proto` 只有 8 个文件共 389 行 —— 照规格重写比移植依赖便宜得多。

AOSP 侧的好消息：`protoc` 和 `libprotobuf-cpp-lite` 都是现成的，
Android.bp 支持 `proto: { type: "lite" }`，可以直接编译这些 `.proto`。

## 三层结构

```
应用/HAL
  ↓  protobuf（SscClientRequest / SscClientResponse …）
QMI SSC 服务（service 400）
  ↓  QMI SDU：7 字节头 + TLV
AF_QIPCRTR socket（QRTR）
  ↓
内核 QRTR → SLPI DSP 上的 SSC
```

另有一条**并行**的必需通路：`hexagonrpcd` 通过 FastRPC 给 DSP 当只读文件
服务器。**没有它 SSC 根本不会启动**，服务 400 也不会出现。两条通路都要在。

## 第一层：QRTR

* `AF_QIPCRTR = 42` —— `bionic/libc/include/sys/socket.h:169`
* `QRTR_PORT_CTRL = 0xfffffffe`、`QRTR_TYPE_NEW_LOOKUP = 10`、
  `QRTR_TYPE_NEW_SERVER = 4`、`struct qrtr_ctrl_pkt`（`__packed`，server 分支是
  service/instance/node/port 四个 `__le32`）
  —— `bionic/libc/kernel/uapi/linux/qrtr.h`
* 服务号 **400** = Snapdragon Sensor Core，即 QMI 的 `QMI_SERVICE_SSC`。

发现流程：`socket(AF_QIPCRTR, SOCK_DGRAM, 0)` → `getsockname()` 取本节点号 →
往 `{本节点, QRTR_PORT_CTRL}` 发 `NEW_LOOKUP` → 收 `NEW_SERVER` 包。
实现参考本仓 `device/huawei/gaokun3/tools/qrtr-lookup/qrtr_lookup.c`。

⚠️ 不要只靠"全零终止包"退出循环 —— 服务表为空时会永久阻塞，必须带超时。

本机实测（Android，2026-08-20）：
```
service  instance  node  port
400      1         9     13
```

## 第二层：QMI over QRTR 的线格式

★ **关键事实：QRTR 上【没有】QMUX 头，也没有 libqmi 内部那个 marker 字节。**
`qmi-endpoint-qrtr.c:547-548` 的注释与代码原文：

```c
/* Build raw QRTR message without QMUX/QRTR header */
raw_message = qmi_message_get_data (message, &raw_message_len, error);
```

而 `qmi_message_get_data()`（`qmi-message.c:664-678`）返回的是
`&full_message->qmi`，长度 = `sizeof(struct service_header)` + 所有 TLV 长度。
所以上线的字节就是：

```c
struct service_header {      /* qmi-message.c:98-103，PACKED，共 7 字节 */
    uint8_t  flags;
    uint16_t transaction;    /* 小端 */
    uint16_t message;        /* 小端，即消息 ID */
    uint16_t tlv_length;     /* 小端，后面所有 TLV 的字节数 */
};
struct tlv {                 /* qmi-message.c:105-108 */
    uint8_t  type;
    uint16_t length;         /* 小端 */
    uint8_t  value[];
};
```

`flags`（`qmi-enums-private.h:79-84`）：

| 值 | 含义 |
|---|---|
| 0 | 请求 |
| 1 | COMPOUND |
| **2** | 响应 |
| **4** | 指示（indication）|

## 第三层：SSC 服务的三条消息

来源 `libqmi/data/qmi-service-ssc.json`（该服务自 libqmi 1.34 起支持）。

### 请求：Control，消息 ID `0x0020`

| TLV | 名称 | 格式 |
|---|---|---|
| `0x01` | Data | 字节数组，**带 `uint16` 长度前缀**（即 TLV 值内部再有一个 u16 长度）|
| `0x10` | Report Type | `uint8`：`0x00` = SMALL，`0x01` = LARGE（`qmi-enums-ssc.h`）|

响应 TLV：`0x02` = 标准 Operation Result、`0x10` = Client ID(`uint64`)、
`0x11` = Response(`uint32`)。

### 指示：Report Small `0x0021` / Report Large `0x0022`

| TLV | 名称 | 格式 |
|---|---|---|
| `0x01` | Client ID | `uint64` |
| `0x02` | Data | 字节数组，带 `uint16` 长度前缀 |

TLV `0x02` 里那串字节就是下一层的 protobuf（`SscClientResponse`）。

## 第四层：protobuf（proto2）

来源 `libssc/data/*.proto`。**注意是 proto2 且大量 `required`**。

```protobuf
message SscUid { required fixed64 low = 1; required fixed64 high = 2; }

message SscClientConfig {
    required int32 processor    = 1 [default = 1];   // APPS
    required int32 suspend_mode = 2 [default = 0];   // WAKEUP
}
message SscClientRequestBody {
    optional bytes unknown    = 1;
    optional bytes msg        = 2;   // 传感器专属请求，见下
    optional bool  is_passive = 3 [default = false];
}
message SscClientRequest {
    required SscUid              uid     = 1;
    required fixed32             msg_id  = 2;
    required SscClientConfig     config  = 3;
    required SscClientRequestBody request = 4;
}
message SscClientResponseBody {
    required fixed32 msg_id    = 1;
    required fixed64 timestamp = 2;   // DSP 内部计时器，19.2 MHz
    required bytes   msg       = 3;
}
message SscClientResponse {
    required SscUid uid = 1;
    repeated SscClientResponseBody response = 2;
}

message SscSuidRequest  { required string data_type = 1;
                          optional bool enable_updates = 2;
                          optional bool only_default_values = 3; }
message SscSuidResponse { required string data_type = 1; repeated SscUid uid = 2; }

message SscEnableConfigRequest { required float sample_rate = 1; }   // Hz

message SscAccelerometerResponse {
    repeated float acceleration = 1;   // X[0] Y[1] Z[2]，单位 m/s²
    required int32 accuracy     = 2;   // 0..3，0 = 不可信
}
```

### 消息 ID（protobuf 层，与 QMI 层的 0x0020 不是一回事）

来源 `libssc/src/libssc-sensor*-private.h`：

| 常量 | 值 |
|---|---|
| `SSC_MSG_REQUEST_GET_ATTRIBUTES` | 1 |
| `SSC_MSG_REQUEST_DISABLE_REPORT` | 10 |
| `SSC_MSG_RESPONSE_GET_ATTRIBUTES` | 128 |
| `SSC_MSG_REQUEST_SUID` | **512** |
| `SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS` | **513** |
| `SSC_MSG_REQUEST_ENABLE_REPORT_ON_CHANGE` | 514 |
| `SSC_MSG_RESPONSE_SUID` / `SSC_MSG_RESPONSE_ENABLE_REPORT` | **768** |
| `SSC_MSG_REPORT_MEASUREMENT_PROXIMITY` | 769 |
| `SSC_MSG_REPORT_MEASUREMENT` | **1025** |

★ **SUID 查找用的哨兵 UID**：`low = high = 0xABABABABABABABAB`
（`SSC_SENSOR_UID_SUID_LOW/HIGH`）。也就是"向那个虚拟的 SUID 传感器发请求，
问某个 data_type 由哪些真实传感器提供"。

### data_type 字符串（`libssc/src/libssc-sensor-*.c`）

| 传感器 | data_type |
|---|---|
| 加速度计 | `accel` |
| 陀螺仪 | `gyro` |
| 环境光 | `ambient_light` |
| 磁力计 | `mag` |
| 接近 | `proximity` |
| 旋转矢量 | `rotv` |
| （服务可用性探针）| `registry` |

## 完整交互流程

1. **等服务就绪**：向 SUID 哨兵查 `data_type = "registry"`，直到有响应。
   libssc 就是这么判断"SSC 活了没有"（`libssc-sensor.c:297,318,627-631`）。
2. **查 UID**：`SscClientRequest{ uid=哨兵, msg_id=512,
   request.msg = SscSuidRequest{data_type:"accel"} }`
   → 收指示 → `SscClientResponse.response[].msg_id == 768`
   → 解 `SscSuidResponse` → 拿到真实传感器的 `SscUid`。
3. **（可选）取属性**：`msg_id=1` → 响应 `msg_id=128`，里面有量程、分辨率、
   **安装矩阵**等。⚠️ 本机安装矩阵**全零**（出厂校准随 Windows 永久丢失），
   libssc 会退回单位矩阵并告警 —— HAL 也必须处理这种情况。
4. **使能**：`msg_id=513`（连续）或 `514`（变化时），
   `request.msg = SscEnableConfigRequest{sample_rate}`。
5. **收数**：指示里 `msg_id == 1025` → 解 `SscAccelerometerResponse`
   → `acceleration[0..2]`（m/s²）+ `accuracy`。
6. **停止**：`msg_id=10`。

## 实现这一层时要知道的坑（都是实测出来的）

* ⚠️ **`sensors/registry/registry` 必须是空文件**。用 `sscregistrygen`
  预生成 142 个文件会把加速度计一起弄坏（干净 A/B 验证过）。
* ⚠️ **别碰环境光**：使能 `ambient_light` 之后从不返回读数，而且会**污染整个
  SSC 会话** —— 之后连加速度计也读不到，必须重启 hexagonrpcd。
  HAL 里如果要暴露光感，得先解决这个，否则会连带弄坏自动旋转。
* ⚠️ **需要沉降时间**：hexagonrpcd 重启后约 20 秒才有读数，6 秒就读实测是 0 行。
  HAL 的初始化重试要按这个量级设计，别 5 秒就判失败。
* ⚠️ DSP 会每秒几十次请求**写** `/persist/sensors/registry/registry/../temp.json`，
  而 hexagonfs 是**设计上只读**的（`hexagonfs.h:34-45` 的 ops 表没有 write）。
  实测这些写拒绝**不影响加速度计**，属可容忍噪声，不要为它去改上游。
* `Handover signaled, but it already happened`（kernel，约 1 Hz）是**良性噪声**：
  任何传感器会话都会伴生，工作正常的加速度计同样每 12 秒 13 条。别当故障。

## Android 侧目前的状态

| 部件 | 状态 |
|---|---|
| `CONFIG_QCOM_FASTRPC` | ✅ 已是 `=y`（M13 实机验证：4 个 `/dev/fastrpc-*` 节点、`/proc/modules` 0 行）|
| `/dev/fastrpc-sdsp` | ✅ 出现（权限由 `ueventd.gaokun3.rc` 给 system:system）|
| `hexagonrpcd`（AOSP 构建）| ✅ 已编出并运行，DSP 20 秒内发来 2880 行文件请求 |
| VFS 根 → `/vendor/etc/hexagonrpcd-root` | ✅ 已进 `device.mk` |
| **QRTR 服务 400** | ✅ **上线**（node 9 port 13）|
| QMI/protobuf 客户端 | ✅ **已实现并实测通过** —— `device/huawei/gaokun3/ssc/`，Android 上读出 `accel` Z≈9.88 m/s² accuracy=3、`gyro` 静止≈0 rad/s |
| AIDL `android.hardware.sensors` HAL | ✅ **已实现并实机验证** —— `device/huawei/gaokun3/sensors-hal/`（AOSP 默认实现的改造副本）|
| sepolicy | ⬜ 未写（当前 SELinux permissive，将来转 enforcing 时必须补）|

## ★ 本规格已被实现验证（2026-08-20）

`device/huawei/gaokun3/ssc/` 按本文档实现了客户端，**在 Android 上一次跑通**：

```
SSC 服务 400 在 node 9 port 13
传感器 accel 的 UID = 61ab5376b4a5c9aa58442ede47acd316
  X=-0.086191 Y= 0.052672 Z= 9.883265  accuracy=3
```

也就是说本文档里的线格式（7 字节头、TLV 的 u16 长度前缀）、消息 ID、
哨兵 UID 全部正确 —— **可以照抄，不必再逆向**。

各 data_type 的实测结果：`accel` ✅、`gyro` ✅（Linux 侧从未验证过）、
`mag` ❌ 本机无磁力计（SSC 明确回答"没有传感器提供"）、
`rotv` ❌ 未注册（多半需要磁力计）、`ambient_light` ❌ 污染会话。
⚠️ `mag`/`rotv` 的查询失败**不会**污染会话，与 `ambient_light` 不同。

**下一个增量**：AIDL `android.hardware.sensors` HAL —— 把 `libgaokun3ssc`
包起来喂 SensorService。额外要处理的：安装矩阵全零（轴向要实机标定一次）、
采样率/batching/flush 语义、sepolicy。
