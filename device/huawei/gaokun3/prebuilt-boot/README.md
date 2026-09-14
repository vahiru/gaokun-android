# 树外内核的产物（供构建 boot.img）

仓库跟踪 Android 构建实际消费的 `vmlinuz.efi` 与
`dtb/gaokun3.dtb`。裸 `Image`、内核配置和其他中间产物不提交。

本机的内核不在 AOSP 树里编（主线 v7.2-rc2 + `refs/gaokun-buildbot/patches/`
那一套 + 本仓 `patches/`），所以作为 prebuilt 放在这里，由构建系统装进 boot 镜像。

## 需要放什么

```
vmlinuz.efi          内核。★用 EFI_ZBOOT 的自解压 PE（13 MB），不要用 Image（37 MB）
dtb/gaokun3.dtb      设备树。目录名固定 —— BoardConfig.mk 的
                     BOARD_PREBUILT_DTBIMAGE_DIR 指向 prebuilt-boot/dtb
```

⚠️★ **`dtb/` 里必须【只有一个】 .dtb 文件**（文件名本身不重要）。
`BOARD_PREBUILT_DTBIMAGE_DIR` 会把目录里**所有** `*.dtb` **首尾拼接**成一个
dtb 段塞进 boot.img，而**构建全程不报一声**。
2026-08-22 真踩过：目录里同时留了 `gaokun3.dtb` 与旧的
`sc8280xp-huawei-gaokun3.dtb`，产出的 dtb 段是 346052 字节 = 恰好 2 × 173026，
**唯一的线索就是"大小是整数倍"**。
`scripts/release.sh` 现在有断言（数 boot.img dtb 段里的 `d00dfeed` 魔数，≠1 就 die），
但换机器 / 手动拷贝时仍要自己注意。

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

⚠️★ **`CROSS_COMPILE` 两处都要带**：编译时不带会在 `prepare0` 阶段就报
`gcc: unrecognized command-line option '-mlittle-endian'`（它去用宿主 gcc 了）；
而 `olddefconfig` 不带会用**宿主 gcc** 去评估 `CC_HAS_*` 这类能力符号，
静默改掉一批配置（本仓踩过，见 `scripts/kernel-config-android.sh` 里的说明）。

## 为什么一定要 EFI_ZBOOT

内核最终会被 postinstall 钩子解到 **ESP** 上，而 ESP 只有 300 MiB，
还要和固件自己那个 73 MiB 的 `Persisted_Capsules.bin` 共处，
并且 A/B **两个槽位各存一份**。
当前未压缩的 `Image` 是 40,176,128 字节，`vmlinuz.efi` 是 14,320,128 字节
（2026-08-21 实测，约 2.8 分之一），每槽一份从约 53 MB 降到约 27 MB。
`vmlinuz.efi` 经 systemd-boot 实机启动验证通过。

## 当前预编译版本

当前文件由
[`pgs666/linux-gaokun-buildbot`](https://github.com/pgs666/linux-gaokun-buildbot)
的 `android/v7.2-egotouch-venus` 分支构建：

- buildbot commit: `99b7701db62ab0b8ccccec99ee315f62bd69cbcc`
- Linux tag: `v7.2-rc2`
- gaokun-android inputs: `844045acae7f6b845d7048845fce9305d76b37bd`
- GitHub Actions run:
  [`32485581600`](https://github.com/pgs666/linux-gaokun-buildbot/actions/runs/32485581600)

产物校验：

```text
029dd6e03c1bdb5b6aa19e8fc07a7e922b4cce8ec5928e846526d31adad96849  vmlinuz.efi
459069e394a3150cb61634cedacbc5e50a7a444e4bbcb892d71fd03dea64a650  dtb/gaokun3.dtb
```

该版本包含更新后的 EGoTouchRev 触摸驱动和 SC8280XP Venus 支持。产物已于
2026-08-21 写入 `boot_a` 并同步到 ESP `slot_a`，在 gaokun3 上实机启动进入
Android，触摸与系统启动正常；`boot_b` 保持为回退槽。
