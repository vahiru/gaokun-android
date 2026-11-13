import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'theme.dart';

/// MD3 的四种按钮 + 危险操作（error 角色）
enum BtnKind { primary, tonal, secondary, text, danger }

class Btn extends StatelessWidget {
  const Btn(this.label, {super.key, this.onPressed, this.kind = BtnKind.primary, this.icon, this.autofocus = false});
  final String label;
  final VoidCallback? onPressed;
  final BtnKind kind;
  final IconData? icon;
  final bool autofocus;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs;
    final child = Text(label);
    final ic = icon == null ? null : Icon(icon);
    switch (kind) {
      case BtnKind.primary:
        return ic == null
            ? FilledButton(onPressed: onPressed, autofocus: autofocus, child: child)
            : FilledButton.icon(onPressed: onPressed, autofocus: autofocus, icon: ic, label: child);
      case BtnKind.tonal:
        return ic == null
            ? FilledButton.tonal(onPressed: onPressed, autofocus: autofocus, child: child)
            : FilledButton.tonalIcon(onPressed: onPressed, autofocus: autofocus, icon: ic, label: child);
      case BtnKind.secondary:
        return ic == null
            ? OutlinedButton(onPressed: onPressed, autofocus: autofocus, child: child)
            : OutlinedButton.icon(onPressed: onPressed, autofocus: autofocus, icon: ic, label: child);
      case BtnKind.text:
        return ic == null
            ? TextButton(onPressed: onPressed, autofocus: autofocus, child: child)
            : TextButton.icon(onPressed: onPressed, autofocus: autofocus, icon: ic, label: child);
      case BtnKind.danger:
        final st = FilledButton.styleFrom(backgroundColor: cs.error, foregroundColor: cs.onError);
        return ic == null
            ? FilledButton(onPressed: onPressed, autofocus: autofocus, style: st, child: child)
            : FilledButton.icon(onPressed: onPressed, autofocus: autofocus, style: st, icon: ic, label: child);
    }
  }
}

/// 可选的卡片（MD3 的可选卡：未选是描边卡，选中是 secondary-container + 单选指示）。
/// 做不到时【禁用 + 写明原因】（C 版 README："'为什么没有这个选项'本身就是用户需要的信息"），
/// 不藏起来；禁用时内容按 MD3 降到 38%，但原因那一行不降 —— 它正是要让人读的。
class ChoiceCard extends StatelessWidget {
  const ChoiceCard({
    super.key,
    required this.title,
    this.body,
    this.warn,
    this.good,
    this.reason,
    this.note,
    this.icon,
    this.trailing,
    this.extra,
    this.selected = false,
    this.onTap,
    this.danger = false,
    this.dense = false,
  });

  final String title;
  final String? body, warn, good;

  /// 紧凑的一行（删除分区页的分区列表：一块盘的分区要一屏看完 —— 出厂布局 7 个分区加空闲区）
  final bool dense;

  /// 不为空 ⇒ 禁用，并把原因写在卡片上
  final String? reason;

  /// 与 reason 同样的样式，但【不】禁用：可以选，只是有件事要先知道
  /// （例如安装器正从这块盘上运行 —— 能装双系统，不能整盘清空）
  final String? note;
  final IconData? icon;
  final Widget? trailing, extra;
  final bool selected, danger;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs, tt = context.tt, gk = context.gk;
    final enabled = reason == null && onTap != null;
    final disabled = reason != null;
    final bg = selected ? (danger ? cs.errorContainer : cs.secondaryContainer) : cs.surfaceContainerLow;
    final onBg = selected ? (danger ? cs.onErrorContainer : cs.onSecondaryContainer) : cs.onSurface;
    final edge = selected ? (danger ? cs.error : cs.primary) : cs.outlineVariant;
    Widget dim(Widget w) => disabled ? Opacity(opacity: 0.38, child: w) : w;
    final single = body == null && warn == null && good == null && extra == null && reason == null && note == null;
    return Card(
      color: bg,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: BorderSide(color: edge, width: selected ? 2 : 1),
      ),
      child: InkWell(
        onTap: enabled ? onTap : null,
        child: ConstrainedBox(
          constraints: BoxConstraints(minHeight: dense ? 56 : 72),
          child: Padding(
            padding: dense ? const EdgeInsets.fromLTRB(16, 8, 16, 8) : const EdgeInsets.fromLTRB(20, 18, 20, 18),
            // 只有一行标题时垂直居中（WiFi 列表那种）；多行时顶端对齐
            child: Row(
              crossAxisAlignment: single ? CrossAxisAlignment.center : CrossAxisAlignment.start,
              children: [
                if (icon != null) ...[
                  dim(
                    Container(
                      width: 48,
                      height: 48,
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        color: danger ? (selected ? cs.error : cs.errorContainer) : (selected ? cs.primary : cs.primaryContainer),
                      ),
                      child: Icon(
                        icon,
                        color: danger ? (selected ? cs.onError : cs.onErrorContainer) : (selected ? cs.onPrimary : cs.onPrimaryContainer),
                      ),
                    ),
                  ),
                  const SizedBox(width: 16),
                ],
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      dim(
                        Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            Text(title, style: (dense ? tt.titleMedium : tt.titleLarge)!.copyWith(color: onBg)),
                            if (warn != null) ...[
                              const SizedBox(height: 4),
                              Text(warn!, style: tt.bodyLarge!.copyWith(color: selected && danger ? onBg : cs.error)),
                            ],
                            if (good != null) ...[const SizedBox(height: 4), Text(good!, style: tt.bodyLarge!.copyWith(color: gk.success))],
                            if (body != null) ...[
                              const SizedBox(height: 4),
                              Text(body!, style: tt.bodyMedium!.copyWith(color: selected ? onBg.withValues(alpha: 0.8) : cs.onSurfaceVariant)),
                            ],
                            if (extra != null) ...[const SizedBox(height: 12), extra!],
                          ],
                        ),
                      ),
                      for (final (t, c) in [if (reason != null) (reason!, cs.onSurfaceVariant), if (note != null) (note!, gk.warning)]) ...[
                        const SizedBox(height: 10),
                        Row(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Icon(Icons.info_outline, size: 20, color: c),
                            const SizedBox(width: 8),
                            Expanded(
                              child: Text(t, style: tt.bodyMedium!.copyWith(color: c)),
                            ),
                          ],
                        ),
                      ],
                    ],
                  ),
                ),
                if (trailing != null) ...[const SizedBox(width: 16), trailing!],
                if (trailing == null && onTap != null) ...[
                  const SizedBox(width: 16),
                  dim(
                    Icon(
                      selected ? Icons.radio_button_checked : Icons.radio_button_unchecked,
                      color: selected ? (danger ? cs.error : cs.primary) : cs.onSurfaceVariant,
                    ),
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}

/// 布局条上的一个分区：起点、大小（MiB）与系统（gk3_probe 的 PART os=，决定颜色，见 [DiskBar.colorOf]）
typedef DiskBarPart = ({int start, int sizeMib, String os});

/// 布局条上的一块空闲区：起点与大小（MiB）
typedef DiskBarFree = ({int start, int sizeMib});

/// 一块盘的布局条：分区按大小占宽度，颜色按系统（都取自 MD3 的角色 + 两个自定义色）。
/// 只吃起点 / 大小 / 系统这几个数，不认识应用自己的磁盘模型（LiveCD：live 的 widgets.dart 的 diskBar() 负责转换）
class DiskBar extends StatelessWidget {
  const DiskBar({super.key, required this.parts, this.free = const [], this.height = 20, this.highlightFree, this.muted = false});
  final List<DiskBarPart> parts;
  final List<DiskBarFree> free;
  final double height;

  /// 强调 [free] 里的第几块（双系统时 Android 要装进去的那块）；null 或不在范围内 = 不强调
  final int? highlightFree;

  /// 只突出空闲区与 Android：已有系统的分区一律画成暗灰，没选中的空闲区画成淡的 success。
  /// 大片的 Windows 分区按 [colorOf] 上色时太抢眼（Windows 安装器的"选位置"页）
  final bool muted;

  /// [parts] 里 os 为它的一段：将要创建的分区（Windows 安装器写入本机的 GK3LIVE），与强调的空闲区同色
  static const planned = 'planned';

  static Color colorOf(BuildContext context, String os) {
    final cs = context.cs, gk = context.gk;
    return switch (os) {
      'windows' => cs.primary,
      'winre' || 'msr' => cs.primaryContainer,
      'esp' || 'fat' => gk.warning,
      'android' || planned => gk.success,
      'linux' || 'luks' => cs.tertiary,
      _ => cs.outline,
    };
  }

  @override
  Widget build(BuildContext context) {
    final cs = context.cs, gk = context.gk;
    final segs = <(int, int, Color, int)>[]; // start, size, color, 最小宽度（千分之几）
    for (final p in parts) {
      final keep = p.os == 'android' || p.os == planned;
      // 将要创建的分区画宽一些：2 GiB 的 GK3LIVE 在 476 GiB 的盘上按比例只有几个像素，看不出位置
      segs.add((p.start, p.sizeMib, muted && !keep ? cs.surfaceContainerHighest : colorOf(context, p.os), p.os == planned ? 24 : 4));
    }
    for (final (i, f) in free.indexed) {
      // 高亮用明亮的 success（与 Android 分区同色："这块将是 Android 的"）；深色的 container 在深色背景上像个洞
      final idle = muted ? gk.success.withValues(alpha: 0.35) : cs.surfaceContainerHighest;
      segs.add((f.start, f.sizeMib, i == highlightFree ? gk.success : idle, 4));
    }
    segs.sort((a, b) => a.$1.compareTo(b.$1));
    final total = segs.fold<int>(0, (a, s) => a + s.$2);
    if (total == 0) {
      return Container(
        height: height,
        decoration: BoxDecoration(color: cs.surfaceContainerHighest, borderRadius: BorderRadius.circular(height / 2)),
      );
    }
    return ClipRRect(
      borderRadius: BorderRadius.circular(height / 2),
      child: SizedBox(
        height: height,
        child: Row(
          children: [
            for (final (i, s) in segs.indexed)
              Expanded(
                // 小分区至少占一点点宽度，不然 300 MiB 的 ESP 在 476 GiB 的盘上看不见
                flex: (s.$2 * 1000 ~/ total).clamp(s.$4, 1000),
                child: Container(
                  margin: EdgeInsets.only(right: i == segs.length - 1 ? 0 : 2),
                  color: s.$3,
                ),
              ),
          ],
        ),
      ),
    );
  }
}

/// 最后一步：按住 2 秒（C 版 README："触摸屏误触太容易，而那一步不可撤销"）。
/// 键盘上按住回车 / 空格也行 —— 触摸坏了不能变砖（stage7-live-installer.md §3）。
/// 样子是 MD3 的胶囊按钮（error-container），按住时 error 色从左往右填满。
class HoldToConfirm extends StatefulWidget {
  const HoldToConfirm({super.key, required this.idle, required this.busy, required this.onConfirmed, this.enabled = true});
  final String idle;
  final String Function(int pct) busy;
  final VoidCallback onConfirmed;
  final bool enabled;

  @override
  State<HoldToConfirm> createState() => _HoldToConfirmState();
}

class _HoldToConfirmState extends State<HoldToConfirm> with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(vsync: this, duration: const Duration(seconds: 2))
    ..addStatusListener((s) {
      if (s == AnimationStatus.completed) {
        HapticFeedback.heavyImpact();
        widget.onConfirmed();
      }
    });
  final _focus = FocusNode();

  void _down() {
    if (widget.enabled) _c.forward(from: _c.value);
  }

  void _up() {
    if (_c.status != AnimationStatus.completed) _c.animateBack(0, duration: const Duration(milliseconds: 250));
  }

  @override
  void dispose() {
    _c.dispose();
    _focus.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final cs = context.cs, tt = context.tt;
    return Focus(
      focusNode: _focus,
      autofocus: true,
      onKeyEvent: (_, e) {
        if (e.logicalKey != LogicalKeyboardKey.enter && e.logicalKey != LogicalKeyboardKey.space) return KeyEventResult.ignored;
        if (e is KeyDownEvent) _down();
        if (e is KeyUpEvent) _up();
        return KeyEventResult.handled;
      },
      child: GestureDetector(
        onTapDown: (_) => _down(),
        onTapUp: (_) => _up(),
        onTapCancel: _up,
        child: AnimatedBuilder(
          animation: Listenable.merge([_c, _focus]),
          builder: (context, _) {
            // ★ 焦点框只在用过键盘之后出现（roadmap 第 1 步）：它 autofocus，按 hasFocus 画的话触摸用户也会一直看见一圈白框
            final ring = _focus.hasFocus && FocusManager.instance.highlightMode == FocusHighlightMode.traditional;
            return Container(
              height: kTouch,
              decoration: ShapeDecoration(
                color: widget.enabled ? cs.errorContainer : cs.onSurface.withValues(alpha: 0.12),
                shape: StadiumBorder(side: ring ? BorderSide(color: cs.onSurface, width: 3) : BorderSide.none),
              ),
              clipBehavior: Clip.antiAlias,
              child: Stack(
                children: [
                  FractionallySizedBox(
                    widthFactor: _c.value,
                    child: Container(color: cs.error),
                  ),
                  Center(
                    child: Text(
                      _c.value == 0 ? widget.idle : widget.busy((_c.value * 100).round()),
                      style: tt.titleMedium!.copyWith(
                        color: !widget.enabled ? cs.onSurface.withValues(alpha: 0.38) : (_c.value > 0.5 ? cs.onError : cs.onErrorContainer),
                      ),
                    ),
                  ),
                ],
              ),
            );
          },
        ),
      ),
    );
  }
}

/// 日志（进度页 / 失败页）
class LogView extends StatelessWidget {
  const LogView(this.lines, {super.key, this.controller});
  final List<String> lines;
  final ScrollController? controller;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs;
    return Container(
      decoration: BoxDecoration(
        color: cs.surfaceContainerLowest,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: cs.outlineVariant),
      ),
      padding: const EdgeInsets.all(14),
      child: ListView.builder(
        controller: controller,
        itemCount: lines.length,
        itemBuilder: (_, i) => Text(
          lines[i],
          style: TextStyle(
            fontFamily: 'monospace',
            fontFamilyFallback: kFontFallback,
            // 中文回退到打包的 Noto Sans SC：它是可变字体、默认实例是 100（Thin），要显式给字重（见 theme.dart 的 w()）
            fontVariations: [FontVariation.weight(400)],
            fontSize: 13,
            height: 1.45,
            color: lines[i].startsWith('!! ') ? cs.error : (lines[i].startsWith('+ ') ? cs.onSurfaceVariant : cs.onSurface),
          ),
        ),
      ),
    );
  }
}
