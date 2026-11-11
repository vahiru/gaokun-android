import 'dart:io';

import 'package:gk3_common/backend/backend.dart';

import 'win_backend.dart';

/// 真后端：Windows 上调程序目录里的 gk3w.exe（后端文件不全时每次调用都报 backend-missing，
/// 检查页上说清楚，不悄悄回放演示数据）。别的平台（Linux 上跑测试）没有真后端，回放演示数据。
Gk3Backend? locateRealBackend() {
  if (!Platform.isWindows) return null;
  return WinBackend.locate(testDisk: requestedTestDisk());
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

/// 本地发布目录里缺了哪些必需的文件（boot.img、super.img.zst；install-artifacts.sha256 是可选的，有就校验）。
/// 只在 Windows 上查：Linux 上跑测试时目录是假的
List<String> missingReleaseFiles(String dir) {
  if (!Platform.isWindows || dir.isEmpty) return const [];
  return [for (final f in const ['boot.img', 'super.img.zst']) if (!File('$dir\\$f').existsSync()) f];
}

/// 安装成功之后删掉下载的安装文件（只删我们下载的那几个，不删目录本身；删不掉就算了）
void deleteDownloadedRelease(String dir) {
  if (!Platform.isWindows) return;
  for (final f in const ['boot.img', 'super.img.zst', 'install-artifacts.sha256', 'boot.img.part', 'super.img.zst.part']) {
    try {
      final x = File('$dir\\$f');
      if (x.existsSync()) x.deleteSync();
    } on FileSystemException {
      // 留着也不影响什么
    }
  }
}
