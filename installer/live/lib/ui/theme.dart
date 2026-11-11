import 'package:flutter/material.dart';
import 'package:material_color_utilities/material_color_utilities.dart';

/// Material Design 3（用户 2026-09-25："一点也不 material design，要更符合 MD3"）。
///
/// * 颜色：一个种子色（沿用 C 版的强调蓝）→ `ColorScheme.fromSeed` 生成整套 MD3 角色；
///   界面里只用角色（primary / secondaryContainer / surfaceContainerLow / error …），不写死颜色。
/// * 字体：MD3 的两套 —— 拉丁 Roboto、中文 Noto Sans CJK SC，都【打包进应用】
///   （pubspec 的 fonts；tool/fetch-fonts.py 取）。不再靠系统字体回退：2026-09-25 真机上
///   中文全是方块，就是 fontconfig 查得到、Flutter 却没回退过去。
/// * 字号：MD3 的 type scale 原样用（Flutter 的 M3 默认值）。
const kSeed = Color(0xFF4DA3FF);

/// 中文按这个顺序回退：打包的那份在前；后两个只在"没取字体就构建"时兜底（有它们总比方块好）
const kFontFallback = ['Noto Sans SC', 'Noto Sans CJK SC', 'WenQuanYi Micro Hei'];

/// ★ 触摸目标。MD3 的下限是 48 dp；本机面板 266 mm 宽（wlr-randr 实测）、画布 1280 逻辑像素
///   ⇒ 1 逻辑像素 ≈ 0.208 mm ≈ 1.31 dp，48 dp ≈ 37 逻辑像素。按钮取 MD3（2025）的中号 56，
///   列表项 ≥ 72，都在下限的 1.5 倍以上。C 版定的 88（"手指的实际接触面积远大于设计稿"）
///   是 MD3 下限的 2.4 倍 —— 按 MD3 收回来。
const double kTouch = 56;

/// 逻辑画布。面板物理 1600×2560 竖屏、平板横用；旋转交给 cage，缩放在这里做。
const Size kCanvas = Size(1280, 800);

/// MD3 没有"成功 / 警告"这两个角色。按 MD3 的自定义颜色做法：先向主色调和（Blend.harmonize，
/// 色相往主色靠一点，免得跳出整套配色），再用同一套算法生成四件套（颜色 / 其上 / 容器 / 容器之上）。
@immutable
class Gk3Colors extends ThemeExtension<Gk3Colors> {
  const Gk3Colors({
    required this.success,
    required this.onSuccess,
    required this.successContainer,
    required this.onSuccessContainer,
    required this.warning,
    required this.onWarning,
    required this.warningContainer,
    required this.onWarningContainer,
  });

  factory Gk3Colors.from(ColorScheme cs) {
    ColorScheme tone(Color c) => ColorScheme.fromSeed(
          seedColor: Color(Blend.harmonize(c.toARGB32(), cs.primary.toARGB32())),
          brightness: cs.brightness,
        );
    final s = tone(const Color(0xFF3DBE7A)), w = tone(const Color(0xFFF2B33D));
    return Gk3Colors(
      success: s.primary,
      onSuccess: s.onPrimary,
      successContainer: s.primaryContainer,
      onSuccessContainer: s.onPrimaryContainer,
      warning: w.primary,
      onWarning: w.onPrimary,
      warningContainer: w.primaryContainer,
      onWarningContainer: w.onPrimaryContainer,
    );
  }

  final Color success, onSuccess, successContainer, onSuccessContainer;
  final Color warning, onWarning, warningContainer, onWarningContainer;

  @override
  Gk3Colors copyWith() => this;

  @override
  Gk3Colors lerp(Gk3Colors? other, double t) => t < 0.5 || other == null ? this : other;
}

extension ThemeX on BuildContext {
  ColorScheme get cs => Theme.of(this).colorScheme;
  TextTheme get tt => Theme.of(this).textTheme;
  Gk3Colors get gk => Theme.of(this).extension<Gk3Colors>()!;
}

ThemeData buildTheme({Brightness brightness = Brightness.dark}) {
  final cs = ColorScheme.fromSeed(seedColor: kSeed, brightness: brightness, dynamicSchemeVariant: DynamicSchemeVariant.tonalSpot);
  final base = ThemeData(useMaterial3: true, colorScheme: cs, fontFamily: 'Roboto', fontFamilyFallback: kFontFallback);

  // ★ 两个字体都是可变字重（wght 轴）。Flutter 不会自己把 fontWeight 映射到轴上 ——
  //   不给 FontVariation 的话，标题的 500 与正文的 400 渲染成同一个粗细，MD3 的层次就没了。
  TextStyle? w(TextStyle? s) => s?.copyWith(fontVariations: [FontVariation.weight((s.fontWeight ?? FontWeight.w400).value.toDouble())]);
  final t0 = base.textTheme;
  final t = t0.copyWith(
    displayLarge: w(t0.displayLarge),
    displayMedium: w(t0.displayMedium),
    displaySmall: w(t0.displaySmall),
    headlineLarge: w(t0.headlineLarge),
    headlineMedium: w(t0.headlineMedium),
    headlineSmall: w(t0.headlineSmall),
    titleLarge: w(t0.titleLarge),
    titleMedium: w(t0.titleMedium),
    titleSmall: w(t0.titleSmall),
    bodyLarge: w(t0.bodyLarge),
    // 本界面里 bodyMedium / bodySmall 一律用作说明文字 —— MD3 的说明文字是 on-surface-variant
    bodyMedium: w(t0.bodyMedium)!.copyWith(color: cs.onSurfaceVariant),
    bodySmall: w(t0.bodySmall)!.copyWith(color: cs.onSurfaceVariant),
    labelLarge: w(t0.labelLarge),
    labelMedium: w(t0.labelMedium),
    labelSmall: w(t0.labelSmall),
  );

  // MD3（2025）中号按钮：高 56、胶囊形、title-medium 的字
  final btn = ButtonStyle(
    minimumSize: const WidgetStatePropertyAll(Size(64, kTouch)),
    padding: const WidgetStatePropertyAll(EdgeInsets.symmetric(horizontal: 24)),
    textStyle: WidgetStatePropertyAll(t.titleMedium),
    iconSize: const WidgetStatePropertyAll(22),
    shape: const WidgetStatePropertyAll(StadiumBorder()),
  );
  return base.copyWith(
    textTheme: t,
    extensions: [Gk3Colors.from(cs)],
    filledButtonTheme: FilledButtonThemeData(style: btn),
    outlinedButtonTheme: OutlinedButtonThemeData(style: btn),
    textButtonTheme: TextButtonThemeData(style: btn),
    cardTheme: CardThemeData(
      margin: EdgeInsets.zero,
      elevation: 0,
      clipBehavior: Clip.antiAlias,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
    ),
    // 2024 版 MD3 的进度条 / 滑块（圆头、轨道与指示之间留缝）。year2023 标着"弃用"，但它的说明
    // 就是"设成 false 来启用 2024 版外观"—— 等它默认成 false 之后删掉这两行
    // ignore: deprecated_member_use
    progressIndicatorTheme: const ProgressIndicatorThemeData(year2023: false),
    // ignore: deprecated_member_use
    sliderTheme: const SliderThemeData(year2023: false),
    inputDecorationTheme: const InputDecorationTheme(border: OutlineInputBorder()),
    dividerTheme: DividerThemeData(color: cs.outlineVariant, space: 24),
  );
}
