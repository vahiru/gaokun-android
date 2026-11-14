import 'package:gk3_common/backend/backend.dart';

/// Web 上没有真后端
Gk3Backend? locateRealBackend() => null;

void Function(String line)? backendLogNote(Gk3Backend b) => null;

/// http://localhost:xxxx/?scenario=installed
String? requestedScenario() => Uri.base.queryParameters['scenario'];

/// ?lang=en：预览英文界面
String? requestedLanguage() => Uri.base.queryParameters['lang'];

/// Web 上没有测试模式
int? requestedTestDisk() => null;

/// Web 上不查本地文件
List<String> missingReleaseFiles(String dir) => const [];

/// 同上：当作目录里有 LiveCD 镜像
String? localLiveIso(String dir) => dir.isEmpty ? null : '$dir\\gaokun3-live.iso';

/// Web 上没有下载的文件
void deleteDownloadedRelease(String dir, {Iterable<String> also = const []}) {}
