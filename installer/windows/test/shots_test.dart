// 离线出图：每一页渲染成 1280×800 的 PNG（与真机上的逻辑画布同尺寸），做法同 ../live/test/shots_test.dart。
//
//   python3 ../common/tool/fetch-fonts.py   # 第一次：取打包进应用的两个字体
//   flutter test test/shots_test.dart       # → test/shots/*.png
//
// ★ 是"出图"，不是像素比对：每次都重写 PNG。行为由 flow_test.dart 管。没有字体时整组跳过。
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_windows/app.dart';
import 'package:gk3_windows/model.dart';
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
  // 安装介质的入口在 0.1.0 里不注册（app.dart 的 mediaEntryEnabled）；页面照样测
  setUpAll(() => mediaEntryEnabled = true);
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

  testWidgets('安装介质', (t) async {
    await pumpApp(t, 'usb');
    await tap(t, find.text(l.railMedia));
    await shot(t, '41-media-about');
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.mediaUsbTitle('Generic Flash Disk', '29.4 GiB')));
    await shot(t, '42-media-target');
    await tap(t, find.text(l.btnNext));
    await waitFor(t, find.textContaining('v1.0.0-rc.3'));
    await shot(t, '43-media-release');
    await tap(t, find.text(l.btnNext));
    await shot(t, '44-media-confirm-usb');
    await hold(t, l.holdToWrite);
    await waitFor(t, find.byType(FailPage), tries: 400);
    await shot(t, '45-media-fail-no-backend');
  }, skip: !haveFont);

  testWidgets('安装介质：本机磁盘', (t) async {
    await pumpApp(t, 'free');
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.mediaInternalTitle));
    await tap(t, find.text(l.btnNext));
    await shot(t, '46a-media-region');
    await tap(t, find.text(l.btnNext));
    await waitFor(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.btnNext));
    await shot(t, '46-media-confirm-internal');
  }, skip: !haveFont);

  testWidgets('安装介质：几块未分配空间', (t) async {
    await pumpApp(t, 'multi');
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.mediaInternalTitle));
    await tap(t, find.text(l.btnNext));
    await shot(t, '46b-media-region-multi');
  }, skip: !haveFont);

  testWidgets('安装介质：本机不满足条件', (t) async {
    await pumpApp(t, 'blocked');
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await shot(t, '47-media-target-blocked');
  }, skip: !haveFont);

  // 回放数据走不到完成页（gk3w 还没有 media 命令），直接从这一页开始
  testWidgets('安装介质：完成（U盘、本机）', (t) async {
    final (_, s) = await pumpApp(t, 'usb', home: const DonePage(task: WriteTask.media));
    s.mediaWritten = MediaTarget.usb(s.usbDisks.first);
    s.selectMediaTarget(s.mediaWritten!);
    await shot(t, '48-media-done-usb');
    s.mediaWritten = const MediaTarget.internal();
    s.selectMediaTarget(s.mediaWritten!);
    await shot(t, '49-media-done-internal');
  }, skip: !haveFont);

  testWidgets('安装介质：英文', (t) async {
    await pumpApp(t, 'usb', language: 'en');
    await tap(t, find.text(en.railMedia));
    await tap(t, find.text(en.btnNext));
    await shot(t, '50-media-target-en');
  }, skip: !haveFont);

  testWidgets('英文', (t) async {
    await pumpApp(t, 'blocked', language: 'en');
    await shot(t, '31-blocked-en');
  }, skip: !haveFont);

  testWidgets('BitLocker：出厂状态先暂停保护；确认页输入恢复密钥', (t) async {
    final (_, s) = await pumpApp(t, 'bitlocker');
    await shot(t, '31-bitlocker-check');
    await t.enterText(find.byType(TextField), kGoodKey);
    await tapShown(t, find.text(l.blVerifySuspend));
    await see(t, find.text(l.chkBitlockerSuspended));
    await shot(t, '32-bitlocker-suspended');
    expect(s.needBitlockerKey, isFalse);
  }, skip: !haveFont);

  testWidgets('BitLocker：确认页', (t) async {
    await pumpApp(t, 'free', map: {'preflight': bitlockerOn()}, respond: {'bitlocker-key': fakeBitlockerKey});
    await shot(t, '33-bitlocker-check-sb-off');
    for (var i = 0; i < 4; i++) {
      await tap(t, find.text(l.btnNext));
    }
    await see(t, find.text(l.confirmBitlocker));
    await shot(t, '34-bitlocker-confirm');
    await t.enterText(find.byType(TextField), kOtherKey);
    await tapShown(t, find.text(l.blVerify));
    await shot(t, '35-bitlocker-confirm-wrong');
    await t.enterText(find.byType(TextField), kGoodKey);
    await tapShown(t, find.text(l.blVerify));
    await shot(t, '36-bitlocker-confirm-ok');
  }, skip: !haveFont);

  testWidgets('BitLocker：英文确认页', (t) async {
    await pumpApp(t, 'free', language: 'en', map: {'preflight': bitlockerOn()}, respond: {'bitlocker-key': fakeBitlockerKey});
    for (var i = 0; i < 4; i++) {
      await tap(t, find.text(en.btnNext));
    }
    await see(t, find.text(en.confirmBitlocker));
    await t.enterText(find.byType(TextField), kGoodKey);
    await tapShown(t, find.text(en.blVerify));
    await shot(t, '37-bitlocker-confirm-ok-en');
  }, skip: !haveFont);

  testWidgets('空间不够：分析可压缩空间', (t) async {
    await pumpApp(t, 'nospace');
    await tap(t, find.text(l.shrinkAnalyze));
    await waitFor(t, find.text(l.sbPagefile), tries: 400);
    await shot(t, '38-shrink-pagefile');
    await t.drag(find.byType(ListView).first, const Offset(0, -400));
    await shot(t, '39-shrink-pagefile-scrolled');
  }, skip: !haveFont);

  testWidgets('空间不够：压缩停在 MFT 上', (t) async {
    final (_, s) = await pumpApp(t, 'nospace-mft');
    await tap(t, find.text(l.shrinkAnalyze));
    await waitFor(t, find.text(l.mftMoveBtn), tries: 400);
    await t.drag(find.byType(ListView).first, const Offset(0, -400));
    await shot(t, '40-shrink-mft');
    await t.runAsync(s.moveMft);
    await waitFor(t, find.text(l.mftStuck), tries: 400);
    await t.drag(find.byType(ListView).first, const Offset(0, -400));
    await shot(t, '41-shrink-mft-moved');
  }, skip: !haveFont);

  testWidgets('空间够用时分析还能压缩出多少', (t) async {
    await pumpApp(t, 'free');
    await tap(t, find.text(l.shrinkAnalyze));
    await waitFor(t, find.text(l.sbPagefile), tries: 400);
    await shot(t, '42-shrink-more');
  }, skip: !haveFont);
}
