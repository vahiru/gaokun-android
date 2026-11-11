# 安装器共用包`gk3_common`

本目录是Flutter包（不是应用），放各个安装器前端都用得到、又与具体运行环境无关的部分。目前的使用者是LiveCD图形安装器[`installer/live/`](../live/README.md)，之后的Windows安装器也从这里取，免得两个应用各抄一份、各自漂移。

`installer/`是Dart pub工作区：根上的[`pubspec.yaml`](../pubspec.yaml)列出成员，依赖版本由同一个`installer/pubspec.lock`钉死；成员的`pubspec.yaml`写`resolution: workspace`，应用以`path: ../common`依赖本包。依赖只用Flutter SDK自带的包。

## 一、内容

```
lib/backend/protocol.dart        行协议：Gk3Event（记录 / 进度 / 日志 / 退出码）、parseStdoutLine/parseStderrLine、pctDecode
lib/backend/backend.dart         后端接口Gk3Backend（call / run / reboot / poweroff / openShell / demoLabel）与CallResult
lib/backend/fixture_backend.dart FixtureBackend：通用的录像回放（不认识任何场景名）
lib/ui/theme.dart                MD3主题：buildTheme、kSeed、kTouch、kCanvas、Gk3Colors、ThemeX（context.cs / tt / gk）、字体族名
lib/ui/canvas.dart               LogicalCanvas（固定1280×800逻辑画布、Esc返回）、DemoBadge（演示数据角标）
lib/ui/widgets.dart              Btn/BtnKind、ChoiceCard、DiskBar、HoldToConfirm、LogView
assets/fonts/                    Roboto与Noto Sans SC（不入库，python3 tool/fetch-fonts.py取，见assets/fonts/README.md）
```

留在应用里的：真后端（LiveCD的`ShellBackend`）、会话状态机、磁盘模型、各个页面、文案（l10n）与录像（`testdata/`）。

## 二、应用怎样接入

- **录像回放**：录像是应用自己的资源，放在应用的`testdata/<场景>/`（每个场景一个`index.txt`索引，各场景共用的放`testdata/common/`），并写进应用`pubspec.yaml`的`assets`。场景列表与默认场景由应用自己定（LiveCD在`lib/main.dart`的`kScenarios`），再`FixtureBackend(<场景>)`；资源根目录不叫`testdata`时传`root:`。
- **演示角标**：`DemoBadge(文字)`，文字由应用按自己的l10n拼好（LiveCD：`L10n.fixtureBadge(backend.demoLabel)`）。
- **控件里的文字**：本包不带文案，按钮、长按确认（`HoldToConfirm`的`idle`/`busy`）等文字都由应用传入。
- **磁盘布局条**：`DiskBar`只吃起点、大小与系统（`DiskBarPart`/`DiskBarFree`），应用把自己的磁盘模型转换过来（LiveCD：`lib/ui/widgets.dart`的`diskBar()`）。
- **字体**：在本包的`pubspec.yaml`里声明，依赖本包的应用自动打包，不必再声明。族名带包前缀，引用时用`kFontLatin`、`kFontCjk`、`kFontFallback`，不要直接写`Roboto`。`flutter test`不会自动加载字体，出图测试要按这两个族名自己注册（见`installer/live/test/shots_test.dart`）。

## 三、验证

本包没有自己的测试，行为由使用它的应用测（LiveCD：在`installer/live/`下运行`flutter test`）。改动后在本目录运行`flutter analyze`。
