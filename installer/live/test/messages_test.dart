// 后端的代码 ↔ 界面的话（v1.0 计划 INST-10）。
// ★ 契约测试：installer-lib.sh / gk3-unsparse.py 里出现的每一个进度代码与 ERR 代码，界面都得认识 ——
//   后端新加了一个、这边没加，这里就红（否则用户看到的是"正在进行：xxx""出错了（代码 xxx）"）。
import 'dart:io';

import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_installer/l10n/app_localizations.dart';
import 'package:gk3_installer/ui/messages.dart';

void main() {
  final zh = lookupL10n(const Locale('zh')), en = lookupL10n(const Locale('en'));
  final lib = File('../../scripts/live/installer-lib.sh').readAsStringSync();
  final unsparse = File('../../scripts/live/gk3-unsparse.py').readAsStringSync();

  Set<String> grab(String src, RegExp re) => re.allMatches(src).map((m) => m.group(1)!).toSet();
  // gk3_prog <百分比表达式> <代码> —— 百分比可能是 "$lo" 或 $(( … ))，代码是它后面第一个 [a-z-] 单词
  final progCodes = {
    ...grab(lib, RegExp(r'gk3_prog (?:\d+|"\$lo"|\$\(\([^)]*\)\)) ([a-z][a-z0-9-]*)')),
    ...grab(lib, RegExp(r'PROGRESS \$\(\([^)]*\)\) ([a-z][a-z0-9-]*) ')),
    ...grab(unsparse, RegExp(r'PROGRESS %d ([a-z][a-z0-9-]*)')),
  };
  final errCodes = grab(lib, RegExp(r'gk3_fail ([a-z][a-z0-9-]*)'));

  test('抓到的代码数量像样（正则没写坏）', () {
    expect(progCodes.length, greaterThan(10));
    expect(errCodes.length, greaterThan(40));
    expect(progCodes, containsAll(['write-super', 'part-delete', 'done']));
  });

  test('每一个进度代码都有中英文的话', () {
    for (final c in progCodes) {
      final p = Gk3Progress(1, c, code: c);
      for (final l in [zh, en]) {
        expect(progressText(l, p), isNot(l.progOther(c)), reason: '进度代码 $c 没有 l10n（${l.localeName}）');
      }
    }
  });

  test('每一个 ERR 代码都有中英文的话', () {
    for (final c in errCodes) {
      final e = Gk3Record('ERR', {'code': c});
      for (final l in [zh, en]) {
        expect(errText(l, e), isNot(l.errOther(c)), reason: 'ERR 代码 $c 没有 l10n（${l.localeName}）');
      }
    }
  });

  test('英文的进度与报错里没有 CJK（占位符给的是 ASCII 时）', () {
    final cjk = RegExp(r'[　-〿一-鿿＀-￯]');
    for (final c in progCodes) {
      expect(cjk.hasMatch(progressText(en, Gk3Progress(1, c, code: c, fields: const {'name': 'boot.img', 'pct': '5'}))), isFalse, reason: c);
    }
    for (final c in errCodes) {
      expect(cjk.hasMatch(errText(en, Gk3Record('ERR', {'code': c}))), isFalse, reason: c);
    }
  });
}
