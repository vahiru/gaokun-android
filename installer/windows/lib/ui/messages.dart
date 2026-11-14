import 'package:gk3_common/backend/protocol.dart';

import '../l10n/app_localizations.dart';
import '../model.dart';

/// 写入类的错误码。走到这些步骤时已经以管理员身份打开了磁盘，此时再报 Windows 错误 5（拒绝访问），
/// 多半是安全软件拦下了这一次写入
const _writeCodes = {'disk-write', 'set-layout', 'super-write', 'mkfs', 'misc-init', 'esp-write', 'gpt-backup'};

/// gk3w 的 ERR code=… → 给用户看的一句话（代码定义在 installer/backend/src/win/*.c）。
/// 没收录的代码照原样报出，用户截图时也能对上日志。[media]：写的是 U 盘（gk3w media），打开磁盘、读磁盘信息失败时说的是 U 盘
String errText(L10n l, Gk3Record err, {bool media = false}) {
  final code = err['code'];
  return switch (code) {
    _ when _writeCodes.contains(code) && err['winerr'] == '5' => l.errWriteDenied,
    'disk-open' when media => l.errMediaOpen,
    'disk-sector' || 'disk-size' when media => l.errMediaOpen,
    'disk-write' || 'set-layout' || 'super-write' || 'misc-init' => l.errDiskWrite,
    'disk-verify' || 'esp-verify' || 'super-bad-lp' => l.errDiskVerify,
    'mkfs' => l.errFormat,
    'esp-write' => l.errEspWrite,
    'esp-mount' || 'esp-unreadable' => l.errEspRead,
    'esp-not-fat' => l.errEspNotFat,
    'disk-layout' || 'disk-not-gpt' => l.errPartTable,
    'verify-layout' || 'update-properties' => l.errLayoutVerify,
    'disk-sector' || 'disk-size' => l.errDiskInfo,
    'gpt-backup' => l.errGptBackup,
    'gpt-full' => l.errGptFull,
    'zst-corrupt' || 'sha-mismatch' || 'bootimg-unpack' => l.errRelease,
    'release-no-boot' || 'release-no-super' || 'release-open' => l.errReleaseMissing,
    'not-removable' => l.errMediaNotUsb,
    'media-too-small' => l.errMediaTooSmall(fmtMib(int.tryParse(err['need_mib']) ?? 0)),
    'livecd-open' || 'livecd-iso' || 'livecd-incomplete' || 'livecd-names' => l.errLiveCd,
    'media-in-use' => l.errMediaInUse,
    'media-read' => l.errMediaRead,
    'media-changed' => l.errMediaChanged,
    'media-readonly' => l.errMediaReadOnly,
    'media-is-source' => l.errMediaIsSource,
    'esp-has-android' => l.errInstalled,
    'plan' || 'bad-region' => l.errPlan,
    'layout-changed' => l.errLayoutChanged,
    'fallback-conflict' => l.errFallbackConflict,
    'no-windows-bootmgr' || 'no-windows-boot' || 'default-os-no-windows' => l.errNoBootmgr,
    'wrong-model' => l.errWrongModel,
    'secure-boot-on' => l.errSecureBoot,
    'fast-startup-on' => l.errFastStartup,
    'bitlocker-key-required' => l.errBitlocker,
    'bitlocker-key-format' when err['group'].isNotEmpty => l.errBitlockerGroup(err['group']),
    'bitlocker-key-format' => l.errBitlockerFormat,
    'bitlocker-key-wrong' => l.errBitlockerWrong,
    'bitlocker-no-recovery-key' => l.errBitlockerNoKey,
    'bitlocker-unverifiable' => l.errBitlockerUnverifiable,
    'bitlocker-suspend-failed' => l.errBitlockerSuspend,
    'bitlocker-converting' => l.errBitlockerConverting,
    'shrink-query' => l.errShrinkQuery,
    'mft-move-failed' => l.errMftFailed,
    'mft-open' || 'mft-extents' || 'volume-bitmap' || 'not-ntfs' => l.errMftRead,
    'volume-open' => l.errVolumeOpen,
    'low-battery' => l.errLowBattery,
    'esp-full' || 'esp-full-min' || 'esp-ota-room' || 'esp-win-reserve' || 'esp-too-small' => l.errEspFull,
    'partlabel-conflict' || 'android-unconfirmed' || 'foreign-content' || 'bad-part-type' || 'misc-too-big' || 'disk-read' =>
      l.errPartsUnclear,
    'not-system-disk' || 'not-vhd' || 'disk-open' => l.errDisk,
    'manifest-unavailable' => l.releaseNone,
    'dl-sums' || 'dl-sums-missing' || 'dl-failed' || 'dl-http' => l.errDownload,
    'dl-write' => l.errDownloadWrite,
    'dl-space' => l.errDownloadSpace(fmtMib(err.intOf('need_mib'))),
    'backend-failed' => l.errBackendFailed,
    'internal' || 'uuid' || 'random' || 'nomem' => l.errUnknown,
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
      'lock' => l.prMediaLock,
      'write-media' => l.prMediaWrite,
      'verify-media' => l.prMediaVerify,
      'partition' => l.prMediaPartition,
      'done' => l.prDone,
      'dl-sums' => l.prDlSums,
      'dl-start' || 'dl' when p['pct'].isNotEmpty => l.prDlPct(p['name'], p['pct']),
      'dl-start' || 'dl' => l.prDl(p['name']),
      'dl-verify' => l.prDlVerify(p['name']),
      'dl-done' || 'dl-ready' => l.prDlDone,
      _ => p.text,
    };
