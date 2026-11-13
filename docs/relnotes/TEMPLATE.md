<!--
  发版说明模板。复制为docs/relnotes/v<版本>.md与docs/relnotes/v<版本>.zh-CN.md后填写；本模板文件本身不对外发布。

  ── 填写说明（填完后删掉本段注释）────────────────────────────────────────
  * 面向用户撰写：先写用户能直观感知到的变化，再说明技术原因与实测范围。
  * 感谢报告者：附上issue链接与@用户名。
  * "Known issues"仅列出本版本特有或本版本发生变化的问题。长期存在的限制统一维护在
    docs/known-limitations.md与docs/known-limitations.zh-CN.md中；下方"Known limitations"一节仅保留摘要与链接。
  * 发版前核对docs/known-limitations.md：本版本已修复的条目及时移除，行为有变化的同步更新。
  * 降级说明：若本版本修改了/data下的持久化结构（数据库迁移、新增persist属性等），需在提示框中注明不支持保留数据降级。
  * Files表：每个文件同时提供GitHub与R2下载链接。R2路径规则见scripts/release.sh上传逻辑：
    OTA包位于builds/<zip名>；安装镜像与源码清单位于install/<zip名去掉.zip>/<文件名>；
    默认域名见release.sh中的HOST（https://ota.072172.xyz）。发布前请用curl -sI确认所有链接返回200。
  * GPL对应源码：release.sh每版自动生成kernel-source.txt、kernel-config.txt、
    kernel-base-patches.tar.gz与crdroid-manifest.xml，发布时一并上传至GitHub Release与R2并核对服务端字节数。
  * 同步提供中文版发版说明（v<版本>.zh-CN.md），结构保持一致。
-->
Android 17 (crDroid 13) for the **Huawei MateBook E Go** (Snapdragon 8cx Gen 3 /
SC8280XP) on a **mainline Linux kernel**, with hardware Vulkan on the Adreno 690.

<One paragraph: what kind of release this is, and the two or three changes a user will notice, in bold.>

**Talk to us:** [Telegram](https://t.me/gaokunAndroid) · QQ group **920133252**

> ### ⚠️ <Alpha / Beta — for 1.0, replace this heading and the first paragraph, keep the rest>
> A fresh install **erases the internal disk** (unless you use the graphical
> installer's dual-boot mode), and no image of the factory state exists anywhere.
> You should be comfortable recovering a machine that will not boot. No warranty.
>
> Any BIOS version works. Secure Boot must be off.
> Instructions: **[docs/INSTALL.md](https://github.com/vahiru/gaokun-android/blob/main/docs/INSTALL.md)** ·
> **Before installing, read [what does not work](https://github.com/vahiru/gaokun-android/blob/main/docs/known-limitations.md)** ·
> [FAQ](https://github.com/vahiru/gaokun-android/blob/main/docs/FAQ.md)
>
> Coming from **v<first> – v<previous>**: update in Settings, nothing else to do.
> <If this release changes data: "Going back to an older version afterwards is not supported with your data kept.">

## <Area, e.g. Standby / Wi-Fi / Audio / USB>

**<What the user sees now>** ([#NN](https://github.com/vahiru/gaokun-android/issues/NN),
thanks to **@reporter**). <Why it was broken, in plain words. What changed. What was measured on this build.>

<Repeat one section per area.>

## Smaller things

* **<Change>**: <one or two sentences>. (<What was / was not tested.>)

## Known issues

<Only problems specific to this release, or that changed in it. Long-standing ones live in known-limitations.md.>

* <Problem>. <Workaround, if any. What log to send: e.g. `adb logcat -b crash` right after it happens.>

## Known limitations

<!-- 一行一条，和 docs/known-limitations.md 逐条对应；那边删了这边也删。 -->
The full list, with workarounds, is in
**[docs/known-limitations.md](https://github.com/vahiru/gaokun-android/blob/main/docs/known-limitations.md)**
([中文](https://github.com/vahiru/gaokun-android/blob/main/docs/known-limitations.zh-CN.md)). In short:

* **Security**: signed with Android's public test keys; `/data` is **not encrypted**; root (KernelSU / ReSukiSU) is
  built in; the boot chain is unlocked.
* **Proprietary components** ship in the images: Huawei firmware and device calibration data, the Huawei Histen audio
  engine, Qualcomm's QNN runtime (for microphone noise suppression), and Google apps (MindTheGapps).
* **Not supported or incomplete**: *Erase all data* in Settings (untested; use the graphical installer's *Reinstall Android*);
  fingerprint payment (screen unlock only); first-generation M-Pencil; GPS / location; DRM video (no Widevine); Bluetooth headset
  microphone in calls; push notifications from apps that do not use FCM while in standby; Google certification
  (register once; Play Integrity never passes).

## Files

The in-system updater will offer this build; nothing here is needed for a normal
update. For a **fresh install** you want `super.img.zst` and `boot.img`.

| File | Download | What |
|---|---|---|
| `crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0.zip` (<size>) | GitHub · [R2](https://ota.072172.xyz/builds/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0.zip) | OTA package, also what the in-system updater downloads |
| `super.img.zst` (<size>) | GitHub · [R2](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/super.img.zst) | system / system_ext / product / vendor, for a fresh install |
| `boot.img` (<size>) | GitHub · [R2](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/boot.img) | kernel + device tree + first-stage ramdisk |
| `install-artifacts.sha256` | GitHub · [R2](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/install-artifacts.sha256) | checksums of the three files above |
| `gaokun3.json` | GitHub | the updater manifest for this build |
| `kernel-source.txt` | GitHub · [R2](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/kernel-source.txt) | kernel corresponding source (GPL-2.0): exact base, patch series and rebuild steps |
| `kernel-config.txt` | GitHub · [R2](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/kernel-config.txt) | the kernel `.config`, extracted from this `boot.img` |
| `kernel-base-patches.tar.gz` | GitHub · [R2](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/kernel-base-patches.tar.gz) | the kernel patches between the upstream tag and the build tree |
| `crdroid-manifest.xml` | GitHub · [R2](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/crdroid-manifest.xml) | exact revisions of the crDroid source tree |

Build stamp `<ro.build.date.utc>`, incremental `<YYYYMMDDhhmmss>`. The checks above were run
on a machine running exactly this build. Not run on it: <list what was not tested>.

Source: tag [`v<X.Y.Z>`](https://github.com/vahiru/gaokun-android/tree/v<X.Y.Z>) · kernel source manifest: [`kernel-source.txt`](https://github.com/vahiru/gaokun-android/releases/download/v<X.Y.Z>/kernel-source.txt) ([R2](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/kernel-source.txt))

The graphical installer is <unchanged — use `<installer version>` from [v<…>](https://github.com/vahiru/gaokun-android/releases/tag/v<…>) / attached: `gaokun3-installer-<version>-…`>.
