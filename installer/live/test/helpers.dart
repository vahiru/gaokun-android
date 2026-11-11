// 流程测试与出图共用的辅助。
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_installer/app.dart';
import 'package:gk3_installer/backend/backend.dart';
import 'package:gk3_installer/backend/fixture_backend.dart';
import 'package:gk3_installer/backend/protocol.dart';
import 'package:gk3_installer/l10n/app_localizations.dart';
import 'package:gk3_installer/session.dart';
import 'package:gk3_installer/ui/screens_risk.dart';
import 'package:gk3_installer/ui/widgets.dart';

/// 包一层 FixtureBackend：记下每一次调用；可以让某个函数失败
class Rec extends Gk3Backend {
  Rec(this.inner, {this.failApply = false, this.failApplyUntouched = false, this.shellError, this.failNetOnce = false, this.holdApply, this.holdNet, this.holdFollow});
  final FixtureBackend inner;
  final bool failApply;

  /// gk3_apply 在第一次写盘之前的检查里失败（ERR touched=no）
  final bool failApplyUntouched;

  /// 第一次 gk3_net_release 失败（下载中断，盘没动），之后照常
  bool failNetOnce;

  /// 给了就让 gk3_apply 停在半路、等它完成（看"写盘期间"界面的样子）
  final Completer<void>? holdApply;

  /// 给了就让 gk3_net_release 停在半路（看"下载期间"界面的样子）
  final Completer<void>? holdNet;

  /// 给了就让 gk3_job_follow 停在半路（接着跟一个写盘任务时的界面）
  final Completer<void>? holdFollow;

  /// 侧栏 / 完成页发出的重启与关机
  final power = <String>[];

  /// openShell 的结果（null = 切过去了）
  final String? shellError;
  final calls = <List<String>>[];

  @override
  String? get demoLabel => inner.demoLabel;

  @override
  Stream<Gk3Event> call(String fn, [List<String> args = const []]) async* {
    calls.add([fn, ...args]);
    if (fn == 'gk3_net_release' && failNetOnce) {
      failNetOnce = false;
      // 照新后端的样子（installer-lib.sh 的 gk3_net_fetch / gk3_fail）：进度与 ERR 都只有代码，中文只在 !! 与日志里
      yield parseStderrLine('PROGRESS 30 dl name=super.img.zst pct=31 speed=1843k left=0:07:12');
      yield const Gk3Log('下载 super.img.zst 中断（curl 退出码 28），3 秒后接着下（第 5/5 次）');
      yield parseStderrLine('ERR code=dl-incomplete name=super.img.zst rc=28 http=200 tries=5 kept_mib=377');
      yield const Gk3Log('!! 下载 super.img.zst 没完成（curl 退出码 28，试了 5 次）；已下的 377 MiB 留着，重试会接着下');
      yield const Gk3Exit(1);
      return;
    }
    if (fn == 'gk3_net_release' && holdNet != null) {
      yield parseStderrLine('PROGRESS 30 dl name=super.img.zst pct=31 speed=1843k left=0:07:12');
      await holdNet!.future;
    }
    if (fn == 'gk3_job_follow' && holdFollow != null) {
      yield parseStderrLine('PROGRESS 30 write-super');
      await holdFollow!.future;
    }
    if (fn == 'gk3_apply' && holdApply != null) {
      yield parseStderrLine('PROGRESS 30 write-super');
      await holdApply!.future;
    }
    if (fn == 'gk3_apply' && failApplyUntouched) {
      yield parseStderrLine('PROGRESS 1 check');
      yield parseStderrLine('ERR code=release-no-super touched=no');
      yield const Gk3Log('!! 发布目录里既没有 super.img.zst 也没有 super.img');
      yield const Gk3Exit(1);
      return;
    }
    if (fn == 'gk3_apply' && failApply) {
      yield parseStderrLine('PROGRESS 5 write-gpt');
      yield const Gk3Log('分区表已备份到 /media/gk3/gaokun3/gpt-backup-nvme0n1-1.bin（还原：sgdisk --load-backup=/media/gk3/gaokun3/gpt-backup-nvme0n1-1.bin /dev/nvme0n1）');
      yield parseStderrLine('ERR code=part-missing name=super disk=/dev/nvme0n1 touched=yes');
      yield const Gk3Log('!! 分区 super 没解析出来（/dev/nvme0n1 上找不到这个 PARTLABEL）');
      yield const Gk3Exit(1);
      return;
    }
    yield* inner.call(fn, args);
  }

  List<String>? last(String fn) => calls.lastWhere((c) => c.first == fn, orElse: () => const []).isEmpty
      ? null
      : calls.lastWhere((c) => c.first == fn);

  @override
  Future<void> reboot() async => power.add('reboot');
  @override
  Future<void> poweroff() async => power.add('poweroff');
  @override
  Future<String?> openShell() async => shellError;
}

final l = lookupL10n(const Locale('zh'));
final en = lookupL10n(const Locale('en'));

/// 直接同步读磁盘的资源包。
/// ★ 不用 rootBundle：它缓存 loadString 的 Future，而资源读取在测试里是真实异步 I/O ——
///   上一个测试结束时还在飞的那次读取挂在它的 FakeAsync 区里、永远完成不了，被缓存下来，
///   下一个测试 await 它就永远等下去（第一版：单独跑全过、一起跑 7 条挂 6 条）。
class DiskBundle extends AssetBundle {
  @override
  Future<ByteData> load(String key) async => ByteData.sublistView(File(key).readAsBytesSync());
  @override
  Future<String> loadString(String key, {bool cache = true}) async => File(key).readAsStringSync();
  @override
  Future<T> loadStructuredData<T>(String key, Future<T> Function(String value) parser) async => parser(await loadString(key));
}

Future<Rec> pumpApp(WidgetTester t, String scenario,
    {Map<String, String> overrides = const {},
    bool failApply = false,
    bool failApplyUntouched = false,
    double speed = 0,
    String language = 'zh',
    LangPrefs? prefs,
    String? shellError,
    bool failNetOnce = false,
    Completer<void>? holdApply,
    Completer<void>? holdNet,
    Completer<void>? holdFollow}) async {
  t.view.physicalSize = const Size(1280, 800);
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.reset);
  final rec = Rec(FixtureBackend(scenario, bundle: DiskBundle(), speed: speed, overrides: overrides),
      failApply: failApply, failApplyUntouched: failApplyUntouched, shellError: shellError, failNetOnce: failNetOnce, holdApply: holdApply, holdNet: holdNet, holdFollow: holdFollow);
  final session = Session(rec, prefs: prefs ?? LangPrefs.none);
  if (prefs == null) session.language = language;
  await t.pumpWidget(InstallerApp(session: session));
  await settle(t);
  return rec;
}

/// 页面上常有转圈（永不停的动画），pumpAndSettle 等不到头；而 fixture 是从资源里读文件，
/// 那是真实的异步 I/O —— 所以【等到目标出现】，不按固定步数赌时序
/// （按固定步数写的第一版：单独跑全过、一起跑一半失败）。
Future<void> waitFor(WidgetTester t, Finder f, {int tries = 150, Duration step = const Duration(milliseconds: 20)}) async {
  for (var i = 0; i < tries && f.evaluate().isEmpty; i++) {
    await t.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 10)));
    await t.pump(step);
  }
}

Future<void> settle(WidgetTester t, [int n = 20]) async {
  for (var i = 0; i < n; i++) {
    await t.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 5)));
    await t.pump(const Duration(milliseconds: 40));
  }
}

/// 断言某个东西出现（先等它）
Future<void> see(WidgetTester t, Finder f, [Matcher m = findsOneWidget]) async {
  await waitFor(t, f);
  expect(f, m);
}

Future<void> tap(WidgetTester t, Finder f) async {
  await waitFor(t, f);
  await t.ensureVisible(f.first);
  await t.tap(f.first, warnIfMissed: false);
  await settle(t);
}

Future<void> next(WidgetTester t, [L10n? loc]) async {
  // "下一步"要等到它可点（方案还在算的时候是禁用的）
  final f = find.ancestor(of: find.text((loc ?? l).btnNext), matching: find.byWidgetPredicate((w) => w is ButtonStyleButton && w.onPressed != null));
  await tap(t, f);
}

/// 写盘前的风险确认页（screens_risk.dart）：滚到底、等够时间、勾选、（要的话）输入确认词，然后继续。
/// [check] 给了就在页面刚出来时调一次（英文界面查 CJK 用）
Future<void> passRisk(WidgetTester t, {L10n? loc, void Function()? check}) async {
  final x = loc ?? l;
  await see(t, find.text(x.riskTitle));
  check?.call();
  await scrollRiskToEnd(t);
  await t.pump(const Duration(seconds: RiskPage.readSeconds));
  await settle(t);
  await tap(t, find.byKey(kRiskCheckKey));
  if (find.byKey(kRiskWordKey).evaluate().isNotEmpty) {
    await t.enterText(find.byKey(kRiskWordKey), x.riskWord);
    await settle(t);
  }
  await tap(t, find.text(x.riskContinue));
}

Future<void> scrollRiskToEnd(WidgetTester t) async {
  await t.drag(find.byKey(kRiskScrollKey), const Offset(0, -30000));
  await settle(t);
}

/// 风险确认页的"继续"能不能点
bool riskContinueEnabled(WidgetTester t, [L10n? loc]) =>
    t.widget<ButtonStyleButton>(find.ancestor(of: find.text((loc ?? l).riskContinue), matching: find.byWidgetPredicate((w) => w is ButtonStyleButton)).first).onPressed != null;

/// 按住 2 秒（确认页 / 缩分区）
Future<void> hold(WidgetTester t, String idleText) async {
  final g = await t.startGesture(t.getCenter(find.textContaining(idleText).first));
  for (var i = 0; i < 25; i++) {
    await t.pump(const Duration(milliseconds: 100));
  }
  await g.up();
  await settle(t, 60);
}


/// CJK 字符（汉字、全角标点、CJK 符号）
final cjk = RegExp(r'[\u3000-\u303F\u3400-\u4DBF\u4E00-\u9FFF\uF900-\uFAFF\uFF00-\uFFEF]');

/// 屏幕上（日志区以外）所有文字里出现的 CJK。"中文"是语言切换按钮自己的名字（用目标语言写它的名字），不算。
/// ★ 日志区（LogView）是后端原样的输出、部分是中文：失败页在英文界面里默认把它收起来（INST-10 的取舍，见 FailPage）
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

/// 记在内存里的语言偏好（测 GUI-19：记住 / gk3.lang=）
class MemPrefs extends LangPrefs {
  MemPrefs({this.stored, this.boot});
  String? stored;
  final String? boot;
  @override
  String? saved() => stored;
  @override
  String? fromBoot() => boot;
  @override
  void save(String lang) => stored = lang;
}
