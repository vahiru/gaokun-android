# SSC 传感器配置实验循环

给 SLPI/SSC 传感器配置做 A/B 实验用。**一次约 60–90 秒，不写 `/vendor`、
不重启整机。** 案卷见 `docs/stage4-findings.md` #72。

## 为什么需要它

`docs/stage4-findings.md` #70 整张实验矩阵是废数据，因为那些配置改动
**从未生效过**：SEE 在 DSP 上初始化过一次之后就不再读文件服务器，
只重启 `hexagonrpcd` 不会让它重读配置（实测：把根目录换成**空目录**，
加速度计照样满血）。要让改动生效，必须**重启 SLPI**。

## 用法

```sh
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

`setfield.sh` 改完会**回读验证**，值不对就 `exit 1` —— 这正是 #70 缺的那一环。

## ⚠️ 每次改造实验前，先跑这两个对照

* **阳性对照（证明 DSP 真读了你的目录）**：把 `sensors/config/*sh3001*`
  从自定义根目录里删掉再跑 `sscexp.sh accel`，必须得到
  「SSC 说没有传感器提供 data_type=accel」。
* **阴性对照（证明失败发生在 probe 层）**：把加速度计的 `slave_config`
  从 54 改成 99，同样应该"消失"。

两个对照都过，实验结果才可信。

## 收工

```sh
adb shell '
  pkill hexagonrpcd
  [ "$(cat /sys/class/remoteproc/remoteproc0/state)" = running ] || \
    echo start > /sys/class/remoteproc/remoteproc0/state
  rm -rf /data/local/tmp/hr
  start vendor.hexagonrpcd-sdsp
'
```

等约 45 秒后用 `gaokun3-ssc-test accel` 与 `dumpsys sensorservice` 双重验收
（★ 框架层也要看 —— 自动旋转靠的是那一层，SSC 通不代表 HAL 会话还在）。
