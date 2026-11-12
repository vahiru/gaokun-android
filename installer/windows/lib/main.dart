import 'package:flutter/material.dart';
import 'package:gk3_common/backend/fixture_backend.dart';

import 'app.dart';
import 'backend/platform.dart';
import 'session.dart';
import 'ui/title_bar.dart';

/// 演示场景：testdata/<场景>/（真 gk3w 的录制，来源与替换写在各自的 index.txt 里；pubspec 的 assets 逐个列出）
const kScenarios = ['free', 'multi', 'installed', 'nospace', 'blocked', 'bad-release', 'usb', 'bitlocker', 'bitlocker-suspended'];

/// Windows：调程序目录里的 gk3w.exe（backend/platform_io.dart）；GK3_FIXTURE=<场景> 回放演示数据，
/// GK3_TEST_DISK=<磁盘号> 是对着一块 VHD 的测试模式。浏览器预览：?scenario=<场景>&lang=zh|en
void main() {
  // WindowControl.init 在 runApp 之前注册原生通道的处理器，绑定要先建好：否则这里抛异常、main 中止，
  // 界面一帧都画不出来，窗口（第一帧画完才显示）也就不出现
  WidgetsFlutterBinding.ensureInitialized();
  final scenario = requestedScenario();
  final real = scenario == null ? locateRealBackend() : null;
  final backend = real ?? FixtureBackend(kScenarios.contains(scenario) ? scenario! : kScenarios.first);
  final session = Session(backend, language: requestedLanguage(), testDisk: real == null ? null : requestedTestDisk());
  attachWindow(session);
  // 界面里没有接住的异常也记进日志文件（日志区之外，用户发来的日志里看得到）
  final note = session.logNote;
  if (note != null) {
    final present = FlutterError.onError;
    FlutterError.onError = (d) {
      note('!! ${d.exceptionAsString()}');
      present?.call(d);
    };
    WidgetsBinding.instance.platformDispatcher.onError = (e, _) {
      note('!! $e');
      return false;
    };
  }
  runApp(WinInstallerApp(session: session));
  session.check();
}
