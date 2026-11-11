import 'package:flutter/material.dart';

import 'app.dart';
import 'backend/backend.dart';
import 'backend/fixture_backend.dart';
import 'backend/platform.dart';
import 'session.dart';
import 'ui/soak.dart';
import 'version.dart';

/// 真机：找 installer-lib.sh，走真后端。
/// 开发：GK3_FIXTURE=<场景>（Linux）或 ?scenario=<场景>（Chrome）回放录好的输出；
///       Web 上没有真后端，默认用 windows-free。
void main() {
  // 进 diag/installer.log（gk3-installer-session 把 stdout / stderr 都接过去了）—— bug 报告先看这一行
  debugPrint('gk3_installer $kInstallerVersion');
  final scenario = requestedScenario();
  Gk3Backend? backend = scenario == null ? locateShellBackend() : null;
  backend ??= FixtureBackend(FixtureBackend.scenarios.contains(scenario) ? scenario! : 'windows-free');
  runApp(InstallerApp(session: Session(backend, prefs: platformLangPrefs()), home: soakRequested() ? const SoakPage() : null));
}
