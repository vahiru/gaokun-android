import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:gk3_common/ui/theme.dart';

import '../app.dart';
import '../session.dart';
import 'run_pages.dart';

/// 和 Windows 原生窗口打交道（windows/runner/flutter_window.cpp 里的 gk3/window 通道）：窗口没有系统标题栏，
/// 最小化、最大化、关闭、选文件夹由界面发起；拖动由系统做（原生按 [setCaption] 给的几何把顶上一行当作标题栏）。
/// 浏览器预览与测试里没有这个通道，调用什么都不做。
class WindowControl {
  static const _ch = MethodChannel('gk3/window');
  static (bool, bool, bool)? _busy;

  /// 窗口是否最大化（原生在 WM_SIZE 时告诉界面：自绘按钮、双击标题栏、Win+↑ 都会变）
  static final maximized = ValueNotifier<bool>(false);

  /// 接收原生发来的调用：closeRequested（下载中途按了 Alt+F4 或任务栏上的关闭）交给 [onCloseRequested]
  static void init({required Future<void> Function() onCloseRequested}) {
    _ch.setMethodCallHandler((call) async {
      switch (call.method) {
        case 'closeRequested':
          await onCloseRequested();
        case 'maximized':
          maximized.value = call.arguments == true;
      }
      return null;
    });
  }

  static Future<T?> _call<T>(String m, [Object? arg]) async {
    try {
      return await _ch.invokeMethod<T>(m, arg);
    } on MissingPluginException {
      return null;
    }
  }

  static Future<void> minimize() => _call<void>('minimize');
  static Future<void> close() => _call<void>('close');
  static Future<void> toggleMaximize() => _call<void>('maximize');

  /// 界面已经问过、收拾好了：直接关（写盘期间原生仍然不关）
  static Future<void> forceClose() => _call<void>('forceClose');

  /// 告诉原生标题栏在哪（界面画布的逻辑尺寸、顶上一行的高度、右上角按钮的宽度）：原生据此把那一行当作标题栏，
  /// 鼠标、触摸、手写笔都由系统拖动（原先模拟鼠标按下，触摸拖不动）
  static void setCaption() =>
      _call<void>('setCaption', {'canvasW': kCanvas.width, 'canvasH': kCanvas.height, 'strip': DragStrip.height, 'buttons': WindowButtons.width});

  /// 按住顶上一行时调用：把这次按下交给 Windows 当作拖动标题栏
  static Future<void> startDrag() => _call<void>('startDrag');

  /// [busy]（gk3w 写盘期间）：原生窗口拒绝关闭，阻止关机（Windows 显示 [shutdown] 作为原因）。
  /// [awake]（整个任务期间，含下载）：电源请求阻止系统因空闲进入睡眠、保持屏幕开启（[power] 是 powercfg /requests
  /// 中显示的原因）—— 这台是现代待机设备，熄屏后桌面程序会被挂起，下载会停住。结束后撤销
  /// [confirmClose]（下载期间，或退出前要删下载的文件）：关闭窗口先交给界面（原生发 closeRequested，界面处理完 [forceClose]）
  static void setBusy(bool busy, {bool awake = false, bool confirmClose = false, String power = '', String shutdown = ''}) {
    final state = (busy, awake || busy, confirmClose);
    if (_busy == state) return;
    _busy = state;
    _pending = _call<void>('setBusy',
        {'busy': busy, 'awake': awake || busy, 'confirmClose': confirmClose, 'power': power, 'shutdown': shutdown});
  }

  /// 最近一次 setBusy 的原生调用（Session.beforeDiskWrite 等它返回）
  static Future<void>? _pending;

  /// 等界面按最新状态发出 setBusy、原生窗口处理完：下一帧 WindowButtons 重建时发出，再等那次调用返回
  static Future<void> settleBusy() async {
    await WidgetsBinding.instance.endOfFrame;
    await _pending;
  }

  /// 当前用户的"下载"文件夹（Windows 的已知文件夹）；没有原生窗口时为 null
  static Future<String?> downloadsDir() => _call<String>('downloadsDir');

  /// 系统的"选择文件夹"对话框（IFileOpenDialog）；取消或没有原生窗口时为 null
  static Future<String?> pickFolder({String? title}) => _call<String>('pickFolder', title);
}

/// 画布最上面一行：按住拖动窗口。每一页这一行都是空的（侧栏与正文的上边距都不小于 32）
class DragStrip extends StatelessWidget {
  const DragStrip({super.key});
  static const height = 32.0;

  @override
  Widget build(BuildContext context) => GestureDetector(
        behavior: HitTestBehavior.translucent,
        // 原生把这一行当作标题栏时，按下根本到不了这里；没有接上原生（旧的 runner）时用鼠标还能拖
        onPanStart: (_) => WindowControl.startDrag(),
        onDoubleTap: WindowControl.toggleMaximize,
        child: const _CaptionReporter(),
      );
}

/// 挂上之后把标题栏的几何告诉原生一次（[WindowControl.setCaption]）
class _CaptionReporter extends StatefulWidget {
  const _CaptionReporter();
  @override
  State<_CaptionReporter> createState() => _CaptionReporterState();
}

class _CaptionReporterState extends State<_CaptionReporter> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => WindowControl.setCaption());
  }

  @override
  Widget build(BuildContext context) => const SizedBox.expand();
}

/// 关闭窗口（自绘的关闭键，或原生转来的 Alt+F4 / 任务栏关闭）：
/// - 下载中途：先问；确认退出就停止下载、删掉已下载的文件（下载阶段还没有动盘，只有这些文件要撤回），再关窗口
/// - 装好之后在完成页勾了"退出时删除下载的安装文件"：先删，再关
Future<void> handleCloseRequest(BuildContext context, Session s) async {
  if (!s.downloading) {
    if (!s.exitCleanupPending) return WindowControl.close();
    s.runExitCleanup();
    return WindowControl.forceClose();
  }
  // 关闭键在 MaterialApp 的 builder 里、Navigator 的外面：弹框要用导航器自己的 context
  final ctx = kNavigatorKey.currentContext;
  if (ctx == null) return;
  final l = ctx.l;
  final quit = await showDialog<bool>(
    context: ctx,
    builder: (c) => AlertDialog(
      title: Text(l.quitDownloadTitle),
      content: Text(l.quitDownloadBody),
      actions: [
        TextButton(onPressed: () => Navigator.pop(c, false), child: Text(l.quitDownloadContinue)),
        TextButton(
          onPressed: () => Navigator.pop(c, true),
          style: TextButton.styleFrom(foregroundColor: c.cs.error),
          child: Text(l.quitDownloadExit),
        ),
      ],
    ),
  );
  // 问的时候下载可能已经结束、开始写盘了：那就不关（写盘期间原生也不会关）
  if (quit != true || !s.downloading) return;
  await s.cancelDownload();
  await WindowControl.forceClose();
}

/// 右上角的最小化、最大化与关闭（Windows 11 的尺寸；关闭键悬停是 #C42B1C 底、白字）。
/// gk3w 写盘时关闭键禁用；下载期间按关闭先问（[handleCloseRequest]）
class WindowButtons extends StatelessWidget {
  const WindowButtons({super.key});
  static const width = 3 * _CaptionButton.width;

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    WindowControl.setBusy(
      s.diskBusy,
      awake: s.writing,
      confirmClose: s.downloading || s.exitCleanupPending,
      power: runTitle(l, s.task),
      shutdown: switch (s.task) {
        WriteTask.install => l.shutdownReasonInstall,
        WriteTask.uninstall => l.shutdownReasonUninstall,
        WriteTask.media => l.shutdownReasonMedia,
      },
    );
    return Row(mainAxisSize: MainAxisSize.min, children: [
      _CaptionButton(icon: Icons.remove, label: context.l.winMinimize, onPressed: WindowControl.minimize),
      ValueListenableBuilder<bool>(
        valueListenable: WindowControl.maximized,
        builder: (context, max, _) => _CaptionButton(
          icon: max ? Icons.filter_none : Icons.crop_square,
          iconSize: max ? 14 : 16,
          label: max ? l.winRestore : l.winMaximize,
          onPressed: WindowControl.toggleMaximize,
        ),
      ),
      _CaptionButton(
        icon: Icons.close,
        label: context.l.winClose,
        danger: true,
        onPressed: s.diskBusy ? null : () => handleCloseRequest(context, s),
      ),
    ]);
  }
}

class _CaptionButton extends StatefulWidget {
  const _CaptionButton({required this.icon, required this.label, this.onPressed, this.danger = false, this.iconSize = 18});
  static const width = 46.0;
  final IconData icon;
  final double iconSize;
  final String label;
  final VoidCallback? onPressed;
  final bool danger;

  @override
  State<_CaptionButton> createState() => _CaptionButtonState();
}

class _CaptionButtonState extends State<_CaptionButton> {
  var _hover = false;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs;
    final on = widget.onPressed != null;
    final bg = !on || !_hover ? Colors.transparent : (widget.danger ? const Color(0xFFC42B1C) : cs.onSurface.withValues(alpha: 0.08));
    final fg = !on ? cs.onSurface.withValues(alpha: 0.38) : (_hover && widget.danger ? Colors.white : cs.onSurfaceVariant);
    return Semantics(
      button: true,
      label: widget.label,
      child: MouseRegion(
        onEnter: (_) => setState(() => _hover = true),
        onExit: (_) => setState(() => _hover = false),
        child: GestureDetector(
          onTap: widget.onPressed,
          child: Container(width: _CaptionButton.width, height: DragStrip.height, color: bg, child: Icon(widget.icon, size: widget.iconSize, color: fg)),
        ),
      ),
    );
  }
}
