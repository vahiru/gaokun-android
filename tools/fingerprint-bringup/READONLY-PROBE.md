# 固定只读指纹通信探针

`qcom_qseecom_fpread.c` 是已编译并完成首轮上机的独立探针，不替换现有
`fpcmd`/`fptest`。目的只到 M2：证明可信应用执行了 `0x8031` 并返回状态。
它不探测 SPI、不采集指纹、不录入、不写模板，也不是 Android 指纹 HAL。

2026-10-30 检查到当前真机是 `6.6.30-android15-8-g0a4d3c12b3 #20`，
没有 `qcom_scm_qseecom_app_load/app_shutdown` 导出；仓库旧模块的 vermagic
则是 `7.2.9-gaokun3+`。后续已确认 uname 受版本伪装影响，不能仅据版本显示判断不兼容；应读取启动 dmesg 和导出符号。首轮在实际 7.2.9 实验内核加载成功，LOOKUP 返回 -ENOENT，尚未执行 SEND；详见 docs/dev/fingerprint-machine-check-2026-10-30.md。

## 使用前提

先构建并核验带 patch 0050 的匹配实验内核及模块。保留正式内核，使用独立启动项，
确认模块实际未引用裸 SMC。编译时可通过命令行覆盖现有 Makefile 的 `obj-m`，
只构建新模块；不要为方便而强制跳过版本或符号检查：

```sh
make -C <匹配内核树> M=<候选源码副本目录> \
  ARCH=arm64 CROSS_COMPILE=<匹配工具链前缀> \
  obj-m=qcom_qseecom_fpread.o modules
```

源码副本需包含 `fp_app_compat.h`。实际编译配置、模块 vermagic、符号版本和
`fingerpr.mbn` 哈希须在上机记录中保存。模块不负责验证固件文件哈希，操作者必须先
与已核验的厂商签名文件比对。

## 固定协议与结果

insmod 只建立 debugfs。写入精确的 `probe` 或 `probe\n` 才执行，单次模块实例
只允许一次尝试。请求为 `31 80 00 00 00 00 00 00 04 00 00 00`，传给 SCM 的
请求/响应长度严格为 12/16；分配的页大小不充当协议长度。

路径是 LOAD → LOOKUP → SEND → SHUTDOWN。LOOKUP 成功必须与 LOAD 的 app_id
一致；仅 `-ENOENT` 作为已知名字缺失允许继续，SEND 始终使用 LOAD 返回的 id。
依据是 `docs/stage4-findings.md` #125 的既有真机记录：同一签名 TA 的
LOAD 返回5、LOOKUP `fingerprint` 返回 `-ENOENT`、SHUTDOWN(5) 成功。
真正注册名仍未知，不能宣称 LOOKUP 已修好；其他错误和 id 不一致一律停止。
根据 `rev/out/sub_ADFC.c`，`0x8031` 写响应偏移 12 的四字节状态，完成时写
请求 word0=`0x5aa5`、word2=0。探针检查 ACK，记录状态原值；仅响应全零不算通过。
状态非零也不能被解释为传感器已初始化。M2 通过还需要人工核对日志、退出码、
实际 ACK 和卸载成功，不能仅搜索 `transport returned`。

所有内存提前分配；固件所在子区按物理地址 4MiB 对齐，完整区间及 I/O 缓冲均检查
低于 4GiB。固件池额外保留 4MiB 对齐余量；某些内核不支持这次 8MiB 连续分配，
此时在 LOAD 前失败。不能删去对齐或 32 位检查来让测试继续。

## 失败行为

LOAD 前失败可释放全部资源。开始 LOAD 前持有模块引用；此后任何 secure call
失败（上文明确列出的 LOOKUP `-ENOENT` 除外）或 LOOKUP id 不一致都会保留
缓冲和模块引用、禁止重复触发，要求重启恢复。
只有明确 SHUTDOWN 成功后才释放缓冲和引用。不要强制卸载模块。

patch 0050 的 listener 轮数上限只能限制每次 SMC **已经返回**后的循环，不能
中断不返回的 SMC；因此不能承诺“不会硬挂”。首条命令还可能触发 TA 初始化时
读取模板信息，无 listener 的失败应答是否能完成仍待实机验证。现场须可恢复电源。
本候选没有注册安全存储 listener，也没有放宽 SELinux。

## 实测更新

第二轮已完成真实SEND：0x8031返回有效ACK 0x5aa5与状态0，SHUTDOWN成功、模块正常卸载。仅证明M2通信；SPI/录入/认证未验证。详细证据见 `docs/dev/fingerprint-machine-check-2026-10-30.md`。
