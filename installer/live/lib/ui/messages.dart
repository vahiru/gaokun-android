// 后端的进度代码与 ERR 代码 → 界面上的话（v1.0 计划 INST-10）。
//
// ★ 后端不再给界面送中文：installer-lib.sh 出 `PROGRESS <百分比> <代码> k=v…` 与 `ERR code=<代码> k=v…`，
//   这里按代码查 l10n。代码的全集就是下面两个 switch —— 后端新加一个代码，这里要一起加
//   （installer-lib.sh 文件头写着同一句）。认不出的代码不抛、不编：进度显示"正在进行：<代码>"，
//   报错显示"出错了（代码 …）"，具体的去看日志。
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';

import '../l10n/app_localizations.dart';
import '../model/model.dart';
import '../session.dart' show kRiskUnackedCode;

String _mib(String v) => fmtMib(int.tryParse(v) ?? 0);

/// 一条进度说给人听。旧格式（一句话，旧 fixture / 旧后端）原样显示
String progressText(L10n l, Gk3Progress p) {
  final c = p.code;
  if (c == null) return p.text;
  return switch (c) {
    'check' => l.progCheck,
    'verify-sha' => l.progVerifySha,
    'test-zst' => l.progTestZst,
    'plan' => l.progPlan,
    'write-gpt' => l.progWriteGpt,
    'format' => l.progFormat,
    'write-super' => p.fields.containsKey('total_mib') ? l.progWriteSuperMib(_mib(p['done_mib']), _mib(p['total_mib'])) : l.progWriteSuper,
    'write-boot' => l.progWriteBoot,
    'bootloader' => l.progBootloader,
    'write-rescue' => l.progWriteRescue,
    'done' => l.progDone,
    'gpt-backup' => l.progGptBackup,
    'part-delete' => l.progPartDelete(p['part']),
    _ => l.progOther(c),
  };
}

/// 一条 ERR 说给人听
String errText(L10n l, Gk3Record e) {
  String f(String k) => e[k];
  return switch (e['code']) {
    'usage' => l.errUsage,
    // 界面自己的（Session 的守卫）：没经过风险确认页就要写盘
    kRiskUnackedCode => l.errRiskUnacked,
    'job-start' => l.errJobStart,
    'job-missing' => l.errJobMissing(f('id')),
    'job-lost' => l.errJobLost,
    'logs-no-target' => l.logsNoTarget,
    'logs-not-fat' => l.errLogsNotFat(f('part')),
    'logs-readonly' => l.errLogsReadonly(f('part')),
    'logs-mount' => l.errLogsMount(f('part')),
    'logs-mkdir' => l.errLogsMkdir,
    'verify-remount' => l.errVerifyRemount(f('dev')),
    'verify-mismatch' => l.errVerifyMismatch(f('dev')),
    'cmd-failed' => l.errCmdFailed(f('cmd')),
    'release-no-boot' => l.errReleaseNoBoot,
    'release-no-super' => l.errReleaseNoSuper,
    'sdboot-missing' => l.errSdbootMissing,
    'tool-missing' => l.errToolMissing(f('tool')),
    'sha256-mismatch' => l.errSha256Mismatch(f('file')),
    'zst-corrupt' => l.errZstCorrupt,
    'bootimg-unpack' => l.errBootimgUnpack,
    'esp-missing' => l.errEspMissing(f('esp')),
    'esp-not-fat' => l.errEspNotFat(f('esp'), f('fs')),
    'esp-not-esp-type' => l.errEspNotEspType(f('esp')),
    'esp-full' => l.errEspFull(_mib(f('need_mib')), _mib(f('free_mib'))),
    'esp-ota-room' => l.errEspOtaRoom(_mib(f('left_mib'))),
    'esp-mount' => l.errEspMount(f('esp')),
    // 双系统（见 tools/gk3boot/README.md）
    'default-os-no-windows' => l.errDefaultOsNoWindows,
    'esp-windows-hibernated' => l.errEspWindowsHibernated(f('part')),
    'bitlocker-key-unconfirmed' => l.errBitlockerKeyUnconfirmed,
    'esp-too-small' => l.errEspTooSmall(f('size_mib')),
    'esp-win-reserve' => l.errEspWinReserve(_mib(f('left_mib')), _mib(f('need_mib'))),
    'misc-init' => l.errMiscInit(f('dev')),
    'wipe-medium' => l.errWipeMedium,
    'wipe-mounted' => l.errWipeMounted(f('mounts').trim()),
    'reinstall-busy' => l.errReinstallBusy(f('parts')),
    'node-vanished' => l.errNodeVanished(f('dev')),
    'esp-write' => l.errEspWrite(f('esp')),
    'esp-umount' => l.errEspUmount(f('esp')),
    'super-write' => l.errSuperWrite,
    'super-bad-lp' => l.errSuperBadLp,
    'part-missing' => l.errPartMissing(f('name')),
    'part-node-timeout' => l.errPartNodeTimeout(f('dev')),
    'part-unknown' => l.errPartUnknown(f('part')),
    // 写盘入口在动盘之前拦的两种分区表（见 docs/dev/installer-rust-design.md）
    'disk-unreadable' => l.errDiskUnreadable(f('disk')),
    'mbr-disk' => l.errMbr,
    'gpt-rewrite' => l.errGptRewrite,
    'not-block' => l.errNotBlock(f('dev')),
    'edit-esp' => l.editWhyEsp,
    'edit-mounted' => l.editWhyMedium,
    'kernel-stale' => l.errKernelStale,
    final c => l.errOther(c),
  };
}

/// 一次失败的调用说给人听：ERR → l10n；没有 ERR（旧后端、起不来的进程）→ 中文界面用 `!!` 那句，
/// 英文界面不搬中文过来，只说退出码、让人看日志
String callErrorText(L10n l, CallResult r) {
  final e = r.err;
  if (e != null) return errText(l, e);
  final raw = r.error;
  if (raw != null && l.localeName.startsWith('zh')) return raw;
  return l.errNoCode('${r.exitCode}');
}
