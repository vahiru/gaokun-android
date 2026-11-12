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

/// gk3w shrink-info 的 SHRINK 记录：C 盘最多还能压缩出多少、压缩停在哪个文件（installer/backend/src/win/shrink.c）
class ShrinkInfo {
  ShrinkInfo(this.r);
  final Gk3Record r;

  String get drive => r['drive'];
  int get maxShrinkMib => r.intOf('max_shrink_mib');

  /// C 盘之后紧挨着的未分配空间：压缩出来的空间与它连成一片
  int get adjacentMib => r.intOf('adjacent_mib');

  /// 压缩 C 盘最多能得到的连续未分配空间
  int get totalMib => maxShrinkMib + adjacentMib;
  int get freeMib => r.intOf('free_mib');

  /// 压缩停在已用空间上（C 盘太满），而不是停在事件里的那个文件上
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

/// 挡住压缩的常见文件，各有各的处理方法（界面按它给出步骤）
enum ShrinkBlocker { pagefile, hiberfil, restorePoint, bitlocker, usnJournal, ntfsMeta, memoryDump, other }

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

/// 可以下载安装的一个版本（VARIANT 记录：LiveCD 的 gk3_net_manifest 的格式，另加 date / pre / source）
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

  /// GitHub 上标成预发布的版本
  bool get pre => r.yes('pre');

  /// 版本号里带 alpha 的（v0.7.1-alpha 之类）：早期的测试版，界面提醒不建议使用
  bool get alpha => _alpha.hasMatch(id);
  static final _alpha = RegExp(r'^v?\d+(\.\d+)*-alpha\b', caseSensitive: false);
}

/// 最新的安装器（LiveCD）：gk3w releases 的 LIVECD 记录，只含 LiveCD、不带系统（gaokun3-installer-<版本>-windows.zip）
class LiveCd {
  LiveCd(this.r);
  final Gk3Record r;
  String get id => r['id'];
  String get url => r['url'];
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
