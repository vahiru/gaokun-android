import 'package:flutter/foundation.dart';
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';

import 'model.dart';

enum WriteTask { install, uninstall, media }

/// 安装介质（LiveCD + 所选版本的安装文件）要占多大：U 盘至少这么大，写到本机时在未分配空间里划出这么大的 GK3LIVE。
/// 暂定值：按 scripts/windows/gaokun3-setup.ps1 的算法（内容 ×1.25 + 128 MiB，按 256 MiB 向上取整）
/// 估 rc.3（LiveCD 约 210 MiB + boot.img 27 MiB + super.img.zst 1290 MiB）。gk3w 还没有写入安装介质的命令，
/// 做好之后改为问 gk3w（与 plan 一样），按所选版本的实际大小算。
const kMediaNeedMib = 2048;

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
    mediaTarget = null;
    mediaRegion = _defaultMediaRegion();
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

  /// 写入本机时放 GK3LIVE 的那块未分配空间；没有放得下的 = null。
  /// GK3LIVE 建在它的【末尾】：之后在 LiveCD 里保留现有系统安装 Android 时，Android 装在它前面、userdata 排在最后，
  /// 正好紧挨着 GK3LIVE —— 装完删掉 GK3LIVE 就能把空间并进 userdata（放在开头的话，空出来的在 misc 前面，并不进去）
  FreeSpan? mediaRegion;
  bool mediaFits(FreeSpan f) => f.sizeMib >= kMediaNeedMib;

  /// 默认选最大的一块：LiveCD 保留现有系统时也装进最大的空闲区（live 的 DiskInfo.largestFree）
  FreeSpan? _defaultMediaRegion() {
    final fits = [for (final f in freeSpans) if (mediaFits(f)) f];
    return fits.isEmpty ? null : fits.reduce((a, b) => a.sizeMib >= b.sizeMib ? a : b);
  }

  void selectMediaRegion(FreeSpan f) {
    mediaRegion = f;
    notifyListeners();
  }

  /// 插着的 U 盘（probe 的 DISK 记录里 USB 总线或可移动、且不是系统盘的）
  List<WinDisk> get usbDisks => [for (final d in probe?.disks ?? const <WinDisk>[]) if (d.usbStick) d];

  /// 安装介质写到哪（目标页选的）；重新检查后清空
  MediaTarget? mediaTarget;
  void selectMediaTarget(MediaTarget t) {
    mediaTarget = t;
    notifyListeners();
  }

  void selectRegion(FreeSpan f) {
    region = f;
    notifyListeners();
  }

  // ── 发布文件 ──
  ReleaseSource source = ReleaseSource.github;
  final Map<ReleaseSource, List<Variant>> _variants = {};
  final Map<ReleaseSource, Gk3Record?> _variantsErr = {};
  final Map<ReleaseSource, LiveCd?> _liveCd = {};
  bool loadingVariants = false;
  Variant? variant;

  /// 用本地文件时的目录（boot.img、super.img.zst、install-artifacts.sha256 放在一起）
  String localDir = '';

  List<Variant> get variants => _variants[source] ?? const [];
  Gk3Record? get variantsErr => _variantsErr[source];

  /// 这个来源上最新的安装器（LiveCD）；本地文件时为 null（LiveCD 也从那个目录取）
  LiveCd? get liveCd => _liveCd[source];

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
    final lc = res.first('LIVECD');
    _liveCd[src] = lc == null ? null : LiveCd(lc);
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
  /// 正在进行安装还是卸载（写盘期间阻止关机时，原因按它写）
  WriteTask task = WriteTask.install;
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
  Future<bool> install({required String releaseDir}) => _downloadThen(WriteTask.install, releaseDir, (dir) {
        final d = disk!, r = region!;
        return ('apply', [
          '--disk', '${d.num}', '--region-start', '${r.start}', '--region-end', '${r.end}',
          '--release', dir, '--default-os', defaultOs, '--real-disk',
          if (bitlockerAck) ...['--bitlocker-key', 'yes'],
        ]);
      });

  /// 制作安装介质：下载所选版本，再由 gk3w 写入 LiveCD 与安装文件 —— U 盘整个清除后写入；本机则在所选未分配空间的
  /// 末尾建 GK3LIVE（见 [mediaRegion]）、在 ESP 里放 LiveCD 的启动文件与启动项。--livecd 是 LiveCD 的下载地址，本地文件时是那个目录。
  /// gk3w 的 media 命令尚未实现（参数是计划中的样子），演示数据里也就没有它的录制，回放时这一步按失败处理。
  Future<bool> writeMedia({required String releaseDir}) {
    mediaWritten = mediaTarget;
    return _downloadThen(WriteTask.media, releaseDir, (dir) {
      final t = mediaWritten!;
      final live = source == ReleaseSource.local ? dir : liveCd!.url;
      if (t.usb) {
        return ('media', ['--target', 'usb', '--disk', '${t.disk!.num}', '--release', dir, '--livecd', live]);
      }
      final d = disk!, r = mediaRegion!;
      return ('media', [
        '--target', 'internal', '--disk', '${d.num}', '--region-start', '${r.start}', '--region-end', '${r.end}',
        '--release', dir, '--livecd', live, '--real-disk',
        if (bitlockerAck) ...['--bitlocker-key', 'yes'],
      ]);
    });
  }

  /// 最近一次写入的安装介质目标：进度页、完成页、失败页按它措辞（重新检查会清空 [mediaTarget]，这个不清）
  MediaTarget? mediaWritten;

  Future<bool> _downloadThen(WriteTask t, String releaseDir, (String, List<String>) Function(String dir) command) async {
    task = t;
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
    final (fn, args) = command(dir);
    result = await backend.run(fn, args, (e) => _onEvent(e, base: local ? 0 : 40, span: local ? 100 : 60));
    writing = false;
    notifyListeners();
    return result!.ok;
  }

  /// 卸载：gk3w uninstall 不报百分比，界面显示一条不定长的进度
  Future<bool> uninstall() async {
    task = WriteTask.uninstall;
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
