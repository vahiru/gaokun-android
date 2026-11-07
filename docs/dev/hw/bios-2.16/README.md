# BIOS 2.16拆包：清单、工具与复现步骤

[`tools/gk3boot/README.md`](../../../../tools/gk3boot/README.md)中关于固件行为的结论（QcomBds的启动选择、UsbDevice / UsbConfig协议、NVMe无EraseBlock、ButtonsDxe、镜像度量等）均来自对本机BIOS 2.16升级包的离线拆包和静态分析。本页记录拆包链、模块清单与分析工具的用法，便于从同一个官方安装包逐字节复现。

> **注意**：安装包和拆出的DXE模块属于华为/高通专有二进制，不入版本库（见[`NOTICE`](../../../../NOTICE)）。入库的仅有拆包脚本、分析脚本和按模块列出的清单（名字、GUID、大小、sha256、depex）。

## 1. 来源

| 项 | 值 |
| --- | --- |
| 文件 | `Gaokun_8CX_BIOS_P02-02W-06F_2.16.exe`（华为面向GK-W7X发布的Windows版BIOS升级程序，NSIS安装包） |
| 大小 | 6,047,152字节 |
| sha256 | `b24abe76148bed32e3add10827701025cb917e216a359861e6f57f0f9797d1a6` |
| 本仓获取途径 | `refs/matebook-e-go-linux/drivers/`（Git LFS；clone后拉取对应LFS对象） |

**版本核对**：实机`/sys/firmware/efi/esrt/entries/entry0/fw_class`为`82315653-fe98-4014-82cf-ef099fa9357c`，与capsule内的`UpdateImageTypeId`一致；`fw_version`为131094（`0x20016`），与capsule厂商头版本字段一致；DMI读出版本为2.16（2023-01-31）。文件名中的`8CX`对应8cx Gen 3（gaokun3）；同目录下的`Gaokun_8C_BIOS_…_2.17.exe`属于8cx Gen 2机型，不可混用（见[`../../hardware.md`](../../hardware.md)）。

## 2. 复现步骤

```sh
bash scripts/clone-refs.sh matebook-e-go-linux
git -C refs/matebook-e-go-linux lfs pull --include 'drivers/Gaokun_8CX_BIOS_P02-02W-06F_2.16.exe'
shasum -a 256 refs/matebook-e-go-linux/drivers/Gaokun_8CX_BIOS_P02-02W-06F_2.16.exe   # 必须等于上表

python3 docs/dev/hw/bios-2.16/tools/extract.py \
    refs/matebook-e-go-linux/drivers/Gaokun_8CX_BIOS_P02-02W-06F_2.16.exe out/bios-2.16
diff out/bios-2.16/stages.tsv   docs/dev/hw/bios-2.16/stages.tsv
diff out/bios-2.16/manifest.tsv docs/dev/hw/bios-2.16/manifest.tsv    # 两个diff均应为空
```

`extract.py`仅依赖Python 3标准库（`lzma` / `zlib` / `hashlib`），无需7-Zip、UEFITool或binwalk，在每一层均校验结构断言。

## 3. 拆包链（每层大小与sha256见[`stages.tsv`](stages.tsv)）

| # | 层 | 识别与拆包方式 |
| --- | --- | --- |
| 1 | NSIS安装包 | firstheader `EF BE AD DE "NullsoftInst"`，非solid，每块独立LZMA（props `5d`、字典8 MiB）。共16块：头部、若干x86/x64 PE（升级程序本体）、bmp、bat及第6块的capsule |
| 2 | FMP capsule | `EFI_CAPSULE_HEADER`（GUID `6dcbd5ed-…-d92a`，HeaderSize `0x20`，Flags `0x50000`）→ `EFI_FIRMWARE_MANAGEMENT_CAPSULE_HEADER` v1（0个驱动、1个载荷）→ ImageHeader v2（UpdateImageTypeId `82315653-…`，ImageSize 8,743,702，VendorCodeSize 178,208）→ `EFI_FIRMWARE_IMAGE_AUTHENTICATION`（MonotonicCount 2，WIN_CERT_UEFI_GUID / PKCS7，830字节）→ 16字节厂商头`MSS1`（u32头长`0x10` + 两个u32 `0x00020016`）→ FV |
| 3 | capsule FV | FFS2；RAW文件`0a85a45e-915f-49db-8bd5-5337861f8082`内含aarch64 ELF（XBL的UEFI段，含构建路径`Build_Gen3_XBL_V_FLASH/…/SocPkg/Makena`） |
| 4 | ELF | 15个程序头；VA `0x9f000000`、6 MiB的`PT_LOAD`即UEFI FD的FV |
| 5 | FD FV | 4个文件：PAD、SEC（TE映像）、`uefiplat.cfg`（内存图等平台配置）、`9e21fd93-…`（FV_IMAGE容器） |
| 6 | 容器 | GUIDED段（GUID `1d301fe9-be79-4353-91c2-d23bc959ae0c`），采用高通gzip封装 |
| 7 | 段流 → DXE FV | gunzip解出16,031,752字节段流：4字节RAW填充段 + FV image段 → DXE FV（共176个文件：1个DXE_CORE、151个DRIVER、4个APPLICATION、20个FREEFORM，含面板xml、bmp、`BDS_Menu.cfg`、`SecParti.cfg`、`uefipil.cfg`、`QcomChargerCfg.cfg`等） |

## 4. 模块清单：[`manifest.tsv`](manifest.tsv)

每行对应一个FFS文件（FD FV 4个 + DXE FV 176个），依次记录：所在FV、偏移、GUID、类型、UI名、文件大小、映像类型（PE32/TE）、映像大小、映像sha256、depex原文（hex，可用`tools/scan.py`解码）。与启动链路直接相关的模块如下：

| 模块 | GUID | 作用 |
| --- | --- | --- |
| QcomBds | `5a50aa81-c3ae-4608-a0e3-41a2e69baf94` | 启动项选择、5分钟看门狗、华为BootFail计数 |
| HwBcdOneKey | `a1124b16-43f6-4b90-a19a-c006e54500af` | `HwStartImage`钩子 |
| SecurityDxe / MeasureBootDxe / TrEEDxe | `5e0eae60-…` / `553c050b-…` / `59cc11dc-…` | 镜像度量与安全启动验签 |
| PartitionDxe / NvmExpressDxe | `1fa1f39e-…` / `5be3bdf4-…` | 自动修复备份GPT；NVMe分区无EraseBlock协议 |
| UsbfnDwc3Dxe / UsbDeviceDxe / UsbConfigDxe / UsbInitDxe / UsbMsdDxe | `94f8a6a7-…` / `3299a266-…` / `cd823a4d-…` / `0a134f0e-…` / `5af77f10-…` | UEFI内USB外设协议栈 |
| ButtonsDxe | `5bd181db-0487-4f1a-ae73-820e165611b3` | 音量键与电源键扫描 |
| RotateScreen | `83ec3fc9-cb74-4b2b-89b9-e98f0972f2a8` | 虚拟横屏GOP模式 |
| WatchdogTimer / HwOpenWdtDxe / QcomWDogDxe | `f099d67f-…` / `4938dac6-…` / `040e1e61-…` | UEFI / EC看门狗 |
| ResetRuntimeDxe | `3ae17db7-3cc5-4b89-9304-9549211057ef` | `EFI_RESETREASON`提供方 |
| RngDxe | `b0d3689e-11f8-43c6-8ece-023a29cec35b` | `EFI_RNG`（KASLR种子来源） |

## 5. 分析工具（[`tools/`](tools/)）

| 脚本 | 作用 |
| --- | --- |
| `extract.py` | 执行上述拆包链，输出`stages.tsv`、`manifest.tsv`、`pe/`、`raw/`、`stage/` |
| `guiddb.py [refs]` | 扫描`refs/edk2`、`refs/clo-abl-5.0/QcomModulePkg`、`refs/systemd-v257/src/boot`、`refs/gbl`中的GUID定义，生成`guiddb.json`（不入库） |
| `scan.py <out>/pe [模块…]` | 字节扫描各`.efi`中出现的已知GUID并解码depex |
| `pe.py <efi> xs <串> [1]` | 查找字符串（`1` = UTF-16）的`adrp`/`add`交叉引用与所在函数；`dis <rva> [n]`输出带注释反汇编（需`pip install capstone`） |
| `xr2.py <efi> <串…>` | 线性扫描全`.text`段，列出引用目标字符串的位置 |
| `xs.sh <efi> <串…>` | 对每个字符串依次运行ASCII与UTF-16模式的`pe.py xs` |
| `str.py` / `allstr.py <正则> <文件…>` | 提取ASCII与UTF-16字符串（`str.py`附带常见EDK2噪声过滤） |

## 6. 其他固件细节

- `uefiplat.cfg`位于FD FV中（不在DXE FV中），开头声明`UnusableDDRMemoryStartAddr = 0x80000000`、`UnusableDDRMemorySizeAtBeginning = 0x600000`，后接完整`[MemoryMap]`。内核获取的内存布局来自固件按该表构建的EFI内存图（DTB中的`/memory`为空），因此内核引导必须经过EFI stub。
- FD FV中的SEC为TE映像（非PE32），导出文件名为`pe/8af09f13-….te`。
