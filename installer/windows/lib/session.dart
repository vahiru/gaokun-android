import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';

import 'backend/platform.dart';
import 'model.dart';

enum WriteTask { install, uninstall, media }

/// 任务失败时盘有没有被动过。unknown：写盘的 gk3w 非零退出却没报 ERR（被安全软件或任务管理器结束、崩溃），
/// 不能说"磁盘未被修改"。下载失败一定有 ERR（net.dart 的 _guard）
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
    // 恢复密钥只用于一次写盘：重新检查之后要重新输入
    _bitlockerKey = null;
    bitlockerErr = null;
    notifyListeners();
    try {
      final pf = await backend.run('preflight');
      final pr = await backend.run('probe');
      final pfRec = pf.ok ? pf.first('PREFLIGHT') : null;
      preflight = pfRec == null ? null : Preflight(pfRec);
      probe = pr.ok ? Probe(pr) : null;
      // 失败原因：先用后端报的 ERR —— preflight 没报 ERR 的失败不能盖住 probe 报的（例如 not-admin）；
      // 失败了却谁都没报（进程崩溃、被结束）、或 preflight 缺了记录：backend-failed，检查页挡住
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
      logNote?.call('!! $e');
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
  /// 正在取版本列表的来源（按来源分别记：一个来源还在取时切到另一个，另一个也要去取）
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
      // 后端的流出错也不能让页面一直转圈
      logNote?.call('!! releases: $e');
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

  /// 资源管理器的"复制文件地址"带一对双引号：去掉（gk3w 会把引号当成路径的一部分，找不到文件）
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

  // ── BitLocker ──
  /// 系统盘的 BitLocker 保护已开启（或状态不明）：安装、卸载前要输入恢复密钥，由 gk3w 与系统盘的恢复密钥比对，
  /// 写入磁盘前再由它暂停保护。保护已暂停或尚未激活时不要（Windows 下次启动不会要求输入恢复密钥）。
  /// 测试模式不写本机系统盘，也不要
  bool get needBitlockerKey => !testMode && preflight != null && !const {'off', 'suspended'}.contains(preflight!.bitlocker);

  /// 系统盘上没有恢复密钥（只有 TPM 之类的保护器）：无从校验，不能继续
  bool get bitlockerNoKey => needBitlockerKey && (preflight!.bitlockerKeyIds?.isEmpty ?? false);

  /// 通过校验的恢复密钥：只在内存里，写盘时经标准输入交给 gk3w；重新检查、任务结束后清掉
  String? _bitlockerKey;
  bool get bitlockerVerified => _bitlockerKey != null;

  /// 可以开始写盘了：不需要密钥，或者密钥已通过校验
  bool get bitlockerReady => !needBitlockerKey || bitlockerVerified;

  /// 正在校验（gk3w bitlocker-key 还没返回）
  bool bitlockerChecking = false;

  /// 上一次校验失败的原因（gk3w 的 ERR）
  Gk3Record? bitlockerErr;

  /// 输入框的内容变了：之前的校验结果作废
  void clearBitlockerKey() {
    if (_bitlockerKey == null && bitlockerErr == null) return;
    _bitlockerKey = null;
    bitlockerErr = null;
    notifyListeners();
  }

  /// 校验用户输入的恢复密钥（gk3w bitlocker-key，密钥经标准输入）。[suspendReboots] 大于 0：通过之后由 gk3w
  /// 暂停 BitLocker 保护，Windows 重启这么多次后自动恢复（关闭 Secure Boot 之前用），然后重新检查
  Future<bool> verifyBitlockerKey(String input, {int suspendReboots = 0}) async {
    final key = normalizeRecoveryKey(input);
    _bitlockerKey = null;
    if (key == null) {
      bitlockerErr = const Gk3Record('ERR', {'code': 'bitlocker-key-format'});
      notifyListeners();
      return false;
    }
    bitlockerChecking = true;
    bitlockerErr = null;
    notifyListeners();
    var ok = false;
    try {
      final r = await Gk3Backend.collect(
          backend.callWithInput('bitlocker-key', [if (suspendReboots > 0) ...['--suspend', '$suspendReboots']], '$key\n'));
      ok = r.ok && r.first('BITLOCKER')?['key'] == 'ok';
      if (ok) {
        _bitlockerKey = key;
      } else {
        bitlockerErr = r.err ?? _failed('bitlocker-key');
      }
    } catch (e) {
      logNote?.call('!! bitlocker-key: $e');
      bitlockerErr = _failed('bitlocker-key');
    } finally {
      bitlockerChecking = false;
      notifyListeners();
    }
    // 保护暂停了：重新检查，BitLocker 一项显示为已暂停，Secure Boot 一项恢复普通的说明
    if (ok && suspendReboots > 0) await check();
    return ok;
  }

  /// 写盘调用的 BitLocker 参数与标准输入（不需要密钥时都为空）
  List<String> get _bitlockerArgs => needBitlockerKey && _bitlockerKey != null ? const ['--bitlocker-key', '-'] : const [];
  String? get _bitlockerInput => needBitlockerKey && _bitlockerKey != null ? '$_bitlockerKey\n' : null;

  // ── 正在跑的任务（下载 + 安装 / 卸载） ──
  /// 正在进行安装还是卸载（写盘期间阻止关机时，原因按它写）
  WriteTask task = WriteTask.install;

  /// 任务进行中（下载 + 写盘）：界面不能返回、不能切换语言，系统保持唤醒
  bool writing = false;

  /// gk3w 正在写盘（apply / uninstall / media）：只有这段时间不许关闭窗口、阻止关机。下载期间关闭窗口先问
  /// （[downloading]），网慢时用户仍然可以退出
  bool diskBusy = false;

  /// 开始写盘之前等它返回：原生窗口确认已经挡住关闭与关机，才启动 gk3w。由 main.dart 接到 WindowControl 上。
  /// 只靠下一帧 WindowButtons 重建时发出 setBusy 的话，gk3w 可能先起来，这一瞬间关掉窗口它就没人看着地写盘。
  /// 测试里为 null
  Future<void> Function()? beforeDiskWrite;

  /// 正在下载：这时关闭窗口先问一句，确认后停止下载、删掉已下载的文件（[cancelDownload]）
  bool downloading = false;

  /// 当前用户的"下载"文件夹（Windows 的已知文件夹，由 main.dart 接到原生上）；null = 拿不到，用 [_releaseDir] 传进来的
  Future<String?> Function()? downloadsDir;

  /// 删掉一个下载目录里的安装文件。回放演示数据时什么都不删（录制里的目录是开发机上真实的下载目录）；
  /// 测试里换成记录调用的
  late void Function(String dir) deleteFiles = backend.demoLabel == null ? deleteDownloadedRelease : (_) {};

  /// 这次安装下载的文件所在的目录（装好之后由用户决定退出时删不删：[deleteOnExit]）
  String? downloadedDir;

  /// 完成页上"退出时删除下载的安装文件"（默认不删：文件在用户自己的"下载"文件夹里，以后重装可以直接用）
  bool deleteOnExit = false;
  void setDeleteOnExit(bool v) {
    deleteOnExit = v;
    notifyListeners();
  }

  /// 退出（关闭窗口、立即重启）之前要做的：勾了"删除"就删掉下载的安装文件
  bool get exitCleanupPending => deleteOnExit && downloadedDir != null;
  void runExitCleanup() {
    final d = downloadedDir;
    if (deleteOnExit && d != null) deleteFiles(d);
    downloadedDir = null;
    // 原生的 confirmClose 跟着撤掉（WindowButtons 重建时发 setBusy）
    notifyListeners();
  }

  /// 写一行到后端的日志文件（WinBackend 的 note；没有日志文件时为 null）
  late void Function(String line)? logNote = backendLogNote(backend);

  // ── 下载中途退出的确认框 ──

  Completer<bool>? _quitPrompt;
  Completer<void>? _quitAfterDownload;

  /// 选了退出、正在停止下载并删除文件（之后关窗口）
  bool _exiting = false;

  /// 确认框正在显示，或已经选了退出、还在收尾：不再弹第二个；确认框开着时下载结束了也先不写盘，等答案（[_downloadThen]）
  bool get quitPromptOpen => _quitPrompt != null || _exiting;
  void openQuitPrompt() {
    _quitPrompt = Completer<bool>();
    notifyListeners();
  }

  /// 确认框关上了。[quit] 为真就撤回这次下载：还在下，停止并删掉；刚下完、正等着这个答案，由 [_downloadThen]
  /// 删掉。删完才返回（之后才能关窗口）
  Future<void> closeQuitPrompt(bool quit) async {
    final c = _quitPrompt;
    _quitPrompt = null;
    c?.complete(quit);
    notifyListeners();
    if (!quit) return;
    // 停止下载可能要等一会儿（重试前的等待、正在算 sha256）：这期间再按关闭不再弹框
    _exiting = true;
    try {
      if (downloading) {
        await cancelDownload();
      } else {
        await _quitAfterDownload?.future;
      }
    } finally {
      _exiting = false;
    }
  }

  /// 下载到哪：当前用户"下载"文件夹下的 Gaokun Android\<版本>\（用户看得到，以后可以在"本地文件"里直接选这个目录）。
  /// 拿不到"下载"文件夹时用调用方给的目录
  Future<String> _releaseDir(String fallback) async {
    final base = await downloadsDir?.call();
    if (base == null || base.isEmpty) return fallback;
    final id = variant!.id.replaceAll(RegExp(r'[<>:"/\\|?*\x00-\x1f]'), '_');
    return '$base\\Gaokun Android\\$id';
  }

  /// 下载那次调用的订阅与结果（[cancelDownload] 取消它）
  StreamSubscription<Gk3Event>? _dlSub;
  Completer<CallResult>? _dlDone;
  String _dlDir = '';

  /// 下载中途退出：停止下载（取消订阅：WinBackend 断开连接、不再重试），等它收拾完（.part 关闭），再删掉
  /// 这次下载目录里的安装文件 —— 下载阶段还没有动盘，撤回就是删掉这些文件
  /// 同时调了两次（第二次确认退出）：等同一个取消做完，不能先返回让窗口关掉、文件还没删
  Future<void> cancelDownload() => _cancelling ??= _cancel().whenComplete(() => _cancelling = null);
  Future<void>? _cancelling;

  Future<void> _cancel() async {
    final sub = _dlSub, done = _dlDone;
    if (sub == null || done == null) return;
    await sub.cancel();
    _dlSub = null;
    if (!done.isCompleted) done.complete(_cancelledResult);
    deleteFiles(_dlDir);
  }

  static const _cancelledResult = CallResult([Gk3Record('ERR', {'code': 'cancelled'})], [Gk3Log('!! download cancelled')], 130);

  /// 订阅一次下载，收集结果（同 Gk3Backend.run，只是留着订阅，可以取消）
  Future<CallResult> _download(List<String> args) {
    final records = <Gk3Record>[], logs = <Gk3Log>[];
    var code = -1;
    final done = _dlDone = Completer<CallResult>();
    _dlDir = args[1];
    _dlSub = backend.call('download', args).listen(
      (e) {
        _onEvent(e, span: 40);
        switch (e) {
          case Gk3Record():
            records.add(e);
          case Gk3Log():
            logs.add(e);
          case Gk3Exit():
            code = e.code;
          case Gk3Progress():
            break;
        }
      },
      onError: (Object e) {
        if (!done.isCompleted) done.complete(CallResult(const [Gk3Record('ERR', {'code': 'dl-failed'})], [Gk3Log('!! $e')], 1));
      },
      onDone: () {
        if (!done.isCompleted) done.complete(CallResult(records, logs, code));
      },
    );
    return done.future;
  }

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

  /// 任务开始：状态清零。lastProgress 也清掉：gk3w uninstall 不报进度，不清的话卸载全程显示上一个任务的最后一条
  void _begin(WriteTask t) {
    task = t;
    writing = true;
    percent = 0;
    lastProgress = null;
    log.clear();
    result = null;
    notifyListeners();
  }

  /// 任务结束（成功、失败或出了异常都走这里）：不然 writing / diskBusy 卡在 true，窗口关不掉、关机被挡
  void _end() {
    writing = false;
    diskBusy = false;
    _bitlockerKey = null;
    notifyListeners();
  }

  /// 任务里出了异常（不该有）：写盘中出的算"不知道动没动过盘"（没有 ERR），之前出的算没动。
  /// 异常的内容进日志区（失败页显示的就是它），用户截图时看得到
  CallResult _crashed(Object e) {
    log.add('!! $e');
    logNote?.call('!! $e');
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
          '--release', dir, '--default-os', defaultOs, '--real-disk', ..._bitlockerArgs,
        ], _bitlockerInput);
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
        return ('media', ['--target', 'usb', '--disk', '${t.disk!.num}', '--release', dir, '--livecd', live], null);
      }
      final d = disk!, r = mediaRegion!;
      return ('media', [
        '--target', 'internal', '--disk', '${d.num}', '--region-start', '${r.start}', '--region-end', '${r.end}',
        '--release', dir, '--livecd', live, '--real-disk', ..._bitlockerArgs,
      ], _bitlockerInput);
    });
  }

  /// 最近一次写入的安装介质目标：进度页、完成页、失败页按它措辞（重新检查会清空 [mediaTarget]，这个不清）
  MediaTarget? mediaWritten;

  /// [command]：下载完之后的写盘调用（函数、参数、标准输入）
  Future<bool> _downloadThen(WriteTask t, String releaseDir, (String, List<String>, String?) Function(String dir) command) async {
    _begin(t);
    final local = source == ReleaseSource.local;
    try {
      var dir = localDir;
      if (!local) {
        downloading = true;
        notifyListeners();
        final CallResult dl;
        try {
          dl = await _download([variant!.base, await _releaseDir(releaseDir)]);
        } finally {
          downloading = false;
          _dlSub = null;
        }
        if (!dl.ok) {
          result = dl;
          return false;
        }
        dir = dl.first('RELEASE')?['dir'] ?? releaseDir;
        // 退出确认框还开着时下载结束了：先等答案再写盘。选了退出就当作撤回（删掉刚下完的文件），不写盘；
        // 不然用户点的"退出"没有任何作用，安装照样开始写盘
        final prompt = _quitPrompt;
        if (prompt != null) {
          final after = _quitAfterDownload = Completer<void>();
          try {
            if (await prompt.future) {
              logNote?.call('download cancelled by the user after it finished');
              deleteFiles(dir);
              result = _cancelledResult;
              return false;
            }
          } finally {
            after.complete();
            _quitAfterDownload = null;
          }
        }
      }
      final (fn, args, input) = command(dir);
      await _startDiskWrite();
      result = await Gk3Backend.collect(input == null ? backend.call(fn, args) : backend.callWithInput(fn, args, input),
          (e) => _onEvent(e, base: local ? 0 : 40, span: local ? 100 : 60));
      // 装好了：下载的文件删不删由用户在完成页上选（退出时删）；本地文件是用户自己的，不动；失败时留着，下次续传
      if (result!.ok && t == WriteTask.install && !local) {
        downloadedDir = dir;
        deleteOnExit = false;
      }
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
      final args = ['--disk', '${disk!.num}', '--real-disk', ..._bitlockerArgs], input = _bitlockerInput;
      result = await Gk3Backend.collect(
          input == null ? backend.call('uninstall', args) : backend.callWithInput('uninstall', args, input), _onEvent);
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
