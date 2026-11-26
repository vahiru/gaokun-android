<!--
  发版说明模板。复制为docs/relnotes/v<版本>.md与docs/relnotes/v<版本>.zh-CN.md后填写；本模板文件本身不对外发布。

  ── 编写原则（填完后删掉本段注释）────────────────────────────────────────
  * 只写面向用户的内容：不放项目固定介绍、不放重复正文的摘要段、不放内部验收统计或实测机型台账（待测项归入docs/TODO.md）。
  * 开头的“升级与安装须知”一节只保留升级/安装前必须了解的事项（如备份提醒、破坏性变更、安装与已知限制入口）；如某版确有需特别醒目提醒的高风险警告，可仅对该条使用GitHub Alert（`> [!WARNING]`），不将整节放入引用块。
  * 按功能模块分组说明用户可感知的变化；某项新功能附带的使用限制直接写在该条目末尾，不另起“已知问题 / 已知限制”重复摘抄docs/known-limitations*。
  * 发版前同步核对docs/known-limitations*与README*：本版本已修复的条目及时移除，状态有变化的同步更新。
  * 下载表仅列出用户实际下载的安装与更新文件（OTA包、super.img.zst、boot.img、install-artifacts.sha256）及R2镜像链接；
    GPL对应源码清单（kernel-source.txt等，由scripts/release.sh生成并上传）放在表后一行集中列出，发布前用curl -sI确认所有链接返回200。
-->
## Upgrade and installation notes

* **System update (OTA)**: users on `v<first>` through `v<previous>` can update directly under *Settings → System → System update*. Back up important data beforehand: once the new build boots successfully, rolling back to the previous version with user data kept is not supported.
* **<Breaking or migration note, if any>**: <one concise sentence>.
* **Fresh installation**: see the [installation guide](https://github.com/vahiru/gaokun-android/blob/v<X.Y.Z>/docs/INSTALL.md), [known limitations](https://github.com/vahiru/gaokun-android/blob/v<X.Y.Z>/docs/known-limitations.md), and [FAQ](https://github.com/vahiru/gaokun-android/blob/v<X.Y.Z>/docs/FAQ.md).

## Changes

### <Area, e.g. Camera / Standby and networking / Display and video / Audio>

* **<User-visible change>** ([#NN](https://github.com/vahiru/gaokun-android/pull/NN), thanks to **@contributor**): <what changed from the user's perspective, and how to configure it if applicable>. *<Inline usage caveat, if any.>*

## Downloads

Existing installations can update directly under *System update* without downloading anything manually. Fresh installations require `super.img.zst` and `boot.img`.

| File | Mirror | Description |
|---|---|---|
| `crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0.zip` (`<size>`) | [R2](https://ota.072172.xyz/builds/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0.zip) | OTA update package (also attached to the GitHub Release) |
| `super.img.zst` (`<size>`) | [R2](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/super.img.zst) | System partitions image for fresh installation |
| `boot.img` (`<size>`) | [R2](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/boot.img) | Kernel and boot image for fresh installation |
| `install-artifacts.sha256` | [R2](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/install-artifacts.sha256) | SHA-256 checksums for the files above |

**Source and build manifests (GPL-2.0)**: [`v<X.Y.Z>`](https://github.com/vahiru/gaokun-android/tree/v<X.Y.Z>) · [`kernel-source.txt`](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/kernel-source.txt) · [`kernel-config.txt`](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/kernel-config.txt) · [`kernel-base-patches.tar.gz`](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/kernel-base-patches.tar.gz) · [`crdroid-manifest.xml`](https://ota.072172.xyz/install/crDroidAndroid-17.0-<YYYYMMDD>-gaokun3-v13.0/crdroid-manifest.xml) (all also attached to the GitHub Release)  
**Feedback**: submit via the [issue forms](https://github.com/vahiru/gaokun-android/issues/new/choose) (version `v<X.Y.Z>`).
