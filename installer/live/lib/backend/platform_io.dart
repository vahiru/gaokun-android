import 'dart:io';

import 'package:gk3_common/backend/backend.dart';

import '../session.dart';
import 'live_backend.dart';

Gk3Backend? locateLiveBackend() => LiveBackend.locate();

/// GK3_FIXTURE=factory 之类：在 Linux 开发机上也用演示数据
String? requestedScenario() => Platform.environment['GK3_FIXTURE'];

/// GK3_SOAK=1：直接进浸泡页（M0 量帧率与 RSS 用，见 ui/soak.dart）
bool soakRequested() => Platform.environment['GK3_SOAK'] == '1';

LangPrefs platformLangPrefs() => const FileLangPrefs();

/// 语言的来源与记忆（v1.0 计划 GUI-19）：
///   * 启动项：内核参数 gk3.lang=en|zh（gk3-installer-session 也会转成 GK3_LANG 环境变量；这里两样都认）
///   * 记住：写进 /run/gk3-installer/lang（tmpfs：安装器被 systemd 重新拉起时还在，重启就没了；LiveCD 不往介质写）。
///     GK3_LANG_FILE 可以指定别的位置。
class FileLangPrefs extends LangPrefs {
  const FileLangPrefs();

  static List<String> get _files => [
        if (Platform.environment['GK3_LANG_FILE'] case final f? when f.isNotEmpty) f,
        '/run/gk3-installer/lang',
      ];

  @override
  String? saved() {
    for (final f in _files) {
      try {
        final v = File(f).readAsStringSync().trim();
        if (v.isNotEmpty) return v;
      } on FileSystemException {
        continue;
      }
    }
    return null;
  }

  @override
  String? fromBoot() {
    final env = Platform.environment['GK3_LANG'];
    if (env != null && env.isNotEmpty) return env;
    try {
      for (final tok in File('/proc/cmdline').readAsStringSync().trim().split(RegExp(r'\s+'))) {
        if (tok.startsWith('gk3.lang=')) return tok.substring('gk3.lang='.length);
      }
    } on FileSystemException {
      // 不是 Linux / 读不了：没有启动项给的默认
    }
    return null;
  }

  @override
  void save(String lang) {
    for (final f in _files) {
      try {
        final d = File(f).parent;
        if (!d.existsSync()) {
          // 只在 /run 下替自己建目录
          if (!f.startsWith('/run/')) continue;
          d.createSync(recursive: true);
        }
        File(f).writeAsStringSync('$lang\n', flush: true);
        return;
      } on FileSystemException {
        continue;
      }
    }
  }
}
