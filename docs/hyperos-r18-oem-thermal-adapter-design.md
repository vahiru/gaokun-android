# R18 OEM ThermalResourceMonitor 适配设计

状态：设计与只读核对，未修改framework或设备。缓存 `~/.cache/hos-r18-thermal-design/`。

## 结论

Android已有可用真实SKIN源，可以消除对不存在board_sensor_temp的反复访问；但不能直接认为gaokun皮温与小米board温度可共用原阈值。建议保留原board优先，在显式gaokun配置下新增命名SKIN后备源，并将“数据可用”和“获准驱动OEM策略”分开。当前消费者/标定证据不足，先不把皮温代入59号旧策略，也不伪造0度或normal事件。

## 目前可证实的调用与语义

`ThermalResourceMonitor` → `BaseResourceMonitor.processValueChange` → `ThresholdGroup.updateCurrentLevels` → `SystemResourceRedlineScenario.dispatchLevel` → `ScenarioManager.notifyLocalCallbacks/notifyRemoteCallbacks`。

59号payload是 `{state: level>0 ? 1:0, level: 0..4, type: thermal-common|thermal-ai-policy}`。两类各五级，使用毫摄氏整数，按大于等于阈值从高到低匹配；无hysteresis。level是数组索引，不是Android THERMAL_STATUS 0..6：

| 类型 | level0 | level1 | level2 | level3 | level4 |
|---|---:|---:|---:|---:|---:|
| thermal-common | 0 | 33000 | 38000 | 40000 | 100000 |
| thermal-ai-policy | 0 | 33000 | 35000 | 39000 | 40000 |

这是OEM资源/AI策略红线，不是HAL的55/58/73°C LIGHT/SEVERE/SHUTDOWN；不能替换成HAL状态码，也不能直接把那三项门限拼成五级数组。

`ScenarioManager`支持注册本地、按优先级本地、远端Binder消费者。已查源码没有找到以59或这两个type字符串显式注册的具体策略实现；注册可能由资源/客户端动态驱动。dump只打印callback集合数量，不列各type的远端身份。故尚不能列出真实59消费者及其level副作用，不能推断它只是日志，或声称禁用无功能影响。需要在后续候选中仅对59注册记录调用UID/PID/接口类别（不记录其他场景内容），并审阅实际客户端。

当前read-only实测：HAL名skin-temp、type3、27.165°C，门限55/58/73；OEM温度LastTemperature0、CurrentLevels[0,0]、10秒周期。该0是初始化残留，不能作为有效数据。

## 源适配（明确平台限定）

新增只读vendor配置，例如 `ro.vendor.gaokun.oem_thermal_source=skin-temp-v1`，缺省保持原设备行为；该配置仅由gaokun镜像显式设置，不根据“任何机器缺路径”自动启用。还需单独reviewed skin policy标识/阈值来源，不能把source开关当成阈值批准。

源状态机：

1. BOARD：原节点存在且读取成功，沿用原board路径、原阈值与事件。优先级保持不变。
2. 对gaokun缺失节点（明确ENOENT），只记录一次原因，转SKIN_PENDING；不再每10秒打开不存在节点。权限拒绝/坏数据与不存在区分，不应静默用fallback遮盖权限问题。
3. SKIN_PENDING/READY：worker调用 `IThermalService.Stub.asInterface(ServiceManager.checkService("thermalservice"))`，再 `getCurrentTemperaturesWithType(Temperature.TYPE_SKIN)`。这是system_server内部API；不走普通应用PowerManager headroom，也不把无量纲值当温度。实际ThermalManagerService该调用从HAL实时取数、检查DEVICE_POWER、未ready返回空数组。
4. 只接受唯一type=SKIN且name精确等于skin-temp的有限值，合理范围检查后用`Math.round((double)celsius * 1000)`转换毫摄氏。重复名、缺源、NaN/Infinity、越界一律无效，绝不取数组首项/最大CPU/名为skin的旧代理。
5. Binder死亡/服务未ready/暂时失联转TEMP_UNAVAILABLE，清binder引用，单独限频重连（例如30秒，成功恢复10秒采样），不创建无限紧密循环、不持monitor锁进行Binder调用。on stop取消所有token/runnable/death recipient，回调带generation避免stop后旧结果生效。
6. 如需恢复board优先，使用低频存在性探测或启动/配置变更时重探，避免异常堆栈循环；发现board有效才切回。来源切换必须记录、失效旧sample，不跨来源套用未审阅阈值。

以上可以真实获得皮温而无需伪建sysfs。其是否可驱动现有59策略必须另由来源专用阈值及消费者审阅决定。

## 不支持/不可用必须有真实状态

新增Availability枚举 `UNINITIALIZED / AVAILABLE_BOARD / AVAILABLE_SKIN / POLICY_UNSUPPORTED / TEMP_UNAVAILABLE`，以及`lastValidMilliCelsius`、`sampleElapsedRealtime`、`source`、`reason`。初始化/stop不能把测温值重设为0当有效值；dump/getter应返回明确有效性/Optional或保留MIN_VALUE，并显示stale年龄。当前getLastTemperature在已读源码无调用点，仍须dex交叉引用后才能改返回契约。

当皮温有值但没有批准的皮温策略时进入POLICY_UNSUPPORTED：可记录shadow值和采样时间，不调用processValueChange、不发布normal，不清上次level制造恢复；仅关闭此thermal monitor的旧board轮询，不触碰BatteryResourceMonitor、component59整体active、Thermal HAL或内核trip。

必须正视旧59协议的局限：现有state=0/1且level0..4没有unknown。发送level=-1会被当前`level>0`映射成state0，反而伪报normal；随意加available字段也不能保证旧消费者理解。仅停止事件可保证“不发送假的normal”，**不能保证所有旧消费者不会自行把缺事件视为normal**。若要求端到端unknown，必须给已识别消费者增加可用性契约并验证，不能只修改producer宣称完成。

`SystemResourceRedlineScenario.resetStateWithNotification()`在整体停用时会给非正常monitor派发0；候选应让thermal的invalid/unsupported状态走显式可用性处理，不调用该整体停用入口，也不修改battery正常复位逻辑。source恢复后首个有效采样必须按该source已批准policy重新计算并强制发送初始有效结果，避免缓存旧0导致无事件。

## 建议落地顺序与验收

1. 对59注册/消费者做限定观测与静态审阅，核对AI/common等级实际副作用；确认皮温阈值策略来源。未完成时最多shadow，不能宣布策略已适配。
2. 主机状态机测试：非gaokun完全沿用原board；gaokun有board仍优先；缺board转真实SKIN；名称/type错误/重复/NaN/失联不得生成normal；毫摄氏边界；stop/start generation；board恢复；battery事件完全不受影响。
3. 在system_server worker中验证真实Binder权限/接口签名，确认没有启动死锁、Binder阻塞或反复重连。比较HAL实时读数与适配器毫摄氏（允许采样时刻差）。
4. 对已批准阈值用注入的host测试源覆盖边界和level事件，禁止在真机伪造温度/跨越关机阈值。普通使用验证AI/common与battery行为；实际温度/校准验证另行规划。
5. 冷启动后观察至少多个原10秒周期：无board ENOENT循环；dump明确source/availability/last valid；真实HAL55/58/73及内核保护不变。失败时回滚单一framework候选，不关闭整套热管理。

当前不建议制作“sysfs软链接到某zone”或“缺路径return0”的补丁，也不建议仅通过关闭59号component消除日志。最小正确候选应是来源/可用性状态机，而不是静默吞掉IOException。
