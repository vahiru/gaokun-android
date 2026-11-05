# SSC客户端库与测试工具（`libgaokun3ssc`）

本机没有任何直接挂在AP侧总线的传感器芯片驱动：加速度计、陀螺仪、环境光、霍尔与铰链角全部连接在SLPI DSP上。AP通过FastRPC向DSP提供只读文件服务（`hexagonrpcd`），DSP启动SSC后通过QRTR上的QMI服务400向AP上报传感器读数。

完整协议规格见[`docs/dev/sensors-ssc-protocol.md`](../../../../docs/dev/sensors-ssc-protocol.md)。

## 一、为何自研客户端

Linux桌面端常用的`libssc`依赖`glib`、`gio`、`gobject`、`qmi-glib`与`libprotobuf-c`，整套GLib栈不适合引入Android vendor分区。而SSC协议本身的`.proto`定义仅数百行，本模块使用AOSP内置的`protoc`与`libprotobuf-cpp-lite`直接实现QRTR服务发现、QMI组包与Protobuf编解码。

## 二、目录结构

| 文件 | 作用 |
|---|---|
| `ssc_client.{h,cpp}` | 可复用客户端库`libgaokun3ssc`：QRTR lookup → QMI组包/解包 → Protobuf序列化，供[`../sensors-hal/`](../sensors-hal/README.md)直接链接 |
| `ssc_test.cpp` | 命令行验证工具`gaokun3-ssc-test`（支持单次采样与`toggle`同会话循环启停测试） |
| `ssc-*.proto` | 取自`libssc`的Protobuf消息定义（GPL-3.0，保留原版权头） |

## 三、实测传感器支持情况

```
$ gaokun3-ssc-test accel 10 8
SSC 服务 400 在 node 9 port 13
传感器 accel 的 UID = 61ab5376b4a5c9aa58442ede47acd316
  X=-0.086191 Y= 0.052672 Z= 9.883265  accuracy=3
```

| `data_type` | 实测表现 |
|---|---|
| `accel` | 正常，平放时Z≈9.88 m/s²，`accuracy=3` |
| `gyro` | 正常，静止时各轴≈0 rad/s，`accuracy=3` |
| `ambient_light` | 存在两个提供者，必须使用流类型为0（连续上报）的SUID：`gaokun3-ssc-test ambient_light 5 10`；流类型为1（变化上报）的默认SUID在收到使能请求时会触发SLPI崩溃，详见[`../sensors-hal/README.md`](../sensors-hal/README.md) |
| `mag` / `rotv` | 本机无磁力计硬件，SSC返回未提供（查询不会污染当前会话） |

## 四、命令行用法

### 1. 常规采样

```sh
# 确认hexagonrpcd已运行且SSC服务400已上线（hexagonrpcd启动后约需20秒沉降时间）
gaokun3-qrtr-lookup 400
gaokun3-ssc-test               # 默认：accel 10 Hz，持续10秒
gaokun3-ssc-test gyro 20 5     # gyro 20 Hz，持续5秒
gaokun3-ssc-test ambient_light 5 10
```

### 2. `toggle`模式（同一会话循环启停测试）

```sh
gaokun3-ssc-test accel 50 20 toggle   # 50 Hz，循环20轮（第3个参数为轮数）
gaokun3-ssc-test gyro  50 20 toggle
```

在同一个`SscClient`与同一个SUID上，每轮执行`EnableContinuous` → 接收2秒 → `Disable`（`msg_id = 10`） → 静默2秒，全部轮次结束后再次调用`FindSensor`确认SSC枚举未损坏（退出码`0`表示全部通过）。

若测试中途引发SLPI或SSC状态异常，可按顺序重启`hexagonrpcd`与`sensors-hal`恢复：

```sh
stop vendor.sensors-gaokun3
stop vendor.hexagonrpcd-sdsp && start vendor.hexagonrpcd-sdsp
sleep 20
start vendor.sensors-gaokun3
```

## 五、构建配置说明

1. `Android.bp`中必须声明`proto: { canonical_path_from_root: false }`，以便`.proto`文件之间的`import "ssc-common.proto"`及生成的头文件包含路径正常解析。
2. 静态链接`libprotobuf-cpp-lite`（因`/system/lib64/libprotobuf-cpp-lite-*.so`对vendor命名空间不可见），并显式声明`shared_libs: ["liblog"]`以满足静态库引用的`__android_log_write`符号。
