# R16 真实温度能力与 OEM 监视器适配

只读调查，未加载模块、改配置、改温度阈值或修改真机。缓存 `~/.cache/hos-r16-startup-audit/`：thermal-capabilities.txt、thermal-adc.txt、live.dtb/live.dts、scenario-dump.txt、modules-list.txt。

## 关键纠正

不能依据 thermal_zone 列表或现有 HAL 注释断言设备没有真实皮温热敏电阻。当前15个已注册zone确实只有CPU/GPU/mem/PMIC，但IIO另有sys_therm1–8，且实际DT明确将sys_therm6连为skin-temp-thermal。

实际设备树链：

- skin-temp-thermal: `thermal-sensors=<0x12f 5>`。
- pmk8280_adc_tm: phandle0x12f，status=okay；sys-therm@5 的 io-channels=<0xe9 0x345>。
- pmk8280_vadc: phandle0xe9；channel@345 标记 sys_therm6，即 PM8350_ADC7_AMUX_THM2_100K_PU(3)。
- `/sys/bus/iio/devices/iio:device0/in_temp_sys_therm6_input` 实读26479，即26.479°C。其他sys_therm1–7约29–30°C；sys_therm8=-40960，不能假定全部通道都连接有效热敏电阻。

因此sys_therm6是有DT语义证据的皮温候选，不是CPU代理。仍需物理位置/校准与时间响应验证，不能直接声称等同小米原机board传感器。

## skin zone 未注册的直接线索

真机 CONFIG_THERMAL_OF=y，CONFIG_QCOM_SPMI_ADC5=y，但 CONFIG_QCOM_SPMI_ADC_TM5=m。实际adc-tm@3400 platform device存在，modalias为qcom,spmi-adc-tm5-gen2，却没有driver链接；/proc/modules无对应adc模块。本轮列出的标准modules目录未找到对应模块。

DT已经okay，不需要盲目改DT。首选下一步核对生产内核对应模块产物与依赖，补入/加载匹配内核的ADC-TM5驱动，让现有thermal zone正常注册。该加载是设备行为变更，本轮没有执行。

启用之前要审查已有skin DT trips和cooling-maps：55°C、58°C passive，73°C critical；驱动注册会激活已有阈值，不能当成仅新增一个读数文件。

## HAL 不能原样承担真实skin语义

当前HAL AIDL3正常连接、Thermal Status0；真实CPU/GPU/PMIC温度可读。`device/huawei/gaokun3/thermal/Thermal.cpp` 启动时Discover一次，只把CPU/GPU/cluster标记feeds_skin，SkinTempC取其最大值。HAL名为skin的值因此是裸片代理，并非sys_therm6。

同文件注释“没有外壳热敏电阻”与上述DT/IIO证据不符，应后续修正。即使ADC-TM驱动成功注册zone，现有HAL也不会自动将其用于SkinTempC；动态注册后还需要重新Discover/重启HAL。

真实skin适配必须同时分离温度类型与阈值。当前所有通道统一60/70/80/90/100/105°C，这是裸片策略，不能原封不动套用真实外壳温度；也不能把真实skin代入后仍声称其阈值经过校准。保持内核CPU保护独立。

## OEM监视器单位与实际阈值

只读dumpsys scenariorecognition确认：

```
thermal-common 0 33000 38000 40000 100000
thermal-ai-policy 0 33000 35000 39000 40000
period 10000
```

ThermalResourceMonitor原样parseInt文件，没有单位转换；以上阈值和IIO输入均为毫摄氏，单位匹配。33–40°C的AI policy阈值更接近board/skin语义，直接接CPU/GPU会导致过早策略激活。当前LastTemperature=0是缺节点后从未更新的初始化值，不能当成真实0°C。

## 是否有thermal专用配置

SystemResourceRedlineScenario(component59)支持thermal-common、thermal-ai-policy各5个阈值，另有battery类型。`period`对全部monitor更新；active启停全部monitor；method返回null，没有发现专用thermal启停命令。

RimComponentManager读取资源数组ID285409606，并用Settings.System的mirim_component_cloud_params按component覆盖。冷启动若59号参数仅含battery与period，thermal无threshold则不会采样；运行中省略thermal不会清除旧阈值，parseThresholds也不接受空数组。此能力仅记录，不建议在存在真实传感器适配路径时先使用它关闭监视器。

## 推荐顺序

1. 核对/补齐同版本ADC-TM5模块，审查现有trip/cooling-map后做可回滚加载验证。
2. 验证skin-temp-thermal读取与IIO sys_therm6一致、数值和响应合理，排查断路/错误标定。
3. 调整HAL识别真实skin并使用经过评估的皮温阈值，替换当前CPU代理；不要改变CPU/GPU阈值语义。
4. OEM监视器改为按明确类型读取真实skin温度并转为毫摄氏，或限定gaokun平台解析已确认传感器。保留不可用状态，不伪造温度，不把任意thermal_zone编号硬编码进去。

本轮提供了可验证真实传感器路径，尚未完成模块加载、热响应或阈值标定；暂不实施unsupported监视器补丁或随意链接OEM路径。

## 已准备的 HAL 候选（尚未部署）

`device/huawei/gaokun3/thermal/Thermal.cpp` 现在精确识别 `skin-temp-thermal`，不再发布CPU代理skin。温度依旧毫摄氏转摄氏。真实skin使用该zone当前sysfs trip：严格要求两个passive和一个critical、正数且严格递增，将其映射LIGHT/SEVERE/SHUTDOWN；未定义等级为NaN，不插值或使用裸片105°C。未知trip类型、重复critical、缺失/畸形/逆序温度均不发布该skin温度及阈值，forecast返回unsupported。现有CPU/GPU/PMIC门限不变。

每次API/轮询快照重新读取本地拓扑，避免ADC-TM模块稍后加载时必须重启服务；没有共享可变zone vector。callback缓存由原数组索引改为(type,name)，消失/读取失败项退出缓存，再出现发送初始事件。远端Binder callback不持有注册锁。无可用skin时不会发送伪造NONE清除旧状态；framework对于曾存在后消失的传感器缓存仍需设备验证，这属于AIDL没有独立sensor-removed通知的限制。

策略共用helper `ThermalPolicy.h`；`tools/hos/tests/thermal_policy_test.cpp` 编译实际helper，覆盖精确类型、毫摄氏与非法/越界输入、正常和缺失/重复/逆序/未知trip、55/58/73°C边界、CPU原阈值。主机C++17、-Wall/-Wextra/-Werror、ASan/UBSan通过。命令：

```
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=undefined,address tools/hos/tests/thermal_policy_test.cpp -o /tmp/gaokun-thermal-policy-test
/tmp/gaokun-thermal-policy-test
```

尚未完成完整Android HAL交叉编译、模拟sysfs服务级测试或设备动态模块/callback验证；主机helper测试不代表完整HAL已编译。候选不调整内核trips/cooling-maps，不建立OEM节点，也尚未修复OEM监视器本身。
