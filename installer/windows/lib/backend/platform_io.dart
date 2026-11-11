import 'dart:io';

import 'package:gk3_common/backend/backend.dart';

/// 真后端（调 gk3w.exe）还没接上：现在 Windows 上也回放演示数据
Gk3Backend? locateRealBackend() => null;

/// GK3_FIXTURE=installed 之类
String? requestedScenario() => Platform.environment['GK3_FIXTURE'];

/// GK3_LANG=en|zh；不给时跟系统语言走
String? requestedLanguage() {
  final env = Platform.environment['GK3_LANG'];
  if (env == 'en' || env == 'zh') return env;
  return Platform.localeName.toLowerCase().startsWith('zh') ? 'zh' : 'en';
}
