// 写盘前的风险确认（用户 2026-10-06）：
//   "分区之前，你用最醒目的方式告诉用户，这玩意儿可能会损坏数据。然后强迫用户阅读这一段话。
//    告诉他们备份。告诉他们在此处操作硬盘可能会带来不可逆的风险。"
//
// ★ 三条写盘的路都先过这一页（RiskKind）：
//     安装（整盘清空 / 双系统 / 重新安装，gk3_apply）  选项 → 本页 → 确认页（按住 2 秒）→ 开始
//     缩分区（gk3_shrink）                              方式 → 本页 → 缩分区页（按住 2 秒）
//     手动调整磁盘（gk3_part_*）                        方式 → 本页 → 调整磁盘页（每一步按住 2 秒）
//   接着跟一个还在跑的写盘任务（GUI-11 的 gk3_job_follow）不过这一页：盘已经在写了，拦它没有意义。
//   Session 再守一道（Session.riskAcked）：没在这一页确认过的路，install / shrink / editDisk 一个字节都不写。
// ★ 门槛（全部满足，"继续"才可点；没满足时按钮旁边写明还差什么）：
//     1. 正文滚到底（正文短到不用滚时自动算读完）
//     2. 页面打开满 [RiskPage.readSeconds] 秒 —— 与 1 同时要求，不是二选一：滚得再快也要停够时间
//     3. 勾选"我已备份重要数据，并理解此操作可能造成不可逆的数据丢失"
//     4. 整盘清空、或者在有 Windows 的盘上手动调整（那里能删掉它）：输入确认词 ERASE（大小写均可）
// ⚠️ 确认词中英文界面都是 ASCII 的 ERASE：live 里没有输入法，软键盘与实体键盘都只出 ASCII
//    （screens_source.dart 的 HiddenNetPage 注释）—— 中文界面要人输"清除"，就是一个永远过不去的门。
// ★ 不和确认页重复：这一页确认的是"知道风险、备份过了"；BitLocker 恢复密钥、开机默认系统这些
//   与具体方案绑定的选择仍在确认页（这一页只提一句"确认页会要你确认恢复密钥"）。
import 'dart:async';

import 'package:flutter/material.dart';

import '../app.dart';
import '../session.dart';
import 'screens_finish.dart' show isOnekey;
import 'soft_keyboard.dart';
import 'theme.dart';
import 'widgets.dart';

/// 风险确认页的路由名：失败页"返回修改"按它跳过这一页、直接回到选项页
const kRiskRoute = 'risk';

/// 测试用的把手
const kRiskScrollKey = ValueKey('risk-scroll');
const kRiskCheckKey = ValueKey('risk-check');
const kRiskWordKey = ValueKey('risk-word');

class RiskPage extends StatefulWidget {
  const RiskPage({super.key, required this.kind, required this.next});
  final RiskKind kind;

  /// 确认之后进的页。安装：确认页（本页留在栈里 —— 从确认页返回，回到这里，已经满足的条件不用再来一遍）。
  /// 缩分区 / 调整磁盘：那一页 pop 时本页跟着 pop、把结果交给方式页（方式页 await 的是"做完了没有"）——
  /// ⚠️ 所以这两种的 [next] 只许 pop 自己、不许 pushReplacement
  final Widget next;

  /// 至少停留多久（秒）
  static const readSeconds = 15;

  static Future<T?> open<T>(BuildContext context, RiskKind kind, Widget next) => go<T>(context, RiskPage(kind: kind, next: next), name: kRiskRoute);

  /// 要不要再输入确认词：整盘清空（盘上的一切都没了）；手动调整而盘上有 Windows（那一页能删掉它）
  static bool needsWord(Session s, RiskKind kind) => switch (kind) {
        RiskKind.install => s.mode == Mode.wipe,
        RiskKind.edit => s.disk?.parts.any((p) => p.os == 'windows' || p.os == 'winre' || isOnekey(p)) ?? false,
        RiskKind.shrink => false,
      };

  @override
  State<RiskPage> createState() => _RiskPageState();
}

class _RiskPageState extends State<RiskPage> {
  final _scroll = ScrollController();
  final _word = TextEditingController();
  Timer? _timer;
  int _left = RiskPage.readSeconds;
  bool _read = false, _checked = false;

  @override
  void initState() {
    super.initState();
    _scroll.addListener(_onScroll);
    _word.addListener(() => setState(() {}));
    _timer = Timer.periodic(const Duration(seconds: 1), (t) {
      if (!mounted) return;
      setState(() => _left--);
      if (_left <= 0) t.cancel();
    });
    // 正文短到不用滚（maxScrollExtent = 0）：第一帧之后就算读完
    WidgetsBinding.instance.addPostFrameCallback((_) => _onScroll());
  }

  @override
  void dispose() {
    _timer?.cancel();
    _scroll.dispose();
    _word.dispose();
    super.dispose();
  }

  /// 读到底一次就算（往回滚不撤销）
  void _onScroll() {
    if (_read || !mounted || !_scroll.hasClients) return;
    final p = _scroll.position;
    if (p.maxScrollExtent - p.pixels <= 8) setState(() => _read = true);
  }

  bool get _wordOk => _word.text.trim().toUpperCase() == context.l.riskWord.toUpperCase();

  /// 还差的条件（空 = 可以继续）
  List<String> _missing(bool needWord) {
    final l = context.l;
    return [
      if (!_read) l.riskNeedScroll,
      if (_left > 0) l.riskNeedWait('$_left'),
      if (!_checked) l.riskNeedCheck,
      if (needWord && !_wordOk) l.riskNeedWord(l.riskWord),
    ];
  }

  Future<void> _continue() async {
    final s = context.session;
    s.ackRisk(widget.kind);
    if (widget.kind == RiskKind.install) {
      go(context, widget.next);
      return;
    }
    final r = await go<Object?>(context, widget.next);
    if (mounted) Navigator.pop(context, r);
  }

  /// 正文：(图标, 小标题, 内容, 是不是这一种方式特有的 —— 特有的用 error 色)
  List<(IconData, String, String, bool)> _sections() {
    final s = context.session, l = context.l;
    final onekey = s.disk?.parts.where(isOnekey).firstOrNull;
    final win = (Icons.key_outlined, l.riskWinHead, [l.riskWinBody, if (s.needBitlockerKey) l.riskWinBitlocker].join('\n\n'), true);
    return [
      (Icons.science_outlined, l.riskExpHead, l.riskExpBody, false),
      (Icons.report_outlined, l.riskDiskHead, l.riskDiskBody, true),
      ...switch (widget.kind) {
        RiskKind.install => switch (s.mode) {
            Mode.wipe => [
                // 华为一键恢复分区：和确认页同一句话（GUI-18）
                (Icons.delete_forever, l.riskWipeHead, [l.riskWipeBody, if (onekey != null) l.confirmOnekeyWarn(onekey.label)].join('\n\n'), true),
              ],
            Mode.alongside => [(Icons.call_split, l.riskAlongHead, l.riskAlongBody, true), if (s.dual) win],
            Mode.reinstall => [(Icons.system_update_alt, l.riskReinstallHead, s.keepData ? l.riskReinstallKeep : l.riskReinstallWipe, true), if (s.dual) win],
            null => const <(IconData, String, String, bool)>[],
          },
        RiskKind.shrink => [(Icons.compress, l.riskShrinkHead, l.riskShrinkBody, true)],
        RiskKind.edit => [(Icons.construction, l.riskEditHead, l.riskEditBody, true)],
      },
      (Icons.backup_outlined, l.riskBackupHead, l.riskBackupBody, true),
      (Icons.power, l.riskPowerHead, l.riskPowerBody, false),
      (Icons.block, l.riskNoCancelHead, widget.kind == RiskKind.install ? l.riskNoCancelInstall : l.riskNoCancelStep, false),
    ];
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, cs = context.cs, tt = context.tt;
    final needWord = RiskPage.needsWord(s, widget.kind);
    final missing = _missing(needWord);
    final ready = missing.isEmpty;
    final typed = _word.text.trim().toUpperCase();
    return StepPage(
      step: widget.kind == RiskKind.install ? Gk3Step.confirm : Gk3Step.mode,
      danger: true,
      title: l.riskTitle,
      subtitle: l.riskSub,
      bottom: Row(children: [
        Btn(l.btnBack, kind: BtnKind.text, icon: Icons.arrow_back, onPressed: () => Navigator.pop(context)),
        const SizedBox(width: 16),
        Expanded(
          child: ready
              ? const SizedBox.shrink()
              : Text(
                  l.riskMissing(missing.join(l.riskSep)),
                  textAlign: TextAlign.end,
                  maxLines: 2,
                  overflow: TextOverflow.ellipsis,
                  style: tt.bodyLarge!.copyWith(color: cs.error),
                ),
        ),
        const SizedBox(width: 16),
        Btn(l.riskContinue, kind: BtnKind.danger, icon: Icons.arrow_forward, onPressed: ready ? _continue : null),
      ]),
      child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        // ── 正文：error 色描边的框，滚到底才算读完 ──
        Expanded(
          child: Container(
            decoration: BoxDecoration(
              color: cs.surfaceContainerLow,
              borderRadius: BorderRadius.circular(20),
              border: Border.all(color: cs.error, width: 2),
            ),
            clipBehavior: Clip.antiAlias,
            child: Stack(children: [
              NotificationListener<ScrollMetricsNotification>(
                onNotification: (_) {
                  _onScroll();
                  return false;
                },
                child: Scrollbar(
                  controller: _scroll,
                  thumbVisibility: true,
                  child: SingleChildScrollView(
                    key: kRiskScrollKey,
                    controller: _scroll,
                    padding: const EdgeInsets.fromLTRB(24, 22, 28, 22),
                    child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                      for (final (icon, head, body, special) in _sections()) ...[
                        Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                          Icon(icon, size: 28, color: special ? cs.error : cs.onSurfaceVariant),
                          const SizedBox(width: 14),
                          Expanded(
                            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                              Text(head, style: tt.titleMedium!.copyWith(color: special ? cs.error : cs.onSurface)),
                              const SizedBox(height: 4),
                              Text(body, style: tt.bodyLarge),
                            ]),
                          ),
                        ]),
                        const SizedBox(height: 20),
                      ],
                      Center(child: Text(l.riskEnd, style: tt.bodyMedium)),
                      // 让"还差：读到最后"的提示条不压住最后一行
                      const SizedBox(height: 36),
                    ]),
                  ),
                ),
              ),
              if (!_read)
                Positioned(
                  left: 0,
                  right: 0,
                  bottom: 12,
                  child: IgnorePointer(
                    child: Center(
                      child: Material(
                        color: cs.error,
                        shape: const StadiumBorder(),
                        elevation: 2,
                        child: Padding(
                          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                          child: Row(mainAxisSize: MainAxisSize.min, children: [
                            Icon(Icons.arrow_downward, size: 20, color: cs.onError),
                            const SizedBox(width: 8),
                            Text(l.riskScrollHint, style: tt.labelLarge!.copyWith(color: cs.onError)),
                          ]),
                        ),
                      ),
                    ),
                  ),
                ),
            ]),
          ),
        ),
        const SizedBox(width: 24),
        // ── 确认：勾选，（整盘清空时）再输入确认词 ──
        SizedBox(
          width: 420,
          // 可滚：英文、或者测试字体（每个字都是方块）时放不下也不溢出
          child: ListView(children: [
            Material(
              color: _checked ? cs.errorContainer : cs.surfaceContainerHigh,
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16), side: BorderSide(color: cs.error, width: 2)),
              clipBehavior: Clip.antiAlias,
              child: CheckboxListTile(
                key: kRiskCheckKey,
                value: _checked,
                onChanged: (v) => setState(() => _checked = v ?? false),
                controlAffinity: ListTileControlAffinity.leading,
                activeColor: cs.error,
                checkColor: cs.onError,
                contentPadding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                title: Text(l.riskCheck, style: tt.titleMedium!.copyWith(color: _checked ? cs.onErrorContainer : cs.onSurface)),
              ),
            ),
            if (needWord) ...[
              const SizedBox(height: 18),
              Text(l.riskWordPrompt(l.riskWord), style: tt.bodyLarge),
              const SizedBox(height: 12),
              TextField(
                key: kRiskWordKey,
                controller: _word,
                style: tt.titleLarge,
                autocorrect: false,
                enableSuggestions: false,
                onSubmitted: (_) => ready ? _continue() : null,
                decoration: InputDecoration(
                  labelText: l.riskWordLabel,
                  prefixIcon: const Icon(Icons.keyboard_outlined),
                  // 打到一半（还是 ERASE 的前缀）不报错；打错了才红
                  errorText: typed.isNotEmpty && !l.riskWord.toUpperCase().startsWith(typed) ? l.riskWordWrong(l.riskWord) : null,
                ),
              ),
              const SizedBox(height: 12),
              SoftKeyboard(controller: _word, onDone: ready ? _continue : null),
            ],
          ]),
        ),
      ]),
    );
  }
}
