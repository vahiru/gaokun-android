# 树外内核的产物（供构建 boot.img）

`.gitignore` 把本目录**整个**忽略，只放行这份 README。

本机的内核不在 AOSP 树里编（主线 v7.2.9 stable + `refs/gaokun-buildbot/patches/`
那一套 + 本仓 `patches/`），所以作为 prebuilt 放在这里，由构建系统装进 boot 镜像。

## 需要放什么

```
vmlinuz.efi          内核（使用 EFI_ZBOOT 自解压 PE，约 14 MB，不要使用未压缩的 Image）
dtb/gaokun3.dtb      设备树。目录名固定 —— BoardConfig.mk 的
                     BOARD_PREBUILT_DTBIMAGE_DIR 指向 prebuilt-boot/dtb
```

**`dtb/` 目录下必须只放一个 `.dtb` 文件**（文件名本身不限）。
`BOARD_PREBUILT_DTBIMAGE_DIR` 会将该目录下所有 `*.dtb` 首尾拼接成一个 dtb 段写入 `boot.img`，且不会报错。
`scripts/release.sh` 会校验 `boot.img` dtb 段中的 `d00dfeed` 魔数数量（不为 1 则报错退出），但手动拷贝时仍需确保目录下没有旧的 `.dtb` 残留。

`vmlinuz.efi` 会被 `PRODUCT_COPY_FILES` 装成 `$(PRODUCT_OUT)/kernel`
—— 那是构建系统认的名字（`build/make/core/Makefile:1014`）。

## 怎么产出

```sh
cd <内核源码树>
bash <本仓>/scripts/kernel-config-android.sh <out 目录>
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=<out 目录> -j$(nproc) vmlinuz.efi dtbs
cp <out>/arch/arm64/boot/vmlinuz.efi                         prebuilt-boot/
mkdir -p prebuilt-boot/dtb
cp <out>/arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dtb prebuilt-boot/dtb/gaokun3.dtb
```

**`CROSS_COMPILE` 在配置与编译两步均需指定**：编译时不带会在 `prepare0` 阶段报错（误用宿主 gcc）；`olddefconfig` 时不带则会用宿主 gcc 评估 `CC_HAS_*` 能力符号，静默改动内核配置（详见 `scripts/kernel-config-android.sh`）。

## 为什么一定要 EFI_ZBOOT

内核最终会被 postinstall 钩子解到 **ESP** 上，而 ESP 只有 300 MiB，
还要和固件自己那个 73 MiB 的 `Persisted_Capsules.bin` 共处，
并且 A/B **两个槽位各存一份**。
未压缩的 `Image` 是 39,422,464 字节，`vmlinuz.efi` 是 14,021,120 字节
（2026-08-20 实测，2.8 分之一），每槽一份从 53 MB 降到约 26 MB。
`vmlinuz.efi` 经 systemd-boot 实机启动验证通过。
