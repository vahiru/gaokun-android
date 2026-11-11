# Windows安装器（原型）

在已经启动的Windows里把Android装到系统盘的空闲空间上，不需要U盘与LiveCD。界面是Flutter，写盘由C后端`gk3w.exe`（[../backend](../backend/)）完成；两者之间是与LiveCD相同的行协议（[../common](../common/README.md)）。

现在还是原型：界面只回放演示数据，真后端（调用`gk3w.exe`、下载发布文件）尚未接上。Windows版在x64上构建过（`flutter build windows`），ARM64要在Windows ARM64的机器上构建。

窗口没有系统标题栏：标题栏由界面自绘（`lib/ui/title_bar.dart`），拖动、最小化、关闭经`gk3/window`通道交给原生窗口（`windows/runner/flutter_window.cpp`）；正在写盘时拒绝关闭。窗口大小按屏幕工作区算，内容按窗口等比缩放。

## 图标

crDroid的荷鲁斯之眼（蓝色、透明底），取自crDroid设置应用的`ic_crdroid_logo.xml`（Apache-2.0；图形本身是crDroid的标志，许可证不含商标使用权）。源文件与来源说明在`tool/icon/crdroid-eye.svg`，改了之后运行`bash tool/icon/make-icon.sh`重新生成Windows的`app_icon.ico`与网页图标。

## 页面

检查（机型、Secure Boot、快速启动、BitLocker、电源、空闲空间）→ 选位置 → 选版本 → 开机默认的系统 → 确认（按住2秒）→ 安装进度 → 完成或失败。已经装过时从检查页进入管理页，可以卸载。

左侧菜单底部的“清除整个磁盘并安装”进入整盘安装：说明 → 检查（同上，另需存放LiveCD的未分配空间）→ 选版本 → 确认 → 准备。Windows端只在未分配空间中创建GK3LIVE分区、写入LiveCD及安装文件，并将下一次开机设为进入LiveCD；清除磁盘与安装在LiveCD中进行。准备这一步对应的`gk3w live`命令尚未实现，演示数据中没有它的录制，回放时这一步按失败处理；GK3LIVE的大小暂定为2 GiB。

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
