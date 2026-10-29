# R19 自动亮度链路调查（尚未修复）

R18现场只读证据位于 `~/.cache/hos-r19-sensors/`。SensorService只枚举SH3001 accel/gyro两路硬件及AOSP融合，没有LIGHT。系统设置screen_brightness_mode=1，但亮屏后的DisplayPowerController仍mLightSensor=null、mBrightnessReason=manual。某控制器mLightSensorEnabled=true仅表示请求启用，不能当成光感实际存在或已出数。当前PM也未声明android.hardware.sensor.light。

这说明自动亮度开关目前没有可用测光数据支撑。没有修改用户亮度偏好、添加虚假LIGHT feature或注入lux。隐藏开关不能当作自动亮度最终修复。

既有实机证据（stage4-findings #118/#121）显示tcs3701曾available，但激活使sensor_process在sns_stream_service.c:436 fatal、SLPI恢复后连IMU也可能失效。此次没有启用ambient_light/proximity、没有重启DSP或破坏现有IMU会话。旧“bus5未应答”不能继续作为当前根因结论。

离线实际R18 vendor hexagonrpcd反汇编确认：fopen模式首字节w/a均被拒绝并返回0x14，错误仅fprintf(stderr)；init无console/stdio_to_kmsg，未在已有日志找到具体registry输出写请求，故尚不能证明写拒绝导致ALS激活fatal。配置output=/persist/sensors/registry/registry指向虚拟输出目录；旧映射含名为registry的0字节占位文件，不等于SEE只写单一registry文件。历史stage4-findings #37还记录过/persist/sensors/registry/registry/../temp.json写请求，不能将当前缓存日志未命中扩大成从未见具体路径。

上游写支持候选的默认映射写/rename过宽，fixture复现借symlink修改映射目录外哨兵；fsync还存在成功空实现。因此不直接移植启用。正在准备独立registry输出backend：普通映射继续只读、精确文件白名单、逐层NOFOLLOW、大小/数量/总量限制、真实同步及错误传播。不得覆盖原JSON/fac_cal或假装写成功。此为修复文件服务缺项的候选，不是ALS已恢复的证据。

下一步：离线真实C代码安全/协议测试与Android构建兼容审核；查明registry真实写入/rename需求及缺失RGB配置组后，才能安排有恢复保障的受控硬件验证。当前自动亮度仍未验收。


## 离线验证新进展

上游sscregistrygen对26原JSON按QRD/449筛选24个，生成141组（加旧占位文件即历史142）。实测sns_amd与sns_amd_sw_enabled生成同名组：base→enabled因open无O_TRUNC留下旧尾部成为非法JSON；反序虽JSON合法但丢enabled。既有文档记录过预生成142文件后accel失效，不能证明就是该缺陷导致，也不应再次预灌。

新增scripts/ssc/registry-schema严格工具，默认拒绝重复组；显式merge-disjoint只接受相同共享值与不相交字段，不猜版本/值优先级。真实数据离线合并141文件，共37480字节；逐输入逐字段保留，原SHA不变，未创造缺失RGB校准组。主控独立复跑9测试通过。

registry受限写prototype仍基于fork，不是生产435ed1c的直接补丁。三个真实C fixture已由主控独立复跑review2通过ASan/UBSan；只读fsync故障传播、跨别名writer锁、部分写留下实际字节均有覆盖。失败written=0不代表未落盘，失败树不得自动当有效registry复用。默认写拒绝码20→1与生产不同，禁止据只读权限相同宣称行为等价。

已从源配置/生成器与历史temp.json路径建立更真实的文件名白名单候选，但backend单文件限制尚未扩展、完整SEE协议仍未验证。下一步优先回移最小所需能力到生产pin，保留原错误码/读路径；明确真实group输出和temp/rename协议，再做Android构建及受控验证。当前R18设备未改，ALS/自动亮度仍未修好。
