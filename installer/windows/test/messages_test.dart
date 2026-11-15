// 后端的代码 ↔ 界面的话：契约测试（同 installer/live/test/messages_test.dart）。
// ★ gk3w（installer/backend/src/win/ 与平台无关的 src/）里出现的每一个进度代码与 ERR 代码，界面都得认识 ——
//   后端新加了一个、这边没加，这里就红（否则用户看到的是"安装器报告了错误：xxx"）。
import 'dart:io';

import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_windows/l10n/app_localizations.dart';
import 'package:gk3_windows/ui/messages.dart';

void main() {
  final zh = lookupL10n(const Locale('zh')), en = lookupL10n(const Locale('en'));
  // 界面不调用的命令：dump / esp-tree / mkpart / rmpart / readpart 只供开发排查（dump.c、write.c）
  const devOnly = {'dump.c', 'write.c'};
  final src = [
    for (final d in ['../backend/src/win', '../backend/src'])
      for (final f in Directory(d).listSync().whereType<File>().where((f) => f.path.endsWith('.c') && !devOnly.contains(f.uri.pathSegments.last)))
        f.readAsStringSync(),
  ].join('\n');

  Set<String> grab(String s, RegExp re) => re.allMatches(s).map((m) => m.group(1)!).toSet();
  Set<String> quoted(String s) => grab(s, RegExp(r'"([a-z][a-z0-9-]*)"'));
  final progCodes = grab(src, RegExp(r'gk3w_progress\(\s*[^,]+,\s*"([a-z][a-z0-9-]*)"\)'))
    // shrink-info 在检查页里跑，不经过进度页
    ..remove('shrink-analyze');
  final errCodes = {
    for (final m in RegExp(r'gk3w_err(?:_kv2?)?\(\s*[^,]+,\s*([^,]+),').allMatches(src)) ...quoted(m.group(1)!),
  }
    // space 查不了时不报错，只是不预先检查下载目录的空间（win_backend.dart 的 space()）
    ..remove('space-failed');

  test('抓到的代码数量像样（正则没写坏）', () {
    expect(progCodes.length, greaterThan(10));
    expect(errCodes.length, greaterThan(50));
    expect(progCodes, containsAll(['write-super', 'write-media', 'verify-media', 'done']));
    expect(errCodes, containsAll(['disk-write', 'esp-missing', 'media-in-use', 'bitlocker-key-wrong']));
  });

  test('每一个进度代码都有中英文的话', () {
    for (final c in progCodes) {
      final p = Gk3Progress(1, c, code: c);
      for (final l in [zh, en]) {
        expect(progressText(l, p), isNot(c), reason: '进度代码 $c 没有 l10n（${l.localeName}）');
      }
    }
  });

  test('每一个 ERR 代码都有中英文的话', () {
    for (final c in errCodes) {
      final e = Gk3Record('ERR', {'code': c});
      for (final l in [zh, en]) {
        for (final (media, internal) in [(false, false), (true, false), (true, true)]) {
          expect(errText(l, e, media: media, internal: internal), isNot(l.errOther(c)), reason: 'ERR 代码 $c 没有 l10n（${l.localeName}）');
        }
      }
    }
  });
}
