import 'dart:io';

import 'package:gk3_common/backend/backend.dart';

import 'win_backend.dart';

/// 真后端：Windows 上调程序目录里的 gk3w.exe（找不到就是安装器的文件不全：每次调用都报 backend-missing，
/// 检查页上说清楚，不悄悄回放演示数据）。别的平台（Linux 上跑测试）没有真后端，回放演示数据。
Gk3Backend? locateRealBackend() {
  if (!Platform.isWindows) return null;
  return WinBackend.locate(testDisk: requestedTestDisk()) ??
      MissingBackend('${File(Platform.resolvedExecutable).parent.path}\\backend\\gk3w.exe');
}

/// GK3_FIXTURE=installed 之类
String? requestedScenario() => Platform.environment['GK3_FIXTURE'];

/// GK3_TEST_DISK=<磁盘号>：测试模式，把那块 VHD 当作系统盘（win_backend.dart 的 WinBackend.testDisk）
int? requestedTestDisk() => int.tryParse(Platform.environment['GK3_TEST_DISK'] ?? '');

/// GK3_LANG=en|zh；不给时跟系统语言走
String? requestedLanguage() {
  final env = Platform.environment['GK3_LANG'];
  if (env == 'en' || env == 'zh') return env;
  return Platform.localeName.toLowerCase().startsWith('zh') ? 'zh' : 'en';
}
