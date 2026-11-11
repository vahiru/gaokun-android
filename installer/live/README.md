# `gaokun3`图形安装器（Flutter）

本目录为运行在Debian LiveCD（`cage` Wayland合成器）上的Flutter图形安装器前端。

## 一、前后端职责边界

图形界面不包含任何分区规划或磁盘写入计算，所有磁盘探针、容量校验、NTFS压缩与镜像写入均调用[`scripts/live/installer-lib.sh`](../../scripts/live/installer-lib.sh)（或兼容行协议的Rust后端[`tools/gk3-installer/`](../../docs/dev/installer-rust-design.md)）：

```
lib/backend/      protocol.dart（行协议与百分号解码）· shell_backend.dart（真实后端调用）· fixture_backend.dart（测试录播回放）
lib/model/        将后端输出的键值对记录解析为强类型数据结构
lib/session.dart  单次安装会话状态机
lib/ui/           Material Design 3界面（固定1280×800逻辑画布自动等比缩放，见app.dart的LogicalCanvas）
testdata/         由真实后端在容器内录制的各场景输出fixture
```

调用后端命令时，`shell_backend.dart`固定通过`bash -c '. "$0" && "$@"' <lib> <函数> <参数…>`传递位置参数，严禁将用户输入（如Wi-Fi密码）拼接进shell字符串。

## 二、本地开发与测试

```sh
python3 tool/fetch-fonts.py               # 首次运行：从google/fonts下载打包所需的Roboto与Noto Sans SC字体
flutter test                              # 运行全部单元测试（协议解析、导航流转、多语言文案与截图测试）
flutter test test/shots_test.dart         # 生成全流程界面截图 → test/shots/*.png
flutter run -d chrome --web-browser-flag=--window-size=1280,800   # 浏览器交互预览（支持 ?scenario=factory|windows-free|blank|android&lang=zh|en）
```

重新录制容器内的后端测试fixture：

```sh
bash scripts/live/test-in-container.sh scripts/live/gen-fixtures.sh
```

| Fixture场景 | 模拟的磁盘状态 |
|---|---|
| `factory` | 出厂Windows全盘布局（无未分配空间，引导进入“缩小现有分区”） |
| `windows-free` | 在`factory`上执行`gk3_shrink`腾出80 GiB空闲区后的状态（引导进入“双系统并存安装”） |
| `blank` | 空盘（引导进入“清空整盘安装”） |
| `android` | 已安装过Android的磁盘（引导进入“重新安装Android”或拒绝重复追加双系统） |

## 三、交互与安全设计要点

- **Material Design 3主题**：基于`ColorScheme.fromSeed`动态派生配色，内嵌可变字重Roboto与Noto Sans SC字体（在`buildTheme()`中显式绑定`FontVariation('wght', ...)`）。
- **强制风险确认页**（`lib/ui/screens_risk.dart`）：任何修改磁盘的操作（整盘清空、双系统、覆盖重装、缩小分区、手动调整分区）执行前均需经过风险确认页（滚动到底部、倒计时阅读并勾选确认；整盘清空或含Windows磁盘的手动调整还需输入`ERASE`），最后一步按钮需长按2秒触发（支持触控长按或键盘按住回车/空格）。
- **不可用选项显式说明原因**：无法选择的磁盘或安装模式（如当前正在运行的Live USB介质、加密卷等）保持展示并注明不可用原因，而非直接隐藏。
- **多语言与文案校验**：后端只输出结构化进度码（`PROGRESS <代码>`）与错误码（`ERR code=<代码>`），由前端`lib/ui/messages.dart`映射为中英文文案。修改`lib/l10n/*.arb`后需运行`flutter gen-l10n`，`test/messages_test.dart`会自动校验`installer-lib.sh`中的所有状态码均具备中英文翻译。

## 四、Linux arm64构建与无头渲染验证

```sh
bash scripts/live/build-flutter.sh                 # 在arm64容器内构建 → out/installer-flutter-linux-arm64/
bash scripts/live/test-render.sh 60                # 在无头cage中实际运行并抓取截图与RSS → out/render-test/
SOAK=1 RENDERER=skia bash scripts/live/test-render.sh 600   # 持续渲染浸泡测试
```

常用运行期环境变量：
- `GK3_FIXTURE=<场景>`：使用内置fixture演示数据运行
- `GK3_RENDERER=skia`：切换为Skia渲染后端（默认启用Impeller）
- `GK3_WINDOWED=1`：窗口模式（默认在`cage -s`下无边框全屏运行）
- `GK3_LANG=en|zh`：指定初始界面语言（真机默认从内核命令行`gk3.lang=`或介质上的`gaokun3/installer-lang`读取）
