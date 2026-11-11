// 来源 →（连 WiFi → 输密码 / 手输隐藏网络 → 选版本）
import 'dart:convert';

import 'package:flutter/material.dart';

import '../app.dart';
import '../backend/protocol.dart';
import '../model/model.dart';
import '../session.dart';
import 'messages.dart';
import 'screens_finish.dart';
import 'soft_keyboard.dart';
import 'theme.dart';
import 'widgets.dart';

class SourcePage extends StatelessWidget {
  const SourcePage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    final rel = s.usbRelease;
    final usbOk = rel?.installable ?? false;
    // U 盘里没有镜像时替用户选到网络那一张
    if (!usbOk && s.source == Source.usb) {
      WidgetsBinding.instance.addPostFrameCallback((_) => s.setSource(Source.net));
    }
    return StepPage(
      step: Gk3Step.source,
      title: l.sourceTitle,
      subtitle: l.sourceSub,
      onBack: () => Navigator.pop(context),
      onNext: () {
        if (s.source == Source.usb) {
          go(context, const OptsPage());
        } else {
          go(context, const NetPage());
        }
      },
      child: ListView(children: [
        ChoiceCard(
          icon: Icons.usb,
          title: l.sourceUsbTitle,
          body: [l.sourceUsbBody, if (rel != null && rel.version.isNotEmpty) rel.version].join('\n'),
          reason: usbOk ? null : l.sourceUsbMissing,
          selected: s.source == Source.usb && usbOk,
          onTap: () => s.setSource(Source.usb),
        ),
        const SizedBox(height: 14),
        ChoiceCard(
          icon: Icons.wifi,
          title: l.sourceNetTitle,
          body: l.sourceNetBody,
          selected: s.source == Source.net,
          onTap: () => s.setSource(Source.net),
        ),
      ]),
    );
  }
}

// ── WiFi 第一步：选网络 ──────────────────────────────────────────────────────
// ★ 两步式（roadmap 欠账第 1 条）：C 版把密码框挤在 AP 列表下面，列表一长就把
//   按钮顶到边上。现在：先选网络，再换一屏只放 SSID + 密码框 + 软键盘。

class NetPage extends StatefulWidget {
  const NetPage({super.key});
  @override
  State<NetPage> createState() => _NetPageState();
}

class _NetPageState extends State<NetPage> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => context.session.scanWifi());
  }

  Future<void> _pick(Ap ap) async {
    final s = context.session;
    if (!ap.secure) {
      await go(context, PasswordPage(ap: ap, open: true));
    } else {
      await go(context, PasswordPage(ap: ap));
    }
    if (mounted && s.online) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, tt = Theme.of(context).textTheme;
    final aps = s.aps;
    final net = s.net;
    return StepPage(
      step: Gk3Step.source,
      title: l.netTitle,
      subtitle: l.netSub,
      onBack: () => Navigator.pop(context),
      bottomLeft: Btn(l.netRescan, kind: BtnKind.secondary, icon: Icons.refresh, onPressed: s.scanning ? null : s.scanWifi),
      onNext: s.online ? () => go(context, const VariantPage()) : null,
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        if (s.online && net != null) ...[
          Row(children: [
            Icon(Icons.wifi, color: context.gk.success),
            const SizedBox(width: 10),
            Text(l.netConnected(net['ssid'], net['ip']), style: tt.bodyLarge!.copyWith(color: context.gk.success)),
          ]),
          const SizedBox(height: 16),
        ],
        if (!s.scanning && (aps == null || aps.isEmpty)) ...[
          Text(l.netNone, style: tt.bodyLarge),
          const SizedBox(height: 16),
        ],
        Expanded(
          child: s.scanning && aps == null
              ? Row(children: [const CircularProgressIndicator(), const SizedBox(width: 16), Text(l.netScanning, style: tt.bodyLarge)])
              : GridView(
                  // 固定行高而不是宽高比：卡片里是标题 + 一行说明，或者标题 + 最多三行的不支持原因（英文的原因要折行；测试字体每个字都是方块、更宽，按它留够）
                  gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(crossAxisCount: 2, mainAxisSpacing: 12, crossAxisSpacing: 12, mainAxisExtent: 140),
                  children: [
                    for (final ap in aps ?? const <Ap>[])
                      ChoiceCard(
                        title: ap.ssid,
                        body: ap.supported ? (ap.auth == 'sae' ? l.netWpa3 : (ap.secure ? null : l.netOpen)) : null,
                        // 连不了的（企业网络、WEP、OWE）标灰并写明原因 —— 原先 WEP / OWE 能点、输了密码再报"密码错"（GUI-9）
                        reason: switch (ap.auth) { _ when ap.supported => null, 'wep' => l.netWep, 'owe' => l.netOwe, _ => l.netEnterprise },
                        selected: s.online && net?['ssid'] == ap.ssid,
                        trailing: SignalBars(ap.bars, secure: ap.secure),
                        onTap: () => _pick(ap),
                      ),
                    // 放在最后、扫不到网络时也在：不广播名字的网络永远不会出现在上面
                    ChoiceCard(
                      title: l.netHidden,
                      body: l.netHiddenBody,
                      trailing: Icon(Icons.wifi_find_outlined, color: context.cs.onSurfaceVariant),
                      onTap: () async {
                        await go(context, const HiddenNetPage());
                        if (mounted) setState(() {});
                      },
                    ),
                  ],
                ),
        ),
      ]),
    );
  }
}

// ── WiFi 第二步：输密码 ──────────────────────────────────────────────────────

class PasswordPage extends StatefulWidget {
  const PasswordPage({super.key, required this.ap, this.open = false});
  final Ap ap;
  final bool open;
  @override
  State<PasswordPage> createState() => _PasswordPageState();
}

class _PasswordPageState extends State<PasswordPage> {
  final _pw = TextEditingController();
  final _focus = FocusNode();
  bool _show = false, _busy = false;
  String? _error;
  int _pct = 0;

  @override
  void initState() {
    super.initState();
    _pw.addListener(() => setState(() {}));
    if (widget.open) WidgetsBinding.instance.addPostFrameCallback((_) => _connect());
  }

  @override
  void dispose() {
    _pw.dispose();
    _focus.dispose();
    super.dispose();
  }

  /// wpa_supplicant 只收 8–63 个字符（wpa-2.10 wpa_supplicant/config.c:571）——
  /// 在这里就拦住，不让用户白等 20 秒超时（gk3_wifi_connect 里也会再验一遍）
  bool get _lengthOk => widget.open || (_pw.text.length >= 8 && _pw.text.length <= 63);

  Future<void> _connect() async {
    if (!_lengthOk || _busy) return;
    setState(() {
      _busy = true;
      _error = null;
      _pct = 0;
    });
    final r = await context.session.connect(widget.ap, widget.open ? '' : _pw.text, (e) {
      if (e is Gk3Progress && mounted) setState(() => _pct = e.percent);
    });
    if (!mounted) return;
    if (r.ok) {
      Navigator.pop(context);
    } else {
      setState(() {
        _busy = false;
        _error = r.err == null ? context.l.netFailed : errText(context.l, r.err!);
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l, tt = Theme.of(context).textTheme;
    return StepPage(
      step: Gk3Step.source,
      title: widget.open ? l.netConnecting(widget.ap.ssid) : l.netPasswordFor(widget.ap.ssid),
      onBack: _busy ? null : () => Navigator.pop(context),
      nextLabel: l.netConnect,
      onNext: _lengthOk && !_busy ? _connect : null,
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        if (!widget.open)
          TextField(
            controller: _pw,
            focusNode: _focus,
            autofocus: true,
            obscureText: !_show,
            enabled: !_busy,
            style: tt.titleLarge,
            onSubmitted: (_) => _connect(),
            decoration: InputDecoration(
              labelText: l.netPassword,
              prefixIcon: const Icon(Icons.key_outlined),
              suffixIcon: IconButton(
                tooltip: l.netShowPassword,
                icon: Icon(_show ? Icons.visibility_off_outlined : Icons.visibility_outlined),
                onPressed: () => setState(() => _show = !_show),
              ),
              // 长度不对是 MD3 的"错误"状态：框变 error 色、下面一行说明
              errorText: _pw.text.isNotEmpty && !_lengthOk ? l.netPasswordLength : null,
            ),
          ),
        const SizedBox(height: 12),
        if (_busy) ...[
          LinearProgressIndicator(value: _pct / 100, minHeight: 8),
          const SizedBox(height: 12),
        ],
        if (_error != null) Text(_error!, style: tt.bodyLarge!.copyWith(color: context.cs.error)),
        const Spacer(),
        if (!widget.open) SoftKeyboard(controller: _pw, onDone: _connect),
      ]),
    );
  }
}

// ── WiFi 第二步的另一种：隐藏的网络，名字手输 ────────────────────────────────
//
// ⚠️ live 里没有输入法：软键盘只有 ASCII，实体键盘在 cage 里也只出 ASCII ——
//    中文名字的隐藏网络连不上。那种网络少见，界面上不专门说。

class HiddenNetPage extends StatefulWidget {
  const HiddenNetPage({super.key});
  @override
  State<HiddenNetPage> createState() => _HiddenNetPageState();
}

class _HiddenNetPageState extends State<HiddenNetPage> {
  final _ssid = TextEditingController(), _pw = TextEditingController();
  final _ssidFocus = FocusNode(), _pwFocus = FocusNode();

  /// 软键盘打进【最后一个得到焦点】的框。不能看"现在谁有焦点"：Linux 上点输入框以外的地方
  /// （包括软键盘本身）输入框就失焦了（EditableText 默认的 onTapOutside）
  late TextEditingController _target = _ssid;
  bool _show = false, _busy = false;
  String? _error;
  int _pct = 0;

  @override
  void initState() {
    super.initState();
    _ssid.addListener(() => setState(() {}));
    _pw.addListener(() => setState(() {}));
    _ssidFocus.addListener(() => _ssidFocus.hasFocus ? setState(() => _target = _ssid) : null);
    _pwFocus.addListener(() => _pwFocus.hasFocus ? setState(() => _target = _pw) : null);
  }

  @override
  void dispose() {
    _ssid.dispose();
    _pw.dispose();
    _ssidFocus.dispose();
    _pwFocus.dispose();
    super.dispose();
  }

  /// 802.11 的 SSID 是 1–32 个【字节】（后端 gk3_wifi_connect 也会再验一遍）
  int get _ssidBytes => utf8.encode(_ssid.text).length;
  bool get _ssidOk => _ssidBytes >= 1 && _ssidBytes <= 32;

  /// 空 = 开放网络；否则同 PasswordPage 的 8–63
  bool get _pwOk => _pw.text.isEmpty || (_pw.text.length >= 8 && _pw.text.length <= 63);

  Future<void> _connect() async {
    if (!_ssidOk || !_pwOk || _busy) return;
    setState(() {
      _busy = true;
      _error = null;
      _pct = 0;
    });
    final ap = Ap.hidden(_ssid.text, secure: _pw.text.isNotEmpty);
    final r = await context.session.connect(ap, _pw.text, (e) {
      if (e is Gk3Progress && mounted) setState(() => _pct = e.percent);
    });
    if (!mounted) return;
    if (r.ok) {
      Navigator.pop(context);
    } else {
      setState(() {
        _busy = false;
        // 连不上（关联失败）对隐藏网络多半是名字打错了：用隐藏网络那句；别的原因按 ERR 说
        _error = r.err == null || r.err!['code'] == 'wifi-assoc' ? context.l.netHiddenFailed : errText(context.l, r.err!);
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l, tt = Theme.of(context).textTheme;
    return StepPage(
      step: Gk3Step.source,
      title: l.netHiddenTitle,
      subtitle: l.netHiddenSub,
      onBack: _busy ? null : () => Navigator.pop(context),
      nextLabel: l.netConnect,
      onNext: _ssidOk && _pwOk && !_busy ? _connect : null,
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        TextField(
          controller: _ssid,
          focusNode: _ssidFocus,
          autofocus: true,
          enabled: !_busy,
          style: tt.titleLarge,
          textInputAction: TextInputAction.next,
          onSubmitted: (_) => _pwFocus.requestFocus(),
          decoration: InputDecoration(
            labelText: l.netSsid,
            prefixIcon: const Icon(Icons.wifi),
            errorText: _ssidBytes > 32 ? l.netSsidTooLong : null,
          ),
        ),
        const SizedBox(height: 16),
        TextField(
          controller: _pw,
          focusNode: _pwFocus,
          obscureText: !_show,
          enabled: !_busy,
          style: tt.titleLarge,
          onSubmitted: (_) => _connect(),
          decoration: InputDecoration(
            labelText: l.netPasswordOptional,
            prefixIcon: const Icon(Icons.key_outlined),
            suffixIcon: IconButton(
              tooltip: l.netShowPassword,
              icon: Icon(_show ? Icons.visibility_off_outlined : Icons.visibility_outlined),
              onPressed: () => setState(() => _show = !_show),
            ),
            errorText: _pw.text.isNotEmpty && !_pwOk ? l.netPasswordLength : null,
          ),
        ),
        const SizedBox(height: 12),
        if (_busy) ...[
          LinearProgressIndicator(value: _pct / 100, minHeight: 8),
          const SizedBox(height: 12),
        ],
        if (_error != null) Text(_error!, style: tt.bodyLarge!.copyWith(color: context.cs.error)),
        const Spacer(),
        // 回车：在名字框里 = 跳到密码框；在密码框里 = 连接
        SoftKeyboard(controller: _target, onDone: identical(_target, _ssid) ? _pwFocus.requestFocus : _connect),
      ]),
    );
  }
}

// ── 选版本 ───────────────────────────────────────────────────────────────────

class VariantPage extends StatefulWidget {
  const VariantPage({super.key});
  @override
  State<VariantPage> createState() => _VariantPageState();
}

class _VariantPageState extends State<VariantPage> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final s = context.session;
      if (s.variants == null) s.fetchVariants();
    });
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, tt = Theme.of(context).textTheme;
    final vs = s.variants;
    return StepPage(
      step: Gk3Step.source,
      title: l.variantTitle,
      subtitle: l.variantSub,
      onBack: () => Navigator.pop(context),
      onNext: s.variant == null ? null : () => go(context, const OptsPage()),
      child: s.variantsFail != null
          ? Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(l.variantFailed, style: tt.bodyLarge!.copyWith(color: context.cs.error)),
              // 后端说的具体原因（2026-09-25 真机：清单 404，原先只有一句"请检查网络连接"，把人往错的方向引）
              const SizedBox(height: 8),
              Text(callErrorText(l, s.variantsFail!), style: tt.bodyMedium),
              const SizedBox(height: 16),
              Btn(l.btnRetry, kind: BtnKind.secondary, onPressed: s.fetchVariants),
            ])
          : vs == null
              ? const Center(child: CircularProgressIndicator())
              : ListView.separated(
                  itemCount: vs.length,
                  separatorBuilder: (_, _) => const SizedBox(height: 12),
                  itemBuilder: (_, i) {
                    final v = vs[i];
                    return ChoiceCard(
                      title: v.nameFor(s.language),
                      body: '${v.descFor(s.language).isEmpty && v.latest ? l.variantLatest : v.descFor(s.language)}\n${l.variantSize(fmtMib(v.sizeMib))}',
                      selected: identical(s.variant, v),
                      onTap: () => s.setVariant(v),
                    );
                  },
                ),
    );
  }
}
