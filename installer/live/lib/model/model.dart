// 把后端的行记录解读成界面用的类型。
//
// ⚠️ 这里只做【解读】，不做任何分区计算：空间够不够、/data 能给多大、放不放得下，
//    一律问 gk3l plan（backend.dart 顶上那条规矩）。记录的格式见 installer/backend/src/linux/ 各命令的文件头。

import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';

const espTypeGuid = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B';

/// 安装器会建的分区名（与 gk3l 的 OUR_NAMES 相同）：盘上已有同名的，双系统就不能再建一套
const kAndroidNames = {'misc', 'metadata', 'boot_a', 'boot_b', 'super', 'userdata', 'gk3rescue'};

class Part {
  Part(Gk3Record r)
      : path = r['path'],
        num = r.intOf('num'),
        start = r.intOf('start'),
        end = r.intOf('end'),
        sizeMib = r.intOf('size_mib'),
        sizeKib = r.intOf('size_kib'),
        type = r['type'].toUpperCase(),
        name = r['name'],
        fs = r['fs'],
        fslabel = r['fslabel'],
        os = r['os'],
        medium = r.yes('medium');
  final String path, type, name, fs, fslabel, os;
  final int num, start, end, sizeMib, sizeKib;

  /// 安装器就是从这个分区跑起来的（gk3l probe 的 PART medium=yes）—— 删除分区页不让删它
  final bool medium;

  bool get isEsp => type == espTypeGuid || name == 'esp';

  /// 给人看的名字：卷标比 PARTLABEL 有用（Windows 的 PARTLABEL 全是 "Basic data partition"）
  String get label => fslabel.isNotEmpty ? fslabel : (name.isNotEmpty ? name : path.split('/').last);
}

class FreeRegion {
  FreeRegion(Gk3Record r)
      : disk = r['disk'],
        start = r.intOf('start'),
        end = r.intOf('end'),
        sizeMib = r.intOf('size_mib');
  final String disk;
  final int start, end, sizeMib;
}

class Disk {
  Disk(Gk3Record r)
      : path = r['path'],
        sizeMib = r.intOf('size_mib'),
        model = r['model'] == '?' ? '' : r['model'],
        removable = r['removable'] == '1',
        bus = r['bus'],
        sector = r.intOf('sector', 512),
        medium = r.yes('medium'),
        table = r['table'];
  final String path, model, bus;
  final int sizeMib;

  /// 逻辑扇区的字节数（512 或 4096）：start / end 的单位，gk3l plan 要它
  final int sector;
  final bool removable, medium;

  /// gk3l probe 的 table=：gpt | damaged | mbr | raw | unreadable（含义见 installer/backend/src/linux/probe.c 文件头）
  final String table;
  final parts = <Part>[];
  final free = <FreeRegion>[];

  bool get external => removable || bus == 'usb';

  /// 分区表读不出来：后端不列它的分区与空闲区，界面不给选
  bool get unreadable => table == 'unreadable';

  /// MBR 盘：后端只写 GPT，非整盘的写盘入口一律 ERR mbr-disk —— 只能整盘清空
  bool get mbr => table == 'mbr';

  /// 有 GPT 的痕迹，主表却不能用：同样只能整盘清空
  bool get damaged => table == 'damaged';

  /// 没有分区表（全新的盘、整盘格式化的 U 盘）
  bool get raw => table == 'raw';
  Part? get esp {
    for (final p in parts) {
      if (p.isEsp) return p;
    }
    return null;
  }

  FreeRegion? get largestFree {
    FreeRegion? best;
    for (final f in free) {
      if (best == null || f.sizeMib > best.sizeMib) best = f;
    }
    return best;
  }

  /// gk3l probe 的输出：DISK 行后面跟着它的 PART / FREE 行（FREE 带 disk= 可以直接认）
  static List<Disk> fromProbe(CallResult r) {
    final disks = <Disk>[];
    for (final rec in r.records) {
      switch (rec.type) {
        case 'DISK':
          disks.add(Disk(rec));
        case 'PART':
          if (disks.isNotEmpty) disks.last.parts.add(Part(rec));
        case 'FREE':
          for (final d in disks) {
            if (d.path == rec['disk']) d.free.add(FreeRegion(rec));
          }
      }
    }
    for (final d in disks) {
      d.parts.sort((a, b) => a.start.compareTo(b.start));
    }
    return disks;
  }
}

enum CheckState { ok, fail, unknown }

/// 欢迎页的一项检查：gk3l preflight 的 PREFLIGHT 记录拆成两项（机型、电源）
class Check {
  const Check(this.id, this.state, this.value, {this.min = '', this.ac = false});
  final String id, value;

  /// 电源那一项的门槛（百分比）与"接着电源"
  final String min;
  final bool ac;
  final CheckState state;

  /// PREFLIGHT model= model_ok=yes|no|unknown ac=yes|no [battery=] battery_min=。
  /// 电量低的判据与 gk3l 写盘之前的把关相同（installer/backend/src/policy.c）：没接电源、读得到电量、低于门槛
  static List<Check> fromPreflight(Gk3Record r) {
    final model = Check(
      'model',
      switch (r['model_ok']) { 'yes' => CheckState.ok, 'no' => CheckState.fail, _ => CheckState.unknown },
      r['model'],
    );
    final ac = r.yes('ac'), has = r.fields.containsKey('battery'), min = r['battery_min'];
    final low = !ac && has && r.intOf('battery') < r.intOf('battery_min', 15);
    final power = Check(
      'power',
      low ? CheckState.fail : (has ? CheckState.ok : CheckState.unknown),
      has ? r['battery'] : '',
      min: min,
      ac: ac,
    );
    return [model, power];
  }
}

class EspInfo {
  EspInfo(Gk3Record r)
      : part = r['part'],
        sizeMib = r.intOf('size_mib'),
        freeMib = r.intOf('free_mib'),
        needMib = r.intOf('need_mib', 150),
        windows = r.yes('windows'),
        ours = r.yes('gaokun3'),
        mountable = r.yes('mountable'),
        small = r.yes('small'),
        bootaa64 = r['bootaa64'],
        bitlocker = r.yes('bitlocker'),
        hibernated = r['hibernated'],
        loaderDefault = r['loader_default'];
  final String part;
  final int sizeMib, freeMib, needMib;
  final bool windows, ours, mountable;

  /// 2026-10-05（S10 / S15）起后端多报的几项；旧 fixture / 旧后端没有 ⇒ 空串 / false（照旧的行为）
  /// small：分区比 GK3_ESP_MIN_SIZE_MIB 小（100 MiB 那种），双系统明确不装（U17）
  final bool small, bitlocker;

  /// none / sdboot / other：EFI/BOOT/BOOTAA64.EFI 与 live 自带的 systemd-boot 比
  final String bootaa64;

  /// yes / no / unknown：同一块盘上的 Windows 卷在不在休眠（U18）
  final String hibernated;

  /// EFI 变量 LoaderEntryDefault 的值；none = 没设
  final String loaderDefault;
  bool get roomy => mountable && freeMib >= needMib;

  /// Windows 卷在休眠：不许写共用的 ESP（双系统、重新安装都不行，U18）
  bool get winHibernated => windows && hibernated == 'yes';

  /// 安装会换掉 BOOTAA64、而 Windows 卷是 BitLocker：要用户确认拿到了恢复密钥（U16）
  bool get needsBitlockerKey => windows && bitlocker && bootaa64 != 'sdboot';
}

class PlanPart {
  PlanPart(Gk3Record r)
      : name = r['name'],
        start = r.intOf('start'),
        end = r.intOf('end'),
        sizeMib = r.intOf('size_mib');
  final String name;
  final int start, end, sizeMib;
}

/// 重新安装时复用的一个现有分区（gk3l plan --mode reinstall 的 PLAN op=reuse）
class PlanReuse {
  PlanReuse(Gk3Record r)
      : name = r['name'],
        path = r['path'],
        sizeMib = r.intOf('size_mib'),
        sizeKib = r.intOf('size_kib', r.intOf('size_mib') * 1024),
        action = r['action'];
  final String name, path;
  final int sizeMib, sizeKib;

  /// write = 写入新系统，format = 格式化（数据清掉），keep = 原样保留
  final String action;
}

/// gk3l plan 的结果：要么 parts + summary，要么 error
class Plan {
  Plan(CallResult r)
      : parts = [for (final p in r.ofType('PLAN').where((p) => p['op'] == 'mkpart')) PlanPart(p)],
        reuse = [for (final p in r.ofType('PLAN').where((p) => p['op'] == 'reuse')) PlanReuse(p)],
        wipe = r.ofType('PLAN').any((p) => p['op'] == 'wipe'),
        summary = r.first('PLANSUM'),
        error = r.first('PLANERR') ?? (r.ok ? null : Gk3Record('PLANERR', {'msg': r.error ?? 'exit-${r.exitCode}'}));
  final List<PlanPart> parts;
  final List<PlanReuse> reuse;
  final bool wipe;
  final Gk3Record? summary, error;

  bool get ok => error == null && summary != null;
  int get userdataMib => summary?.intOf('userdata_mib') ?? 0;
  int get availMib => summary?.intOf('avail_mib') ?? 0;
  int get fixedMib => summary?.intOf('fixed_mib') ?? 0;
  int get totalNewMib => parts.fold(0, (a, p) => a + p.sizeMib);
  bool get keepData => summary?.yes('keep_data') ?? false;
}

/// MiB → 给人看的大小：100 GiB 以下保留一位小数（"21.2 GiB"），整数不显示 ".0"
/// 不到 1 MiB 的按 KiB 报（本机的 misc 是 1007 KiB —— 报成"0 MiB"看着像个空分区）
String fmtKib(int kib) => kib < 1024 ? '$kib KiB' : fmtMib(kib ~/ 1024);

String fmtMib(int mib) {
  if (mib < 1024) return '$mib MiB';
  final g = mib / 1024;
  if (g >= 1024) return '${(g / 1024).toStringAsFixed(1)} TiB';
  final s = g >= 100 ? g.toStringAsFixed(0) : g.toStringAsFixed(1);
  return '${s.endsWith('.0') ? s.substring(0, s.length - 2) : s} GiB';
}

/// gk3l release-info 的 RELEASE 记录（安装介质上 gaokun3/payload/ 里的系统文件）
class Release {
  Release(Gk3Record r)
      : dir = r['dir'],
        hasBoot = r.yes('boot'),
        superKind = r['super'],
        hasSha = r.yes('sha256'),
        version = r['version'] == '?' ? '' : r['version'],
        superMib = r.intOf('super_mib');
  final String dir, superKind, version;
  final bool hasBoot, hasSha;
  final int superMib;

  /// 能不能拿它装：boot.img 与 super 都在
  bool get installable => hasBoot && superKind != 'no' && superKind.isNotEmpty;
}
