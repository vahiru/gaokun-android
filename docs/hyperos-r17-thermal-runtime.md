# R17 温度驱动运行时验证（未完成持久部署）

设备仍是R16，boot ID `9eb885f6-2f6a-4298-a030-bfe416f671ac`。本轮不是“硬件没有皮温传感器”：实际DT存在skin-temp-thermal→ADC-TM5 channel5→sys_therm6；缺的是已配置为模块、却未安装的驱动。

同一R16源/配置构建的 `qcom-spmi-adc-tm5.ko` SHA256为 `a83d7104ff8f1a32cb20da0f74695de6bf07986f6badb25caf9b39fbbfcfbc67`。已从已部署R16 EFI解压Image验证真实内建vermagic，匹配 `7.2.9-gaokun3+ SMP preempt mod_unload aarch64`；uname的6.6.30是现有兼容补丁返回值，未使用强制加载。37个导入符号由vmlinux提供，无额外模块依赖。

隔离guard通过后正常insmod成功，驱动绑定adc-tm@3400，新增thermal_zone15，type=skin-temp-thermal。读回温度约27.3°C；trip为55°C passive、58°C passive、73°C critical，各hyst1°C，并绑定原DT的CPU cooling map。没有修改温度阈值或伪造读数。模块显示(O)为树外构建标记。

连续10组zone/temp与IIO in_temp_sys_therm6_input读取最大差0.006°C，数值约27.38°C；这是通道一致性证据，不是独立温度计校准或实际升温/降温验收。未为了触发critical而加热设备。

原Thermal HAL仍将CPU/GPU最大值作为skin，在候选HAL部署前不能把内核zone注册称为Android皮温适配完成。候选改为只识别真实zone并使用其trip，保留原CPU/GPU阈值；缺失/异常皮温配置不发布虚构数值。候选host策略测试通过，完整Android构建及运行时验证进行中。

当前模块从/data/local/tmp手动加载，重启不保证保留。计划下一内核将CONFIG_QCOM_SPMI_ADC_TM5改为y，确保HAL与框架读取门限之前传感器已经就绪。OEM ThermalResourceMonitor仍读不存在的小米board节点，不能把真实skin未经语义评估直接冒充该板温接口。

证据：`/Users/vahiru/.cache/hos-r17-thermal/` 的build产物、efi-version-evidence.json、load-result.txt、sensor-pairs.txt及before.txt。
