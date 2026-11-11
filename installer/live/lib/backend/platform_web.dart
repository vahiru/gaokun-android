import '../session.dart';
import 'backend.dart';

Gk3Backend? locateShellBackend() => null;

/// http://localhost:xxxx/?scenario=factory
String? requestedScenario() => Uri.base.queryParameters['scenario'];

bool soakRequested() => Uri.base.queryParameters['soak'] == '1';

/// ?lang=en：预览英文界面（Web 上不记住）
LangPrefs platformLangPrefs() => _WebLang();

class _WebLang extends LangPrefs {
  @override
  String? saved() => null;
  @override
  String? fromBoot() => Uri.base.queryParameters['lang'];
  @override
  void save(String lang) {}
}
