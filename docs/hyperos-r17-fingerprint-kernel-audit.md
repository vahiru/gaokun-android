# R17 基线上加入0050的只读审查

2026-10-30：仅通过SSH读取CICD两棵树及本地临时副本检查。没有修改远端源码、
启动构建、启动VM或触碰真机。目标是保留R17修复，在独立新树加入指纹所需SCM接口；
不把crDroid旧实验EFI直接用作HyperOS内核。

## 当前输入与可复现性

| 项目 | R17隔离基线 | 旧指纹实验树 |
|---|---|---|
| 路径 | `/home/vahiru/hos-r17-kernel-thermal` | `/home/vahiru/gk3-kernel-72y` |
| EFI SHA256 | `d22e364af4fb403a65e03b1545e847765865c5c28012473779dd6b42cf3b60bc` | `9c6cb6794d5ab6aecf7373a3a6afe2c8daf39a65355bcef6d6b8bcdfd67d64b2` |
| qcom_scm.c SHA256 | `0440862805d3c25b62de3772c6f6877ac4ad578221272d2089785c6bf2addcd5` | `def25c996a2e67275221959785a871d69586ad0e588286ca5aafbc5b76bfe0dc` |
| qcom_scm.h SHA256 | `7c946d7e6509af94b9902f17e572153db88956a815d5897f3730a3abbe647d04` | `8ed2dcd2d5e870a4f155e62a15852d16763adbc1ed24704be4f8400eb4f45970` |
| ADC_TM5 | built-in `y` | module `m` |

补丁 `0050-firmware-qcom-scm-qseecom-app-load-shutdown-listener.patch` SHA256
`467009d7947249d32741f4fe57f9f23116e15e88c1a2096a26160b5f1b501d9e`。
在下载的R17两文件临时副本运行 `git apply --check` 成功；应用后两文件逐字节等于
已实测旧实验树的对应文件。因此没有必要复制旧实验树的其他内核改动或配置。

`qcom_tzmem.c`两树一致，SHA256
`410b1293897a636b505e07b7edb2516878882f0e75e64259dc91c1f160c110b3`；
`qcom_scm-smc.c`两树一致，SHA256
`399d29d832d265c972a8c927669be7047800bce1d46b0452fe17a8b875bf1f2c`。
本机证据在 `~/.cache/fingerprint-20261030-check/hos0050-audit/`，包括完整源码快照
和 `source-compatibility.json`。R17无.git是独立snapshot，应依靠输入hash审查。

## 32位DMA的实际影响范围

0050在qseecom_init发现有效版本后，对 **共享scm->dev** 调用
`dma_set_coherent_mask(...,32)`。`qcom_tzmem.c:207`所有pool都使用全局
`qcom_tzmem_dev`进行dma_alloc_coherent，该设备由SCM probe注册。因此它不是
指纹pool专用约束；UEFI变量缓冲、SCM扩展参数以及其他SCM内存也受后续分配策略影响。

实际R17的 `drivers/` 源码中，直接tzmem用户是qcom_scm、qcom_scm-smc和
uefisecapp；不能据此把其他通过SCM helper的功能当作完全无关。
原有qcom_scm_lock、convention选择、SHM Bridge协议与SMC报文并未被0050替换。

R17启用ZONE_DMA/ZONE_DMA32、CMA128MiB和TZMEM_SHMBRIDGE。地址收窄能满足
指纹加载的低4GiB要求，但会改变低地址分配压力与大块连续内存成功率。
补丁注释“缓冲都很小、无副作用”不能作为证明：指纹 staging本身可能申请8MiB。

SCM probe先创建initial_size=0的按需pool、发布__scm，再进入qseecom_init设置mask。
设置mask不会迁移已存在pool块，也不能保证此前并发SCM调用没有分配过高地址内存。
目前指纹工具先新建独立pool并检查完整物理区间、4MiB对齐；这些检查仍必须保留，
不能因为内核有0050就删掉。mask失败路径只是告警，调用者不能假定它总成功。

## listener与UEFI共享调用链

R17 `qcom_qseecom.c` 的in-tree客户端表只有 `qcom.tz.uefisecapp`。
uefisecapp所有EFI get/set/enumerate/query操作都经qcom_scm_qseecom_app_send；
其pool初始4KiB、增长上限256KiB。补丁不改变正常SUCCESS请求帧和返回数据，
但新listener处理位于所有QSEECOM调用共用的qcom_scm_qseecom_call中。

- 全局QSEECOM mutex跨listener往返持有。指纹不完整调用可以延迟UEFI变量操作。
  探针用户态/内核msleep间隔在app_send返回后执行，不会把该mutex跨整段轮询持有。
- 未注册listener收到FAILURE回复，改变了原先仅WARN的行为；这是恢复TA所需的
  显式处理，不是针对指纹名称的分支。
- 256轮限制只能限制已返回的listener循环，不会超时中断不返回的SMC。
  SCM本身还有全局调用mutex；一个真正挂住的SMC可影响比QSEECOM更广的调用者。
- 超限的最后一次FAILURE回应仍可能再次返回INCOMPLETE；函数返回-ELOOP不能
  证明TA已退出等待。当前探针secure_pending/pin和保留缓冲策略应继续保持。
- BLOCKED_ON_LISTENER返回-EBUSY，没有实现CONTINUE_BLOCKED_REQUEST。
- callback在QSEECOM锁下执行，不能递归调用任何QSEECOM接口；长时间I/O会阻塞
  其他客户。当前诊断没有注册持久存储listener，不能把后续录入功能也算验收。

在指纹未知状态时不要再发EFI写入作“恢复探测”。正常冷启动与清理后的只读EFI枚举
可以验证uefisecapp仍工作，不能用单次成功的指纹事务代替此项。

## 可选TEE入口与接口边界

当前R17 .config没有TEE_QSEECOM设置，drivers/tee也没有该Kconfig定义。
0050新增平台device分支受IS_ENABLED(CONFIG_TEE_QSEECOM)控制，当前为false；
不会仅因打补丁自动产生用户态QSEECOM TEE设备。此次应保持配置不变，不混入另一个
TEE前端或启用任意TA访问。

0050导出的app_load接口检查非空image、非零长度和mdt_len，但没有在核心再验证
32位完整地址范围/4MiB对齐或res.data转u32溢出。当前固定探针自行严格验证输入且
仅用已核验签名TA，可以沿用已测路径；未来面向用户态的正式driver必须独立收紧
这些边界，不能把原型接口视为通用已加固ABI。

## 候选与上机检查范围

1. 独立复制R17全树，保留Himax尺寸/坐标、UCSI、内置ADC_TM5、initramfs和编译元数据。
   0050仅改SCM两文件；.config应逐字节保持R17，而不是从旧实验树更新。
2. 构建后确认新增导出与版本真实一致、未启用TEE前端，记录EFI/模块/输入hash；
   分类检查对象差异，不能只比最终EFI大小。已有0050源码等同性不是新产物构建验收。
3. 使用独立oneshot内核条目，保留R17 EFI与默认启动项；启动后以cmdline、启动dmesg、
   实际导出符号验证，不能仅依赖被伪装的uname。
4. 首先在未加载指纹模块时验证EFI变量只读枚举、真实皮温/ADC绑定、USB device-only、
   Wi-Fi/ADB和IMU。关注qseecom初始化、tzmem/SHM Bridge与DMA分配错误。
5. 固定签名TA的LOAD/8031/无帧门控测试后，明确SHUTDOWN成功、secure_pending=false、
   GPIO恢复与模块清理，再做同样UEFI只读/系统回归。没有真实手指时无帧不是失败。

审查结论：最小0050源码可直接应用R17，且等同已测实验SCM实现；可以继续独立候选
构建。全局DMA与listener影响需要上述实际回归，不构成自动否定移植的理由，也不能
用旧crDroid实验运行记录直接声称HyperOS全部功能已通过。
