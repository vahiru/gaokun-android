// 流程测试与出图共用的辅助（等待 / 按住 / 读资源的做法同 ../live/test/helpers.dart，理由见那里的注释）
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/fixture_backend.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_common/ui/widgets.dart';
import 'package:gk3_windows/app.dart';
import 'package:gk3_windows/l10n/app_localizations.dart';
import 'package:gk3_windows/session.dart';

/// 包一层 FixtureBackend：记下每一次调用与重启
class Rec extends Gk3Backend {
  Rec(this.inner);
  final FixtureBackend inner;
  final calls = <List<String>>[];
  var reboots = 0;

  @override
  String? get demoLabel => inner.demoLabel;

  @override
  Stream<Gk3Event> call(String fn, [List<String> args = const []]) {
    calls.add([fn, ...args]);
    return inner.call(fn, args);
  }

  @override
  Future<void> reboot() async => reboots++;
  @override
  Future<void> poweroff() async {}
  @override
  Future<String?> openShell() async => null;
}

final l = lookupL10n(const Locale('zh'));
final en = lookupL10n(const Locale('en'));

/// 直接同步读磁盘的资源包（不用 rootBundle：理由见 ../live/test/helpers.dart 的 DiskBundle）
class DiskBundle extends AssetBundle {
  @override
  Future<ByteData> load(String key) async => ByteData.sublistView(File(key).readAsBytesSync());
  @override
  Future<String> loadString(String key, {bool cache = true}) async => File(key).readAsStringSync();
  @override
  Future<T> loadStructuredData<T>(String key, Future<T> Function(String value) parser) async => parser(await loadString(key));
}

Future<(Rec, Session)> pumpApp(WidgetTester t, String scenario, {String language = 'zh'}) async {
  t.view.physicalSize = const Size(1280, 800);
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.reset);
  final rec = Rec(FixtureBackend(scenario, bundle: DiskBundle(), speed: 0));
  final session = Session(rec, language: language);
  await t.pumpWidget(WinInstallerApp(session: session));
  await t.runAsync(session.check);
  await settle(t);
  return (rec, session);
}

Future<void> waitFor(WidgetTester t, Finder f, {int tries = 150}) async {
  for (var i = 0; i < tries && f.evaluate().isEmpty; i++) {
    await t.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 10)));
    await t.pump(const Duration(milliseconds: 20));
  }
}

Future<void> settle(WidgetTester t, [int n = 20]) async {
  for (var i = 0; i < n; i++) {
    await t.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 5)));
    await t.pump(const Duration(milliseconds: 40));
  }
}

Future<void> see(WidgetTester t, Finder f) async {
  await waitFor(t, f);
  expect(f, findsWidgets);
}

Future<void> tap(WidgetTester t, Finder f) async {
  await waitFor(t, f);
  await t.tap(f.first);
  await settle(t);
}

/// 底栏的按钮能不能点
bool enabled(WidgetTester t, String label) =>
    t.widget<ButtonStyleButton>(find.ancestor(of: find.text(label), matching: find.byWidgetPredicate((w) => w is ButtonStyleButton)).first).onPressed != null;

/// 按住 2 秒
Future<void> hold(WidgetTester t, String idleText) async {
  final g = await t.startGesture(t.getCenter(find.textContaining(idleText).first));
  for (var i = 0; i < 25; i++) {
    await t.pump(const Duration(milliseconds: 100));
  }
  await g.up();
  await settle(t, 40);
}

final cjk = RegExp(r'[　-〿㐀-䶿一-鿿豈-﫿＀-￯]');

/// 屏幕上（日志区以外）的 CJK 文字；"中文"是语言切换按钮自己的名字，不算
List<String> cjkOnScreen(WidgetTester t) {
  final inLog = find.descendant(of: find.byType(LogView), matching: find.byType(RichText)).evaluate().map((e) => e.widget).toSet();
  final out = <String>[];
  for (final e in find.byType(RichText).evaluate()) {
    if (inLog.contains(e.widget)) continue;
    final s = (e.widget as RichText).text.toPlainText();
    if (s == '中文') continue;
    if (cjk.hasMatch(s)) out.add(s);
  }
  return out;
}
