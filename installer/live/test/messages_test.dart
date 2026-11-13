// 后端的代码 ↔ 界面的话（v1.0 计划 INST-10）。
// ★ 契约测试：gk3l（installer/backend/src/linux/ 与平台无关的 src/）里出现的每一个进度代码与 ERR 代码，界面都得认识 ——
//   后端新加了一个、这边没加，这里就红（否则用户看到的是"正在进行：xxx""出错了（代码 xxx）"）。
import 'dart:io';

import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_installer/l10n/app_localizations.dart';
import 'package:gk3_installer/ui/messages.dart';

void main() {
  final zh = lookupL10n(const Locale('zh')), en = lookupL10n(const Locale('en'));
  final src = [
    for (final d in ['../backend/src/linux', '../backend/src'])
      for (final f in Directory(d).listSync().whereType<File>().where((f) => f.path.endsWith('.c'))) f.readAsStringSync(),
  ].join('\n');

  Set<String> grab(String s, RegExp re) => re.allMatches(s).map((m) => m.group(1)!).toSet();
  Set<String> quoted(String s) => grab(s, RegExp(r'"([a-z][a-z0-9-]*)"'));
  final progCodes = {
    ...grab(src, RegExp(r'gk3w_progress\(\d+, "([a-z][a-z0-9-]*)"\)')),
    // 手拼的进度行：printf 的 "PROGRESS %u write-super …"、"PROGRESS 50 part-delete …"（proto.c 的 "%u %s" 是通用的那一处，不算）
    ...grab(src, RegExp(r'PROGRESS (?:%u|\d+) ([a-z][a-z0-9-]*)[ \\]')),
  };
  // gk3l_err / _kv / _kv2 的代码参数（可能是三元表达式），以及手拼的 ERR 行；ESP 空间不足的代码来自 gk3w_esp_check_code
  final errCodes = {
    for (final m in RegExp(r'gk3l_err(?:_kv2?)?\(\s*[^,]+,\s*([^,]+),').allMatches(src)) ...quoted(m.group(1)!),
    for (final m in RegExp(r'gk3w_line_kv\(&?l, "code", ([^;]+)\);').allMatches(src)) ...quoted(m.group(1)!),
    'esp-too-small', 'esp-full', 'esp-ota-room', 'esp-win-reserve',
  }..remove('loadervar-stuck'); // 这是 NOTE 的代码，完成页另说

  test('抓到的代码数量像样（正则没写坏）', () {
    expect(progCodes.length, greaterThan(10));
    expect(errCodes.length, greaterThan(50));
    expect(progCodes, containsAll(['write-super', 'part-delete', 'done']));
    expect(errCodes, containsAll(['disk-write', 'esp-busy', 'not-root', 'job-lost', 'logs-no-target']));
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
