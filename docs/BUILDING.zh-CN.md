# 自己构建

这份文档写给想自己编译系统的人；只想安装的话，看[INSTALL.zh-CN.md](INSTALL.zh-CN.md)。

需要一台Linux构建主机，建议至少64 GB内存（AOSP官方建议）、约400 GB可用磁盘空间。

```sh
repo init -u https://github.com/crdroidandroid/android.git -b 17.0 --git-lfs
cp <本仓库>/manifests/local_manifest_gaokun3.xml .repo/local_manifests/
repo sync -c -j"$(nproc)"

cp -a <本仓库>/device/huawei/gaokun3 device/huawei/gaokun3
python3 <本仓库>/scripts/crdroid-tree-fixes.py .   # 为什么要改，脚本里写了
bash <本仓库>/scripts/camera/setup-libcamera-tree.sh . <上游libcamera仓库>   # 见patches/libcamera/README.md
source build/envsetup.sh
lunch lineage_gaokun3-cp2a-userdebug
m bacon superimage
```

* `bacon`和`superimage`要在**同一次**`m`里编：分两次会得到两个构建戳，OTA包和`super.img`互不相认（[`scripts/release.sh`](../scripts/release.sh)）。
* 编**`userdebug`**，不要编`user`：`gaokun3_smmustall`和`gaokun3_hangdump`的SELinux规则（[`sepolicy/gaokun3_scripts.te`](../device/huawei/gaokun3/sepolicy/gaokun3_scripts.te)）依赖`userdebug_or_eng`豁免，`user`变体尚未经验证。
* 默认编出来的是**发布构建**（变体仍是`userdebug`）：adb默认关、要用户自己打开，每台电脑都要授权；不监听TCP 5555；`ro.debuggable`为0；镜像里没有adb公钥。`GAOKUN3_DEV_BUILD=1 m bacon superimage`编出的是**开发构建**，带着以前那几样开发便利（adb免授权、TCP 5555、`ro.debuggable=1`、烤进你的公钥）——绝不要拿它发布。开关在[`lineage_gaokun3.mk`](../device/huawei/gaokun3/lineage_gaokun3.mk)，v0.7.1-alpha之后才有。
* 有几样构建输入**不在**本仓库里，各目录的README写了怎么准备：`firmware/`（华为固件，从你自己的机器上取——[`firmware/README.md`](../device/huawei/gaokun3/firmware/README.md)）、`hexagonrpcd-root/`（传感器DSP用的文件）、`prebuilt-boot/`（内核，见下）、`effects/prebuilt/`（Histen库；缺了构建照样通过，只是悄悄没了扬声器增强）、`micarray/prebuilt/`（麦克风阵列系数文件，另需对固件里的`audioreach-tplg.bin`跑一次`tplg-four-channel-capture.py`；缺了构建照样通过，阵列不接管录音——[`micarray/README.md`](../device/huawei/gaokun3/micarray/README.md)）、`denoise/prebuilt/`（由[`scripts/fetch-denoise-blobs.py`](../scripts/fetch-denoise-blobs.py)拉取高通QNN运行库与CDSP外壳，缺了构建会失败——[`denoise/README.md`](../device/huawei/gaokun3/denoise/README.md)）、`firmware/fingerpr.mbn`与`firmware/gaokun3-goodix-cfg.bin`（指纹TA与Goodix配置模板，缺了构建会失败——[`firmware/README.md`](../device/huawei/gaokun3/firmware/README.md)第四节）。开发构建还要`adb_keys`（你自己的adb公钥：`cp ~/.android/adbkey.pub device/huawei/gaokun3/adb_keys`）；发布构建不用它。

内核单独构建，分几层：主线**v7.2.9** stable（1.0.0-rc.1起；之前是v7.2-rc2），上面是[`linux-gaokun-buildbot`](https://github.com/KawaiiHachimi/linux-gaokun-buildbot)的补丁，再打本仓[`patches/`](../patches/)里的补丁（`scripts/kernel-apply-patches.sh <内核树>`，幂等）和ReSukiSU（`scripts/kernel-setup-resukisu.sh <内核树>`）。构建机需安装`pahole`（`dwarves` ≥ 1.22）和`libelf-dev`，以生成`CONFIG_DEBUG_INFO_BTF`所需的BTF调试信息。某一版内核到底用了哪些源码，看那一版release附件里的`kernel-source.txt`（v0.7.1-alpha发布时还没有这个附件，见[`docs/relnotes/v0.7.1-alpha-sources.md`](relnotes/v0.7.1-alpha-sources.md)）。`scripts/clone-refs.sh`拉的是buildbot的`main`分支，只供参考，不代表发版用的就是它。Android相关的配置断言在[`scripts/kernel-config-android.sh`](../scripts/kernel-config-android.sh)；`vmlinuz.efi`与DTB怎么编、放哪里，见[`prebuilt-boot/README.md`](../device/huawei/gaokun3/prebuilt-boot/README.md)。
