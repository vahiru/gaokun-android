# PowerKeeper APK 的59号场景消费者审计

本轮直接分析 `out/hyperos-r13/PowerKeeper.apk`，SHA256
`164f2b6271eef11afaf5c97984011f01329173099accf69bada3b815a26cbd44`。
产物在 `~/.cache/hos-r18-powerkeeper/`，未修改out、设备或VM。

## 实际发现

APK只有classes.dex。逐ZIP成员解压检查（含dex、资源、assets、native成员）未发现
`thermal-common`、`thermal-ai-policy`、`SystemResourceRedline`、`ResourceRedline`
任何明文匹配。全包JADX输出3406个Java文件，任务报告12个反编译错误，日志保留；不能将其当成完全无遗漏的源代码。

定向检索全部ScenarioRecognition/RimParameter调用，确认三个直接场景注册：

| 注册点 | 场景 | callback读取与作用 |
|---|---|---|
| EventLogManager.java:133 | INTERACTIVE_ACTION_TYPE_CAMERA = 140737488355328 | :122 callback只转发type/active到notifyFullScreen，不读RimParameter |
| PeSchedManager.java:96 | 63 | :31 callback只读`view_value`，转发PeGameController.d0 |
| PadSleepModeController.java:2562 | 18014398509481984 | mLidStatusCallback，盖子状态管理 |

另外两类主动调用而非59订阅：

- DisplayFrameSetting.java:1119/:5721 调用ScenarioRecognitionManager.method(1806,5,RimParameter.from(bundle))。
- n/a.java（ScenarioSharedMemory）构造器调用AppScenarioStub.method(1793,1)，取SharedMemoryPolicy的ParcelFileDescriptor；读取的是共享场景时间/ID，不见59温度level解包。

上述文件路径相对cache的decompiled/sources，其中具名类位于com/miui/powerkeeper对应包。
数字59检索命中protobuf/perfetto字段、序列化/日期/常量及JADX诊断，不见59场景注册。

## 与59号不能混淆的真实热策略

PowerKeeper有独立ThermalManager路径：

- feedbackcontrol/ThermalManager.java:1492 `getVirtualBoardSensorTemp()`直接读board_sensor_temp，合法数字parseInt，否则返回-1。这是自己的sysfs读取器，不是接收59号payload。
- :2018 `ThermalPerfLimitsArraySend()`依据mPerfLimitsTemp、norLevel/midLevel及lowCapLevel计算0/1/2，随后发sticky broadcast `action_thermal_control_change`，extra=`cur_level`。这不是SystemResourceRedline的0..4五级协议。

因此不能把这个独立0..2等级表拿来解释59号level0..4，也不能据这段代码声称新增valid/source已被OEM59消费者读取。

## 结论与覆盖边界

本APK静态证据不支持“PowerKeeper是thermal-common/thermal-ai-policy的59号消费者”。未找到59号level0..4策略作用，也未找到该协议的valid/source处理；三个确认的场景callback里没有该协议。

已覆盖：该APK唯一dex的明文标识、全部JADX场景API引用与数字59、所有ZIP成员对应标识扫描。scan.json记录输入hash/DEX列表/无命中及输出规模，jadx.log保留12错误。没有审计运行时下载/加密配置、外部框架库实现、动态加载其他APK/JAR或native间接绑定，故不排除这些渠道，也不将“APK未发现消费者”扩展为“系统没有消费者”。

下一步应改查AI服务/其他已安装系统客户端对ScenarioRecognitionManager的59注册，而非继续把PowerKeeper已有独立thermal控制等同该OEM新协议；或在59注册点做只针对该ID的UID/PID观测以确定实际客户端，再定向审阅。

主控补充：当前R18设备/system_ext/app/PowerKeeper/PowerKeeper.apk整文件SHA256与本审计归档完全一致，排除“用旧APK替代当前版本”的偏差。后续对其他系统AI客户端的扫描另行进行，不能把PowerKeeper无匹配推广为系统无消费者。

## 扩展系统客户端扫描

先扫描out全部30个APK的DEX/native；除PowerKeeper的ScenarioRecognitionManager/Callback描述符外，无SystemResourceConstants或目标thermal/Redline命中。补扫cache中Settings/SystemUI/plugin/miuisystem/miuix/framework-ext-res也未找到目标常量。没有修改out。

仅只读列系统包路径/UID后，门禁源码确认允许远端注册者是UID<10000且不等于2000，或PowerKeeper包。以下七个经授权从只读系统分区拉到cache；原路径、UID、完整SHA与DEX/native清单在candidate-scan.json：

| 包 | UID | 原系统APK |
|---|---:|---|
| com.xiaomi.aon | 1000 | /product/app/MiAONServiceW/MiAONServiceW.apk |
| com.xiaomi.aiasst.vision | 1000 | /product/app/AiasstVision/AiasstVision.apk |
| com.xiaomi.joyose | 1000 | /system/app/Joyose/Joyose.apk |
| com.xiaomi.cameramind | 6120 | /system_ext/app/CameraMind/CameraMind.apk |
| com.miui.thirdappassistant | 1000 | /product/app/ThirdAppAssistant/ThirdAppAssistant.apk |
| com.miui.contentcatcher | 1000 | /product/app/ContentCatcherOS4/ContentCatcherOS4.apk |
| com.miui.securitycenter | 1000 | /product/priv-app/MIUISecurityCenterPad/MIUISecurityCenterPad.apk |

七包均有真实DEX（不是仅资源/已剥离代码），都未找到SystemResourceConstants类名、thermal-common、thermal-ai-policy、SystemResourceRedline及直接ScenarioRecognitionManager/Callback描述符。

继续补查点号反射名，发现Joyose与SecurityCenter含`miui.scenariorecognition.ScenarioRecognitionManager`。对这两个命中包定向反编译后确认：

- joyose/sources/b/a.java 的CoreScenarioRecognition仅反射getInstance、RimParameter.from/toBundle、setScenarioState的两个重载及notifyScenarioChanged。
- securitycenter/sources/nm/a.java 同样只反射以上生产者接口。
- 两树无registerScenarioCallback/IScenarioRecognitionCallback/目标thermal常量命中。因此这些反射调用是场景上报方向，不是59消费者订阅证据。反编译日志保留，均有JADX错误，不能扩展为代码形式化完备证明。

其余五包连scenariorecognition相关明文都未命中。`candidate-broad-strings.json`记录反射发现，避免仅依赖类描述符漏判。

另在系统包列表发现AIService(com.xiaomi.aiservice UID10112)、MIUIAICR(com.xiaomi.aicr UID10119)；两者不在当前直接远端注册门禁允许范围，未继续拉取。可以通过其他有权限代理/共享框架提供功能，但不能仅凭AI名字认定它们直接订阅59。

本轮结果收窄了七个优先候选，没有找到真实59客户端，故仍不能写出level0..4的终端策略或valid/source读取契约。未覆盖其余所有UID1000系统APK、外部JAR/native服务以及运行时下载模块；下一步较高收益是59注册点限定观测或框架本地资源驱动注册表，而非猜测AI包。
