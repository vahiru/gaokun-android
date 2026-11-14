import 'package:flutter/material.dart';
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/fixture_backend.dart';

import 'app.dart';
import 'backend/platform.dart';
import 'session.dart';
import 'ui/soak.dart';
import 'version.dart';

/// 演示场景：testdata/<场景>/（installer/tools/test/gen-fixtures.sh 用真后端录的，pubspec 的 assets 里逐个列出）。
/// gk3_common 的 FixtureBackend 不认识场景名，有哪些、默认用哪个由应用自己定
const kScenarios = ['factory', 'windows-free', 'windows-live', 'windows-setup', 'windows-setup-shrunk', 'blank', 'android'];

/// 真机：找 gk3l，走真后端。
/// 开发：GK3_FIXTURE=<场景>（Linux）或 ?scenario=<场景>（Chrome）回放录好的输出；
///       Web 上没有真后端，默认用 windows-free。
void main() {
  // 进 installer.log（gk3-installer-session 把 stdout / stderr 都接过去了）—— bug 报告先看这一行
  debugPrint('gk3_installer $kInstallerVersion');
  final scenario = requestedScenario();
  Gk3Backend? backend = scenario == null ? locateLiveBackend() : null;
  backend ??= FixtureBackend(kScenarios.contains(scenario) ? scenario! : 'windows-free');
  runApp(InstallerApp(session: Session(backend, prefs: platformLangPrefs()), home: soakRequested() ? const SoakPage() : null));
}
