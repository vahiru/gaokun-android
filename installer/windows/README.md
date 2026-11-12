# Windows安装器

在已经启动的Windows里把Android装到系统盘的空闲空间上，不需要U盘与LiveCD。界面是Flutter，写盘由C后端`gk3w.exe`（[../backend](../backend/)）完成；两者之间是与LiveCD相同的行协议（[../common](../common/README.md)）。

ARM64版（平板）由GitHub Actions构建（`.github/workflows/installer-windows.yml`）：推送`windows-installer-vX.Y.Z`标签时做发布构建，版本号取自标签，打包后建一个草稿Release（发布时不要设为最新版本，Latest留给ROM的Release）；也可以在Actions页面手动运行，产物是可以直接运行的程序目录。x64版只用于在PC上对着VHD测试，在本地构建（`flutter build windows`）。

## 后端

`lib/backend/win_backend.dart`：检查、分区方案、安装与卸载调用程序目录`backend\`里的`gk3w.exe`，参数按argv传递；`--sdboot`、`--mke2fs`、`--backup-dir`由它补上，界面不需要知道这些文件在哪里。版本列表与下载在Dart中实现（`lib/backend/net.dart`），输出与LiveCD的`gk3_net_manifest`、`gk3_net_release`相同；下载的规则沿用LiveCD的`gk3_net_fetch`（断点续传、停滞判定、只重试暂时性错误、换版本时丢弃旧文件），完成后用`gk3w hash`校验sha256。每次调用的参数与输出写入`%ProgramData%\gaokun3\logs\`。

程序清单要求以管理员身份运行（`windows/runner/runner.exe.manifest`），启动时出现一次UAC提示。

构建前先编好后端（在Linux上交叉编译，见[../backend](../backend/)）：

```sh
make -C ../backend win-x64 win-arm64 mke2fs sdboot   # gk3w.exe、mke2fs.exe + mke2fs.conf、systemd-bootaa64.efi
flutter build windows                               # CMake把它们放进程序目录的 backend\
```

缺少这些文件时构建只给出警告，这样的程序会在检查页报告“安装器文件不完整”。

## 测试模式与端到端测试

`GK3_TEST_DISK=<磁盘号>`：把那块VHD当作系统盘。此时写盘的调用去掉`--real-disk`（只允许写VHD）与`--bitlocker-key`，`bitlocker-key`去掉`--suspend`，`mft-move`去掉`--execute`，不改动这台电脑的C盘与BitLocker；检查页显示本机的检查项但不因它们阻止继续，不要求恢复密钥，也不提供压缩分析与移动MFT；“立即重启”不执行，右上角标出测试模式。

`integration_test/vhd_test.dart`驱动真实界面与真实后端，在VHD上走完安装、管理、卸载。需要管理员权限，在已提权的终端中运行：

```bat
set GK3_TEST_DISK=<VHD的磁盘号>
set GK3_TEST_RELEASE=<本地发布目录>   :: 可选；不设置则从GitHub下载默认选中的版本
flutter test integration_test\vhd_test.dart -d windows
```

窗口没有系统标题栏：标题栏由界面自绘（`lib/ui/title_bar.dart`），拖动、最小化、关闭经`gk3/window`通道交给原生窗口（`windows/runner/flutter_window.cpp`）；正在写盘时拒绝关闭。窗口大小按屏幕工作区算，内容按窗口等比缩放。

## 图标

crDroid的荷鲁斯之眼（蓝色、透明底），取自crDroid设置应用的`ic_crdroid_logo.xml`（Apache-2.0；图形本身是crDroid的标志，许可证不含商标使用权）。源文件与来源说明在`tool/icon/crdroid-eye.svg`，改了之后运行`bash tool/icon/make-icon.sh`重新生成Windows的`app_icon.ico`与网页图标。

## 页面

检查（机型、Secure Boot、快速启动、BitLocker、电源、空闲空间）→ 选位置 → 选版本 → 开机默认的系统 → 确认（按住2秒）→ 安装进度 → 完成或失败。已经装过时从检查页进入管理页，可以卸载。确认页提醒安全软件可能拦截`gk3w.exe`。

BitLocker（`lib/ui/bitlocker_box.dart`，后端`gk3w bitlocker-key`）：系统盘的保护已开启时，安装与卸载的确认页要求输入48位恢复密钥，由`gk3w`通过WMI与系统盘的恢复密钥比对，一致才能开始；密钥只经标准输入传给`gk3w`，不进参数与日志。写入磁盘前`gk3w`暂停保护到下次重启，Windows届时按新的启动链重新绑定。Secure Boot与BitLocker都开启时（出厂状态），检查页先校验密钥并暂停保护（Windows重启两次后自动恢复），用户再去固件设置关闭Secure Boot。系统盘没有恢复密钥、或正在加密或解密时，检查不通过。

压缩指引（`lib/ui/shrink_guide.dart`，后端`gk3w shrink-info`与`gk3w mft-move`）：检查页“可用未分配空间”一行的“分析可压缩空间”做一次与“磁盘管理”的“压缩卷”相同的分析，给出C盘最多还能压缩出多少，以及压缩停在哪个文件上（应用程序日志的259号事件），按文件类别给出处理步骤。停在`$MFT`上时可以“移动MFT”：用碎片整理接口把MFT位于后部的片段移到前部，执行期间不能关闭窗口、不能往下走，完成后自动重新分析。

左侧菜单底部的“制作独立安装介质”（0.1.0不注册这个入口：`app.dart`的`mediaEntryEnabled`；页面与测试都在）把最新的LiveCD与所选版本的安装文件写入U盘或本机磁盘：说明 → 目标（写到本机时另有位置页）→ 选版本 → 确认 → 写入。U盘整个清除后写入；写到本机时需满足首页的检查项，另有一页选择未分配空间并在分区条上标出GK3LIVE的位置：GK3LIVE建在所选未分配空间的末尾（默认选最大的一块，与LiveCD保留现有系统时选的相同），之后在LiveCD中安装的Android的userdata紧挨着它，删除GK3LIVE后可并入userdata；EFI分区中另添加LiveCD的启动项。安装方式（清除整个磁盘、保留现有系统、重新安装）在LiveCD中选择，本程序不向LiveCD传递状态。写入这一步对应的`gk3w media`命令尚未实现，演示数据中没有它的录制，回放时这一步按失败处理；介质大小暂定为2 GiB。`releases`另输出最新安装器的`LIVECD`记录。

分区够不够、数据分区多大都问`gk3w plan`，界面里不做分区计算。

## 预览

```sh
flutter run -d chrome --web-browser-flag=--window-size=1280,800   # ?scenario=<场景>&lang=zh|en
flutter test test/flow_test.dart                                   # 各场景从头走到尾
flutter test test/net_test.dart test/win_backend_test.dart         # 下载（本机HTTP服务）、版本列表、gk3w的参数
flutter test test/shots_test.dart                                  # 出图到 test/shots/（不入库，字体见 ../common）
```

| 场景 | 内容 |
|---|---|
| `free`（默认） | 检查全过，系统盘有40 GiB空闲 → 安装流程；分析可压缩空间停在分页文件上 |
| `multi` | 系统盘有几块空闲区 → 选位置 |
| `installed` | 平板上真实的已安装布局 → 管理页、卸载 |
| `nospace` | 系统盘没有空闲区 → 分析可压缩空间，停在分页文件上 |
| `nospace-mft` | 同`nospace`，压缩停在`$MFT`上 → 移动MFT（成功后换到`nospace-mft-after`） |
| `nospace-mft-after` | MFT移到前部之后，分析仍停在MFT开头的系统记录上 |
| `bitlocker` | 出厂状态：Secure Boot与BitLocker都开着 → 先校验恢复密钥并暂停保护（成功后换到`bitlocker-suspended`） |
| `bitlocker-suspended` | BitLocker保护已暂停、Secure Boot仍开着 |
| `bitlocker-on` | Secure Boot已关闭、BitLocker开着 → 确认页输入恢复密钥 |
| `blocked` | 机型、Secure Boot、快速启动不过，BitLocker开着 |
| `bad-release` | 发布文件不完整，安装在写盘前停下 |
| `usb` | 同`free`，另插着一个U盘 → 制作独立安装介质时可选U盘 |

演示数据都是真`gk3w`的录制，来源与替换写在各场景`testdata/<场景>/index.txt`的开头。例外：`usb`场景中的U盘是虚构的（尚无插着真U盘的录制）；`bitlocker*`的检查结果由平板的录制改写而来；`bitlocker-key`的回放对任何格式正确的密钥都按通过处理。
