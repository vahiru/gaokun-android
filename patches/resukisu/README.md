# patches/resukisu/

本目录存放对**ReSukiSU**（在[`scripts/kernel-setup-resukisu.sh`](../../scripts/kernel-setup-resukisu.sh)中固定上游commit）的本地适配补丁。脚本以`git apply`将这些补丁打入`<内核树>/KernelSU/`，因此补丁内的文件路径均相对于ReSukiSU仓库根目录（`kernel/...`）。

## 选型说明：ReSukiSU与SukiSU-Ultra

| 对比维度 | SukiSU-Ultra | ReSukiSU |
|---|---|---|
| 主线Linux 7.2编译表现 | `selinux/sepolicy.c`报4处编译错误 | 仅需下方3个小型头文件/兼容补丁 |
| 差异原因 | 直接引用Android通用内核（ACK）私有的`policydb.android_netlink_route`等字段 | 同一处代码使用`KSU_COMPAT_HAS_*`宏做条件编译保护 |
| 跨内核版本探测机制 | 手写`LINUX_VERSION_CODE`上限（仅覆盖至6.18） | `tools/kernel_compat.mk`通过扫描内核源码头文件探测各API是否存在 |

## 补丁列表

### `0001`：`asm/patching.h`更名为`asm/text-patching.h`

`hook/patch_memory.h`按`LINUX_VERSION_CODE >= 5.14`无上界包含`asm/patching.h`。而在Linux 6.13中，arm64的该头文件已合并至跨架构的`asm/text-patching.h`（含`aarch64_insn_patch_text*`声明），7.2主线不再存在`asm/patching.h`。补丁改用`__has_include`按头文件存在性探测。

### `0002`：补充主线已移除的`strncpy()`兼容实现

Linux 7.2的`include/linux/string.h`与`lib/string.c`已移除`strncpy`定义，而ReSukiSU中有9处调用（由于GCC自带`strncpy`内建原型，编译期不报错，仅在最终链接阶段报`undefined reference to 'strncpy'`）。

补丁在兼容头中补充了与原语义一致的`strncpy`内联实现（`dispatch.c`拷贝用户态`uname`缓冲时依赖尾部补零，`throne_tracker.c`则依赖源串过长时不追加终止符，因此不逐点替换为`strscpy`）。`tools/kernel_compat.mk`中探测声明时避免在`$(shell ...)`内使用未配对的转义括号，以免触发GNU Make条件语法解析错误。

### `0003`：无条件包含`<linux/sched/task_stack.h>`

`kernel/hook/syscall_hook_manager.c`原本将`#include <linux/sched/task_stack.h>`置于`#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)`条件块内，在Linux 7.2.9上被跳过，而文件内调用的`task_pt_regs(current)`依赖该头文件定义的`task_stack_page()`。补丁将该`#include`移出版本条件块。
