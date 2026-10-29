# R14 剩余日志离线审计

范围：只读 `out/hyperos-r14/resume-logcat.txt`、既有 donor 反编译源码与本地 A17 参考源码。未操作设备、修改 out、启动 VM 或调整日志等级。

本次日志统计：DolbyVisionGralloc5 70 条、init `failed write pid` 52 条、millet socket 创建失败 47 次；没有 FATAL EXCEPTION、Fatal signal 或 AddTidToCgroup。没有同步帧时间轨迹，不能把这些日志直接认定为用户掉帧根因。

## 1. Millet：实际不可用服务反复退出，应优先处理

日志例如 20:53:38 显示 socket type 1 创建失败 → 服务正常状态码 0 退出 → init 重新启动。即使没有 native crash，这仍是实际服务退出循环；并会产生额外 Binder 清理、进程与 cgroup 创建。CPU 事件部分显示采样占用为零，所以当前证据不支持它是高 CPU 消耗者；调度唤醒与电耗影响仍需测量。

已有兼容开关：`persist.sys.powmillet.enable`。donor product 默认 true；`GreezeManagerDebugConfig.mPowerMilletEnable` 在类加载时读取。`GreezeManagerService` 初始化仅在该值为 true 时设置 `sys.millet.monitor=1`。`init.milletmonitor.rc` 中服务本来就带 disabled，只有这个属性触发显式 start。

最小可行修复候选是在不具备 Xiaomi Millet 内核协议的设备上，把此属性默认及已有持久值设为 false，重启验证监控器从未启动。它禁用的是依赖缺失内核接口的 Xiaomi 扩展，不需要关闭 AOSP cgroup/freezer。

进一步审计明确其范围**不止监控进程**：它还门控 OEM job/network 限制与 Immobulus/launch 模式。donor `CachedAppOptimizer.updateUseFreezer()` 读取该属性，CN 且 true 时否决 Google freezer；false 移除此否决，但已有 `cached_apps_freezer=disabled` 仍优先。`GreezeManagerStubImpl.allowGoogleFreeze` 继续按 Google/CTS 应用筛选，因此也不能声称所有应用自动获得 AOSP 冻结支持。`AurogonImmobulusMode.initCtsStatus` 在 false 时不执行可能写 disabled 的初始化；已有持久设置不会自动迁移。此开关不改 cgroup 挂载或内核 freezer 能力，但应用冻结策略可能变化，需独立回归。

已增加显式生成入口（尚未接入镜像或设备）：`python3 tools/hos/prepare-startup-compat.py --kind product-props --input PRODUCT_BUILD_PROP --out NEW_PRODUCT_BUILD_PROP`。只替换唯一且值为 true/false 的 `persist.sys.powmillet.enable`，拒绝缺失、重复或陌生值，保留其他属性；默认 init 模式行为不变。输出使用独占创建，避免覆盖输入/已有产物。该工具不修改当前 data 持久属性或 cached_apps_freezer 设置；部署前须记录这些值并在重启后分别验证 monitor 停止与后台应用恢复。

注意：不能把运行时 setprop 加 stop 当作完整证明。Java 字段已缓存旧值；监控器死亡注册的 AlarmListener 会在偶数轮无条件再次写 `sys.millet.monitor=1`。从干净新 boot 验证最可靠：至少覆盖两次 180 秒 alarm 周期，确认 monitor 停止、无 PID 重生，并检查通知/后台应用恢复与休眠。

**不要设置 `persist.sys.millet.cgroup=true` 来尝试修复 socket。** 现有 rc 会在该触发器中把 tmpfs 挂到 `/sys/fs/cgroup` 并建立 v1 freezer，与已工作的 cgroup v2 挂载可能冲突。缺失协议不能通过广泛 SELinux 放行获得。

## 2. init memcg：不是此前 system_server 任务迁移失败

同一时刻可见 `failed write pid 10640 to memcg` 后约 3 ms，libprocessgroup 成功创建 `/sys/fs/cgroup/system/uid_0/pid_10640`，服务随后正常开始执行。失败没有阻止进程创建。与此前 `AddTidToCgroup ... EACCES` 是不同路径；后者在当前日志为零。

本地 AOSP/Lineage init 源码没有这个日志字符串；donor init 二进制包含该字符串以及 `/dev/memcg/mimd/critical0/cgroup.procs`，提示 donor MIMD 特有接入，但未完成指令交叉引用，不能把该路径当作已证明的唯一失败目标。

目前不建议为了日志修改内核 cgroup 配置、重挂载层级或放宽权限。先移除 Millet 退出循环，减少无效启动及伴生 memcg 消息；若剩余消息关联真实内存管理异常，再精确读回失败目标及 donor feature 开关。未找到经验证的单独 init memcg 兼容开关。

## 3. Dolby metadata：可解释的 mapper 能力不匹配，尚无掉帧因果证据

70 条日志集中在 21:03:03–21:03:40 的交互期，来自同一 PID/TID 828/867，没有同步显示提交失败、fatal 或 mapper handle 错误。每次 metadata 错误可能增加工作和日志，但不能从次数推断渲染阻塞时间。

本地 A17 minigbm stable-C `mapper_stablec/Mapper.cpp` 的 getter 支持 SMPTE2086、CTA861_3、SMPTE2094_50，但没有 SMPTE2094_40 分支，落入 `-AIMAPPER_ERROR_UNSUPPORTED`。旧 gralloc4 mapper 对 SMPTE2094_40 明确编码 `std::nullopt`。这与 donor Dolby 查询 HDR10+ metadata 失败吻合。实际运行 mapper 是否与此参考文件完全对应、以及 framework 把 unsupported 映射成 -74 的精确路径，尚需二进制/源码版本核对，不能仅凭错误码断言元数据损坏。

未在已读 product 属性或源码中发现可验证的单独 DolbyVisionGralloc5 开关。不要猜测 debug 属性，也不要把错误等级降到不可见。

若确认运行源码相符，最小候选是给 stable-C mapper 的 SMPTE2094_40 查询实现“明确无可用元数据”的合规响应，参照 gralloc4 的 nullopt 行为；这不是伪造 HDR metadata，也不宣称支持 HDR10+/Dolby Vision。是否还需要 descriptor 查询与 supported-metadata 列表同步，应按 stable-C 接口契约验证。不能仅加一个返回值便视为完成：需验证编码/解码 nullopt、正常 SDR 合成、视频路径及现有 SMPTE2094_50 元数据不受影响。若产品真实要求 HDR10+ 写入/透传，则必须完整实现存储与获取，不能丢弃写入。

当前适合先做 Millet 的现成兼容门控；Dolby mapper 修改应在实际 UI 帧时间证据或明确接口正确性验证支持下实施，避免为消除非致命日志引入整个图形栈回归。
