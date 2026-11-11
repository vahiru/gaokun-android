// 后端的进度代码与 ERR 代码 → 界面上的话（v1.0 计划 INST-10）。
//
// ★ 后端不再给界面送中文：installer-lib.sh 出 `PROGRESS <百分比> <代码> k=v…` 与 `ERR code=<代码> k=v…`，
//   这里按代码查 l10n。代码的全集就是下面两个 switch —— 后端新加一个代码，这里要一起加
//   （installer-lib.sh 文件头写着同一句）。认不出的代码不抛、不编：进度显示"正在进行：<代码>"，
//   报错显示"出错了（代码 …）"，具体的去看日志。
import '../backend/backend.dart';
import '../backend/protocol.dart';
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
    'shrink-trial' => l.progShrinkTrial,
    'shrink-ntfs' => l.progShrinkNtfs,
    'fsck' => l.progFsck,
    'shrink-ext' => l.progShrinkExt,
    'gpt-edit' => l.progGptEdit,
    'verify' => l.progVerify,
    'wifi-assoc' => p['ssid'].isEmpty ? l.progWifiAssocAny : l.progWifiAssoc(p['ssid']),
    'wifi-dhcp' => l.progWifiDhcp,
    'wifi-ok' => l.progWifiOk,
    'dl-sums' => l.progDlSums,
    'dl-start' => l.progDlStart(p['name']),
    'dl' => l.progDl(p['name'], p['pct']),
    'dl-verify' => l.progDlVerify(p['name']),
    'dl-done' => l.progDlDone(p['name']),
    'dl-ready' => l.progDlReady,
    'part-delete' => l.progPartDelete(p['part']),
    'part-format' => p['part'].isEmpty ? l.progFormatAs(p['fs']) : l.progPartFormat(p['part'], p['fs']),
    'part-create' => l.progPartCreate(_mib(p['mib'])),
    'grow-entry' => l.progGrowEntry,
    'grow-fs' => l.progGrowFs,
    _ => l.progOther(c),
  };
}

/// curl 的速度列（"2048k"、"1.5M"，1024 进制，curl 的单位）→ 字节/秒；认不出 → null
double? parseCurlSpeed(String s) {
  final m = RegExp(r'^([0-9.]+)([kMGTP]?)$').firstMatch(s);
  if (m == null) return null;
  final v = double.tryParse(m.group(1)!);
  if (v == null) return null;
  const mul = {'': 1.0, 'k': 1024.0, 'M': 1048576.0, 'G': 1073741824.0, 'T': 1099511627776.0, 'P': 1125899906842624.0};
  return v * mul[m.group(2)]!;
}

/// curl 的剩余时间列（"0:06:59"）→ Duration；认不出 → null
Duration? parseCurlLeft(String s) {
  final m = RegExp(r'^(\d+):(\d\d):(\d\d)$').firstMatch(s);
  if (m == null) return null;
  return Duration(hours: int.parse(m.group(1)!), minutes: int.parse(m.group(2)!), seconds: int.parse(m.group(3)!));
}

String fmtSpeed(double bps) {
  if (bps < 1024 * 1024) return '${(bps / 1024).toStringAsFixed(0)} KB';
  return '${(bps / 1048576).toStringAsFixed(1)} MB';
}

String fmtEta(L10n l, Duration d) {
  if (d.inSeconds < 60) return l.etaUnderMinute;
  final mins = (d.inSeconds / 60).ceil();
  if (mins < 60) return l.etaMinutes('$mins');
  return l.etaHours('${mins ~/ 60}', '${mins % 60}');
}

/// 下载进度的速度 / 剩余时间一行（GUI-5）；这条进度不是下载、或者 curl 没给速度 → null
String? downloadRate(L10n l, Gk3Progress p) {
  if (p.code != 'dl') return null;
  final sp = parseCurlSpeed(p['speed']);
  if (sp == null) return null;
  final left = parseCurlLeft(p['left']);
  return left == null ? l.runSpeed(fmtSpeed(sp)) : l.runSpeedLeft(fmtSpeed(sp), fmtEta(l, left));
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
    'ntfs-hibernated' => l.shrinkWhyHibernated,
    'ntfs-dirty' => l.shrinkWhyDirty,
    'ntfs-mount' => l.errNtfsMount(f('part')),
    'shrink-too-small' => l.errShrinkTooSmall(_mib(f('target_mib')), _mib(f('floor_mib'))),
    'shrink-not-smaller' => l.errShrinkNotSmaller,
    'part-unknown' => l.errPartUnknown(f('part')),
    'gpt-read' => l.errGptRead(f('part')),
    // 写盘入口在动盘之前拦的两种分区表（见 docs/dev/installer-rust-design.md）
    'disk-unreadable' => l.errDiskUnreadable(f('disk')),
    'mbr-disk' => l.errMbr,
    'ntfs-dryrun' => l.errNtfsDryrun,
    'ntfs-shrink' => l.errNtfsShrink,
    'fsck' => l.errFsck(f('rc')),
    'resize2fs' => l.errResize2fs,
    'fs-unsupported' => l.errFsUnsupported(f('fs')),
    'gpt-rewrite' => l.errGptRewrite,
    'partuuid-changed' => l.errPartuuidChanged,
    'shrink-size-mismatch' => l.errShrinkSizeMismatch(_mib(f('have_mib')), _mib(f('want_mib'))),
    'wifi-no-if' => l.netNoAdapter,
    'wpa-start' => l.errWpaStart,
    'ssid-too-long' => l.netSsidTooLong,
    'psk-length' => l.netPasswordLength,
    'wpa-reject' => l.errWpaReject,
    'wifi-assoc' => l.netFailed,
    'wifi-dhcp' => l.netNoIp,
    'manifest-unavailable' => l.errManifestUnavailable,
    'dl-incomplete' => l.errDlIncomplete(f('name'), _mib(f('kept_mib'))),
    'dl-failed' => l.errDlFailed(f('name'), f('rc')),
    'dl-sha256' => l.errDlSha256(f('name')),
    'dl-sums' => l.errDlSums,
    'dl-sums-missing' => l.errDlSumsMissing(f('name')),
    'not-block' => l.errNotBlock(f('dev')),
    'edit-esp' => l.editWhyEsp,
    'edit-mounted' => l.editWhyMedium,
    'part-attr' => l.errPartAttr,
    'kernel-stale' => l.errKernelStale,
    'mkfs' => l.errMkfs(f('part')),
    'part-type' => l.errPartType,
    'create-overlap' => l.errCreateOverlap,
    'create-failed' => l.errCreateFailed,
    'create-node' => l.errCreateNode,
    'grow-same' => l.errGrowSame,
    'grow-no-room' => l.errGrowNoRoom(_mib(f('max_mib'))),
    'ntfs-check' => l.errNtfsCheck,
    'grow-kernel-stale' => l.errGrowKernelStale,
    'grow-ntfs' => l.errGrowNtfs,
    'grow-resize2fs' => l.errGrowResize2fs,
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
