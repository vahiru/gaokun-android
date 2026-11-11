import 'package:flutter/foundation.dart';
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';

import 'model.dart';

/// 一次安装器会话：检查结果、用户的选择、正在跑的任务。界面只读它、调它的方法。
/// ★ 分区怎么排、够不够，一律问 gk3w（plan / apply），这里不算。
class Session extends ChangeNotifier {
  Session(this.backend, {String? language}) : language = language ?? 'zh';

  final Gk3Backend backend;
  String language;

  void setLanguage(String lang) {
    language = lang;
    notifyListeners();
  }

  // ── 检查 ──
  bool checking = false;
  Preflight? preflight;
  Probe? probe;

  /// 检查本身失败（gk3w 跑不起来、不是管理员 …）：给用户看的说明在 checkErr
  Gk3Record? checkErr;

  Future<void> check() async {
    checking = true;
    checkErr = null;
    notifyListeners();
    final pf = await backend.run('preflight');
    final pr = await backend.run('probe');
    preflight = pf.ok && pf.first('PREFLIGHT') != null ? Preflight(pf.first('PREFLIGHT')!) : null;
    probe = pr.ok ? Probe(pr) : null;
    checkErr = !pf.ok ? pf.err : (!pr.ok ? pr.err : null);
    plans.clear();
    region = null;
    if (disk != null && !installed) await _loadPlans();
    checking = false;
    notifyListeners();
  }

  WinDisk? get disk => probe?.systemDisk;
  bool get installed => disk != null && probe!.androidOn(disk!).isNotEmpty;
  List<FreeSpan> get freeSpans => disk == null ? const [] : probe!.freeOf(disk!);

  /// 每块空闲区装得下吗、userdata 能有多大（gk3w plan）
  final Map<FreeSpan, Plan> plans = {};
  FreeSpan? region;

  /// 装 Android 至少要多大的空闲区（问 gk3w plan 一个空区间，它在 PLANERR 里报 need_mib）
  int minNeedMib = 0;

  Future<void> _loadPlans() async {
    final d = disk!;
    final need = await backend.run('plan', ['--sector', '${d.sector}', '--region-start', '0', '--region-end', '0', '--disk', '${d.num}']);
    minNeedMib = Plan(need).needMib;
    for (final f in freeSpans) {
      final res = await backend.run('plan', [
        '--sector', '${d.sector}', '--region-start', '${f.start}', '--region-end', '${f.end}', '--disk', '${d.num}',
      ]);
      plans[f] = Plan(res);
    }
    final fits = [for (final f in freeSpans) if (plans[f]?.ok ?? false) f];
    // 默认选最大的那块
    if (fits.isNotEmpty) region = fits.reduce((a, b) => a.sizeMib >= b.sizeMib ? a : b);
  }

  bool get hasRoom => plans.values.any((p) => p.ok);

  void selectRegion(FreeSpan f) {
    region = f;
    notifyListeners();
  }

  // ── 发布文件 ──
  ReleaseSource source = ReleaseSource.github;
  final Map<ReleaseSource, List<Variant>> _variants = {};
  final Map<ReleaseSource, Gk3Record?> _variantsErr = {};
  bool loadingVariants = false;
  Variant? variant;

  /// 用本地文件时的目录（boot.img、super.img.zst、install-artifacts.sha256 放在一起）
  String localDir = '';

  List<Variant> get variants => _variants[source] ?? const [];
  Gk3Record? get variantsErr => _variantsErr[source];

  void setSource(ReleaseSource s) {
    source = s;
    variant = variants.where((v) => v.latest).firstOrNull ?? variants.firstOrNull;
    notifyListeners();
    if (s != ReleaseSource.local) loadVariants();
  }

  /// 取一个来源的版本列表（GitHub 的 releases / R2 上的 OTA 清单），取过就不再取
  Future<void> loadVariants({bool force = false}) async {
    final src = source;
    if (src == ReleaseSource.local || loadingVariants || (!force && _variants.containsKey(src))) return;
    loadingVariants = true;
    notifyListeners();
    final res = await backend.run('releases', ['--source', src.name]);
    _variants[src] = [for (final r in res.ofType('VARIANT')) Variant(r)];
    _variantsErr[src] = res.ok ? null : res.err;
    loadingVariants = false;
    if (source == src) variant = variants.where((v) => v.latest).firstOrNull ?? variants.firstOrNull;
    notifyListeners();
  }

  void selectVariant(Variant v) {
    variant = v;
    notifyListeners();
  }

  void setLocalDir(String d) {
    localDir = d.trim();
    notifyListeners();
  }

  bool get releaseReady => source == ReleaseSource.local ? localDir.isNotEmpty : variant != null;

  // ── 选项 ──
  /// android / windows：开机默认进哪个（gk3w apply --default-os）
  String defaultOs = 'android';
  void setDefaultOs(String os) {
    defaultOs = os;
    notifyListeners();
  }

  /// BitLocker 开着（或读不到）时，用户确认手上有恢复密钥（gk3w 的 --bitlocker-key yes）
  bool bitlockerAck = false;
  bool get needBitlockerAck => preflight != null && preflight!.bitlocker != 'off';
  void setBitlockerAck(bool v) {
    bitlockerAck = v;
    notifyListeners();
  }

  // ── 正在跑的任务（下载 + 安装 / 卸载） ──
  bool writing = false;
  int percent = 0;
  Gk3Progress? lastProgress;
  final List<String> log = [];
  CallResult? result;

  /// 盘有没有被动过：任务失败时，ERR 记录的 touched=
  bool get touched => result?.err?['touched'] == 'yes';

  void _onEvent(Gk3Event e, {int base = 0, int span = 100}) {
    switch (e) {
      case Gk3Progress():
        percent = base + (e.percent * span ~/ 100);
        lastProgress = e;
      case Gk3Log():
        log.add(e.line);
      case Gk3Record():
        // 日志里照后端的原样（TYPE k=v …），用户截图时能和 gk3w 的输出对上
        log.add([e.type, for (final f in e.fields.entries) '${f.key}=${f.value}'].join(' '));
      case Gk3Exit():
        break;
    }
    notifyListeners();
  }

  /// 下载（0–40 %）再安装（40–100 %）；用本地文件时直接安装（0–100 %）
  Future<bool> install({required String releaseDir}) async {
    writing = true;
    percent = 0;
    log.clear();
    result = null;
    notifyListeners();
    final local = source == ReleaseSource.local;
    var dir = localDir;
    if (!local) {
      final dl = await backend.run('download', [variant!.base, releaseDir], (e) => _onEvent(e, span: 40));
      if (!dl.ok) {
        result = dl;
        writing = false;
        notifyListeners();
        return false;
      }
      dir = dl.first('RELEASE')?['dir'] ?? releaseDir;
    }
    final d = disk!, r = region!;
    result = await backend.run('apply', [
      '--disk', '${d.num}', '--region-start', '${r.start}', '--region-end', '${r.end}',
      '--release', dir, '--default-os', defaultOs, '--real-disk',
      if (bitlockerAck) ...['--bitlocker-key', 'yes'],
    ], (e) => _onEvent(e, base: local ? 0 : 40, span: local ? 100 : 60));
    writing = false;
    notifyListeners();
    return result!.ok;
  }

  /// 卸载：gk3w uninstall 不报百分比，界面显示一条不定长的进度
  Future<bool> uninstall() async {
    writing = true;
    percent = 0;
    log.clear();
    result = null;
    notifyListeners();
    result = await backend.run('uninstall', [
      '--disk', '${disk!.num}', '--real-disk',
      if (bitlockerAck) ...['--bitlocker-key', 'yes'],
    ], _onEvent);
    writing = false;
    notifyListeners();
    return result!.ok;
  }

  /// 卸载之后空出来的空间（gk3w uninstall 的 FREE 记录里最大的一段）
  int get freedMib {
    var m = 0;
    for (final r in result?.ofType('FREE') ?? const <Gk3Record>[]) {
      if (r.intOf('size_mib') > m) m = r.intOf('size_mib');
    }
    return m;
  }
}
