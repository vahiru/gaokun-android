// 离线出图：每一页渲染成 1280×800 的 PNG（与真机上的逻辑画布同尺寸），做法同 ../live/test/shots_test.dart。
//
//   python3 ../common/tool/fetch-fonts.py   # 第一次：取打包进应用的两个字体
//   flutter test test/shots_test.dart       # → test/shots/*.png
//
// ★ 是"出图"，不是像素比对：每次都重写 PNG。行为由 flow_test.dart 管。没有字体时整组跳过。
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_windows/app.dart';
import 'package:gk3_windows/session.dart';
import 'package:gk3_windows/ui/run_pages.dart';

import 'helpers.dart';

const _fonts = {
  kFontLatin: '../common/assets/fonts/Roboto-VF.ttf',
  kFontCjk: '../common/assets/fonts/NotoSansSC-VF.ttf',
  'monospace': '../common/assets/fonts/NotoSansSC-VF.ttf',
};

Future<void> shot(WidgetTester t, String name) async {
  await settle(t, 6);
  await expectLater(find.byType(WinInstallerApp), matchesGoldenFile('shots/$name.png'));
}

void main() {
  final haveFont = _fonts.values.every((f) => File(f).existsSync());
  setUpAll(() async {
    if (!haveFont) return;
    autoUpdateGoldenFiles = true;
    for (final MapEntry(key: fam, value: f) in _fonts.entries) {
      await (FontLoader(fam)..addFont(Future.value(ByteData.sublistView(File(f).readAsBytesSync())))).load();
    }
    final icons = File('${Platform.environment['FLUTTER_ROOT']}/bin/cache/artifacts/material_fonts/MaterialIcons-Regular.otf');
    if (icons.existsSync()) {
      await (FontLoader('MaterialIcons')..addFont(Future.value(ByteData.sublistView(icons.readAsBytesSync())))).load();
    }
  });

  testWidgets('安装流程', (t) async {
    await pumpApp(t, 'free');
    await shot(t, '01-check');
    await tap(t, find.text(l.btnNext));
    await shot(t, '02-region');
    await tap(t, find.text(l.btnNext));
    await waitFor(t, find.textContaining('v1.0.0-rc.3'));
    await shot(t, '03-release');
    await tap(t, find.text(l.btnNext));
    await shot(t, '04-options');
    await tap(t, find.text(l.btnNext));
    await shot(t, '05-confirm');
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    await shot(t, '06-done');
  }, skip: !haveFont);

  testWidgets('几块空闲区、本地文件、R2', (t) async {
    await pumpApp(t, 'multi');
    await tap(t, find.text(l.btnNext));
    await shot(t, '07-region-multi');
    await tap(t, find.text(l.btnNext));
    await waitFor(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.srcR2));
    await waitFor(t, find.text('crDroid 13.0'));
    await shot(t, '08-release-r2');
    await tap(t, find.text(l.srcLocal));
    await shot(t, '09-release-local');
  }, skip: !haveFont);

  testWidgets('已装：管理与卸载', (t) async {
    await pumpApp(t, 'installed');
    await shot(t, '11-check-installed');
    await tap(t, find.text(l.btnManage));
    await shot(t, '12-manage');
    await tap(t, find.text(l.manageUninstall));
    await shot(t, '13-uninstall-confirm');
    await hold(t, l.holdToUninstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    await shot(t, '14-uninstall-done');
  }, skip: !haveFont);

  testWidgets('检查不过、没有空间、发布文件损坏', (t) async {
    await pumpApp(t, 'blocked');
    await shot(t, '21-blocked');
  }, skip: !haveFont);

  testWidgets('没有空间', (t) async {
    await pumpApp(t, 'nospace');
    await shot(t, '22-nospace');
  }, skip: !haveFont);

  testWidgets('发布文件损坏', (t) async {
    await pumpApp(t, 'bad-release');
    for (var i = 0; i < 4; i++) {
      await tap(t, find.text(l.btnNext));
      await waitFor(t, find.textContaining('v1.0.0-rc.3'));
    }
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(FailPage), tries: 400);
    await shot(t, '23-fail');
  }, skip: !haveFont);

  testWidgets('整盘安装', (t) async {
    await pumpApp(t, 'free');
    await tap(t, find.text(l.railWipe));
    await shot(t, '41-wipe-about');
    await tap(t, find.text(l.btnNext));
    await shot(t, '42-wipe-check');
    await tap(t, find.text(l.btnNext));
    await waitFor(t, find.textContaining('v1.0.0-rc.3'));
    await shot(t, '43-wipe-release');
    await tap(t, find.text(l.btnNext));
    await shot(t, '44-wipe-confirm');
    await hold(t, l.holdToPrepare);
    await waitFor(t, find.byType(FailPage), tries: 400);
    await shot(t, '45-wipe-fail-no-backend');
  }, skip: !haveFont);

  testWidgets('整盘安装：没有空间', (t) async {
    await pumpApp(t, 'nospace');
    await tap(t, find.text(l.railWipe));
    await tap(t, find.text(l.btnNext));
    await shot(t, '46-wipe-nospace');
  }, skip: !haveFont);

  testWidgets('整盘安装：准备完成（回放数据走不到这一页，直接从这一页开始）', (t) async {
    await pumpApp(t, 'free', home: const DonePage(task: WriteTask.prepare));
    await shot(t, '47-wipe-done');
  }, skip: !haveFont);

  testWidgets('整盘安装：英文', (t) async {
    await pumpApp(t, 'free', language: 'en');
    await tap(t, find.text(en.railWipe));
    await tap(t, find.text(en.btnNext));
    await tap(t, find.text(en.btnNext));
    await waitFor(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(en.btnNext));
    await shot(t, '48-wipe-confirm-en');
  }, skip: !haveFont);

  testWidgets('英文', (t) async {
    await pumpApp(t, 'blocked', language: 'en');
    await shot(t, '31-blocked-en');
  }, skip: !haveFont);
}
