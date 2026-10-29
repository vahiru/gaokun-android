# 2026-10-30 数据分区搬迁记录复核

用户告知已自行操作分区表，要求自行查看。只读检查本地 `out/repart-20261030/` 的 check、stage1/2/3、gpt、grow 日志及脚本；未执行脚本或写设备。当前ADB/fastboot均无设备，已知救援SSH地址未连通，因此以下是日志复核，不是现场重新读取GPT或启动验证。

| 分区 | 名称 | 新起止扇区（512字节） | 旧→新容量 |
| --- | --- | --- | --- |
| p2 | userdata（crDroid） | 839845888–948561919 | 376 GiB → 51.84 GiB |
| p12 | userdata_hos（HyperOS） | 616448–789178367 | 32 GiB → 376.02 GiB |

日志前后12条分区表逐条解析对比：仅p2/p12起止位置与容量改变，其他10个分区不变；新布局无重叠，sgdisk验证通过。两分区编号、PARTLABEL、PARTUUID保持不变：

- p2 PARTUUID `4ba36282-f11a-4b1e-b3a4-37940c2319b4`
- p12 PARTUUID `ff17b5af-a4fd-45df-874f-a814d1dba691`

stage1把crDroid文件系统缩到12GiB并复制到临时区域；stage2搬迁32GiB HyperOS；stage3搬回12GiB crDroid。三阶段均记录cmp一致且exit=0。GPT日志先提示内核仍使用旧表，随后partprobe/partx刷新后的sysfs起点与大小已匹配新表，不能把前面的提示误读成刷新最终失败。

grow日志记录两个文件系统扩容后e2fsck完成，exit=0；只读挂载检查HyperOS约370G文件系统、可用361G，crDroid约51G文件系统、可用48G。该检查不能替代两个Android系统实际启动/解密/数据访问验收。

后续操作必须以新布局为准并重新读取现场GPT。`tools/hos/install-baseline.py`是旧32GiB首次安装器，涉及格式化，不能拿它当现有系统升级工具重跑；无需为了此次搬迁去放宽其保护。当前ramdisk按分区名称挂载/data，名称及GUID未改，未发现必须修改fstab的证据。

R16内核已安装事实见hyperos-r16-deployment.md，但此次分区操作发生在其后。恢复连接后应核对实际GPT、sysfs映射、/data来源及容量，再继续设置界面修复。
