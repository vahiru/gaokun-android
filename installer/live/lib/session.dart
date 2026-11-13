// 一次安装的全部状态：用户的选择 + 从后端问来的事实。
//
// ★ 每一个"能不能 / 够不够 / 多大"都来自后端（gk3l）的一次调用，这里只是记下来、组合起来。
//   例如"双系统要多少空间"：用一个空区间去问 gk3l plan，让它在 PLANERR need_mib= 里自己报出来 ——
//   installer/backend/src/plan.h 里的常量一改，界面上的数字跟着变。
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';

import 'model/model.dart';

/// reinstall：盘上已经有我们的 Android 时，复用现有分区重新安装（用户 2026-09-25）
enum Mode { wipe, alongside, reinstall }

/// 双系统可不可行、不可行的原因（方式页据此"禁用 + 写明原因"，而不是藏起来）
sealed class Along {
  const Along();
}

class AlongOk extends Along {
  const AlongOk(this.region, this.esp, this.windows);
  final FreeRegion region;
  final Part esp;
  final bool windows;
}

class AlongNoEsp extends Along {
  const AlongNoEsp();
}

class AlongEspSmall extends Along {
  const AlongEspSmall(this.freeMib, this.needMib);
  final int freeMib, needMib;
}

/// 100 MiB 那种 EFI 分区（用户自己重装 Windows 建的）：1.0 明确不装（U17，ERR esp-too-small）
class AlongEspTooSmall extends Along {
  const AlongEspTooSmall(this.sizeMib);
  final int sizeMib;
}

/// Windows 在休眠 / 快速启动：不许往共用的 EFI 分区写（U18，ERR esp-windows-hibernated）
class AlongHibernated extends Along {
  const AlongHibernated();
}

class AlongInstalled extends Along {
  const AlongInstalled(this.names);

  /// 冲突的分区名（盘上已有的、与 Android 的分区同名的）；
  /// 空串 = 分区名不冲突、但这个 ESP 上已经有 gaokun3 的启动项
  final String names;
}

class AlongNoRoom extends Along {
  const AlongNoRoom(this.haveMib, this.needMib, [this.fixedMib]);
  final int haveMib;
  final int? needMib;

  /// 除 /data 之外要的固定开销（PLANERR fixed_mib；旧后端没有这个字段 → null）
  final int? fixedMib;
}

class AlongMbr extends Along {
  const AlongMbr();
}

/// 有 GPT 的痕迹、主表却不能用（gk3l probe 的 table=damaged）：只能整盘清空
class AlongBadTable extends Along {
  const AlongBadTable();
}

class AlongError extends Along {
  const AlongError(this.msg);
  final String msg;
}

/// 下载被取消时 install() 最后报的退出码（130 = 被信号打断的惯例）
const kCancelledExit = 130;

/// 两条写盘的路，写盘之前都要阅读风险提示。每一条在 UI 上都先过 RiskPage（ui/screens_risk.dart）；
/// Session 再守一道：没确认过的那条路，install / deletePartition 一个字节都不写（[Session.riskAcked]）
enum RiskKind {
  /// 整盘清空 / 双系统 / 重新安装：gk3l apply
  install,

  /// 删除分区：gk3l delete
  edit,
}

/// 没经过风险确认页就要写盘时报的 ERR 代码（界面自己的，不是后端的；touched=no ⇒ "盘没动过"那种失败页）
const kRiskUnackedCode = 'risk-not-acknowledged';

/// 语言从哪来、选了存到哪（v1.0 计划 GUI-19）。真机上是 backend/platform_io.dart 的实现
/// （内核参数 gk3.lang= 与介质上的一个小文件）；测试与 Web 预览用 [LangPrefs.none]
abstract class LangPrefs {
  const LangPrefs();

  /// 用户上次在安装器里选的（记住的），没有 → null
  String? saved();

  /// 启动项给的默认（内核参数 gk3.lang=），没有 → null
  String? fromBoot();
  void save(String lang);

  static const LangPrefs none = _NoPrefs();
}

class _NoPrefs extends LangPrefs {
  const _NoPrefs();
  @override
  String? saved() => null;
  @override
  String? fromBoot() => null;
  @override
  void save(String lang) {}
}

/// 'en'、'en_US'、'zh-CN' → 'en' / 'zh'；不认识 → null
String? normalizeLang(String? v) {
  final t = v?.trim().toLowerCase() ?? '';
  if (t.startsWith('zh')) return 'zh';
  if (t.startsWith('en')) return 'en';
  return null;
}

class Session extends ChangeNotifier {
  /// 语言：记住的 > 启动项的 gk3.lang= > 中文。记住的排在前面：用户在这台机器上明确选过一次，
  /// 安装器重启（cage 拉起来）、回 Windows 关快速启动再回来，都该沿用；gk3.lang= 是第一次进来时的默认
  Session(this.backend, {LangPrefs prefs = LangPrefs.none})
      : _prefs = prefs,
        language = normalizeLang(prefs.saved()) ?? normalizeLang(prefs.fromBoot()) ?? 'zh';
  final Gk3Backend backend;
  final LangPrefs _prefs;

  void _changed() => notifyListeners();

  // ── 正在写盘 ────────────────────────────────────────────────────────────
  /// 安装、删除分区在跑的时候为 true：侧栏的重启 / 关机据此禁用（v1.0 计划 GUI-3）
  bool get writing => _writing > 0;
  int _writing = 0;

  Future<T> _whileWriting<T>(Future<T> Function() f) async {
    _writing++;
    _changed();
    try {
      return await f();
    } finally {
      _writing--;
      _changed();
    }
  }

  // ── 风险确认（用户 2026-10-06）────────────────────────────────────────────
  /// 确认过的是"哪条路、哪块盘、哪种方式"：换了盘、换了方式、改了保不保留数据，都要重新确认
  final _riskAcked = <String>{};
  String _riskKey(RiskKind k) => switch (k) {
        RiskKind.install => 'install ${disk?.path} ${mode?.name} keep=$keepData',
        _ => '${k.name} ${disk?.path}',
      };

  /// 风险确认页读完、勾选（、输入确认词）之后调
  void ackRisk(RiskKind k) => _riskAcked.add(_riskKey(k));
  bool riskAcked(RiskKind k) => _riskAcked.contains(_riskKey(k));

  static const _riskErr = Gk3Record('ERR', {'code': kRiskUnackedCode, 'touched': 'no'});
  static const _riskLog = Gk3Log('!! 没有经过写盘前的风险确认页，不写盘（这是界面的缺陷）');
  static const _riskRefused = CallResult([_riskErr], [_riskLog], 1);

  // ── 语言 ────────────────────────────────────────────────────────────────
  String language;
  void toggleLanguage() {
    language = language == 'zh' ? 'en' : 'zh';
    _prefs.save(language);
    _changed();
  }

  // ── 预检与安装介质 ─────────────────────────────────────────────────────
  List<Check>? checks;
  Release? usbRelease;
  bool get blocked => checks?.any((c) => c.state == CheckState.fail) ?? false;

  /// 安装器（重新）起来时还在跑的写盘任务（GUI-11）：界面崩过、被 systemd 拉起来了，写盘进程在独立单元里没死。
  /// 欢迎页看到它就直接进运行页、gk3l job follow 接着跟，而不是让人对着一块正在被写的盘从头再装一遍
  Gk3Record? runningJob;

  Future<void> start() async {
    // 最近的一个 state=running（id 以时间开头，按 id 排序就是按时间）
    final jobs = (await backend.run('job', ['status'])).ofType('JOB').where((j) => j['state'] == 'running').toList()
      ..sort((a, b) => a['id'].compareTo(b['id']));
    runningJob = jobs.lastOrNull;
    final pf = (await backend.run('preflight')).first('PREFLIGHT');
    checks = pf == null ? const [] : Check.fromPreflight(pf);
    final r = (await backend.run('release-info')).first('RELEASE');
    usbRelease = r == null ? null : Release(r);
    _changed();
  }

  // ── 盘 ──────────────────────────────────────────────────────────────────
  List<Disk>? disks;
  String? probeError;

  Future<void> probe() async {
    disks = null;
    probeError = null;
    _changed();
    final r = await backend.run('probe');
    disks = Disk.fromProbe(r);
    if (!r.ok) probeError = r.error ?? 'exit ${r.exitCode}';
    _changed();
  }

  Disk? disk;
  Along? along;

  /// 重新安装的可行性：盘上有我们的分区时才问（null = 这块盘上没有我们的 Android，不出这一项）。
  /// 用 gk3l plan --mode reinstall 问，不在界面里自己判 —— 缺哪个分区、哪个太小都是它报的
  Plan? reinstall;
  EspInfo? espInfo;
  bool assessing = false;

  /// 选定一块盘：把两种方式能不能走都问清楚
  Future<void> assess(Disk d) async {
    disk = d;
    mode = null;
    plan = null;
    userdataMib = null;
    along = null;
    reinstall = null;
    espInfo = null;
    defaultOs = 'android';
    bitlockerKeyOk = false;
    assessing = true;
    _changed();
    along = await _assessAlong(d);
    if (d.parts.any((p) => const {'super', 'userdata', 'boot_a', 'boot_b'}.contains(p.name))) {
      reinstall = Plan(await backend.run('plan', _reinstallArgs(d, keep: false)));
    }
    assessing = false;
    _changed();
  }

  Future<Along> _assessAlong(Disk d) async {
    // 分区表本身不能用的先说：MBR、主 GPT 坏了（gk3l 都只列整盘清空一条路）
    if (d.mbr) return const AlongMbr();
    if (d.damaged) return const AlongBadTable();
    final esp = d.esp;
    if (esp == null) return const AlongNoEsp();
    final esr = await backend.run('esp-info', ['--disk', d.path]);
    final er = esr.first('ESP');
    if (er != null) espInfo = EspInfo(er);
    if (er == null && esr.err != null) return AlongError(esr.err!['code']);
    final info = espInfo;
    // 顺序：分区本身太小（清理也没用）> 在休眠（回 Windows 关机就好）> 空闲不够（清理）
    if (info != null && info.small && !info.ours) return AlongEspTooSmall(info.sizeMib);
    if (info != null && info.winHibernated) return const AlongHibernated();
    if (info != null && !info.roomy && !info.ours) return AlongEspSmall(info.freeMib, info.needMib);
    // 盘上已有与 Android 同名的分区：再建一套就重名（Android 按名字挂分区，重名时挂哪个说不准）。
    // 这里只是把 probe 报的名字列出来；apply 动盘之前会按同一条规则再拒绝一次（ERR partlabel-conflict）
    final dup = [for (final p in d.parts) if (kAndroidNames.contains(p.name)) p.name];
    if (dup.isNotEmpty) return AlongInstalled(dup.join(','));
    // ESP 上已经有我们的启动项（Android 装在另一块盘上、共用这个 ESP）：再装一份会覆盖那边的启动项 —— 同样拦住
    if (info != null && info.ours) return const AlongInstalled('');
    final f = d.largestFree;
    // 没有空闲区也问一次（空区间）：让 gk3l plan 自己报"至少要多少"
    final p = Plan(await backend.run('plan', _alongArgs(d, f?.start ?? 0, f?.end ?? 0)));
    if (p.ok && f != null) return AlongOk(f, esp, info?.windows ?? false);
    final e = p.error;
    return switch (e?['msg']) {
      'not-enough-space' => AlongNoRoom(e!.intOf('avail_mib'), e.intOf('need_mib'), e.fields.containsKey('fixed_mib') ? e.intOf('fixed_mib') : null),
      _ when f == null => AlongNoRoom(0, null),
      _ => AlongError(e?['msg'] ?? 'unknown'),
    };
  }

  /// gk3l plan 的双系统方案：纯计算，要扇区大小与区间（盘号只是写进 PLAN 行）
  List<String> _alongArgs(Disk d, int start, int end) => [
        '--sector', '${d.sector}', '--region-start', '$start', '--region-end', '$end', '--disk', d.path,
        if (userdataMib != null) ...['--userdata-mib', '$userdataMib'],
      ];

  // ── 删除分区 ────────────────────────────────────────────────────────────
  /// 删除一个分区（gk3l delete）。成功与否都重新探测这块盘 ——
  /// 分区号、空闲区都可能变了，界面上显示的必须是盘上【现在】的样子
  Future<CallResult> deletePartition(String part, void Function(Gk3Event) onEvent) async {
    if (!riskAcked(RiskKind.edit)) return _riskRefused;
    final r = await _whileWriting(() => backend.run('delete', ['--part', part], onEvent));
    await probe();
    final again = disks?.where((x) => x.path == disk?.path).firstOrNull;
    if (again != null) disk = again;
    _changed();
    return r;
  }

  // ── 方式与选项 ──────────────────────────────────────────────────────────
  Mode? mode;
  int? userdataMib; // null = 剩下的全给 /data
  Plan? plan;
  bool planning = false;

  /// 重新安装时 /data 留不留。默认清除（用户 2026-09-25 定的）：换到更旧的版本时，留下的数据可能起不来
  bool keepData = false;

  List<String> _reinstallArgs(Disk d, {required bool keep}) => ['--mode', 'reinstall', '--disk', d.path, '--keep-data', keep ? 'yes' : 'no'];

  Future<void> setKeepData(bool v) async {
    keepData = v;
    await computePlan();
  }

  void setMode(Mode m) {
    mode = m;
    plan = null;
    _changed();
  }

  List<String> _planArgs() {
    final d = disk!, al = along;
    return switch (mode) {
      Mode.reinstall => _reinstallArgs(d, keep: keepData),
      Mode.wipe => ['--mode', 'wipe', '--disk', d.path, if (userdataMib != null) ...['--userdata-mib', '$userdataMib']],
      _ => al is AlongOk ? _alongArgs(d, al.region.start, al.region.end) : _alongArgs(d, 0, 0),
    };
  }

  /// gk3l apply 的参数：方式、盘、区间（双系统）、/data 大小、保不保留数据（重新安装）
  List<String> _applyArgs() {
    final d = disk!, al = along;
    return [
      '--disk', d.path,
      '--mode', switch (mode) { Mode.wipe => 'wipe', Mode.reinstall => 'reinstall', _ => 'alongside' },
      if (mode == Mode.alongside && al is AlongOk) ...['--region-start', '${al.region.start}', '--region-end', '${al.region.end}'],
      if (mode == Mode.reinstall) ...['--keep-data', keepData ? 'yes' : 'no'],
      if (mode != Mode.reinstall && userdataMib != null) ...['--userdata-mib', '$userdataMib'],
    ];
  }

  Future<void> computePlan() async {
    if (disk == null || mode == null) return;
    planning = true;
    _changed();
    plan = Plan(await backend.run('plan', _planArgs()));
    planning = false;
    _changed();
  }

  /// 专业分区页：/data 的最大值 = 默认方案里给它的全部
  int? maxUserdataMib;

  /// 进入选项页：按默认（/data 占满剩余空间）算一次方案，记下 /data 的最大值
  Future<void> startOpts() async {
    userdataMib = null;
    await computePlan();
    maxUserdataMib = plan?.ok == true ? plan!.userdataMib : null;
  }

  Future<void> setUserdata(int? mib) async {
    userdataMib = mib;
    await computePlan();
  }

  // ── 双系统（见 tools/gk3boot/README.md）──────────────────────────────────
  /// 这次装完是不是双系统：ESP 上有 Windows 的启动管理器，而且是装在它旁边 / 在它旁边重新安装
  bool get dual {
    final w = espInfo?.windows ?? false;
    return switch (mode) {
      Mode.alongside => along is AlongOk && w,
      Mode.reinstall => w,
      _ => false,
    };
  }

  /// 冷开机默认进哪个系统：预选 Android —— 依赖最少（不写任何变量），而且第一次开机本来就要进 Android
  /// 走开机向导。选 Windows 时后端把它写成 GK3 的 set_default 请求，由统一启动入口第一次运行时生效
  String defaultOs = 'android';
  void setDefaultOs(String v) {
    defaultOs = v;
    _changed();
  }

  /// U16：BitLocker + 这次要换 BOOTAA64 ⇒ 确认页上必须勾"已拿到恢复密钥"才能开始
  bool get needBitlockerKey => dual && (espInfo?.needsBitlockerKey ?? false);
  bool bitlockerKeyOk = false;
  void setBitlockerKeyOk(bool v) {
    bitlockerKeyOk = v;
    _changed();
  }

  /// 走的是 Windows 脚本那条路（安装器从内置盘上的 GK3LIVE 跑）：脚本已经先暂停过 BitLocker（§4.9.7 规则 6 的两种文案）
  bool get viaWindowsScript => disks?.any((d) => d.medium && !d.external) ?? false;

  /// 安装期间后端给的提醒（stdout 上的 NOTE 记录，例如 EFI 变量删不掉）—— 完成页逐条说
  final notes = <Gk3Record>[];

  // ── 安装 ────────────────────────────────────────────────────────────────
  /// 接着跟一个还在跑的写盘任务（[runningJob]）。算写盘阶段：侧栏的重启 / 关机照样禁用
  Stream<Gk3Event> follow(String id) async* {
    _writing++;
    _changed();
    try {
      yield* backend.call('job', ['follow', id]);
    } finally {
      _writing--;
      runningJob = null;
      _changed();
    }
  }

  /// 用安装介质自带的系统文件（gaokun3/payload/）安装：gk3l apply
  Stream<Gk3Event> install() async* {
    // 风险确认页没过（界面的缺陷才会走到这里）：一个字节都不写
    if (!riskAcked(RiskKind.install)) {
      yield _riskLog;
      yield _riskErr;
      yield const Gk3Exit(1);
      return;
    }
    final rel = usbRelease?.dir ?? '';
    _writing++;
    _changed();
    try {
      final args = [
        '--release', rel, ..._applyArgs(),
        if (dual) ...['--default-os', defaultOs],
        if (needBitlockerKey && bitlockerKeyOk) '--bitlocker-confirmed',
      ];
      notes.clear();
      await for (final e in backend.call('apply', args)) {
        if (e is Gk3Record && e.type == 'NOTE') notes.add(e);
        yield e;
      }
    } finally {
      _writing--;
      _changed();
    }
  }
}
