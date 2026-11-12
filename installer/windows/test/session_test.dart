// Session 里不经过界面也能测的几件事：按来源取版本列表、重新检查后的 BitLocker 确认、本地目录的引号
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/fixture_backend.dart';
import 'package:gk3_windows/model.dart';
import 'package:gk3_windows/session.dart';

import 'helpers.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('本地目录：去掉"复制文件地址"带的一对双引号', () {
    final s = Session(Rec(FixtureBackend('free', bundle: DiskBundle(), speed: 0)));
    s.setLocalDir('  "D:\\Downloads\\gaokun3 rc3"  ');
    expect(s.localDir, r'D:\Downloads\gaokun3 rc3');
    s.setLocalDir('"');
    expect(s.localDir, '"');
  });

  test('重新检查之后 BitLocker 的确认要重新勾选', () async {
    final s = Session(Rec(FixtureBackend('blocked', bundle: DiskBundle(), speed: 0)));
    await s.check();
    s.setBitlockerAck(true);
    await s.check();
    expect(s.bitlockerAck, isFalse);
  });

  test('一个来源还在取版本列表时切到另一个：另一个也会去取', () async {
    // 按录制的节奏回放（GitHub 640 ms、R2 410 ms），两次取在时间上重叠
    final rec = Rec(FixtureBackend('free', bundle: DiskBundle(), speed: 1));
    final s = Session(rec);
    final gh = s.loadVariants();
    expect(s.loadingVariants, isTrue);
    s.setSource(ReleaseSource.r2);
    await Future<void>.delayed(const Duration(milliseconds: 1500));
    await gh;
    expect(rec.calls.where((c) => c.first == 'releases').map((c) => c.last), ['github', 'r2']);
    expect(s.variants.single.name, 'crDroid 13.0');
    s.setSource(ReleaseSource.github);
    expect(s.variants, isNotEmpty);
    expect(s.loadingVariants, isFalse);
  });

  test('版本列表的调用出错（流里抛异常）：不一直转圈，报"无法获取版本列表"', () async {
    final rec = Rec(FixtureBackend('free', bundle: DiskBundle(), speed: 0));
    rec.replace['releases'] = (_) => Stream.error(StateError('No element'));
    final s = Session(rec);
    await s.loadVariants();
    expect(s.loadingVariants, isFalse);
    expect(s.variants, isEmpty);
    expect(s.variantsErr!['code'], 'manifest-unavailable');
  });
}
