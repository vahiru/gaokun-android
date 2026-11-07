# 安装器Rust后端设计（`tools/gk3-installer/`）

> **当前状态**：并行演进轨道。已完成骨架与首批只读入口（`gk3_probe`、`gk3_preflight`、`gk3_plan`），在容器测试环境中与`scripts/live/installer-lib.sh`逐字节对拍一致；尚未替换Live镜像中的默认shell调用。
>
> **相关代码**：[`tools/gk3-installer/`](../../tools/gk3-installer/Cargo.toml)（零第三方依赖单crate）、对拍驱动[`scripts/live/duel-lib.sh`](../../scripts/live/duel-lib.sh)与[`scripts/live/test-duel.sh`](../../scripts/live/test-duel.sh)。

## 1. 背景与设计目标

`scripts/live/installer-lib.sh`（约2700行、75个函数）在演化过程中暴露出的缺陷几乎都源于shell的静默失败特性：管道只取末段退出码、`2>/dev/null`吞掉外部工具报错、`$(...)`子shell吞掉返回值或将标准输出混入控制流、`shift`在参数不足时不移位导致死循环。

Rust后端的目标是在不动Flutter前端（[`live/installer-flutter/`](../../live/installer-flutter/README.md)）的前提下，用类型系统消除静默失败，并支持按函数粒度逐步替换：
1. **编译期零隐式崩溃**：`Cargo.toml`开启`#[forbid(unsafe_code)]`，并将`clippy::unwrap_used`、`expect_used`、`panic`、`todo`、`unimplemented`、`unreachable`、`indexing_slicing`、`print_stdout`、`print_stderr`、`let_underscore_must_use`、`exit`、`unused_must_use`全部设为`deny`；发布构建开启`overflow-checks = true`。忽略结果必须显式调用`discard(r)`并注释原因。
2. **协议逐字节兼容**：stdout记录、stderr上的`PROGRESS` / `ERR` / `JOB`行及退出码与shell版保持一致，前端`shell_backend.dart`与`protocol.dart`无需改动。
3. **单静态二进制**：编译为`aarch64-unknown-linux-musl`完全静态链接ELF（约540 KiB，无外部C交叉工具链依赖，使用`rust-lld`），部署方式与`gk3-misc`相同。

## 2. 外部命令执行与诊断模型

`src/exec.rs`中的`Cmd::run()`返回`Result<Output, ExecError>`：
* **输入隔离**：子进程`stdin`固定重定向至`/dev/null`，杜绝外部工具（如`ntfsresize`）停下等待交互确认。
* **超时与信号**：只读查询默认60秒超时（`QUERY_TIMEOUT`），超时后发送`SIGKILL`并返回`Status::TimedOut`；子进程不脱离当前进程组，确保前端取消任务向进程组发`SIGTERM`时不会留下孤儿写盘进程。
* **显式可接受退出码**：每个调用点通过`host::capture(host, cmd, accept, diag)`显式声明正常退出码集合（如`blkid`为`[0, 2]`、`findmnt`为`[0, 1]`）。不在`accept`内的非零退出码一律记录到`Diag`，在入口结束时以`gk3-installer: 注意：<命令行>：<状态>`输出至stderr（进入前端详情日志）。
* **`TouchGuard`写盘标记**：`Failure`携带`touched: Option<Touched>`。写盘入口在首次修改磁盘前调用`TouchGuard::mark()`，任何后续报错或panic hook均能准确向前端报告`touched=yes|no`。

## 3. 分区表判据与边界修正

在对照shell版实现并跑容器实录（`gdisk 1.0.10`、`util-linux 2.41.5`、`dm-error`）的过程中，shell版与Rust版同步确立了统一的分区表分类判据（shell版`gk3__read_table` / `gk3__table_guard`，Rust版`gpt::classify` / `gpt::read_table`）：

1. **`unreadable`（不可读）**：`sgdisk -p`退出码非零，或**stderr**出现读错误（`read error` / `error N reading`，注意整盘EIO时`sgdisk -p`退出码仍为0但会在stderr报警并在内存中新建空表），或stdout缺少`First usable sector is N, last usable sector is M`。
   * `gk3_probe`输出`DISK table=unreadable`、不列分区与空闲区、报`ERR code=disk-unreadable touched=no`并以退出码1返回；写盘入口在动盘前直接拒绝。
2. **`mbr`（MBR分区表）**：`blkid -p -o value -s PTTYPE`返回`dos`，或`sgdisk -p`在stdout输出`Found invalid GPT and valid MBR; converting MBR to GPT format`。
   * `gk3_probe`标记`DISK table=mbr`；非整盘模式（双系统、保留/清除数据重装、缩分区、手动分区操作）在动盘前一律拒绝并报`mbr-disk`；仅整盘清空模式放行。
3. **`gpt`**：其余情形（含无分区表的空盘）。

此外，针对shell版在极端输入下会死循环或输出断行的4类情形，Rust版做了防御性收敛：

| 异常输入 | shell版行为 | Rust版行为 |
| --- | --- | --- |
| 选项缺少参数值（如末尾单独`--mode`） | `shift 2`失败不移位，陷入死循环 | `PLANERR msg=missing-value:<参数>`，退出码1 |
| 非规范十进制数字参数（如`010`、`1+1`、`abc`） | 走bash算术求值，与`[ -ge ]`十进制比较语义分裂 | `PLANERR msg=bad-number:<参数>` |
| `sgdisk -i`输出无法解析的起止扇区 | 走bash算术产生错误扇区号 | `PLANERR msg=reinstall-part-unreadable name=…` |
| 未编码字段中混入空白字符 | 输出记录断行，前端解析错位 | 自动转为百分号编码并记入`Out.coerced` |

## 4. 迁移阶段与渐进切换

| 阶段 | 范围（对应`installer-lib.sh`函数） | 验证方式 | 状态 |
| --- | --- | --- | --- |
| 0 | 协议编解码、命令执行器、`Host`抽象、Clippy lint、musl静态编译、对拍框架 | 单元测试 | 已完成 |
| 1a | 核心只读计算：`gk3_probe`、`gk3_preflight`、`gk3_plan`（含重装方案） | 协议面逐字节对拍 | 已完成 |
| 1b | 其余只读入口：`gk3_esp_info`、`gk3_release_info`、`gk3_shrink_info` / `gk3_shrink_scan`、`gk3_log_targets`、`gk3_job_status`、`gk3_net_status` / `gk3_net_manifest` | 协议面逐字节对拍 | 待迁移 |
| 2 | 写盘动作：`gk3_part_*`、`gk3_shrink`、`gk3_apply`、`gk3_save_logs`及内置sparse流式写盘 | 稀疏镜像终态逐项对拍 + `test-apply.sh`全套断言 | 待迁移 |
| 3 | 网络、Wi-Fi与后台任务：`gk3_net_fetch` / `gk3_net_release`、`gk3_wifi_*`、`gk3_job_*` | 桩环境 + 对拍 | 待迁移 |
| 4 | 打包进入Live镜像，在`installer-lib.sh`中以薄壳转发並完成实机验收 | 镜像内自检 + 实机安装 | 待迁移 |

切换时无需改动Flutter前端，只需在`installer-lib.sh`中将已迁移函数替换为一行转发（支持通过环境变量`GK3_IMPL=shell`随时回退）：

```sh
gk3_probe() { "${GK3_RS:-$GK3_LIBDIR/gk3-installer}" gk3_probe "$@"; }
```

## 5. 构建与测试命令

```sh
# 单元测试与静态检查（宿主机直接运行）
bash tools/gk3-installer/build.sh test
bash tools/gk3-installer/build.sh lint

# 编译aarch64-unknown-linux-musl静态二进制
bash tools/gk3-installer/build.sh musl

# 在arm64 Debian容器内运行专项边角对拍与安装全流程对拍
GK3_TEST_DUEL=/repo/tools/gk3-installer/target/aarch64-unknown-linux-musl/release/gk3-installer \
    bash scripts/live/test-in-container.sh scripts/live/test-duel.sh
GK3_TEST_DUEL=/repo/tools/gk3-installer/target/aarch64-unknown-linux-musl/release/gk3-installer \
    bash scripts/live/test-in-container.sh scripts/live/test-apply.sh
```
