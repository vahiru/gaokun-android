# micarray/prebuilt/ —— 麦克风阵列的系数文件（不入版本库）

这个目录里只有本README受版本控制。阵列处理要的系数文件放在：

```
hdrec_bf_data.bin
```

`device.mk`用`$(wildcard …)`判断：**有就装到`/vendor/etc/micarray/`，没有照样能编**。
没有它时`libgaokunmicarray`打开内置麦就读不到系数，日志里是
`weights: /vendor/etc/micarray/hdrec_bf_data.bin: cannot open: … -- array off`，录音和没有这个库时完全一样。

正式构建时由`scripts/sync-device-tree.sh`把这个目录带到构建机，并核对系数文件的sha256；
缺了或对不上会停下来报错，确实要不带它构建时设`GK3_ALLOW_NO_MICARRAY=1`。

## 为什么不入库

它是华为Windows音频驱动（HWVEAPO，华为的录音音效）里的数据，**未获再分发授权**。
和`firmware/`、`effects/prebuilt/`同一套规矩：整目录`.gitignore`、只放行README。

## 从哪来

和`firmware/README.md`第三种来源是同一个公开的驱动包release：

```bash
curl -LO https://github.com/matebook-e-go/uup-drivers-sc8280xp/releases/download/200.0.10.0/200.0.10.0.zip
unzip -j 200.0.10.0.zip '*HWVEAPO.cab'
cabextract -F hdrec_bf_data.bin HWVEAPO.cab     # 会提示文件尾有约9.8 KB多余数据，那是签名，不影响
cp hdrec_bf_data.bin device/huawei/gaokun3/micarray/prebuilt/
```

| 文件 | 大小 | sha256 |
|---|---|---|
| `HWVEAPO.cab` | 2460732 | `f16481286a8f243c9a8ba1a6563420a73814efb678b8f0921a791c09f31da485` |
| `hdrec_bf_data.bin` | 347592 | `34a5fb58b694703c1db7b70f01b2cc56fc5283109ca9d3be4b8d6207a77e1654` |

库在加载时还会核对文件大小和文件尾的两个校验字（前面86896个32位字的异或，再异或`0x55555555`），
对不上就不用，日志里写明原因。
