# 统一启动入口 gk3boot.efi 与执行端 fastboot.img 的预编译产物（随 vendor 下发）

`.gitignore`把本目录**整个**忽略，只放行这份README——与`prebuilt-boot/`同一个规矩：二进制不入库，构建机上由`scripts/sync-device-tree.sh`同步过去并断言。

统一启动入口与执行端的完整实现与规范见[`tools/gk3boot/README.md`](../../../../tools/gk3boot/README.md)。

## 需要放什么

```
gk3boot.efi    tools/gk3boot/build/efi/gk3boot.efi（aarch64 PE，约 100 KB）
version        一行：构建它时的 BOOT_VERSION（只准 [A-Za-z0-9._+-]，≤ 64 字符，不能是 log）
fastboot.img   可选：执行端 initramfs（gzip cpio，2–4 MiB），tools/gk3boot/build/fastboot/fastboot.img
```

`device.mk` 用 wildcard：前两样都在才装进 `/vendor/boot/gk3boot/{gk3boot.efi,version}`；不在照样能编，
ROM 里只是没有入口（开机完成线程报 `vendor.gaokun3.bootentry.error=no /vendor/boot/gk3boot in this build`，
不部署、也不撤已有的）。`fastboot.img` 在前两样都在的前提下有就一起装进 `/vendor/boot/gk3boot/fastboot.img`；
缺了照样能编，只是**没有执行端**：gk3boot 找不到执行端会照常启动 Android，HAL / postinstall 也不部署
菜单里的 `gk3boot-tools.conf`。

`version` 是**承重的**：ESP 上的目录叫 `EFI/gk3boot/<version>/`，boot_control HAL 与 OTA postinstall 都按它判断
"ESP 上的入口是不是这一版"。所以：
- 它必须与二进制里嵌的版本串一致（入口把 `androidboot.bootloader=gk3boot-<串>` 交给内核）——
  `sync-device-tree.sh` 第 3c 步用 `grep -ao 'gk3boot-[A-Za-z0-9._+-]*'` 从二进制里抠出来比对；
- **换了二进制就必须换版本串**。同一个版本串配两份不同的二进制，HAL 会用新的那份覆盖 ESP 上的旧文件
  （它按字节比对、不同就重写），但 OTA postinstall 与"上一版入口（gk3prev）"的轮换都以为版本没变 ——
  回退那一档就丢了。
- **`fastboot.img` 没有自己的版本串**：它与 `gk3boot.efi` 共用这个 `version`，部署到同一个
  `EFI/gk3boot/<version>/`、一起轮换（gk3prev 那一版带着它自己的执行端）、一起回收（整目录删）。
  所以**换了 `gk3boot.efi`、`fastboot.img` 任何一个（包括"这一版加上 / 去掉执行端"）都要换版本串**，理由同上。

## 怎么产出

```sh
colima start
bash scripts/gk3boot/test-boot.sh              # 在 arm64 容器里编 gk3boot.efi 并跑 QEMU 全部场景；版本串 = 0.2.0-e5.g<提交>
colima stop
cp tools/gk3boot/build/efi/gk3boot.efi device/huawei/gaokun3/prebuilt-gk3boot/
echo 0.2.0-e5.g<提交> > device/huawei/gaokun3/prebuilt-gk3boot/version   # 与 test-boot.sh 打印的"gk3boot 版本串"一致
shasum -a 256 device/huawei/gaokun3/prebuilt-gk3boot/gk3boot.efi
```

⚠️ 版本串里带 `.dirty`（工作区有未提交的改动时编的）就不要放进来：那一版对不上任何提交。

执行端（可选）：

```sh
colima start                                   # 已经在跑、且有别人的容器时别停它
bash scripts/gk3boot/build-fastboot-img.sh     # 产物 tools/gk3boot/build/fastboot/fastboot.img（+ 打印字节数与 sha256），4 MiB 预算断言
cp tools/gk3boot/build/fastboot/fastboot.img device/huawei/gaokun3/prebuilt-gk3boot/
shasum -a 256 device/huawei/gaokun3/prebuilt-gk3boot/fastboot.img   # 与构建脚本打印的一致
```

和 `gk3boot.efi` 一起换 `version`（上面的规矩）。`sync-device-tree.sh` 第 3c 步在构建机上断言它是 gzip、≤ 4 MiB、
`gzip -t` 通过并打印 sha256；没放时只打一行"这一版不带执行端"。

## 装上之后什么时候生效

装进 vendor **不等于**部署到 ESP。部署由属性 `persist.vendor.gaokun3.gk3boot` 决定（缺省 `off`，1.0 发版时再定默认值）：

| 值 | 开机完成时（boot_control HAL，`boot_control/Gk3Boot.cpp`） | OTA 时（`bin/gaokun3-ota-postinstall.sh`） |
|---|---|---|
| `off` / 没设 | 删掉全部 `gk3boot-*` / `gk3prev-*` 条目（含 `gk3boot-tools.conf`）与 `EFI/gk3boot/<ver>/`（`log/` 留着） | 删条目（目录留给新槽的 HAL 回收） |
| `observe` | 现役入口 = 本槽 vendor 那一版、`gk3.observe=1`（只算不写 misc）；执行端照样铺，`gk3boot-tools.conf` 删掉 | ESP 上还没有入口 ⇒ 直接部署 `+3`；已有别的版本 ⇒ 只铺目录 + `.staged` |
| `action` | 同上、`gk3.observe=0`（扣 tries、自动回滚、写 GK3 记录）；这一版的 `fastboot.img` 在 ESP 上时再写 `gk3boot-tools.conf` | 同上；第一次部署 / ESP 上已是这一版时对齐 `gk3boot-tools.conf`，铺 `.staged` 时不动它 |

文件（`gk3boot.efi`、`fastboot.img`）都走同一条规则：已逐字节相同就不写；`.new` → sync → 读回比对 → rename。
`fastboot.img` 写失败（ESP 满 / 介质错 / vendor 那份不是 gzip）**不挡**入口部署，只记日志（HAL 还写进
`vendor.gaokun3.bootentry.error`），也就不部署 `gk3boot-tools.conf`。HAL 在写之前看 ESP 剩余，不到
"它的大小 + 1 MiB" 就不写；postinstall 的空间门槛按 vendor 里两份文件的实际大小加进去。
ESP 上常态最多两版目录（现役 + gk3prev）；OTA 后、新槽开机完成之前是三版（再加 staged），HAL 激活时回收最老那版。

条目格式（HAL 与 postinstall 写的一模一样）：

```
title      Android                          （observe 时 "Android (gk3boot observe)"；上一版 "Android (previous loader)"）
version    gk3boot-<ver>
sort-key   0gk3                             （上一版 0gk3prev；都排在直连条目 zandroid<x> 前面）
efi        /EFI/gk3boot/<ver>/gk3boot.efi
options    gk3.observe=<0|1> gk3.hint=<x>
```

`gk3boot-tools.conf`（非默认，菜单里直接进执行端；不带计数、不 bless，`default *-android-<x>.conf` 命中不到它）：

```
title      Android fastboot / boot menu
version    gk3boot-<ver>
sort-key   0gk3tools                        （排在 0gk3 / 0gk3prev 之后、直连条目之前）
efi        /EFI/gk3boot/<ver>/gk3boot.efi   （总是现役那一版）
options    gk3.action=fastboot
```

`release.sh` 在本目录有 `gk3boot.efi` 时断言 `$OUT/vendor/boot/gk3boot/` 里那两份与这里逐字节相同；
本目录有 `fastboot.img` 时它也必须逐字节进了 vendor（本目录没有而 vendor 里有，提示是更早构建留下的）。
