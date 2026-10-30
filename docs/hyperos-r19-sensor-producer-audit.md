# R19 fusion故障：真实HAL的无样本上报审计

本审计只读源码、归档镜像和已有测试日志，没有读取设备、启动服务或注入数据。

## 二进制身份与源码边界

从只读 `out/hyperos-r4/vendor.img` 的 `/bin/hw/android.hardware.sensors-service.gaokun3`
提取到 `~/.cache/hos-r19-fusion-probe/producer-audit/hos-vendor-r4-sensors-hal`，SHA256：
`ffc36303c168796322ec5483cd73bea7abc111b208ab1c98bdcd99725850687e`，与主代理给出的实机SHA完全一致。
BuildID为 `2ef6fe920223c897a31abc0bacbd60dc`。

该ELF虽stripped，但带 `.gnu_debugdata`；其内符号表直接定位了下面的实际函数，
所以关键行为不只是根据相似源码推断。`scripts/ssc/hal-producer/audit-hal-binary.py`
对完整SHA、mini-debug符号和关键AArch64指令作可复现断言。

模块入口是 `device/huawei/gaokun3/sensors-hal/Android.bp`，srcs为main/Sensors/Sensor/SscHub。
本地tracked无ALS源码最后变更commit `adb93029ad8de3c5241d0f3e9ef555f4327c9935`，
与所核对的实际函数和Sample/Hub布局一致。CICD当前source已经包含ALS分支，产品输出SHA为
`99cd5ecb0b0bbb6f248e928e5f2102baf5319f15fcbe14564a4ef1af923e9f0a`，不是本HOS二进制，
**不能整体复制当前CICD HAL替换FFC基线**。

没有找到FFC当时完整构建输入哈希清单；尚未逐字节重建全HAL或证明全部静态依赖版本。
下述候选基于独立复制的本地无ALS源码，只改已经和FFC实际ELF核对过的readEvents逻辑；
不把“关键函数吻合”写成“整个源码/构建已精确复原”。

## 真实代码中的缺口

- `SscHub.h::Sample` 默认xyz全0、accuracy0、valid=false。
- `SscHub::Fresh` 超过2秒未更新返回默认Sample；停用一路时缓存也被置为默认Sample。
- `Sensor::run`（内联于实际 `Sensor::startThread`，VA0x1e70c）按本地sampling timer调readEvents。
  实际0x1e834使用CLOCK_BOOTTIME；0x1e84c–1e860计算period/last tick并把当前时间更新进去。
- `Sensor::readEvents` VA0x1eb6c–1ec38总是调用elapsedRealtimeNano、调用void payload函数，
  然后分配并返回一条96字节Event。没有“当前没有SSC样本”这一返回分支。
- `AccelSensor::readEventPayload` VA0x1eec4、Gyro对应0x1f068，都无条件写xyz。
  Accel实际0x1ef10/1ef18写向量，0x1ef1c–1ef24才依据valid/accuracy选择status，
  **valid只决定UNRELIABLE，而不决定是否发布事件**。
- `SscHub::Accel` VA0x1f4d4中，0x1f55c/1f560确实在过期时返回20字节全0Sample；
  Gyro的0x1f630/1f634同样如此。Get初始化也显式清零cache及valid字段。

因此没有新鲜SSC样本时，HAL仍制造带新单调时间戳的零向量事件。
这与把真实传感器零值标为不可靠是不同的事实：缺失数据不应被编码成一条测量。
`UNRELIABLE`不是“没有事件”，不能用来假定framework会跳过这条零向量。

## 与冷启动观测的关联

已有 `cold-results.txt` 中，首两轮acc/gyro全部为0；第一轮acc348条、gyro349条，
第二轮各349条，时间戳仍连续更新且间隔约20ms。第三轮采样末尾各11条变为非零，
gravity/game仍持续NaN。增强probe按原规则把有限、时间正确的零数据判为通过，
这正说明仅有数值/时间戳检查无法区分“有效零测量”和“无样本缓存”。

上述源码+实际ELF确认了一条能够制造该启动序列的真实producer缺口；日志本身没有
读取HAL内部valid位，因此不靠“零看起来不合理”就断言某个单独事件一定伪造。
后续需要candidate冷启动A/B来检验因果，而不是放宽probe或用9.8替换零。

## 最小候选（离线Android构建已完成，尚未上机）

独立目录：`producer-audit/baseline-src`、`producer-audit/candidate-src`。
候选先修改 `Sensor.cpp::Sensor::readEvents`：

1. 对acc/gyro只取一次SscHub快照。
2. `valid=false`立即返回空events，甚至不取新的事件时间戳。
3. `valid=true`才构造事件，原xyz和accuracy映射保持，包括真实gyro静止0或自由落体acc0。
4. 其他sensor类型仍走原虚函数；未改SSC枚举、流启停、恢复策略、ALS或权限。

只在外层先检查valid、随后再让readEventPayload重取一次cache会有失效竞态，故没有采用双读。
候选仍保留原2秒Fresh语义和有效缓存的timer重采样；没有在此混入SSC时间域转换或去重重设计。

后续sink检查还发现必要配套：`Sensor::run`仍会把空vector交给`Sensors.h::postEvents`，
旧sink直接取`events.front()`（空vector未定义行为），零长度write若成功还会触发FMQ flag wake。
候选在postEvents入口增加`events.empty()`早退。因此最终手写变化为Sensor.cpp与
include/sensors-impl/Sensors.h两文件；周期不改，空数据不访问front/FMQ/wake/wakelock。
实际sink body提取测试覆盖空vector合同错误、两种wakeup参数、FMQ失败/无flag，以及真实零的非空事件。


归档补丁：`patches/hos-sensors-no-unavailable-vector-events.patch`。
host测试提取baseline/candidate的真实readEvents、真实Accel/Gyro payload与Fresh函数，
以mock cache/时钟运行ASan/UBSan。已复现旧代码缺陷，并验证候选无样本/过期不发事件、
有效零与accuracy0照发、单快照避免二次读取竞态、其他路径不变。有效但NaN的输入仍保持
原可观察行为，候选不借过滤掩盖额外质量问题。

```sh
python3 scripts/ssc/hal-producer/audit-hal-binary.py
python3 scripts/ssc/hal-producer/test-unavailable-sample.py
python3 scripts/ssc/hal-producer/test-empty-event-post.py
```

测试替身覆盖控制流，不宣称验证了Android AIDL ABI、整HAL链接或硬件。源码/ELF审计、
候选输入哈希和host输出均在缓存 `producer-audit`。

producer门控与framework防护是互补的：真实有效的全零加速度仍可能出现，Fusion仍需避免
零norm初始化除零及坏状态永久传播。不能为了让Fusion通过而从producer删除真实零数据。


## R20成对Android构建与恢复

离线在CICD同一AOSP工作区、同一模块名/依赖下，先构建无ALS源码对照，再只应用上述两文件修复。
两次构建均成功：

- 无ALS重建baseline：`b4eae0636856e53095f944dce59bbde8dd9249884c9b1fb06d4302cb16cf873d`。
- producer候选：`ab794e19343bfee8e3690d656203ff4b3a737189c8527591dfaa4029b0f548d4`。

baseline不等于旧FFC，section级比较还显示.text及其它alloc sections差异，并非仅debug/BuildID，
故没有宣称完全复原旧构建。旧FFC、新baseline、新candidate三者的
DT_NEEDED、dynsym imports/exports合同无差异；动态六依赖均为libbinder_ndk、libc++、libc、
libdl、liblog、libm。符号地址/函数长度不是ABI合同，OBJECT尺寸及符号版本名纳入了比较。
三份stripped ELF均无.comment，不能以空列表相等声称编译器版本相同；Android.bp的静态依赖
模块清单有记录，但FFC当时的静态库精确哈希仍未知。

源码/传感器元数据保持无ALS的acc+gyro列表，init rc与VINTF字节相同。构建事务结束后，
CICD原ALS源码、该模块Soong中间目录，以及product vendor/symbols/init/vintf四项输出均逐项
恢复并核对内容/模式/mtime；原已安装HAL读回`99cd5e...e9f0a`。
另外发现m生成的prebuilt和symbol-map派生缓存未纳初始快照，已从恢复后的原vendor/symbol ELF
重新生成，并记录候选→恢复后哈希；不谎称这两项初始mtime可证明，也不称global Soong记账未变。

完整产物与证据位于 `~/.cache/hos-r20-sensors-hal`。没有设备安装、服务重启、注入数据或部署。
