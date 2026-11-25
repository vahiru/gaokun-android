import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';

/// gk3w preflight 的 PREFLIGHT 记录（installer/backend/src/win/sys.c）
class Preflight {
  Preflight(this.r);
  final Gk3Record r;

  String get manufacturer => r['manufacturer'];
  String get model => r['model'];
  bool get modelOk => r.yes('model_ok');

  /// on / off / unknown
  String get secureBoot => r['secure_boot'];

  /// on / off / suspended（已加密，保护已暂停或尚未激活）/ unknown
  String get bitlocker => r['bitlocker'];

  /// 系统盘恢复密钥的 ID（各取前 8 位，用户据此在 Microsoft 账户里找对应的密钥）；null = 没查到
  List<String>? get bitlockerKeyIds {
    final v = r.fields['bitlocker_key_ids'];
    return v == null ? null : [for (final id in v.split(',')) if (id.isNotEmpty) id];
  }

  String get fastStartup => r['fast_startup'];
  String get hibernate => r['hibernate'];

  /// yes / no / unknown
  String get ac => r['ac'];
  int? get battery => r.fields.containsKey('battery') ? r.intOf('battery') : null;

  /// gk3w apply / uninstall 的 --real-disk 检查同一套条件（sys.c 的 gk3w_sys_gate）。界面只是提前告诉用户。
  bool get lowBattery => ac == 'no' && battery != null && battery! < 15;
}

/// 用户输入的 BitLocker 恢复密钥：去掉空格与连字符后是 48 位数字就按 6 位一组、连字符分隔返回，否则 null。
/// 每组的校验与比对由 gk3w 做（installer/backend/src/blkey.c），这里只决定"校验"按钮能不能按
String? normalizeRecoveryKey(String input) {
  final d = input.replaceAll(RegExp(r'[\s-]'), '');
  if (!RegExp(r'^[0-9]{48}$').hasMatch(d)) return null;
  return [for (var i = 0; i < 48; i += 6) d.substring(i, i + 6)].join('-');
}

/// gk3w shrink-info 的 SHRINK 记录：用户本次开机后最近一次在"磁盘管理"里压缩系统盘上的分区时，压缩停在哪个文件上
/// （installer/backend/src/win/shrink.c：解读 Windows 那次分析记下的 259 号事件，不自己分析）
class ShrinkInfo {
  ShrinkInfo(this.r);
  final Gk3Record r;

  /// 本次开机以来还没有这样的记录（用户还没在"磁盘管理"里点过"压缩卷"）
  bool get none => r['event'] == 'none';

  /// 事件的记录号：与上次读到的相同 = 用户处理之后还没重新"压缩卷"
  int get record => r.intOf('record');

  /// 事件的本地时间（HH:MM），读不出时为"--:--"
  String get time => r['time'].isEmpty ? '--:--' : r['time'];

  /// "D:" 这样的盘符
  String get drive => r['drive'];

  /// 只有字母（"D"），界面上写"D盘"、"D:"用
  String get letter => drive.isEmpty ? 'C' : drive.substring(0, 1);

  /// 压缩停在已用空间上（分区太满），而不是停在事件里的那个文件上
  bool get limitedBySpace => r['limit'] == 'space';

  /// 最后一个不可移动的文件（卷内路径，去掉了 ::$DATA 之类的流名）；没找到为 null
  String? get file {
    final f = r['file'];
    if (f.isEmpty) return null;
    final colon = f.indexOf(':');
    return colon < 0 ? f : f.substring(0, colon);
  }

  ShrinkBlocker? get blocker => file == null ? null : classifyShrinkBlocker(file!);
}

/// gk3w mft-move 的 MFTMOVE 记录说 MFT 已经只剩开头必须留在原处的部分：这个分区不能再因移动 MFT 压缩得更多
bool mftStuck(Gk3Record r) => r.intOf('unplaced_mib') == 0 && r.intOf('end_after_mib') <= r.intOf('fixed_end_mib');

/// 挡住压缩的常见文件，各有各的处理方法（界面按它给出步骤）
enum ShrinkBlocker { pagefile, hiberfil, restorePoint, bitlocker, usnJournal, mft, ntfsMeta, memoryDump, other }

/// 按卷内路径（不分大小写）归类
ShrinkBlocker classifyShrinkBlocker(String path) {
  final p = path.toLowerCase();
  if (p == r'\pagefile.sys' || p == r'\swapfile.sys') return ShrinkBlocker.pagefile;
  if (p == r'\hiberfil.sys') return ShrinkBlocker.hiberfil;
  const svi = r'\system volume information\';
  if (p.startsWith('${svi}fve2.')) return ShrinkBlocker.bitlocker;
  // 卷影副本（还原点）的差异文件：{GUID}{3808876b-c176-4e48-b7ae-04046e6cc752}
  if (p.startsWith(svi) && p.contains('{3808876b-c176-4e48-b7ae-04046e6cc752}')) return ShrinkBlocker.restorePoint;
  if (p.startsWith(r'\$extend\$usnjrnl')) return ShrinkBlocker.usnJournal;
  // 主文件表（$Mft 的 $DATA 或 $BITMAP，流名已去掉）：除了开头的系统记录，都能挪到前部（gk3w mft-move）
  if (p == r'\$mft') return ShrinkBlocker.mft;
  // NTFS 的元数据：根目录下以 $ 开头的单个文件（$Mft、$LogFile、$Bitmap …）或 $Extend 里的；
  // \$Recycle.Bin\…、\$WINDOWS.~BT\… 这类以 $ 开头的普通目录不算
  if (RegExp(r'^\\\$[^\\]+$').hasMatch(p) || p.startsWith(r'\$extend\')) return ShrinkBlocker.ntfsMeta;
  if (p == r'\memory.dmp' ||
      p == r'\windows\memory.dmp' ||
      p.startsWith(r'\windows\minidump\') ||
      p.startsWith(r'\windows\livekernelreports\')) {
    return ShrinkBlocker.memoryDump;
  }
  return ShrinkBlocker.other;
}

/// gk3w probe 的 DISK 记录
class WinDisk {
  WinDisk(this.r);
  final Gk3Record r;
  String get path => r['path'];
  int get num => r.intOf('num', -1);
  int get sizeMib => r.intOf('size_mib');
  int get sector => r.intOf('sector', 512);
  String get model => r['model'];
  bool get system => r.yes('system');

  /// nvme / usb / sd / vhd …（probe.c 的 bus_name）
  String get bus => r['bus'];
  bool get removable => r['removable'] == '1';

  /// 可以写入安装介质的 U 盘：USB 总线或可移动介质，且不是系统盘
  bool get usbStick => !system && (bus == 'usb' || removable);
}

/// gk3w probe 的 PART 记录
class WinPart {
  WinPart(this.r);
  final Gk3Record r;
  String get path => r['path'];
  String get disk => r['disk'];
  int get start => r.intOf('start');
  int get end => r.intOf('end');
  int get sizeMib => r.intOf('size_mib');
  String get name => r['name'];
  String get type => r['type'];
  String get fs => r['fs'];
  String get fslabel => r['fslabel'];
  String get letter => r['letter'];
  String get os => r['os'];
}

/// gk3w probe 的 FREE 记录（≥ 16 MiB 的空闲区，逻辑扇区）
class FreeSpan {
  FreeSpan(this.r);
  final Gk3Record r;
  String get disk => r['disk'];
  int get start => r.intOf('start');
  int get end => r.intOf('end');
  int get sizeMib => r.intOf('size_mib');
}

/// 安装器会建、卸载会删的分区名（与 gk3w uninstall 的 OUR_NAMES 相同）
const kAndroidNames = {'misc', 'metadata', 'boot_a', 'boot_b', 'super', 'gk3rescue', 'userdata'};

/// XBOOTLDR 分区类型（gk3w probe 的 PART type=）：本机磁盘上的安装器分区 GK3LIVE 用它（installer/backend/src/win/win.h）
const kXbootldrType = 'BC13C2FF-59E6-4262-A352-B275FD6F7172';

class Probe {
  Probe(CallResult res)
      : disks = [for (final r in res.ofType('DISK')) WinDisk(r)],
        parts = [for (final r in res.ofType('PART')) WinPart(r)],
        free = [for (final r in res.ofType('FREE')) FreeSpan(r)];

  final List<WinDisk> disks;
  final List<WinPart> parts;
  final List<FreeSpan> free;

  /// Windows 所在的那块盘：安装与卸载只对它（gk3w 的 --real-disk 也只认它）
  WinDisk? get systemDisk {
    for (final d in disks) {
      if (d.system) return d;
    }
    return null;
  }

  List<WinPart> partsOf(WinDisk d) => [for (final p in parts) if (p.disk == d.path) p]..sort((a, b) => a.start.compareTo(b.start));
  List<FreeSpan> freeOf(WinDisk d) => [for (final f in free) if (f.disk == d.path) f]..sort((a, b) => a.start.compareTo(b.start));
  List<WinPart> androidOn(WinDisk d) => [for (final p in partsOf(d)) if (kAndroidNames.contains(p.name)) p];
}

/// gk3w plan 的结果：成功时 PLAN 行与 PLANSUM，失败时 PLANERR（plan.c 的 gk3w_plan_print）
class Plan {
  Plan(CallResult res)
      : parts = [for (final r in res.ofType('PLAN')) r],
        _sum = res.first('PLANSUM'),
        _err = res.first('PLANERR');

  final List<Gk3Record> parts;
  final Gk3Record? _sum, _err;

  bool get ok => _sum != null && _err == null;
  int get userdataMib => _sum?.intOf('userdata_mib') ?? 0;

  /// 不够时：至少要多大（MiB）
  int get needMib => _err?.intOf('need_mib') ?? 0;
}

/// 可以下载安装的一个版本（VARIANT 记录，由 backend/net.dart 从发布清单整理出来，带 date / pre / source）
class Variant {
  Variant(this.r);
  final Gk3Record r;
  String get id => r['id'];
  String get name => r['name'];

  /// 安装文件所在的目录（boot.img、super.img.zst、install-artifacts.sha256 都在它下面）
  String get base => r['base'];
  int get sizeMib => r.intOf('size_mib');
  String get date => r['date'];
  bool get latest => r.yes('latest');
}

/// 最新的 Live 安装器：gk3w releases 的 LIVECD 记录，不带系统文件的 gaokun3-live-<标签版本>.iso 与核对它的 SHA256SUMS
class LiveCd {
  LiveCd(this.r);
  final Gk3Record r;

  /// 标签版本：标签去掉 live-installer-v，形如 0.2.0-for-v1.0.0-rc.5（每个 ROM 发布后各出一版，见 .github/workflows/installer-live.yml）
  String get id => r['id'];

  /// 界面上显示的安装器版本（0.2.0）：一并写入的系统文件是最新版本或本地文件，"for"后面的 ROM 只决定 Live 系统用的内核
  String get version => id.split('-for-').first;
  String get url => r['url'];
  String get sums => r['sums'];
  int get sizeMib => r.intOf('size_mib');
}

/// 安装介质写到哪：一个 U 盘（整个清除），或系统盘的未分配空间（[disk] 为 null）
class MediaTarget {
  const MediaTarget.usb(WinDisk this.disk);
  const MediaTarget.internal() : disk = null;
  final WinDisk? disk;
  bool get usb => disk != null;

  bool same(MediaTarget o) => usb ? identical(disk, o.disk) : !o.usb;
}

/// 发布文件从哪来
enum ReleaseSource { github, r2, local }

String fmtMib(int mib) {
  if (mib < 1024) return '$mib MiB';
  final g = mib / 1024;
  if (g >= 1024) return '${(g / 1024).toStringAsFixed(1)} TiB';
  final s = g >= 100 ? g.toStringAsFixed(0) : g.toStringAsFixed(1);
  return '${s.endsWith('.0') ? s.substring(0, s.length - 2) : s} GiB';
}
