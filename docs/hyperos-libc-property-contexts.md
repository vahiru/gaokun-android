# HyperOS libc 诊断属性与 smmustall 审计洪泛

2026-10-29 离线分析；本任务未操作设备，未改动 out 下文件。

`gaokun3_smmustall` 的 sleep/log 进程反复读取 default_prop，并非脚本主动调用 getprop。`device/huawei/gaokun3/bin/smmu-nostall.sh` 用系统 sleep/log，每轮exec都加载 donor libc；运行时证据指出其启动诊断路径尝试读取四个没有属性标签的名字：

| 精确属性名 | 可确定用途程度 |
| --- | --- |
| persist.track.malloc.enable | 日志确认由libc探测；名称指向malloc跟踪，具体开关格式/实现未获小米源码确认 |
| ro.sanitize | libc探测已确认；名称指向sanitizer配置，不能等同AOSP的ro.sanitize.address等布尔属性 |
| persist.resleak.enable.nativeheap | libc探测已确认；名称指向native heap泄漏诊断，具体行为未确认 |
| persist.fd.monitor.enable | libc探测已确认；名称指向FD监控，具体行为未确认 |

`out/hyperos-r14/resume-logcat.txt:18830` 起，同PID12769的四条 `libc: Access denied finding property` 紧邻四条comm=sleep、scontext=gaokun3_smmustall、tcontext=default_prop的AVC；后续12770等每秒重复。脚本active时0.1秒轮询、suspended时1秒轮询，进程启动检查反复执行，将缺少标签放大为持续审计负荷。audit_lost可能还有其他来源，不能把总计下降目标仅归给这一处。

不能声称“所有donor进程都会报”。动态链接同份donor libc且走这些启动检查、SELinux域无default_prop读取权时会发生；已有读取权限的域不会拒绝，静态程序或不同libc路径不适用。现有日志中的多个HAL/工具也有同名libc警告，问题不只属于smmustall。修改PATH或给该脚本default_prop读取权不能准确修复属性分类。

## 最小修复

向最终 `/system/etc/selinux/plat_property_contexts` 增加四条 **exact string** 映射到既有 `u:object_r:libc_debug_prop:s0`，不改变任何属性值、不增加allow。选择string是保持未知donor值语义，不冒认其为bool。`ro.sanitize` 的exact匹配不会覆盖现有 `ro.sanitize.address` 等条目。

现有平台同类属性 `libc.debug.malloc.options`、`libc.debug.malloc.program`、`libc.debug.hooks.enable` 就使用libc_debug_prop。提取的donor平台CIL已有：

- `plat_sepolicy.cil:25243`：`allow domain libc_debug_prop (file (read getattr map open))`。
- `:57799` 对base_typeattr_247禁止property_service set；`:85414` 定义该属性集为domain减init。
- `:8472` 对非coredomain也禁止set；`:85426`定义base_typeattr_241为domain减coredomain。

因此重用此类型不需变更CIL，保留已有写入限制；不开放default_prop，不增加dontaudit，不关闭audit。标签改变后所有domain能够读取这些指定诊断属性，与现有libc诊断配置契约一致。若设备原来存在非零值，从“被拒绝”变为“可读”可能激活相应诊断逻辑；集成前应只读记录四项当前值，不能擅自清空或改为0。

## 五分区核对与证据范围

已对下列完整保存的property_contexts进行语义前缀检查：

- plat：`out/hyperos-r13/codec-offline/system-etc/selinux/plat_property_contexts`。
- system_ext/product：`out/hyperos-r2/probe-files/{system_ext,product}/etc/selinux/*_property_contexts`（原donor）。
- vendor：`out/hyperos-r13/vendor-policy-input/vendor_property_contexts`（gaokun3）。
- odm：`out/hyperos-r9/donor-odm-tree/etc/selinux/odm_property_contexts`（原donor额外检查）。

全部没有四项精确映射或匹配的非default前缀。原donor vendor也无四项。此为保存文件核对，未重新读取实机当前五分区；尤其gaokun3最终odm可能无property_contexts，不能用donor快照冒充最终文件。生成器要求集成时对五分区逐一提供最终文件或明确声明文件不存在，避免依赖旧快照判断。

## 生成器与验证

`tools/hos/prepare-libc-property-contexts.py`：

- 重复 `--context PARTITION=PATH` 提供plat/system_ext/product/vendor/odm；确实没有上下文文件的分区用 `--absent PARTITION`，plat不能缺失。
- `--plat-policy PATH` 校验已有全domain只读规则；同时要求plat的标准malloc诊断映射存在。
- `--out NEW_DIRECTORY` 写出plat_property_contexts和含输入/输出hash、缺失分区记录、变更列表的manifest。已有输出目录拒绝覆盖。
- 对任何分区的目标重复/不同映射、冲突前缀、非法条目拒绝处理；已存在本工具四项正确映射时字节级幂等。
- 属性值和策略文件只读，不产生新的allow。

`python3 -B tools/hos/test_libc_property_contexts.py` 通过5组测试：exact且幂等、跨分区/前缀冲突、重复映射、缺分区/既有策略规则、非法输入。用五份真实保存上下文运行纯函数验证通过，未写out。CLI输出/manifest在系统临时目录验证。

下次镜像仅替换plat_property_contexts，通过现有property-info编译/镜像校验流程后重启（已构建的property_info与映射不能靠运行时改文本刷新）。复测每次exec sleep的四种名称警告及对应AVC消失，其他审计仍工作，smmustall心跳/轮询保持；观察audit_lost增长率，不能只看总计是否为零。

主代理实机只读核验：四项 getprop 均返回空值，没有发现现存非零诊断开关。修复仍待镜像集成及重启验证。

已重新从当前R14设备读取全部五分区上下文和plat CIL，严格生成通过；没有缺失分区，没有发现冲突。新上下文SHA256为 `8e101399e5877562df1b44e1eecad2b2de31b2db84e1ff95542e4bb047a57c84`，仅追加四个exact映射，保留原文。输出在本次主机临时目录，尚未写入镜像或设备。

## R18 当前镜像候选（2026-10-30）

已从运行中的R17重新拉取五分区property_contexts和平台CIL；四项当前值仍全空，未修改。生成器与5项测试通过。以正在使用的R12 system-codec/image.erofs为基础生成新system镜像，验证4,807个条目的内容、元数据和所有抽取xattr，唯一改动system/etc/selinux/plat_property_contexts。

候选路径 `/Users/vahiru/.cache/hos-r18-libc/system/image.erofs`，SHA256 `d76882116fec9ce910afa252083462f2d9c49e6fe01ec59980c66fa85061544b`；新context SHA256 `8e101399e5877562df1b44e1eecad2b2de31b2db84e1ff95542e4bb047a57c84`。保留Codec2等原system修改，不增加allow/dontaudit、不更改属性值。

初次repack传入生成器输出目录而非目录内文件，被regular-file预检拒绝，未产生镜像；修正到plat_property_contexts.fixed/plat_property_contexts后构建和完整核对成功。此候选尚未打包super/部署；当前设备拒绝日志未因此消失，须后续冷启动验收属性标签和audit增长。
