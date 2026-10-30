# HyperOS hardware probe

Maintained copy of the R14 application, same package/activity and manifest. Existing media/audio/camera tests remain; the default sensor test now uses the strict simultaneous listener, avoiding the previous `events > 0` false PASS for NaN.

Strict-only invocation (after installing the locally signed APK; do not uninstall an existing app to bypass a signature mismatch):

```sh
adb shell am force-stop org.gaokun.hosprobe
adb shell am start -W -n org.gaokun.hosprobe/.Main --ez strict_sensors true --ei sensor_rounds 3
adb shell run-as org.gaokun.hosprobe cat files/results.txt
```

Wait for this run's `DONE STRICT SENSORS` before reading results. Rounds default to1, accepted range1..3. Each round simultaneously registers accelerometer, gyro, gravity and game rotation, waits2s warmup and records5s, then unregisters; between rounds waits1s. External clients may keep the shared system fusion running, so this does not guarantee a full global fusion reset.

PASS requires successful registration, at least one valid event during the5s window, no nonfinite value at any time (including warmup), and positive strictly increasing sensor timestamps. Summary reports events, validSamples, nonFinite, NaN events, badTimestamp, first-valid arrival delay, last vector and sample-window mean. A later finite event never erases an earlier invalid event. This checks delivery/data sanity, not physical orientation accuracy. Simultaneous accel/gravity means are more useful than the old sequential snapshots, but movement during the window still prevents a static comparison.

Host predicate test:

```sh
javac -d /tmp/hos-probe-tests src/org/gaokun/hosprobe/StrictSensorStats.java tests/StrictSensorStatsTest.java
java -cp /tmp/hos-probe-tests org.gaokun.hosprobe.StrictSensorStatsTest
```

APK build uses JDK17 + Android SDK35 jar, javac source/target8, d8 min-api28, aapt2 link with AndroidManifest.xml, zip classes.dex, zipalign, then apksigner. Existing local debug keystore is `out/hyperos-r11/probe/debug.jks` (alias test); do not commit it or create a replacement signer. Its certificate SHA256 is `4839029b0a0d8e0990ba02ce22e13a07271add5a12de97a731aba2e2d70d21cd`. Always compare output signature with the already installed probe's archived APK before proposing an update.

2026-10-29 build: `/tmp/hos-strict-sensors-r14/probe.apk`, SHA256 `4e7361a7af4eb94d3220d730ad0788fdbd7d921d9046d2d23a1ae7f5ae09609d`. Predicate tests and APK compilation/signature verification passed; device tests belong to the parent agent. No out artifacts were modified.

Thermal-only mode (no camera, microphone, codec, network or sensor-listener tests):

```sh
adb shell am force-stop org.gaokun.hosprobe
adb shell am start -W -n org.gaokun.hosprobe/.Main --ez thermal_only true
adb shell run-as org.gaokun.hosprobe cat files/results.txt
```

Wait for this run's `DONE THERMAL`. Public `PowerManager.getCurrentThermalStatus()` must return0..6. `getThermalHeadroom(0)` is called exactly once and must be finite/nonnegative; the value is dimensionless, not a physical temperature. Leave at least10seconds between runs to avoid API rate-limit NaN. SDK35+ public `getThermalHeadroomThresholds()` reports its original map, checks nonempty/legal statuses/finite nonnegative values. API unavailable is explicitly SKIP for thresholds only; headroom remains independently checked. NaN/headroom failures are not converted to PASS. This checks public framework delivery, not thermal calibration or a load/shutdown test.

R17 thermal build: `~/.cache/hos-r17-thermal/probe/probe.apk`, SHA256
`e726651d10b90e9254b69c0608340a95c13598328c31b5bcf1fc70bf27bdd8ae`.
JDK17 / SDK35 / build-tools36; existing alias test signer, certificate SHA256
`4839029b0a0d8e0990ba02ce22e13a07271add5a12de97a731aba2e2d70d21cd`,
verified equal to archived R14 APK. Build and APK signature verification passed;
no installation or device operation was performed during preparation.

## Bounded fusion-input observations (R19 diagnostic APK)

The strict test still uses the same registration, finite-value, timestamp and
sample-window PASS predicate. It does not replace values, filter events, normalize
vectors, or treat a real zero vector as a new failure. Free-fall acceleration and
a stationary gyro can legitimately report zero.

Only accelerometer(type1) and gyro(type4) receive additional diagnostic metadata:

- `obsWarmup*` and `obsSample*` separately report finite-vector count, minimum
  Euclidean norm of the first three axes, and all-zero three-axis count. Norms
  use double arithmetic to avoid float overflow/underflow in this observation.
  Values retain sensor units (m/s² for acceleration, rad/s for gyro). No finite
  vector in a phase is reported as `MinNorm=none`, not a fabricated zero.
- These finite-vector observations are independent of timestamp validity; the
  original timestamp failure remains in force. Events outside both time windows
  do not contribute to either phase's norm/zero counters.
- At most the first eight events per type per round are saved, including invalid
  events when present. `OBS strict-sensor` lines show phase, original sensor
  timestamp, arrival `elapsedRealtimeNanos`, elapsed time relative to round start,
  finite/timestamp flags and copied raw values. Arrays are cloned on receipt;
  Android's reuse of `SensorEvent.values` cannot overwrite earlier records.
  Gravity/game-rotation events retain their previous checks without these extra
  snapshots or norm counters.

Host tests cover original PASS failures, legitimate zeros, exact phase boundaries,
nonfinite/short inputs, float extrema, timestamp errors, bounded storage, reused
SensorEvent arrays and defensive copies returned to observers.

Build artifact: `~/.cache/hos-r19-fusion-probe/probe.apk`, SHA256
`d8c700a3b7b395d9ecb9759ba01244f2d02435504e6d53ba2ae20bfca9d4f163`.
The existing test certificate remains
`4839029b0a0d8e0990ba02ce22e13a07271add5a12de97a731aba2e2d70d21cd`,
verified against the archived R17 APK. Build logs, exact input hashes and signer
comparison are in the same cache directory. No out artifact or keystore was
modified, and this preparation did not install the app or access the device.


## R21 首帧精度诊断

前八个acc/gyro事件现在另外记录`SensorEvent.accuracy`及每轴float原始位值，汇总同时显示框架提供的maxRange/resolution。保留原始精度，不把UNRELIABLE提升为可靠；未传入精度的host fixture明确显示unknown。原有限值/时间戳/数据交付PASS条件不变，不证明物理准确性。首帧可能是HAL缓存重发，不能由APK事件时间戳断言对应不同SSC样本。

构建与实机证据：`~/.cache/hos-r21-accuracy-probe/`。APK SHA256 `7b59561f9469c12c47b2c7789f0b57b5405e668ee852ffbf61d573a32311b3ac`，沿用既有签名。host tests与签名检查通过，已在R21同boot安装测试三轮、12项原检查通过；每轮gyro前两帧三轴`0xc20ba058`且accuracy=0，第三帧为正常小值且accuracy=3。元数据仍报maxRange17.453293/resolution0.0005326322，来源量程待核验。已停止探针，APK保留。
