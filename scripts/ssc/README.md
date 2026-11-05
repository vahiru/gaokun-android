# SSC传感器配置A/B实验工具

本目录提供在不修改`/vendor`分区、无需整机重启的前提下验证SLPI/SSC传感器JSON配置的实验脚本。SLPI传感器架构与QMI/Protobuf协议详见[`../../docs/dev/sensors-ssc-protocol.md`](../../docs/dev/sensors-ssc-protocol.md)与[`../../device/huawei/gaokun3/hexagonrpcd-root/README.md`](../../device/huawei/gaokun3/hexagonrpcd-root/README.md)。

---

## 一、工作原理

SLPI固件中的SEE（Sensor Execution Environment）仅在DSP启动初始化阶段通过`hexagonrpcd`读取一次JSON配置文件；若仅重启用户态`hexagonrpcd`而不重启SLPI remoteproc，配置修改不会生效。`sscexp.sh`通过停止`hexagonrpcd`、重启SLPI（`/sys/class/remoteproc/remoteproc0/state`）、以自定义目录`/data/local/tmp/hr`启动临时`hexagonrpcd`并调用`gaokun3-ssc-test`验证，在60–90秒内完成一轮完整测试。

---

## 二、使用方法

```bash
adb push scripts/ssc/sscexp.sh   /data/local/tmp/sscexp.sh
adb push scripts/ssc/setfield.sh /data/local/tmp/setfield.sh
adb shell chmod 755 /data/local/tmp/sscexp.sh /data/local/tmp/setfield.sh

adb shell '
  rm -rf /data/local/tmp/hr && mkdir -p /data/local/tmp/hr
  cp -r /vendor/etc/hexagonrpcd-root/* /data/local/tmp/hr/
  sh /data/local/tmp/setfield.sh \
     /data/local/tmp/hr/sensors/config/8280_qrd_tcs3701.json bus_instance 1
  sh /data/local/tmp/sscexp.sh ambient_light 40
'
```

`setfield.sh`在修改JSON字段后会自动回读校验，若未命中目标字段则以非零状态码退出。

### 有效性对照检查

在验证新传感器配置前，建议先运行以下两个对照实验确认DSP确实读取了临时目录`/data/local/tmp/hr`：

- **移除配置对照**：从`/data/local/tmp/hr/sensors/config/`删除`*sh3001*`后运行`sscexp.sh accel`，应返回无`accel`传感器。
- **错误地址对照**：将加速度计的`slave_config`从`54`改为`99`，`sscexp.sh accel`同样应报告未探测到传感器。

---

## 三、恢复默认服务

实验结束后恢复系统自带的`vendor.hexagonrpcd-sdsp`服务：

```bash
adb shell '
  pkill -x hexagonrpcd
  [ "$(cat /sys/class/remoteproc/remoteproc0/state)" = running ] || \
    echo start > /sys/class/remoteproc/remoteproc0/state
  rm -rf /data/local/tmp/hr
  start vendor.hexagonrpcd-sdsp
'
```

等待约45秒后，通过`gaokun3-ssc-test accel`与`dumpsys sensorservice`确认SSC层与Android SensorService框架层均已恢复正常。
