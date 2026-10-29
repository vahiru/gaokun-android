# R15 cadlauncher SELinux 启动失败审计

状态：离线审计；待真机 AVC / loaded policy 验证；未修改或部署策略。

## 已观察事实

`/Users/vahiru/.cache/hos-r15-nav/overlay-crash-after.txt` 记录 2026-10-30 00:06:23–24 左右 UID 10253 的 zygote 子进程 SIGABRT：

```
selinux_android_setcontext(10253, 0,
  "mslg_app:targetSdkVersion=33:complete", "com.xiaomi.cadlauncher") failed
```

这是应用子进程在 Java 应用代码运行前失败的证据，不能据此认定 zygote 主进程或 system_server 重启。

`cadlauncher-context.txt` 的真机读取结果确认 `/system_ext/etc/selinux/system_ext_seapp_contexts:32` 已有：

```
user=_app seinfo=mslg_app name=com.xiaomi.cadlauncher domain=mslg_app type=app_data_file levelFrom=all
```

另有相同包名的 `seinfo=platform` 规则。缺少 package 精确映射不是当前证据支持的解释，不能通过再添加重复映射修复。

`out/hyperos-r13/codec-offline/system-etc/selinux/plat_mac_permissions.xml` 的 package 条目将 cadlauncher、cajlauncher、wpslauncher、mslgrdp 标记为 `mslg_app`，与崩溃中的 seinfo 相符。

## 离线策略差异

审计 `out/hyperos-r4/debug-policy/root` 全部 CIL：

- `system_ext_sepolicy.cil:774` 定义 `mslg_app` 类型；它在 `domain`、`coredomain` 中。
- `mslg_app` 未加入 `appdomain`；未发现直接以该域为源或目标的 allow。mapping 文件仅保留版本映射。
- 平台 `plat_sepolicy.cil:82671` 通过 `allow zygote appdomain (process (dyntransition))` 允许应用切换域。平台 `roletype r domain` 已覆盖该域，因此不能把单独缺少 `roletype r mslg_app` 误判成根因。
- `prepare-debug-policy.py` 只处理匿名属性命名空间及一条 llkd ptrace 规则，不会特意删除 mslg_app 的应用规则。

完整 donor 的 `out/hyperos-r9/donor-vendor-tree/etc/selinux/vendor_sepolicy.cil` 则包含 `mslg_app` 的 `appdomain`、`netdomain`、`bluetoothdomain` 等属性，以及应用基础权限、userfaultfd/devpts 类型和 transition、mslgd 通信与 mslg 文件类型访问。donor odm CIL 也包含该域的 appdomain 成员关系。

因此，最强离线解释是：替换 donor vendor 时，系统分区保留 mslg_app 类型和 seapp 映射，但丢失原来位于 donor vendor 的应用域实现。它不是单纯缺一个包名规则，也不是 donor 原系统必然有相同缺陷。

## 证据边界

`cadlauncher-context.txt` 过滤后的 logcat 未提供实际 AVC / libselinux lookup 错误。随后 `/Users/vahiru/.cache/hos-r15-nav/logcat-after-charge.txt` 的末 5000 行没有 cadlauncher/mslg_app 相关失败，无法从该片段确认失败 errno 或证明已恢复。

尚未核对本次启动的实际 loaded policy 是否确实缺 appdomain 属性和 dyntransition 权限，也未确认是否存在额外策略合入。因此“缺失 vendor 应用域实现”是有具体源码依据的首要根因候选，不能声称运行时 AVC 已证实。

## 最窄下一步

1. 在下一次自然复现时保留未过滤的相关 logcat、内核 AVC 和应用包信息；只读核对当前 loaded policy 的 mslg_app 属性与 zygote dyntransition。
2. 确认 cadlauncher 对 mslgd / 应用引擎的实际依赖及当前镜像是否提供这些组件。如果后端不存在，启动 APK 本身也不等于功能可用。
3. 如果需要保留此功能，候选修复应保持独立 mslg_app 域，以当前平台的应用域基础规则为依据补齐必要实现，逐条评估 donor 特定依赖；先编译并执行 neverallow 检查，再考虑可回滚验证。

不推荐直接映射为 platform_app / priv_app，不推荐导入整套 donor vendor 策略，不推荐单独加 dyntransition 掩盖后续缺失的权限，更不推荐 permissive 或宽泛 allow。此文不包含部署授权或可直接刷入的策略变更。
