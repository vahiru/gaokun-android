import 'package:gk3_common/backend/protocol.dart';

import '../l10n/app_localizations.dart';

/// gk3w 的 ERR code=… → 给用户看的一句话（代码定义在 installer/backend/src/win/*.c）。
/// 没收录的代码照原样报出，用户截图时也能对上日志。
String errText(L10n l, Gk3Record err) {
  final code = err['code'];
  return switch (code) {
    'zst-corrupt' || 'sha-mismatch' || 'release-no-boot' || 'release-no-super' || 'bootimg-unpack' => l.errRelease,
    'esp-has-android' => l.errInstalled,
    'plan' || 'bad-region' => l.errPlan,
    'layout-changed' => l.errLayoutChanged,
    'fallback-conflict' => l.errFallbackConflict,
    'no-windows-bootmgr' || 'no-windows-boot' => l.errNoBootmgr,
    'wrong-model' => l.errWrongModel,
    'secure-boot-on' => l.errSecureBoot,
    'fast-startup-on' => l.errFastStartup,
    'bitlocker-key-unconfirmed' => l.errBitlocker,
    'low-battery' => l.errLowBattery,
    'esp-full' || 'esp-full-min' || 'esp-ota-room' || 'esp-win-reserve' || 'esp-too-small' => l.errEspFull,
    'partlabel-conflict' || 'android-unconfirmed' || 'foreign-content' || 'bad-part-type' => l.errPartsUnclear,
    'not-system-disk' || 'not-vhd' || 'disk-open' => l.errDisk,
    'manifest-unavailable' => l.releaseNone,
    'dl-sums' || 'dl-sums-missing' || 'dl-failed' || 'dl-http' => l.errDownload,
    'dl-write' => l.errDownloadWrite,
    'backend-missing' || 'tool-missing' => l.errBackendMissing,
    'hash-failed' => l.errRelease,
    'not-admin' => l.errNotAdmin,
    _ => l.errOther(code),
  };
}

/// 进度代码（gk3w apply 的 PROGRESS、下载的 dl-*）→ 一句话
String progressText(L10n l, Gk3Progress p) => switch (p.code) {
      'check' || 'verify-sha' || 'test-zst' => l.prVerify,
      'plan' => l.prPlan,
      'write-gpt' => l.prGpt,
      'format' => l.prFormat,
      'write-super' when p['total_mib'].isNotEmpty => l.prSuperMib(p['done_mib'], p['total_mib']),
      'write-super' => l.prSuper,
      'write-boot' => l.prBoot,
      'bootloader' => l.prBootloader,
      'done' => l.prDone,
      'dl-sums' => l.prDlSums,
      'dl-start' || 'dl' when p['pct'].isNotEmpty => l.prDlPct(p['name'], p['pct']),
      'dl-start' || 'dl' => l.prDl(p['name']),
      'dl-verify' => l.prDlVerify(p['name']),
      'dl-done' || 'dl-ready' => l.prDlDone,
      _ => p.text,
    };
