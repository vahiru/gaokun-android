import 'package:flutter/material.dart';
import 'package:gk3_common/backend/fixture_backend.dart';

import 'app.dart';
import 'backend/platform.dart';
import 'session.dart';
import 'ui/title_bar.dart';

/// 演示场景：testdata/<场景>/（真 gk3w 的录制，来源与替换写在各自的 index.txt 里；pubspec 的 assets 逐个列出）
const kScenarios = ['free', 'multi', 'installed', 'nospace', 'blocked', 'bad-release', 'usb'];

/// Windows：调程序目录里的 gk3w.exe（backend/platform_io.dart）；GK3_FIXTURE=<场景> 回放演示数据，
/// GK3_TEST_DISK=<磁盘号> 是对着一块 VHD 的测试模式。浏览器预览：?scenario=<场景>&lang=zh|en
void main() {
  final scenario = requestedScenario();
  final real = scenario == null ? locateRealBackend() : null;
  final backend = real ?? FixtureBackend(kScenarios.contains(scenario) ? scenario! : kScenarios.first);
  final session = Session(backend, language: requestedLanguage(), testDisk: real == null ? null : requestedTestDisk())
    ..beforeDiskWrite = WindowControl.settleBusy;
  // 下载中途按 Alt+F4 或任务栏上的关闭：原生先交给界面问（ui/title_bar.dart 的 confirmQuitDuringDownload）
  WindowControl.init(onCloseRequested: () async {
    final ctx = kNavigatorKey.currentContext;
    if (ctx != null) await confirmQuitDuringDownload(ctx, session);
  });
  runApp(WinInstallerApp(session: session));
  session.check();
}
