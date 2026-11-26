# 预编译内核与设备树产物目录（`prebuilt-boot/`）

本目录通过`.gitignore`忽略所有二进制产物，仅将本说明文件纳入版本控制。

由于本机使用外部构建的主线Linux内核（主线7.2.y stable + `refs/gaokun-buildbot/patches/` + 本仓[`patches/`](../../../../patches/)），内核镜像与DTB需作为预编译产物放置于本目录，供AOSP构建系统打包`boot.img`。

## 一、目录结构与文件要求

```text
vmlinuz.efi          内核镜像（开启EFI_ZBOOT的自解压PE镜像，约14 MB；请勿放入未压缩的Image）
dtb/gaokun3.dtb      设备树二进制（目录名由BoardConfig.mk中的BOARD_PREBUILT_DTBIMAGE_DIR固定指向prebuilt-boot/dtb）
```

- **`dtb/`目录下必须仅保留单个`.dtb`文件**：`BOARD_PREBUILT_DTBIMAGE_DIR`会将该目录下所有`*.dtb`首尾拼接写入`boot.img`的dtb段而不报错。`scripts/release.sh`会校验`boot.img` dtb段中的`d00dfeed`魔数数量（不为1则终止），手动更新时请勿在`dtb/`目录下留存旧文件备份。
- **`vmlinuz.efi`安装目标**：由[`BoardConfig.mk`](../BoardConfig.mk)中的`TARGET_PREBUILT_KERNEL`指向本文件，再由Lineage `kernel.mk`复制为`$(PRODUCT_OUT)/kernel`（请勿再在`PRODUCT_COPY_FILES`中重复复制至`kernel`，否则会触发make目标冲突）。
- **为何使用`EFI_ZBOOT`**：OTA `postinstall`钩子会将内核解包写入300 MiB的ESP分区（A/B双槽各存一份，且与固件胶囊文件共用空间）。`EFI_ZBOOT`将单份内核体积从约39.4 MB压缩至约14.0 MB，显著降低ESP空间占用，并可直接由`systemd-boot`或`gk3boot.efi`加载启动。

## 二、编译与更新步骤

```sh
cd <内核源码树>
bash <本仓>/scripts/kernel-config-android.sh <out目录>
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=<out目录> -j$(nproc) vmlinuz.efi dtbs
cp <out目录>/arch/arm64/boot/vmlinuz.efi                         prebuilt-boot/
mkdir -p prebuilt-boot/dtb
cp <out目录>/arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dtb prebuilt-boot/dtb/gaokun3.dtb
```

`scripts/kernel-config-android.sh`默认使用`CROSS_COMPILE=aarch64-linux-gnu-`（可通过环境变量覆盖），编译步`make`须显式指定`CROSS_COMPILE=aarch64-linux-gnu-`，避免使用宿主机编译器评估`CC_HAS_*`能力符号而静默改变内核配置；宿主机还需安装`pahole`（1.22及以上版本，Ubuntu/Debian的`dwarves`或`pahole`包）与`libelf-dev`，以生成`CONFIG_DEBUG_INFO_BTF`所需的BTF类型信息并编译宿主工具`resolve_btfids`（详见[`scripts/kernel-config-android.sh`](../../../../scripts/kernel-config-android.sh)）。
