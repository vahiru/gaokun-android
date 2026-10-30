# R22 陀螺仪精度保护与启动观测部署记录

已完成隔离super更新与整区读回，已正常冷启动，早期数据交付仍失败，后续出数检查通过。目标是让融合模块尊重gyro UNRELIABLE状态，并为SSC开机无数据和启动首帧异常补充有界诊断；不宣称已修复SSC启动延迟或原始gyro生产端异常。

## 已完成

实际OEM库process路径确实忽略status。新库基于已部署R21零向量保护库，增加16字节guard；status=0仅跳过融合消费并推进mGyroTime，不改变原始事件或其他消费者，accuracy1/2/3保留。静态失败注入与实际OEM转换/消费路径重放通过；消费边界mock不代表完整估计器/方向精度验收。详见[精度保护审查](hyperos-gyro-accuracy-guard-candidate.md)。

system EROFS已构建并独立复核全部4807条目：仅system/lib64/libsensorservice.so内容变更，其他内容及全部元数据相同，原ACC保护和libc属性标签保留。

- 库SHA256：a24232d05bba1ffb748baf9f9fa8d75a1b066462460daaf60d354dfd36d12185。
- system SHA256：52c834f9dc75be57e40f229196312e5177c51d4eabe9c3e7f3b76862f0b8bd1c，958590976字节。
- 证据：~/.cache/hos-r22-gyro-startup/system/及system-independent-review.json。

诊断HAL SHA476a01d2301792c7fcf39e42e8c8285319c4e541742740847f2e88ab92997f64已独立Android编译，不修改CICD源/cache/派生产品。仅SscHub源码添加Open/registry等待/查询耗时及enable后前8帧原始位模式、accuracy、DSP时间戳日志；每进程512条上限，未变更等待、数据、会话及ALS策略。与实际R21 ab794的动态依赖和导入/导出相同。独立重编基线仍与ab794有.text等差异，不能声称字节复现或完整行为等价；构建审计见~/.cache/hos-r21-startup-observation/remote/。vendor已完成，除sensors HAL外另19项替换/所有选项/元数据/xattrs与R21一致，e2fsck通过。vendor SHA256为c312e2001debbdab596f3dd714cc93982459694b1dde23aed220b4f06acfa01b，402653184字节。

## 部署前与验收待办

vendor与super离线验证已完成，独立更新门槛审阅与专用救援冷更新均已完成，不做连续zygote热重启。设备目前仍R21 super＋指纹候选内核，用户接触测试后模块已卸载，12项传感器/温控契约通过；指纹无有效帧，仍未可用。

R22冷启动必须分别记录最早窗口的事件交付、SSC阶段耗时、首样本、raw accuracy及融合结果，不以稍后PASS抵消早期FAIL；保留姿态准确度、原始数据量程和启动延迟的独立边界。还需回归USBdevice、无线ADB、温控、SELinux与恢复请求。设置按压渲染、ALS、指纹等其他问题未因此关闭。

为打包腾空间，仅在完整校验R21 super与6个原输入后移除6个完全相同、可重建的unpacked副本；原镜像、回退镜像、GPT/boot备份均保留，证明位于redundant-unpacked-cleanup.json。


## 完整super构建及磁盘空间限制下的验证

完整super_hos.raw.img已经lpmake生成，大小12884901888字节，SHA256 `76dec7d23d766c921d07bcfee1b2f7e45c3ded90da5944afd5a8e5817fedd2ad`，现已刷入固定super_hos并完成整区读回。相对R21仅system_a/vendor_a改变，另外四个分区相同。

完整lpunpack过程中主机空间不足，明确停止本轮构建容器，保留super及已完成lpdump输出，只删除本轮不完整解包副本；记录unpack-interruption.json。不能声称完整lpunpack通过。替代验证脚本verify-super-stream.py：先验证旧R21 super完整pin，要求新旧lpdump逐字相同、只接受六个已知single-linear extent；逐一按映射读取六分区并核对真实源SHA与大小；再将两处替换extent以外的所有字节与R21逐字比较，包括全部LP metadata/slot记录、对齐空隙及尾部空间。全部通过，最后计算新super完整SHA。结果保存在super/manifest.json。

这是对完整镜像内容和既有映射的流式核验，不需要同时落盘六份解包副本，不代表实机读回或冷启动验收。


## 救援更新完成

rescue boot1ba461ea-c40e-46cd-ad97-5f9e134d3d14，独立device-only入口、session claim、休眠runtime masks与USB ECM连接验证通过。新manifest pin750d7b4c134e9fa2fec3d9dbbc100890467c079a18d6dfa01563572f0cf037d9：GPT两份CRC及12条目、bootA/B、ESP71项与前次保护匹配，ESP只多已知R21entry；旧p7完整SHA仍aafb5c…09a0。已完成R21marker经再次整盘/保护区核验后独占归档，原记录保留，未绕过unfinished-update门槛。

preview通过后，固定p7收到精确12884901888字节及EOF，stream SHA/BLKFLSBUF后全分区回读/保护区复核全部通过，exit0且VERIFIED，新marker保留。未改GPT、bootA/B、userdata。随后重新核验新super、p1挂载身份及全ESP，独占创建R22entry；保留R17内核与默认Android B，oneshot回读通过。已请求完整重启，并预先启动早期HAL日志采集及boot_completed传感器观察器，尚待结果。

证据~/.cache/hos-r22-gyro-startup/deploy/的reviewed.json、archive-and-preview.log、update.log、stream-progress.json、install-entry.log。新R22entry意味着未来ESP保护清单需更新，旧R21入口现在也引用R22 super，不能把它当成整套旧系统回滚。


## R22首次冷启动结果

boot `3c8c0360-1732-4a7c-a4e8-42bcaed6dbb7`，SystemServer实际namespace库SHAa24232…2185、HAL476a01…7f64均匹配，Enforcing、USBdevice、ADB持久5555、userdata p12/370G保持。180秒早期日志采集完成且起止同boot。

boot_completed后uptime16.35秒启动探针，早期三轮四类传感器均0事件，12项FAIL，记录保留；不能以没有NaN或稍后PASS抵消无数据。新观测定位：Open15.45575s耗约0.15ms，registry-wait约0.70ms即成功。后续查询立即返回空provider，67.508s仍空，69.511/69.512s两路首次成功。整个过程HAL pid792保持、没有sscrecover或服务重启。说明本次54秒等待在DSP provider注册一侧，不是HAL查询超时。SLPI t61.505s重复handover消息仅为相关线索，不能宣称已定位60秒根因。

重新订阅于117.763/117.773s，首accel/gyro报告117.890s，两路延迟约117–126ms。首gyro两个报告DSP timestamp2473050086→2473432260，约19.905ms间隔，均accuracy0、三轴bits c20ba050；第三报告accuracy3且正常小值。因此至少两个独立SSC测量报告含启动异常，不是仅HAL缓存重发。另一轮第二报告数值已小但accuracy仍0，不能把“不可靠”和固定异常位值等同。

同boot出数后三轮12项检查PASS，无非有限值/NaN/异常时间戳；融合保护库已实际加载，但此检查不证明方向精度或完整估计器正确。native thermal契约RESULT PASS failures=0，真实SKIN约30.21°C。更广功能抽查首次遇到探针四项runtime权限未授予，音频/相机/蓝牙失败保留为测试前置问题，临时授予后复测通过，结束后四项权限已回读恢复为原granted=false。

证据deploy/cold-log、cold-early/results.txt、after-ready-results.txt、runtime-identity.txt、thermal-regression.txt。SSC注册延迟、原始gyro量程/数据异常、ALS、指纹和设置按压渲染仍开放。


## 功能抽查完成与清理

同boot的临时权限复测完成：16MiB存储write/fsync/read逐字相等；应用HTTPS200且读取785B；四类传感器再验通过；麦克风144000样本非零、播放头48000帧；双摄YUV取帧357/267帧；AES/GCM临时密钥加密21B且已删除；BLE收186条广播。额外媒体模式双摄JPEG通过、H.264软件编码→V4L2解码120/120帧且EOS。无照片或音频文件保存。上述播放不证明实际听感，BLE不证明配对/蓝牙音频，摄像头取帧不证明全部分辨率或画质。

应用公共温控状态0、headroom约0.05733为合法无量纲值、threshold map有效；native接口负参数合约另验通过。探针已停止、临时native可执行文件删除，四项runtime权限恢复，APK保留供诊断。无线ADB重新连接并核对同boot成功。运行约8分钟最终检查crash buffer为空、pstore无条目、misc_hos前832字节为零，无待执行恢复请求。SELinux Enforcing、USBdevice保持，RescueParty未关闭，FindDevice仍hidden，powmillet=false。

allow_suspend=0仍是调试保持设置，不计深睡通过。没有在本轮关闭所有功能目标；启动延迟、原始gyro数据/量程、ALS、指纹、设置按压渲染和实际外设等仍开放。完整证据及边界汇总deploy/runtime-summary.json。
