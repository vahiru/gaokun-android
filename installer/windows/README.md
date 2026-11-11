# Windows安装器（原型）

在已经启动的Windows里把Android装到系统盘的空闲空间上，不需要U盘与LiveCD。界面是Flutter，写盘由C后端`gk3w.exe`（[../backend](../backend/)）完成；两者之间是与LiveCD相同的行协议（[../common](../common/README.md)）。

现在还是原型：界面只回放演示数据，真后端（调用`gk3w.exe`、下载发布文件）尚未接上，也尚未构建Windows版。

## 页面

检查（机型、Secure Boot、快速启动、BitLocker、电源、空闲空间）→ 选位置 → 选版本 → 开机默认的系统 → 确认（按住2秒）→ 安装进度 → 完成或失败。已经装过时从检查页进入管理页，可以卸载。

分区够不够、数据分区多大都问`gk3w plan`，界面里不做分区计算。

## 预览

```sh
flutter run -d chrome --web-browser-flag=--window-size=1280,800   # ?scenario=<场景>&lang=zh|en
flutter test test/flow_test.dart                                   # 各场景从头走到尾
flutter test test/shots_test.dart                                  # 出图到 test/shots/（不入库，字体见 ../common）
```

| 场景 | 内容 |
|---|---|
| `free`（默认） | 检查全过，系统盘有40 GiB空闲 → 安装流程 |
| `installed` | 平板上真实的已安装布局 → 管理页、卸载 |
| `nospace` | 系统盘没有空闲区 |
| `blocked` | 机型、Secure Boot、快速启动不过，BitLocker开着 |
| `bad-release` | 发布文件不完整，安装在写盘前停下 |

演示数据都是真`gk3w`的录制，来源与替换写在各场景`testdata/<场景>/index.txt`的开头。
