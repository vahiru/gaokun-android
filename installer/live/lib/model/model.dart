// 把后端的行记录解读成界面用的类型。
//
// ⚠️ 这里只做【解读】，不做任何分区计算：空间够不够、/data 能给多大、放不放得下，
//    一律问 gk3_plan（backend.dart 顶上那条规矩）。
import 'dart:convert';

import '../backend/backend.dart';
import '../backend/protocol.dart';

const espTypeGuid = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B';

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

  /// 安装器就是从这个分区跑起来的（gk3_probe 的 PART medium=yes）—— 调整磁盘页不让动它
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
        tran = r['tran'],
        medium = r.yes('medium'),
        table = r['table'];
  final String path, model, tran;
  final int sizeMib;
  final bool removable, medium;

  /// gk3_probe 的 table=：gpt | mbr | unreadable | unknown（旧后端 / 旧 fixture 没有这个字段 → 空串，按 gpt 待）。
  /// 设计稿见 docs/dev/installer-rust-design.md
  final String table;
  final parts = <Part>[];
  final free = <FreeRegion>[];

  bool get external => removable || tran == 'usb';

  /// 分区表读不出来：后端不列它的分区与空闲区，界面不给选
  bool get unreadable => table == 'unreadable';

  /// MBR 盘：后端只写 GPT，非整盘的写盘入口一律 ERR mbr-disk —— 只能整盘清空
  bool get mbr => table == 'mbr';
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

  /// gk3_probe 的输出：DISK 行后面跟着它的 PART / FREE 行（FREE 带 disk= 可以直接认）
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

class Check {
  Check(Gk3Record r)
      : id = r['id'],
        state = switch (r['ok']) { 'yes' => CheckState.ok, 'no' => CheckState.fail, _ => CheckState.unknown },
        value = r['value'],
        why = r['why'],
        missing = r['missing'],
        min = r['min'],
        ac = r.yes('ac'),
        pkgs = r['pkgs'];
  final String id, value, why, missing;

  /// CHECK id=power 的门槛（百分比）与"接着电源"
  final String min;
  final bool ac;

  /// CHECK id=tools 缺的工具对应的 Debian 包名（INST-16，逗号分隔；旧后端没有）
  final String pkgs;
  final CheckState state;
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

class Shrinkable {
  Shrinkable(Gk3Record r)
      : part = r['part'],
        fs = r['fs'],
        curMib = r.intOf('cur_mib'),
        minMib = r.intOf('min_mib'),
        can = r.yes('can'),
        why = r['why'];
  final String part, fs, why;
  final int curMib, minMib;
  final bool can;

  /// gk3_shrink 自己再加的余量（installer-lib.sh：floor = min + 512）。
  /// ⚠️ 这是界面上滑块的下限，【不是】判据 —— 判据在 gk3_shrink 里，它会自己再验一遍。
  int get floorMib => minMib + 512;
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

/// 重新安装时复用的一个现有分区（gk3_plan --mode reinstall 的 PLAN op=reuse）
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

/// gk3_plan 的结果：要么 parts + summary，要么 error
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

class Ap {
  Ap(Gk3Record r)
      : signal = r.intOf('signal', -100),
        secure = r.yes('secure'),
        auth = r['auth'],
        ssidHex = r['ssid_hex'],
        ssid = r['ssid'],
        hidden = false;

  /// 手输的隐藏网络（不广播名字，扫描列表里没有它）。名字按 UTF-8 转成 ssid_hex ——
  /// 与扫描来的网络走同一条 hex: 路径，引号、空格都不会坏；后端再加 scan_ssid=1
  Ap.hidden(this.ssid, {required this.secure})
      : signal = -100,
        auth = secure ? 'psk' : 'open',
        ssidHex = utf8.encode(ssid).map((b) => b.toRadixString(16).padLeft(2, '0')).join(),
        hidden = true;

  final int signal;
  final bool secure, hidden;
  final String auth, ssidHex, ssid;

  /// 安装器连得上的：开放、WPA/WPA2（psk）、纯 WPA3（sae）。企业网络（eap）、WEP、OWE 连不了 —— 列表里标灰
  /// 并写明原因，不再让人输了密码再报"密码错"（v1.0 计划 GUI-9）
  bool get supported => const {'open', 'psk', 'sae'}.contains(auth);

  /// 信号格数 0–4（dBm 数字对用户没有意义 —— roadmap 欠账第 1 条）
  int get bars => signal >= -55 ? 4 : signal >= -65 ? 3 : signal >= -75 ? 2 : signal >= -85 ? 1 : 0;
}

/// 变体清单的一行。base= 指向 R2 上的一个发布目录（installer-lib.sh 的 gk3_net_manifest）
class Variant {
  Variant(Gk3Record r)
      : id = r['id'],
        name = r['name'],
        desc = r['desc'],
        base = r['base'],
        sizeMib = r.intOf('size_mib'),
        latest = r.yes('latest'),
        _rec = r;
  final String id, name, desc, base;
  final Gk3Record _rec;

  /// 按界面语言取 name_<语言> / desc_<语言>，清单里没有就用 name / desc（installer-lib.sh 的 gk3_net_manifest）
  String nameFor(String lang) => _rec.fields['name_$lang'] ?? name;
  String descFor(String lang) => _rec.fields['desc_$lang'] ?? desc;
  final int sizeMib;

  /// 从 OTA 清单推出来的"最新发布"（变体清单还没发布时，gk3__ota_variant）
  final bool latest;
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

/// gk3_release_info / gk3_net_release 的 RELEASE 记录
class Release {
  Release(Gk3Record r)
      : dir = r['dir'],
        hasBoot = r.yes('boot'),
        superKind = r['super'],
        hasSha = r.yes('sha256'),
        rescue = r.yes('rescue'),
        version = r['version'] == '?' ? '' : r['version'],
        superMib = r.intOf('super_mib');
  final String dir, superKind, version;
  final bool hasBoot, hasSha, rescue;
  final int superMib;

  /// 能不能拿它装：boot.img 与 super 都在
  bool get installable => hasBoot && superKind != 'no' && superKind.isNotEmpty;
}
