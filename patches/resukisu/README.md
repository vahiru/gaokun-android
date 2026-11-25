# patches/resukisu/

本目录存放对**ReSukiSU**（在[`scripts/kernel-setup-resukisu.sh`](../../scripts/kernel-setup-resukisu.sh)中固定上游commit）的本地适配补丁。脚本以`git apply`将这些补丁打入`<内核树>/KernelSU/`，因此补丁内的文件路径均相对于ReSukiSU仓库根目录（`kernel/...`）。

## 选型说明：ReSukiSU与SukiSU-Ultra

| 对比维度 | SukiSU-Ultra | ReSukiSU |
|---|---|---|
| 主线Linux 7.2编译表现 | `selinux/sepolicy.c`报4处编译错误 | 仅需下方1个兼容补丁 |
| 差异原因 | 直接引用Android通用内核（ACK）私有的`policydb.android_netlink_route`等字段 | 同一处代码使用`KSU_COMPAT_HAS_*`宏做条件编译保护 |
| 跨内核版本探测机制 | 手写`LINUX_VERSION_CODE`上限（仅覆盖至6.18） | `kernel/tools/kernel_compat.mk`通过扫描内核源码头文件探测各API是否存在 |

## 补丁列表

### `0002`：补充主线已移除的`strncpy()`兼容实现

Linux 7.2的`include/linux/string.h`与`lib/string.c`已移除`strncpy`定义，而ReSukiSU中有9处调用（由于GCC自带`strncpy`内建原型，编译期不报错，仅在最终链接阶段报`undefined reference to 'strncpy'`）。

补丁在兼容头中补充了与原语义一致的`strncpy`内联实现（`dispatch.c`拷贝用户态`uname`缓冲时依赖尾部补零，`throne_tracker.c`则依赖源串过长时不追加终止符，因此不逐点替换为`strscpy`）。`kernel/tools/kernel_compat.mk`中探测声明时避免在`$(shell ...)`内使用未配对的转义括号，以免触发GNU Make条件语法解析错误。

### 已去掉的补丁

2026-11-15起钉住的版本改为`v4.2.0-rc3`标签（此前是2026-08-22的`b2ac2fc`）。原来的`0001`（arm64的`asm/patching.h`改名为`asm/text-patching.h`）和`0003`（无条件包含`<linux/sched/task_stack.h>`）在上游已经修好，所以去掉了。升级的原因：`b2ac2fc`的内核UAPI版本是2，而当时最新发布的管理器v4.2.0-rc3是4。管理器发现内核的UAPI比自己低，就只提示"需要更新内核"，无法给Shell授权root。
