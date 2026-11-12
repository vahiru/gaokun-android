/// 安装器自己的版本 —— 与它装的 ROM 版本无关（网络安装默认装最新版）。随 ROM 的发布附带发出（用户 2026-09-27）。
/// ⚠️ 必须等于 pubspec.yaml 的 version 去掉 "+构建号" 的部分（test/version_test.dart 核对）；
///   scripts/live/build-live.sh 也从 pubspec 读它，写进镜像的 /etc/gaokun3-release 与 U 盘上的 gaokun3/release.txt
const kInstallerVersion = '0.2.0-rc.2';
