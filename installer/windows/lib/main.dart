import 'package:flutter/material.dart';
import 'package:gk3_common/backend/fixture_backend.dart';

import 'app.dart';
import 'backend/platform.dart';
import 'session.dart';

/// 演示场景：testdata/<场景>/（真 gk3w 的录制，来源与替换写在各自的 index.txt 里；pubspec 的 assets 逐个列出）
const kScenarios = ['free', 'multi', 'installed', 'nospace', 'blocked', 'bad-release'];

/// Windows：GK3_FIXTURE=<场景> 回放演示数据（真后端接上之前一律回放）；
/// 浏览器预览：?scenario=<场景>&lang=zh|en
void main() {
  final scenario = requestedScenario();
  final backend = (scenario == null ? locateRealBackend() : null) ??
      FixtureBackend(kScenarios.contains(scenario) ? scenario! : kScenarios.first);
  final session = Session(backend, language: requestedLanguage());
  runApp(WinInstallerApp(session: session));
  session.check();
}
