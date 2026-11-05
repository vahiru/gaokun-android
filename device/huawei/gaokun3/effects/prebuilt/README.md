# 华为Histen音频引擎预置目录（`effects/prebuilt/`）

本目录通过`.gitignore`忽略所有二进制文件，仅将本说明文件纳入版本控制。供扬声器增强功能（[`../README.md`](../README.md)）调用的专有引擎放置路径为：

```text
lib64/soundfx/libhw_histen_processing.so
```

[`device.mk`](../../device.mk)通过`$(wildcard ...)`检查该文件：存在时安装至`/vendor/lib64/soundfx/libhw_histen_processing.so`；缺失时不影响ROM正常编译，音效库在运行时会自动回退为仅执行自研扬声器高通与限幅链路（日志输出`Histen unavailable -- speaker chain only`）。

---

## 一、来源与二进制修改说明

- **来源**：提取自麒麟V10 SP1（ARM64桌面Linux）的华为音频栈（iMedia Audio 8.0 / Histen 6.1.9），属华为专有二进制，未获再分发授权，故不纳入源码仓库。
- **Bionic兼容性补丁**：原始文件针对glibc编译（`GCC: (Ubuntu/Linaro 7.4.0-1ubuntu1~18.04.1)`）。为使Android Bionic动态链接器能够加载，该二进制文件经过`patchelf`修改（最初由PR #7引入）：
  - 将`DT_NEEDED`中的`libc.so.6`与`libm.so.6`改为`libc.so`与`libm.so`；
  - 移除`DT_VERNEED`并将`.gnu.version`中的符号版本统一置为全局（版本号1），避免Bionic因缺失`GLIBC_2.17`版本定义而拒绝加载；
  - 仅导入`memcpy`、`memset`、`sincos`、`sqrtf`、`abort`与`__stack_chk_*`基础符号，无glibc/Bionic ABI冲突。

提取命令（从本仓历史PR引用恢复）：

```sh
git fetch upstream pull/7/head:pr7
D=device/huawei/gaokun3/effects/prebuilt/lib64/soundfx
mkdir -p "$D" && git show 4d90d48:"$D/libhw_histen_processing.so" > "$D/libhw_histen_processing.so"
```

---

## 二、文件校验信息

```text
SHA-256: 338b774e70feeccd7718f9d2f25c5bfddf8254ddd2623fa25a778ffcaaf38a2e
大小:    336713 字节
BuildID: 3117f9af8c89da027458cf61ea5e66a09abe6a36
```

`scripts/sync-device-tree.sh`在同步设备树至构建机时会校验上述SHA-256摘要。
