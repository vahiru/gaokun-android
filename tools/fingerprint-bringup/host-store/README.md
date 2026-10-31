# 主机侧指纹模板存储（参考实现）

对应设计决策 D19/D20（`docs/fingerprint-driver-design.md`）。目标是把 TA 导出的
不透明模板 blob 变成一个可校验、可原子写入、带几何/配置绑定的存储文件。

这是主机侧库的参考实现（C11），用于离线测试和后续 client driver 复用；
不包含 Android 存储策略本身（见“限制”）。

## 原始 blob 校验（D19）

`ft_tpl_validate()` 在调用 `FtIniAlgTplData_v1` 之前执行：

1. `len >= 216 + 96*slot_bytes + 2`，且 `(len - fixed) % 44 == 0`（特征数）；
2. `u32@0 == 0x1415927D`；
3. `u32@4 == 208`；
4. `CRC16(blob[0..len-2)) == u16le@(len-2)`。

CRC16 参数：反射多项式 `0x8005`（实现常量 `0xA001`）、init 0、无输出反射/异或。

`slot_bytes` 由传感器配置决定（当前配置：40×196 / 196×40 为
`489 + 984 + 981 = 2454`，96×96 为 `2802`），必须由调用者传入。

注意：

- **短缓冲危险**：TA 用头部解析出的 offset 定位 CRC，不看 `tplSize`。调用前必须
  校验长度，不能把短缓冲交给 TA。
- **非零 load rc 是致命的**：CRC 失败返回 -3 时 TA 的库已经填充，identify 仍会
  命中。本库的校验通过后这种路径应不可达；若仍出现，关闭本次认证。
- **几何不可从 blob 推断**：40×196 与 196×40 的 blob 头部逐字节相同（D20），
  所以几何必须外带。

## 容器格式

单文件，头部 64 B + 原始 blob：

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 4 | magic `"FPT1"` |
| 4 | 2 | version = 1 |
| 6 | 2 | header_size = 64 |
| 8 | 1 | fingerId (0..5) |
| 9 | 3 | reserved = 0 |
| 12 | 2 | width |
| 14 | 2 | height |
| 16 | 4 | slot_bytes |
| 20 | 4 | blob_len |
| 24 | 32 | config digest（调用者提供，如配置字节的 SHA-256） |
| 56 | 2 | blob 尾部 CRC16（快速一致性检查） |
| 58 | 2 | 头部 CRC16，覆盖 `[0,58)` |
| 60 | 4 | reserved = 0 |
| 64 | blob_len | 原始模板 blob |

写路径为 `<path>.tmp` → `fsync` → `rename`，权限 `0600`，且 `O_NOFOLLOW`。
文件建议命名 `finger<N>.tpl`。

## API

```c
unsigned ft_tpl_crc16(const uint8_t *data, size_t len);
enum ft_tpl_status ft_tpl_validate(const uint8_t *blob, size_t len,
                                   uint16_t width, uint16_t height,
                                   uint32_t slot_bytes);
enum ft_tpl_status ft_tpl_store_write(const char *path,
                                      const struct ft_tpl_meta *meta,
                                      const uint8_t *blob, size_t len);
enum ft_tpl_status ft_tpl_store_read(const char *path,
                                     const struct ft_tpl_meta *expect,
                                     uint8_t **blob_out, size_t *len_out);
```

`ft_tpl_store_read()` 会把头部元数据与 `expect`（finger/几何/slot/config digest）
逐项比较，不匹配即失败；返回的 blob 用 `ft_tpl_free()` 释放。

## 测试

```sh
clang -std=c11 -Wall -Wextra -Werror -O2 tpl_store.c test_tpl_store.c \
    -o /tmp/tpl_store_test && /tmp/tpl_store_test
```

覆盖：CRC 向量、magic/headerSize/长度/CRC/截断、两种几何、写入-读回、权限、
临时文件清理、几何/手指/config digest 不匹配、blob 篡改、头部篡改、文件截断。

另外已用离线 harness 导出的三种真实 blob 验证 `ft_tpl_validate()`：
40×196（238,970 B）、196×40（239,938 B）、96×96（273,962 B）全部返回 0。

## 限制

- CRC 只防意外损坏，不防可重算 CRC 的恶意改写；容器没有签名或 MAC。
- 生产环境还需要：目录 `fsync`（掉电一致性）、跨进程锁、用 TEE/Keystore
  保存的密钥做加密与 HMAC、回滚计数（防旧模板重放）、以及文件属主/权限检查。
- 本库不做磨损均衡或分区管理；卷格式、配额、删除语义由上层负责。
- 当前是 host 参考实现，未在 Android kernel/HAL 环境编译验证。
