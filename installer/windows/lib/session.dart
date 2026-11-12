import 'package:flutter/foundation.dart';
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';

import 'backend/platform.dart';
import 'model.dart';

enum WriteTask { install, uninstall, media }

/// 任务失败时盘有没有被动过。unknown：写盘的 gk3w 非零退出却没报 ERR（被安全软件或任务管理器结束、崩溃），
/// 不能说"磁盘未被修改"（审查 11-12 第 3 条）。下载失败一定有 ERR（net.dart 的 _guard）
enum Touched { no, yes, unknown }

/// 安装介质（LiveCD + 所选版本的安装文件）要占多大：U 盘至少这么大，写到本机时在未分配空间里划出这么大的 GK3LIVE。
/// 暂定值：按 scripts/windows/gaokun3-setup.ps1 的算法（内容 ×1.25 + 128 MiB，按 256 MiB 向上取整）
/// 估 rc.3（LiveCD 约 210 MiB + boot.img 27 MiB + super.img.zst 1290 MiB）。gk3w 还没有写入安装介质的命令，
/// 做好之后改为问 gk3w（与 plan 一样），按所选版本的实际大小算。
const kMediaNeedMib = 2048;

/// 一次安装器会话：检查结果、用户的选择、正在跑的任务。界面只读它、调它的方法。
/// ★ 分区怎么排、够不够，一律问 gk3w（plan / apply），这里不算。
class Session extends ChangeNotifier {
  Session(this.backend, {String? language, this.testDisk}) : language = language ?? 'zh';

  final Gk3Backend backend;

  /// 测试模式：真后端对着这块 VHD（win_backend.dart 的 WinBackend.testDisk）。检查页照样显示本机的检查项，
  /// 但不因它们挡住（这台 PC 不是 MateBook E Go）；角上标出测试模式
  final int? testDisk;
  bool get testMode => testDisk != null;
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
    // 一次确认只用于一次写盘：重新检查之后要重新勾选（审查 11-12 第 11 条）
    bitlockerAck = false;
    notifyListeners();
    try {
      final pf = await backend.run('preflight');
      final pr = await backend.run('probe');
      final pfRec = pf.ok ? pf.first('PREFLIGHT') : null;
      preflight = pfRec == null ? null : Preflight(pfRec);
      probe = pr.ok ? Probe(pr) : null;
      // 失败原因：先用后端报的 ERR —— preflight 没报 ERR 的失败不能盖住 probe 报的（例如 not-admin）；
      // 失败了却谁都没报（进程崩溃、被结束）、或 preflight 缺了记录：backend-failed，检查页挡住（审查 11-12 第 8 条）
      checkErr = (pf.ok ? null : pf.err) ?? (pr.ok ? null : pr.err) ??
          (pfRec == null ? _failed('preflight') : (pr.ok ? null : _failed('probe')));
      plans.clear();
      region = null;
      mediaTarget = null;
      mediaRegion = _defaultMediaRegion();
      if (disk != null && !installed) await _loadPlans();
    } catch (e) {
      checkErr = _failed('check');
      log.add('!! $e');
    } finally {
      checking = false;
      notifyListeners();
    }
  }

  static Gk3Record _failed(String fn) => Gk3Record('ERR', {'code': 'backend-failed', 'fn': fn});

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
  /// 正在取版本列表的来源（按来源分别记：一个来源还在取时切到另一个，另一个也要去取 —— 审查 11-12 第 10 条）
  final Set<ReleaseSource> _loading = {};
  bool get loadingVariants => _loading.contains(source);
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
    if (src == ReleaseSource.local || _loading.contains(src) || (!force && _variants.containsKey(src))) return;
    _loading.add(src);
    notifyListeners();
    try {
      final res = await backend.run('releases', ['--source', src.name]);
      _variants[src] = [for (final r in res.ofType('VARIANT')) Variant(r)];
      _variantsErr[src] = res.ok ? null : (res.err ?? const Gk3Record('ERR', {'code': 'manifest-unavailable'}));
      final lc = res.first('LIVECD');
      _liveCd[src] = lc == null ? null : LiveCd(lc);
    } catch (e) {
      // 后端的流出错也不能让页面一直转圈（审查 11-12 第 4 条）
      _variants[src] = const [];
      _variantsErr[src] = const Gk3Record('ERR', {'code': 'manifest-unavailable'});
    } finally {
      _loading.remove(src);
    }
    if (source == src) variant = variants.where((v) => v.latest).firstOrNull ?? variants.firstOrNull;
    notifyListeners();
  }

  void selectVariant(Variant v) {
    variant = v;
    notifyListeners();
  }

  /// 资源管理器的"复制文件地址"带一对双引号：去掉（原先 gk3w 收到带引号的路径，按住确认之后才报找不到文件 —— 审查 11-12 第 14 条）
  void setLocalDir(String d) {
    var t = d.trim();
    if (t.length >= 2 && t.startsWith('"') && t.endsWith('"')) t = t.substring(1, t.length - 1).trim();
    localDir = t;
    notifyListeners();
  }

  /// 本地目录里缺的必需文件（Windows 上查；进入下一步之前就说清楚）
  List<String> get localMissing => missingReleaseFiles(localDir);

  bool get releaseReady => source == ReleaseSource.local ? localDir.isNotEmpty && localMissing.isEmpty : variant != null;

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

  /// 任务进行中（下载 + 写盘）：界面不能返回、不能切换语言，系统保持唤醒
  bool writing = false;

  /// gk3w 正在写盘（apply / uninstall / media）：只有这段时间不许关闭窗口、阻止关机。下载期间可以关掉
  /// （.part 留着，下次接着下）—— 原先下载一开始就挡住关闭与关机，网慢时只能强制结束（审查 11-12 第 2 条）
  bool diskBusy = false;

  /// 开始写盘之前等它返回：原生窗口确认已经挡住关闭与关机，才启动 gk3w。由 main.dart 接到 WindowControl 上；
  /// 原先只靠下一帧 WindowButtons 重建时发出 setBusy，gk3w 可能先起来，这一瞬间关掉窗口它就没人看着地写盘
  /// （修复审查 11-12 第 3 条）。测试里为 null
  Future<void> Function()? beforeDiskWrite;

  /// 正在下载（进度页的说明换成"下载期间可以关闭本窗口"）
  bool downloading = false;

  Future<void> _startDiskWrite() async {
    diskBusy = true;
    notifyListeners();
    await beforeDiskWrite?.call();
  }
  int percent = 0;
  Gk3Progress? lastProgress;
  final List<String> log = [];
  CallResult? result;

  /// 盘有没有被动过：任务失败时看 ERR 记录的 touched=；没有 ERR 的失败 → 不知道（见 [Touched]）
  Touched get touched {
    final r = result;
    if (r == null || r.ok) return Touched.no;
    final err = r.err;
    if (err == null) return Touched.unknown;
    return err['touched'] == 'yes' ? Touched.yes : Touched.no;
  }

  /// 任务开始：状态清零。lastProgress 也清掉 —— 原先先装后卸时，卸载全程显示上一次的"已完成"（审查 11-12 第 6 条）
  void _begin(WriteTask t) {
    task = t;
    writing = true;
    percent = 0;
    lastProgress = null;
    log.clear();
    result = null;
    notifyListeners();
  }

  /// 任务结束（成功、失败或出了异常都走这里）：不然 writing / diskBusy 卡在 true，窗口关不掉、关机被挡（审查 11-12 第 4 条）
  void _end() {
    writing = false;
    diskBusy = false;
    bitlockerAck = false;
    notifyListeners();
  }

  /// 任务里出了异常（不该有）：写盘中出的算"不知道动没动过盘"（没有 ERR），之前出的算没动。
  /// 异常的内容进日志区（失败页显示的就是它），用户截图时看得到
  CallResult _crashed(Object e) {
    log.add('!! $e');
    return CallResult(diskBusy ? const [] : const [Gk3Record('ERR', {'code': 'internal'})], [Gk3Log('!! $e')], 1);
  }

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
    _begin(t);
    final local = source == ReleaseSource.local;
    try {
      var dir = localDir;
      if (!local) {
        downloading = true;
        final CallResult dl;
        try {
          dl = await backend.run('download', [variant!.base, releaseDir], (e) => _onEvent(e, span: 40));
        } finally {
          downloading = false;
        }
        if (!dl.ok) {
          result = dl;
          return false;
        }
        dir = dl.first('RELEASE')?['dir'] ?? releaseDir;
      }
      final (fn, args) = command(dir);
      await _startDiskWrite();
      result = await backend.run(fn, args, (e) => _onEvent(e, base: local ? 0 : 40, span: local ? 100 : 60));
      // 装好了：下载的 1.3 GiB 没有用了，删掉（本地文件是用户自己的，不动；失败时留着，下次续传）。
      // 回放演示数据时不删：录制里的目录是开发机上真实的下载目录
      if (result!.ok && t == WriteTask.install && !local && backend.demoLabel == null) deleteDownloadedRelease(dir);
    } catch (e) {
      result = _crashed(e);
    } finally {
      _end();
    }
    return result!.ok;
  }

  /// 卸载：gk3w uninstall 不报百分比，界面显示一条不定长的进度
  Future<bool> uninstall() async {
    _begin(WriteTask.uninstall);
    try {
      await _startDiskWrite();
      result = await backend.run('uninstall', [
        '--disk', '${disk!.num}', '--real-disk',
        if (bitlockerAck) ...['--bitlocker-key', 'yes'],
      ], _onEvent);
    } catch (e) {
      result = _crashed(e);
    } finally {
      _end();
    }
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
