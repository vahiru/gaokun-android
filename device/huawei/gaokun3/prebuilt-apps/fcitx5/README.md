# fcitx5-android（预置中文输入法）

APK从fcitx5-android项目的GitHub Release获取（使用上游项目自身签名；后续仅能从GitHub Release覆盖更新，F-Droid构建的签名不同，无法直接覆盖安装）。

| 项目 | 说明 |
|---|---|
| 版本 | 0.1.3（tag `0.1.3`，提交`048f581c`） |
| 文件 | `org.fcitx.fcitx5.android-0.1.3-0-g048f581c-arm64-v8a-release.apk` → 本目录`fcitx5-android-arm64-v8a.apk`（不入库，`.gitignore`） |
| 大小 | 45,935,104字节 |
| sha256 | `8e5de1036aea1f55895b39b4fc25a2d563e7b0663fa2708c86598f81768b106d`（入库校验文件`fcitx5-android-arm64-v8a.apk.sha256`） |
| 许可证 | LGPL-2.1（作为独立应用随ROM分发） |
| 源码 | <https://github.com/fcitx5-android/fcitx5-android/tree/0.1.3>（含子模块`fcitx5` / `libime` / `fcitx5-chinese-addons`等） |

下载与校验：

```sh
cd device/huawei/gaokun3/prebuilt-apps/fcitx5
curl -L -o fcitx5-android-arm64-v8a.apk \
  https://github.com/fcitx5-android/fcitx5-android/releases/download/0.1.3/org.fcitx.fcitx5.android-0.1.3-0-g048f581c-arm64-v8a-release.apk
shasum -a 256 -c fcitx5-android-arm64-v8a.apk.sha256
```

构建：APK存在时自动打包进`product`分区（`device.mk`，`wildcard`）；设`GAOKUN3_WITH_FCITX5=false`可显式跳过。构建机上由`scripts/sync-device-tree.sh`同步并校验sha256。升级版本时同步更新上表、`.sha256`与`NOTICE`中的源码链接。
