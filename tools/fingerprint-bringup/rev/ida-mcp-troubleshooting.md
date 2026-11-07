# traex 中 IDA MCP 打开失败

2026-10-30 已在 traex 的 `workspace-write`、`network_access=true` 沙箱中复现并修复。

## 原因

REA 6.8.0 最初将 `idb_open` 返回的错误按成功结果解析，导致只看到
`success: Invalid input: expected true; session: ... undefined`。
本机通过 `patch_rea_ida_open_error.py` 修补了错误传播，CLI 和 MCP bundle 均有备份。
升级 REA 会覆盖该补丁；可用同一脚本的 `--restore` 恢复备份。

底层错误为 `Remote end closed connection without response`。捕获 worker 日志后发现：

```text
FATAL ERROR: /Users/vahiru/.idapro/ida.reg: Operation timed out
```

IDA 即使只分析输入文件，也会写入用户注册表 `ida.reg`。traex 工作区沙箱不允许
写主目录中的这个文件，worker 等待超时后以状态码 1 退出，导致本地 HTTP 连接断开。

## 工作区修复

`ida-mcp.json` 现在使用原来的 Python 环境运行 `ida_mcp_logged.py`。
该启动器在系统临时目录中，为每个 worker 创建权限为 0700 的独立 `IDAUSR`，
复制现有 `ida.reg`、`ida-config.json`、`proccache.lst` 和 `*.hexlic`（若存在）。
副本权限为 0600；worker 退出并由 supervisor 回收时删除该目录。
原始用户配置不被修改，无需扩大 traex 沙箱写权限。

worker stdout/stderr 保留在系统临时目录的 `rea-ida-worker-logs-501/worker-*.log`
（后缀数字取当前 UID）。断连错误中包含退出状态、日志路径和日志末尾。
强制结束 supervisor 时，worker 及其临时配置可能保留到另行清理。

## 验证

修复前，在以下相同沙箱中复现了 `ida.reg` 写入超时；修复后成功取得 `sub_ADFC`
的全部伪代码，并确认临时用户配置已删除：

```sh
traex sandbox macos \
  -c 'sandbox_mode="workspace-write"' \
  -c 'sandbox_workspace_write.network_access=true' -- \
  env REA_IDA_MCP_CONFIG="$PWD/tools/fingerprint-bringup/rev/ida-mcp.json" \
  REA_ANALYSIS_PROVIDER=ida \
  /opt/homebrew/bin/rea decompile \
  "$PWD/tools/fingerprint-bringup/rev/cabs/qctree/fingerpr.mbn" \
  0xadfc --format json
```
