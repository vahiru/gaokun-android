# 仅更新 super_hos 的救援工具

`tools/hos/update-isolated-super.py` 独立于旧安装计划。默认preview，只有显式`--apply`才写；工具不会分区、格式化、修改ESP/EFI或重启。

调用：

```
python3 tools/hos/update-isolated-super.py --manifest reviewed.json --manifest-sha256 REVIEWED_SHA256 --image super_hos.raw.img
```

preview也执行完整只读预检，需要root救援环境，根挂载必须是`/dev/nvme0n1p3`。不提供自定义磁盘、目标或跳过校验选项。apply增加`--apply`，只覆写固定p7的12GiB；镜像必须是同尺寸raw文件。

manifest精确字段示例（省略号必须换成真实值）：

```json
{
  "schema": 1,
  "disk_sectors": 1000215216,
  "disk_guid": "e6c13d1a-e678-468a-b6b6-b79f19efb0e5",
  "gpt_primary_sha256": "...",
  "gpt_secondary_sha256": "...",
  "entries": {"1": "128字节条目hex", "7": "128字节条目hex"},
  "target": {
    "number": 7, "name": "super_hos", "uuid": "实际分区UUID",
    "start": 814606336, "end": 839772159, "size": 12884901888,
    "old_sha256": "..."
  },
  "image_sha256": "...",
  "protected_partitions": {
    "boot_a": {"number": 5, "sha256": "..."},
    "boot_b": {"number": 6, "sha256": "..."}
  },
  "esp_assets": {"loader/loader.conf": "...", "其他全部ESP相对路径": "..."}
}
```

entries必须包含全部非空GPT条目，不是仅示例中的1和7。直接复用本地install-baseline.read_gpt验证两份header/table CRC与一致性，并逐字节对比manifest；primary哈希为磁盘前34扇区，secondary为末33扇区。固定boot_a=p5(814344192..814475263)、boot_b=p6(814475264..814606335)，保护整分区哈希。

ESP必须挂载于`/boot/efi`且来自p1。esp_assets是全部regular文件的精确路径/哈希集合，涵盖loader全集；前后新增、删除、内容变化均失败。应在进入救援后采样最终manifest，避免正常启动时random-seed/bootlog变化误配；稳定启动资产仍需与既有审阅记录比对。

检查物理disk字节数与512逻辑扇区、sysfs p7 start/size/parent、PARTUUID指向、所有可见进程mount namespace和holders。apply用O_EXCL打开block设备，验证旧super哈希后再次预检。开始前在救援p3的`/var/lib/gaokun-hos-super-update.started.json`独占创建并fsync标记及父目录。标记永久保留，即使成功也拒绝重复执行；失败/半写不得删除标记直接重试，应独立分析、备份和审阅恢复方案。

流写严格限12GiB，同时计算输入流SHA；fsync并BLKFLSBUF清block缓存后整分区回读SHA。写入成功或异常均复核GPT、bootA/B和完整ESP。异常意味着本次更新失败，不能继续启动验证；工具不尝试回滚写盘，也不会自行重启。

测试只使用临时小文件和mock，不访问真实磁盘：

```
python3 -m unittest discover -s tools/hos -p test_update_isolated_super.py
```

10项测试覆盖pin、重复键/路径逃逸、GPT校验错误传播/精确条目、固定boot范围、UUID、devno挂载别名、有限写入及尾部guard、短读/输入变化/回读错误、错误救援/root、持久标记及dangling symlink、默认preview不写、旧哈希阻断apply、ESP新增文件。真实救援preview及apply尚未执行；单元测试不替代操作者对manifest和设备状态的离线审阅。

## SSH stdin 流模式

`--stdin` 与 `--image` 互斥。救援空间不足时无需把12GiB镜像中转到用户data：在主机先验证镜像hash，再通过SSH标准输入传输。`--stdin` preview只验证目标旧内容、GPT、保护区及manifest，**不会读取stdin，也不验证即将传入的镜像字节**；输出明确标记`incoming_bytes_preverified=false`。

apply在持久start marker落盘后复制stdin fd，按块累积短read，恰好接收12GiB，再读1字节确认EOF。截断、超长或hash错误都失败；超长字节不写入目标之外。镜像流只有写入后才完成SHA与整区回读验证，网络中断可能留下部分super，必须保留marker走独立恢复审阅。发送端必须关闭流以传达EOF；不能把交互shell输入当作镜像流。

追加防护：disk和p7设备节点major:minor须与各自sysfs/dev一致；mountinfo的ENOENT/ESRCH仅在对应进程已消失时跳过。测试现为14项，新增真实pipe短read/截断/超长/hash错误、stdin preview不读、main apply成功/失败marker持久保留及保护区finally复核。
