# HyperOS R14：UCSI 初始化超时修复候选审查

**后续生命周期审查结论：当前重试补丁暂停应用/构建，不可部署。** 发现初始化早期ETIMEDOUT的既有引用清理缺口，以及gaokun驱动没有调用core suspend取消工作的PM集成限制。下述12场景测试仅验证决策，不能覆盖这两个问题；详见末节。

2026-10-29，仅离线源码审查。没有设备操作、没有修改现有镜像或 out 文件，也未把候选纳入内核构建。

## 已证实问题及更正

R14 启动日志 `post-boot-logcat.txt:6228` 报 con2 failed to register alt modes，`:7149` 报 PPM init failed -ETIMEDOUT。当前 Type-C sysfs 为空，但 gadget/USB ADB正常；这两条路径可以独立工作。

v7.2.9 `ucsi_register()` 排队异步初始化后就返回0。`ucsi_init()` 的端口初始化失败路径会撤销已注册端口、释放尚未发布的 connector 数组、清空 capabilities 并 reset PPM。`ucsi_init_work()` 仅重试 -EPROBE_DEFER，不会重试 ETIMEDOUT，因此本次初始化停止后不会因用户拔插自行重建端口。

**撤回早先关于当前补丁链存在 num_ports 清理遗漏的判断。** 仓库 `patches/0088-usb-typec-ucsi-huawei_gaokun-set-the-number-of-ports-at-probe.patch` 已在 ports_init 中设置 `uec->num_ports`，同时限制最大端口数、移除 refresh 对该值的改写。`scripts/kernel-apply-patches.sh` 明确先应用0088再应用0090。仅查看原版v7.2.9与0090–0093会遗漏此修复；完整已应用0088的构建不应再按这个理由判定 remove 漏掉 mux 引用。

## 不宜直接增加 GET_ALTERNATE_MODES 原地重发

`ucsi_register_altmodes()` 按 connector、recipient、offset 分页查询。底层 `ucsi_run_command()` 先等待命令完成，再执行 ACK_CC_CI，两阶段的超时都会向上传递 ETIMEDOUT。现有两条错误不能区分查询超时和 ACK 超时。

0092 的特殊重发仅用于 GET_CONNECTOR_STATUS，其说明已经承认未经 CANCEL 的重发依赖实测 EC 行为，并存在延迟旧 completion 误完成下一命令的风险。该实测不能直接外推到 GET_ALTERNATE_MODES。只读命令不等于协议层可随意重放；分页结果可能被错配，已经注册的 altmode 还可能影响后续清理。

v7.2.9 `ucsi_run_command()` 仅在观察到 CCI_BUSY 时走 CANCEL；通常的 timeout 并不意味着命令已经被固件取消。因此不能简单将0092条件扩成两个命令，也不能以“读操作”为理由忽略迟到响应。

## 最小持久修复候选

优先考虑**Huawei限定的、预算为两次额外尝试的整轮初始化重试**，保留实际错误和全部清理，而不是对每个GET/ACK重复发送。该方案仍需内核构建及故障注入验证，当前只是设计。

具体实现边界：

1. 在 UCSI core 的 init-work 决策层加入显式 opt-in quirk/状态，仅由 gaokun 驱动设置；其他平台行为不变。不通过把ETIMEDOUT改成EPROBE_DEFER来借用已有大预算重试，因为那会混淆原始原因。
2. 仅允许 `ucsi_init()` 返回ETIMEDOUT且 connector尚未发布的情况重试。每个UCSI实例额外尝试至多两次，延迟建议1秒、3秒；失败日志保留原始errno与尝试编号。成功后不保留后台重试。
3. 重用现有失败清理和下一次init开头的PPM reset。必须确认上一轮cleanup及reset已结束，下一轮reset成功后才查询capability。现有err_reset忽略reset返回值，需要补充日志；不能把reset失败解释成固件已经空闲。
4. 保持原来的EPROBE_DEFER重试语义独立，ETIMEDOUT预算不得被它重置。remove/suspend必须取消新排队的工作，不能在设备释放后继续重试。
5. 若日志显示connector已发布后的read_cci超时，不允许套此路径，否则可能重复注册/泄漏端口。已经有外接DP/partner工作的运行期也不适用。

这条路线会再执行PPM reset，因此仍可能影响充电/PD/外接显示。必须先在隔离构建里验证，不能将它宣传成无影响修复。若根因是EC永久不响应，它应在预算耗尽后保留失败状态，而不是不断重置。

## 还需的证据与验证

- 启动时记录 `ucsi_run_command` trace和CCI，区分GET_ALTERNATE_MODES、ACK_CC_CI及错误恢复阶段，保留connector/offset；无需记录用户数据。
- 故障注入：第一轮查询超时后成功、ACK超时、所有轮次持续超时、reset超时、成功初始化后read_cci失败、延迟旧completion。检查尝试上限和原始错误码。
- 每次失败后无残留Type-C port/altmode/partner/workqueue，成功后两个端口各注册一次；remove与延迟重试并发无UAF/死锁。
- 有/无USB主机、两种插头方向、纯充电、外接坞/DP分别测试；保持有效唤醒锁约束，不通过强制解锁制造休眠。
- 运行期正常USB必须仍正常；本次USB已自行恢复，不能将恢复归功于任何未部署候选。

没有充分证据支持立即部署命令级重试。以下整轮初始化候选已离线实现，尚未内核构建或部署。

## 离线候选实现与验证（同日继续）

受阻候选：`tools/hos/patches/ucsi-gaokun-bounded-init-timeout-retry.patch`。补丁头已显式标记 `BLOCKED - DO NOT BUILD OR DEPLOY`，当前SHA-256为
`7fbea87d5f5540dbc23d5b20a62b1ec39b59eecad1ff38f64ec6158c76c7080e`。
下述测试时的未标记版本hash为 `8b7122483e80d91b7e878efedcc4c0ab7c756c1129f2f9fdedbed4e4d1bb2574`；阻止标记仅改变说明头，不改变受阻代码。

新增 `UCSI_RETRY_INIT_TIMEOUT` quirk（v7.2.9 中空闲的 BIT(3)），只在 gaokun probe 中设置。`init_timeout_retries` 随 kzalloc 创建的UCSI实例归零；只有ETIMEDOUT、quirk开启、connector尚未发布三个条件同时满足才排队。延迟1秒/3秒，总共最多两次额外尝试，不重置该预算；原有work_count及EPROBE_DEFER分支不变。继续使用原ucsi->work，未增加线程或异步生命周期。

cleanup的reset错误新增日志并保留原始初始化errno。为了下一次区分查询/ACK超时，在quirk开启且connector尚未发布时，`ucsi_run_command` 分别记录transfer失败和ACK失败，包含完整command及errno。没有对运行期正常命令新增日志，也没有改变GET/ACK协议。

**源码链核对**：在临时目录下载公开 gregkh/linux v7.2.9 的ucsi.c、ucsi.h、ucsi_huawei_gaokun.c；核对 `out/rc2-1792421010/kernel-source.txt` 的78项清单，涉及这三个文件的恰好是0087–0093，七项本地补丁SHA-256全部匹配，按顺序以fuzz=0应用成功。候选再应用到这个结果上。其余71项不修改这三个文件，未伪称重新构建整棵内核。

0088 SHA-256 为 `b6235b427e38bbe1b8a03940fb7bcf1bb609fd4456b4b79bb51e82b4f7dc44c9`。该值也存在于本地78项rc2d后续发布清单。结合R14 `kernel/source-audit.txt` 对78项/73文件一致的审计记录及对象对比记录，支持R14继承0088；不是仅根据当前脚本列表推断运行内核已应用。

已完成的检查：

- `git apply --check`、实际应用后字节一致、反向应用检查通过。
- Linux v7.2.9 checkpatch：0 errors、0 warnings；运行使用 `--no-tree --no-signoff`，未提供其拼写/const辅助数据，因此不是完整上游提交审查。
- `tools/hos/patches/verify-ucsi-init-retry.py <patched-kernel-root>` 提取真正修改后的init-worker，使用主机C编译器 `-std=gnu11 -Wall -Wextra -Werror` 编译并执行12个状态转换：两次延迟及上限、穿插EPROBE_DEFER独立预算、成功、connector已发布、非opt-in、EIO/ENOMEM、原defer上限。全部通过。
- 核对现有remove顺序：0090先停register work、注销notifier、停usb fallback，再调用core unregister；core `cancel_delayed_work_sync(&ucsi->work)` 覆盖新增排队，不新增清理对象。此为源码审查，未进行并发/内核故障注入。

未完成且不能省略的验证：完整目标内核编译、reset/GET/ACK故障注入、旧completion与remove竞态、真实PD/DP/供电及两端口枚举。主机提取函数测试只证明决策路径，不证明Linux工作队列并发或硬件恢复。此候选不在默认kernel-apply-patches构建列表，不能直接当作已修复上机。

此外，原始 `ucsi_register_altmodes()` 对EBUSY有不设次数的循环；本候选只处理返回到init-work的ETIMEDOUT，不解决卡在该循环内的其他问题。两次额外尝试是调用次数上限，不是总耗时上限；单轮可能包含多个10秒协议等待及PPM reset。

## 深入生命周期审查：停止当前候选

### remove 中取消自排队工作：代码支持

v7.2.9 kernel/workqueue.c 的 `cancel_delayed_work_sync()` 调用 `__cancel_work_sync(..., WORK_CANCEL_DELAYED)`。后者先通过WORK_CANCEL_DISABLE暂时禁止排队并撤下pending实例，再flush正在执行的回调，最后enable_work。内核文档明确允许回调重新排队自身；只要没有外部并发新enqueue，返回时不会有pending或执行中的实例。

0090先disable辅助驱动register工作、注销notifier、disable USB fallback，再进入core unregister。新增重试复用core的同一个work，未引入独立重排队者。因此没有发现该自排队本身越过remove取消、在释放ucsi后继续执行的路径。取消仍可能等待当前协议超时/reset结束；这不是立即中止EC操作。

### 早期ETIMEDOUT：确切引用泄漏，阻止当前广义重试

完整0087–0093后的 `ucsi_register_port()` 执行顺序为：

1. 创建con->wq，初始化锁/工作/partner_tasks。
2. `cap->fwnode = ucsi_find_fwnode(con)`（源码注释明确调用者须put）。
3. `con->usb_role_sw = fwnode_usb_role_switch_get(cap->fwnode)`。
4. 发送GET_CONNECTOR_CAPABILITY；若返回ETIMEDOUT，直接 `goto out_unlock`。

`out_unlock` 跳过前面的 `out: fwnode_handle_put(cap->fwnode)`，仅解锁并destroy_workqueue、置wq=NULL后返回。此时con->port尚未建立。上层err_unregister使用 `for (con = connector; con->port; con++)`，不会调用该失败connector的unregister，而是最终直接kfree数组。0093放掉usb_role_sw的update_connector钩子还未执行，所以这个路径既漏fwnode引用，也可能漏取得的role-switch引用。

这不直接证明已释放数组被工作线程复用：失败connector的wq已销毁，connector数组没有发布到ucsi，ntfy尚未打开connector-change通知。但资源泄漏是真实路径，重复初始化会放大它。当前候选只检查ETIMEDOUT与connector未发布，无法排除此早期超时，因此不能证明整轮重试拥有完整回滚。

本次实机观察的altmode失败发生在端口创建之后：它走out并put fwnode，0093已释放role switch，wq销毁后上层按非空port调用unregister清理altmodes/电源信息/port。这个较晚路径与前述早期泄漏不能混为一谈；下一轮重试仍可能在更早步骤超时，不能仅依据首次错误阶段放行。

### suspend：不能把core函数存在当作已集成

gaokun辅助驱动没有pm_ops，完整补丁链没有调用 `ucsi_suspend()`/`ucsi_resume()`。因此虽然core提供取消init work的suspend函数，不能声称这台设备会调用它。初始化和重试在 `system_long_wq`，不是0090 USB fallback使用的system_freezable_wq。原本首次初始化也有这一窗口，新候选将它扩大。

在无法保证init/retry执行时设备不挂起、EC可正常接收完成通知之前，不应部署。应单独设计并验证正确PM生命周期，不能临时扩大wakelock或把workqueue换个名字就宣称安全。

### 下一步边界

先修复并验证早期port失败路径的引用清理，或让所有重试仅在被证明完整回滚的失败状态进行（且后续尝试同样受约束）；同时解决/验证PM集成。之后重新审查全路径并进行内核故障注入。当前patch保留为可审查的已知受阻候选，不纳入构建或部署。

## 独立诊断补丁：供完整编译候选

`tools/hos/patches/ucsi-gaokun-init-diagnostics.patch`，SHA-256
`ebe6e41cd5156bcb0c2043044a12062ce1b84c9a07d09cdfa1ad5849eaa39919`。

仅增加 `UCSI_INIT_DIAGNOSTICS` opt-in，在gaokun probe设置；无retry计数、无新工作、无重试、无PM/清理改变。仅在connector尚未发布时，分别记录 `Init command ... transfer failed` 与 `... ACK failed`；另外记录初始PPM reset errno以及cleanup reset errno/原始errno。成功和运行期不增加命令日志。已有错误返回保持原样，不能据此声称修复初始化。

它必须独立应用到原基线，不能和受阻retry补丁叠加（两个实验占用同一个空闲quirk bit）。应用/逆向检查及字节比对通过；checkpatch 0错误0警告。完整目标内核编译由主代理另行执行，本审查未操作VM或设备。

本地复原基线与主代理R15隔离复制的三个源文件哈希逐项一致：

| 文件 | SHA-256 |
|---|---|
| ucsi.c | 08c10900833b2a867e41eecc3cb17af1a19f9fe87b08142ac0f3bba03c443ea5 |
| ucsi.h | 07be96a4b835f659108a630607908b808153b1db6761f29fa73f33e683015e42 |
| ucsi_huawei_gaokun.c | 520b356a59f42837ffd4f2de503b6eaac197083c61a3c6ac6403b9f8539bdad7 |

此对应关系比仅检查补丁可应用更强，确认0088及其后UCSI补丁的复原结果与实际构建副本一致。


## 诊断内核完整编译（主代理）

独立复制R14构建树后，仅应用 `ucsi-gaokun-init-diagnostics.patch`，未应用被阻止的重试补丁。完整vmlinuz.efi编译成功，SHA256为 `e8d4d91fb09d328da40e3ef67b4fe98e23908dfd00867bedae390ebb66316f00`，下载到 `/tmp/hos-r15-diagnostics/vmlinuz.efi` 后再次匹配。远端独立树 `/home/vahiru/hos-r15-kernel-ucsi` 保留产物及构建日志。

3799个对象逐个比较，仅ucsi.o、ucsi_huawei_gaokun.o及下游链接/压缩对象变化；重新编译的vDSO相关对象实际逐字相同。配置、内建cpio、version.o与version-timestamp.o均与R14相同。原R14 EFI仍为 `0b595a30926f04d4877ba15e7770b68c50750ca781db0ecea5e952eb641de771`。设备尚未部署/重启，不能将编译通过解释为初始化故障恢复。
