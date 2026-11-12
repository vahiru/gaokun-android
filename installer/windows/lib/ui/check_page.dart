import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../l10n/app_localizations.dart';
import '../model.dart';
import 'bits.dart';
import 'install_pages.dart';
import 'manage_pages.dart';
import 'messages.dart';

/// 第一页：这台电脑能不能装。每一项都写清楚怎么处理；有一项不过就不让往下走（gk3w 写盘前还会再查一遍）
class CheckPage extends StatelessWidget {
  const CheckPage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    final d = s.disk;
    final (rows, blockedByPreflight) = preflightRows(context);
    var blocked = blockedByPreflight;
    if (!s.checking && s.probe != null) {
      if (d == null) {
        rows.add(CheckRow(state: CheckState.fail, title: l.chkNoSystemDisk));
        blocked = true;
      } else if (s.installed) {
        rows.add(CheckRow(state: CheckState.info, title: l.chkInstalled, detail: l.chkInstalledHow));
      } else if (!s.hasRoom) {
        final biggest = s.freeSpans.fold<int>(0, (m, f) => f.sizeMib > m ? f.sizeMib : m);
        rows.add(CheckRow(
          state: CheckState.fail,
          title: l.chkNoRoom(fmtMib(s.minNeedMib)),
          detail: '${biggest == 0 ? l.chkNoRoomNone : l.chkNoRoomBiggest(fmtMib(biggest))}\n${l.chkNoRoomHow(fmtMib(s.minNeedMib))}',
        ));
        blocked = true;
      } else {
        final r = s.region!;
        rows.add(CheckRow(state: CheckState.ok, title: l.chkRoom(fmtMib(r.sizeMib)), detail: l.chkRoomDetail(d.model, fmtMib(d.sizeMib))));
      }
    }
    return WinStepPage(
      step: WinStep.check,
      title: l.checkTitle,
      subtitle: l.checkSub,
      bottomLeft: Btn(l.btnRecheck, onPressed: s.checking ? null : s.check, kind: BtnKind.secondary, icon: Icons.refresh),
      nextLabel: s.installed ? l.btnManage : l.btnNext,
      onNext: s.checking || blocked || (s.probe == null)
          ? null
          : () => go(context, s.installed ? const ManagePage() : const RegionPage()),
      child: s.checking
          ? Center(
              child: Column(mainAxisSize: MainAxisSize.min, children: [
                const CircularProgressIndicator(),
                const SizedBox(height: 16),
                Text(l.checking, style: context.tt.bodyLarge),
              ]),
            )
          : ListView(children: [
              ...rows,
              if (blocked) ...[const SizedBox(height: 12), NoteBox(text: l.chkBlockedNote, warn: true)],
            ]),
    );
  }
}

/// 首页的检查项（检查失败、机型、Secure Boot、快速启动、BitLocker、电源），以及其中有没有挡住继续的项。
/// 写入本机磁盘的安装介质也要满足这些（media_pages.dart 的 _internalBlockers 按同样的条件判断）。
(List<CheckRow>, bool) preflightRows(BuildContext context) {
  final s = context.session, l = context.l;
  final pf = s.preflight;
  final rows = <CheckRow>[];
  var blocked = false;
  if (!s.checking && s.checkErr != null) {
    rows.add(CheckRow(state: CheckState.fail, title: l.chkFailed, detail: errText(l, s.checkErr!)));
    blocked = true;
  }
  if (!s.checking && pf != null) {
    rows.add(pf.modelOk
        ? CheckRow(state: CheckState.ok, title: l.chkModel, detail: '${pf.manufacturer} ${pf.model}')
        : CheckRow(state: CheckState.fail, title: l.chkModelBad, detail: l.chkModelBadHow('${pf.manufacturer} ${pf.model}')));
    // 已经装了 Android（只会去管理 / 卸载）：Secure Boot 与快速启动不挡 —— gk3w 卸载的闸门本来就不查这两项
    // （sys.c 的 GK3W_GATE_UNINSTALL），显示成提醒
    final inst = s.installed;
    rows.add(switch (pf.secureBoot) {
      'off' => CheckRow(state: CheckState.ok, title: l.chkSecureBootOff),
      'on' => CheckRow(state: inst ? CheckState.warn : CheckState.fail, title: l.chkSecureBootOn, detail: l.chkSecureBootHow),
      _ => CheckRow(state: CheckState.warn, title: l.chkSecureBootUnknown, detail: l.chkSecureBootHow),
    });
    rows.add(pf.fastStartup == 'on'
        ? CheckRow(state: inst ? CheckState.warn : CheckState.fail, title: l.chkFastStartupOn,
            detail: pf.hibernate == 'off' ? l.chkFastStartupHowReg : l.chkFastStartupHow)
        : CheckRow(state: CheckState.ok, title: l.chkFastStartupOff));
    rows.add(pf.bitlocker == 'off'
        ? CheckRow(state: CheckState.ok, title: l.chkBitlockerOff)
        : CheckRow(state: CheckState.warn, title: pf.bitlocker == 'on' ? l.chkBitlockerOn : l.chkBitlockerUnknown, detail: l.chkBitlockerHow));
    rows.add(pf.lowBattery
        ? CheckRow(state: CheckState.fail, title: l.chkBatteryLow, detail: l.chkBatteryLowHow)
        : CheckRow(state: CheckState.ok, title: l.chkPower, detail: _power(l, pf)));
    // 测试模式（对着一块 VHD）：本机的检查项照样显示，但不挡住
    if (!s.testMode) blocked |= !pf.modelOk || pf.lowBattery || (!inst && (pf.secureBoot == 'on' || pf.fastStartup == 'on'));
  }
  return (rows, blocked);
}

String _power(L10n l, Preflight pf) {
  final b = pf.battery == null ? '' : l.chkBatteryPct(pf.battery!);
  return switch (pf.ac) {
    'yes' => l.chkOnAc(b),
    'no' => l.chkOnBattery(b),
    _ => b,
  };
}
