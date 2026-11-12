import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model.dart';
import 'bits.dart';
import 'messages.dart';

/// 输入框的占位文字：恢复密钥的样子（8 组、每组 6 位）
const kRecoveryKeyHint = '000000-000000-000000-000000-000000-000000-000000-000000';

/// 输入并校验 BitLocker 恢复密钥（确认页；检查页上关闭 Secure Boot 之前暂停保护也用它）。
/// 密钥交给 gk3w 与系统盘的恢复密钥比对（Session.verifyBitlockerKey），界面上不显示系统保存的密钥。
/// [suspendReboots] 大于 0：按钮是"校验并暂停保护"，通过之后暂停保护这么多次重启
class BitlockerKeyBox extends StatefulWidget {
  const BitlockerKeyBox({super.key, required this.text, this.suspendReboots = 0});
  final String text;
  final int suspendReboots;

  @override
  State<BitlockerKeyBox> createState() => _BitlockerKeyBoxState();
}

class _BitlockerKeyBoxState extends State<BitlockerKeyBox> {
  final _key = TextEditingController();

  /// 校验结果那一行：校验完滚动到看得见（方框在页面下方时，结果可能被底栏挡住）
  final _statusKey = GlobalKey();

  @override
  void dispose() {
    _key.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, cs = context.cs, gk = context.gk;
    final ids = s.preflight?.bitlockerKeyIds;
    final canVerify = !s.bitlockerChecking && normalizeRecoveryKey(_key.text) != null;
    Future<void> submit() async {
      if (!canVerify) return;
      await s.verifyBitlockerKey(_key.text, suspendReboots: widget.suspendReboots);
      if (!mounted) return;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        final c = _statusKey.currentContext;
        if (c != null && c.mounted) {
          Scrollable.ensureVisible(c, duration: const Duration(milliseconds: 200), alignmentPolicy: ScrollPositionAlignmentPolicy.keepVisibleAtEnd);
        }
      });
    }

    final fg = gk.onWarningContainer;
    final status = s.bitlockerChecking
        ? Row(children: [
            const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)),
            const SizedBox(width: 12),
            Text(l.blChecking, style: context.tt.bodyLarge!.copyWith(color: fg)),
          ])
        : s.bitlockerVerified
            ? Row(children: [
                Icon(Icons.check_circle, color: gk.success),
                const SizedBox(width: 12),
                Text(l.blOk, style: context.tt.bodyLarge!.copyWith(color: fg)),
              ])
            : s.bitlockerErr != null
                ? Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Icon(Icons.cancel, color: cs.error),
                    const SizedBox(width: 12),
                    Expanded(child: Text(errText(l, s.bitlockerErr!), style: context.tt.bodyLarge!.copyWith(color: fg))),
                  ])
                : null;
    return NoteBox(
      warn: true,
      text: widget.text,
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        if (ids != null && ids.isNotEmpty) ...[
          Text(l.blKeyIds(ids.join(' / ')), style: context.tt.bodyLarge!.copyWith(color: fg)),
          const SizedBox(height: 12),
        ] else
          const SizedBox(height: 4),
        Row(children: [
          Expanded(
            child: TextField(
              controller: _key,
              enabled: !s.bitlockerChecking,
              keyboardType: TextInputType.number,
              autocorrect: false,
              enableSuggestions: false,
              // 数字、连字符与空格（Microsoft 账户页面上复制来的密钥带连字符，有时带空格）
              inputFormatters: [FilteringTextInputFormatter.allow(RegExp(r'[0-9 \-]')), LengthLimitingTextInputFormatter(64)],
              style: context.tt.bodyLarge!.copyWith(fontFeatures: const [FontFeature.tabularFigures()]),
              decoration: InputDecoration(
                labelText: l.blKeyLabel,
                hintText: kRecoveryKeyHint,
                filled: true,
                fillColor: cs.surface,
                border: const OutlineInputBorder(),
                prefixIcon: const Icon(Icons.key_outlined),
              ),
              onChanged: (_) {
                s.clearBitlockerKey();
                setState(() {});
              },
              onSubmitted: (_) => submit(),
            ),
          ),
          const SizedBox(width: 12),
          Btn(widget.suspendReboots > 0 ? l.blVerifySuspend : l.blVerify, kind: BtnKind.secondary, onPressed: canVerify ? submit : null),
        ]),
        if (status != null) ...[const SizedBox(height: 12), KeyedSubtree(key: _statusKey, child: status)],
      ]),
    );
  }
}
