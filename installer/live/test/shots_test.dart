// 离线出图：每一屏、每个关键状态渲染成 1280×800 的 PNG（与真机上的逻辑画布同尺寸）。
//
//   python3 tool/fetch-fonts.py         # 第一次：取打包进应用的两个字体（与设备上同一份）
//   flutter test test/shots_test.dart   # → test/shots/*.png
//
// ★ 这是 C 版 `make shots` 的等价物，也是本仓那条铁律："目标机器同时是作者的日用平板，
//   经常拿不到。没有离线渲染，改一行文案都要排队等上机"（C 版 README.md:13-14；C 版已删，git show 445e978:live/installer/README.md）。
// ★ 它是"出图"，不是像素比对：每次都重写 PNG（autoUpdateGoldenFiles）。行为由
//   flow_test.dart 管；像素级 golden 跨平台字体渲染不一致，比对只会制造噪音。
// ⚠️ 没有字体时整组跳过（不然全是方块，看了等于没看）。
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart' show Scrollable;
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_installer/app.dart';
import 'package:gk3_installer/ui/screens_risk.dart';

import 'helpers.dart';

// 打包进应用的那两个字体（pubspec 的 fonts；python3 tool/fetch-fonts.py 取）。
// 测试里不会自动加载 pubspec 声明的字体，这里按同样的族名注册 —— 出的图与设备上同一份字形。
const _fonts = {
  'Roboto': 'assets/fonts/Roboto-VF.ttf',
  'Noto Sans SC': 'assets/fonts/NotoSansSC-VF.otf',
  // 日志区的 monospace：设备上是系统的等宽字体，出图时用 Noto 顶上（不然是方块）
  'monospace': 'assets/fonts/NotoSansSC-VF.otf',
};

Future<void> shot(WidgetTester t, String name) async {
  await settle(t, 6);
  await expectLater(find.byType(InstallerApp), matchesGoldenFile('shots/$name.png'));
}

void main() {
  final haveFont = _fonts.values.every((f) => File(f).existsSync());
  setUpAll(() async {
    if (!haveFont) return;
    autoUpdateGoldenFiles = true;
    for (final MapEntry(key: fam, value: f) in _fonts.entries) {
      await (FontLoader(fam)..addFont(Future.value(ByteData.sublistView(File(f).readAsBytesSync())))).load();
    }
    // 图标：不加载就全是空心方块。字体在 Flutter SDK 里（flutter test 会设 FLUTTER_ROOT）
    final icons = File('${Platform.environment['FLUTTER_ROOT']}/bin/cache/artifacts/material_fonts/MaterialIcons-Regular.otf');
    if (icons.existsSync()) {
      await (FontLoader('MaterialIcons')..addFont(Future.value(ByteData.sublistView(icons.readAsBytesSync())))).load();
    }
  });

  Future<void> toDisk(WidgetTester t) async {
    await tap(t, find.text(l.btnStart));
    await waitFor(t, find.textContaining('/dev/nvme0n1'));
  }

  Future<void> toMode(WidgetTester t) async {
    await toDisk(t);
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await waitFor(t, find.text(l.modeWipeTitle));
  }

  testWidgets('01 欢迎 / 预检通过', (t) async {
    await pumpApp(t, 'windows-free');
    await waitFor(t, find.text('2.16'));
    await shot(t, '01-welcome');
  }, skip: !haveFont);

  testWidgets('02 欢迎 / 预检拦住（安全启动；BIOS 2.17 放行）', (t) async {
    await pumpApp(t, 'blank', overrides: {'gk3_preflight': 'preflight-secureboot.txt'});
    await waitFor(t, find.text(l.checkBlocked));
    await shot(t, '02-welcome-blocked');
  }, skip: !haveFont);

  testWidgets('03 选盘（安装 U 盘列出但禁用）', (t) async {
    await pumpApp(t, 'factory');
    await toDisk(t);
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await shot(t, '03-disk');
  }, skip: !haveFont);

  testWidgets('03b 选盘（免 U 盘：安装器跑在内置盘上，可选、带提示）', (t) async {
    await pumpApp(t, 'windows-live');
    await toDisk(t);
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await shot(t, '03b-disk-internal-medium');
  }, skip: !haveFont);

  testWidgets('04b 方式（免 U 盘：整盘清空禁用，双系统可行）', (t) async {
    await pumpApp(t, 'windows-live');
    await toMode(t);
    await tap(t, find.text(l.modeAlongTitle));
    await shot(t, '04b-mode-internal-medium');
  }, skip: !haveFont);

  testWidgets('04 方式：出厂布局，没有空闲区 → 可以缩分区', (t) async {
    await pumpApp(t, 'factory');
    await toMode(t);
    await tap(t, find.text(l.modeShrinkTitle));
    await shot(t, '04-mode-factory');
  }, skip: !haveFont);

  testWidgets('05 缩分区', (t) async {
    await pumpApp(t, 'factory');
    await toMode(t);
    await tap(t, find.text(l.modeShrinkTitle));
    await next(t);
    await passRisk(t);
    await tap(t, find.textContaining('Data'));
    await shot(t, '05-shrink');
  }, skip: !haveFont);

  testWidgets('06 方式：双系统可行', (t) async {
    await pumpApp(t, 'windows-free');
    await toMode(t);
    await tap(t, find.text(l.modeAlongTitle));
    await shot(t, '06-mode-alongside');
  }, skip: !haveFont);

  testWidgets('07 方式：已经装过', (t) async {
    await pumpApp(t, 'android');
    await toMode(t);
    await shot(t, '07-mode-installed');
  }, skip: !haveFont);

  testWidgets('08 来源 / U 盘里没有镜像', (t) async {
    await pumpApp(t, 'windows-free', overrides: {'gk3_release_info': 'release_info-none.txt'});
    await toMode(t);
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await shot(t, '08-source-nousb');
  }, skip: !haveFont);

  testWidgets('09–11 WiFi 两步式 与 选版本', (t) async {
    await pumpApp(t, 'windows-free', overrides: {'gk3_release_info': 'release_info-none.txt'});
    await toMode(t);
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await next(t);
    await waitFor(t, find.text('宿舍网-5G'));
    await shot(t, '09-wifi');
    await tap(t, find.text('宿舍网-5G'));
    for (final k in 'hunter'.split('')) {
      await tap(t, find.text(k));
    }
    await shot(t, '10-wifi-password');
    await tap(t, find.text(l.btnBack));
    await t.scrollUntilVisible(find.text(l.netHidden), 200, scrollable: find.byType(Scrollable).last);
    await tap(t, find.text(l.netHidden));
    for (final k in 'lab-guest'.split('')) {
      await tap(t, find.text(k == '-' ? '123' : k));
      if (k == '-') {
        await tap(t, find.text('-'));
        await tap(t, find.text('abc'));
      }
    }
    await shot(t, '10b-wifi-hidden');
    await tap(t, find.text(l.btnBack));
    await t.scrollUntilVisible(find.text('宿舍网-5G'), -200, scrollable: find.byType(Scrollable).last);
    await tap(t, find.text('宿舍网-5G'));
    for (final k in 'hunter'.split('')) {
      await tap(t, find.text(k));
    }
    for (final k in '42'.split('')) {
      await tap(t, find.text('123'));
      await tap(t, find.text(k));
      await tap(t, find.text('abc'));
    }
    await tap(t, find.text(l.netConnect));
    await next(t);
    await tap(t, find.text('标准版'));
    await shot(t, '11-variant');
  }, skip: !haveFont);

  testWidgets('12–13 选项 与 分区大小', (t) async {
    await pumpApp(t, 'windows-free');
    await toMode(t);
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await next(t);
    await waitFor(t, find.textContaining('/data'));
    await shot(t, '12-opts');
    await tap(t, find.text(l.optsAdvanced));
    await shot(t, '13-adv');
  }, skip: !haveFont);

  // 写盘前的风险确认（用户 2026-10-06）：刚进来（四个门槛都没满足、提示往下滚）与全部满足之后
  testWidgets('13b–13d 风险确认：整盘清除（出厂盘，要确认词）', (t) async {
    await pumpApp(t, 'factory');
    await toMode(t);
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    await next(t);
    await next(t);
    await waitFor(t, find.text(l.riskTitle));
    await shot(t, '13b-risk-wipe');
    await scrollRiskToEnd(t);
    await t.pump(const Duration(seconds: RiskPage.readSeconds));
    await tap(t, find.byKey(kRiskCheckKey));
    await t.enterText(find.byKey(kRiskWordKey), 'ERASE');
    await shot(t, '13c-risk-wipe-ready');
  }, skip: !haveFont);

  testWidgets('13d 风险确认：双系统（Windows 与 BitLocker）', (t) async {
    await pumpApp(t, 'windows-free', overrides: {'gk3_esp_info': 'esp_info-bitlocker.txt'});
    await toMode(t);
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await next(t);
    await next(t);
    await waitFor(t, find.text(l.riskTitle));
    await scrollRiskToEnd(t);
    await shot(t, '13d-risk-alongside');
  }, skip: !haveFont);

  testWidgets('13e 风险确认：英文、整盘清除', (t) async {
    await pumpApp(t, 'factory', language: 'en');
    await tap(t, find.text(en.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t, en);
    await tap(t, find.text(en.modeWipeTitle));
    await next(t, en);
    await next(t, en);
    await next(t, en);
    await waitFor(t, find.text(en.riskTitle));
    await shot(t, '13e-risk-wipe-en');
  }, skip: !haveFont);

  testWidgets('14 确认：整盘清除，逐条列出将被删除的分区', (t) async {
    await pumpApp(t, 'factory');
    await toMode(t);
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    await next(t);
    await next(t);
    await passRisk(t);
    await waitFor(t, find.text(l.confirmWipeHead));
    await shot(t, '14-confirm-wipe');
  }, skip: !haveFont);

  testWidgets('15b 确认：重新安装（默认清除数据）', (t) async {
    await pumpApp(t, 'android');
    await toMode(t);
    await tap(t, find.text(l.modeReinstallTitle));
    await next(t);
    await next(t);
    await next(t);
    await passRisk(t);
    await waitFor(t, find.text(l.confirmReinstallHead));
    await shot(t, '15b-confirm-reinstall');
  }, skip: !haveFont);

  testWidgets('04c 调整磁盘（选中 Data、调整大小）', (t) async {
    await pumpApp(t, 'factory');
    await toMode(t);
    await tap(t, find.text(l.editEntryTitle));
    await next(t);
    await passRisk(t);
    await waitFor(t, find.text(l.editTitle));
    await tap(t, find.textContaining('Data'));
    await tap(t, find.text(l.editResize));
    await shot(t, '04c-edit-resize');
  }, skip: !haveFont);

  testWidgets('15 确认：双系统', (t) async {
    await pumpApp(t, 'windows-free');
    await toMode(t);
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await next(t);
    await next(t);
    await passRisk(t);
    await waitFor(t, find.text(l.confirmAlongHead));
    await shot(t, '15-confirm-alongside');
  }, skip: !haveFont);

  testWidgets('16–17 进度（写 super 到一半）与 完成', (t) async {
    // speed 1：按录制时的停顿回放，好停在半路截图
    await pumpApp(t, 'windows-free', speed: 1);
    await toMode(t);
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await next(t);
    await next(t);
    await passRisk(t);
    await waitFor(t, find.text(l.confirmHoldIdle));
    final g = await t.startGesture(t.getCenter(find.text(l.confirmHoldIdle)));
    for (var i = 0; i < 25; i++) {
      await t.pump(const Duration(milliseconds: 100));
    }
    await g.up();
    await waitFor(t, find.textContaining('写入 super（'));
    await tap(t, find.text(l.runShowLog));
    await shot(t, '16-run');
    // 按录制节奏回放，整个 apply 有 20 秒上下的停顿（fixture 的 D 行）—— 大步推进假时间
    await waitFor(t, find.text(l.doneTitle), tries: 300, step: const Duration(milliseconds: 250));
    await shot(t, '17-done');
  }, skip: !haveFont);

  testWidgets('18 失败', (t) async {
    await pumpApp(t, 'blank', failApply: true);
    await toMode(t);
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    await next(t);
    await next(t);
    await passRisk(t);
    await hold(t, l.confirmHoldIdle);
    await waitFor(t, find.text(l.failTitle));
    await shot(t, '18-fail');
  }, skip: !haveFont);

  testWidgets('19–20 英文：欢迎 与 整盘确认（查长文案的排版）', (t) async {
    await pumpApp(t, 'factory', language: 'en');
    await waitFor(t, find.text('2.16'));
    await shot(t, '19-welcome-en');
  }, skip: !haveFont);
}
