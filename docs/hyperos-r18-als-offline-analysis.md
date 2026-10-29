# R18 ALS: offline evidence and bounded registry-write design

2026-10-30. No ambient_light session, DSP restart, device access or VM work was
performed. Images were opened read-only in the existing local tools container;
extractions and upstream checkouts are under `~/.cache/hos-r18-als/`.

## What is established

`stage4-findings.md` #118/#121 supersede the earlier #43/#72 bus hypothesis.
The old bus sweep preceded a working `tcs3701` available-provider response.
Activation later returned SSC critical error and a contemporaneous
`sns_stream_service.c:436` sensor_process fatal crash. Four request encodings
failed alike. A stream-service assertion is not evidence of an AP I2C collision,
nor does it prove a missing calibration group caused the failure.

The R18 vendor image still contains 26 JSON files, `is_dri=0` for ALS/proximity,
SSC bus_instance=5, address=57 (0x39), rail_on_state=2 and the sensor_vddio rail.
`refs/linux-mainline/.../sc8280xp.dtsi` has AP `i2c5@994000` disabled by default;
its numeric label does not establish a mapping to SSC instance 5. The gaokun3
reference DTS enables i2c4/i2c15, not that node. Do not enable AP i2c5, change IRQ
127 or sweep SSC buses based on the old inference.

Firmware strings name nine registry leaf groups, whereas current packaged JSON
provides seven. Missing: `tcs3701.rgb.config` and
`tcs3701_platform.rgb.fac_cal`. `config-group-evidence.json` records the comparison.
This is a schema gap, not permission to invent RGB calibration: firmware may
support optional RGB, and the Windows QRD package was already recorded as the
same configuration. No zero/identity calibration or unrelated donor values were
added. Registry activation dependencies remain unproven without driver source or
an existing request/error trace.

## Concrete missing implementation in the installed server

Manifest pins `https://github.com/linux-msm/hexagonrpc.git` at
`435ed1cd68e20f8a9fe4871233ed1f8ce7ccf9e8`.
`apps_std.c:153-208` implements `apps_std_fopen_with_env`; at 168-172, mode
starting with `w` or `a` prints `Tried to open %s for writing` to **stderr** and
returns `AEE_EUNSUPPORTED`. The installed R18 hexagonrpcd independently matches:
AArch64 0x451c reads the mode byte, 0x4520/0x4528 compare w/a, branch to 0x45ac,
and return 0x14 at 0x45c8. See cached disassembly and `inspect-server.py`.
The original dispatch table has no ordinary fopen/fwrite/fsync/remove/rename
implementation. Replacing a single conditional branch cannot supply those APIs.

The init service runs system:system with `-R /vendor/etc/hexagonrpcd-root`; its
RC has no console or stdio_to_kmsg. These fprintf messages are not Android log
calls. Normal init stdio is /dev/null absent an explicit redirect. Searches of
existing R3/R11/R15/R17/R18 cached text logs found no concrete registry-path write
request. Absence from logcat therefore does not disprove a write request, but we
also cannot claim such a request was observed during the #121 activation crash.
**Correction after wider historical review:** #37, line 641 explicitly records
repeated DSP writes to `/persist/sensors/registry/registry/../temp.json`. This is
older device evidence of a concrete output path, overlooked in the first cache
log search; it is not a new R18 trace or proof of crash causality.

`sns_reg.conf` specifies output `/persist/sensors/registry/registry`. The VFS
layout matters: the pinned builder maps this virtual path to the **directory**
`<root>/sensors/registry/`, containing an empty file named `registry`. Do not
mistake it for a one-file mount. That empty file remains zero bytes in R18.

## Existing upstream write implementation: useful base, unsafe to adopt unchanged

Read-only fetch into cache of
`https://github.com/FadyAckad/hexagonrpc.git`, branch `sp11-sensors`, resolved to
`40e50412812adce9dddc2be9a4510ebce05eef89`. Five commits above upstream base:

- `518fdf0`: mapped create/write/truncate/unlink/rename.
- `7543f33`: apps_std fopen/fwrite/fsync/fremove/ftrunc/frename.
- `f0dfd50`: answer unsupported requests and fetch input buffers over 256 bytes.
- `7f2fc1d`: map registry parent for temporary-file rename.
- `40e5041`: descriptor lifetime and write-path fixes.

There are also 19 intervening upstream commits since the deployed pin, including
new interpreter/handle APIs. This is not a five-patch drop-in against 435ed1c.
The original source remains untouched; checkout is in cache only.

Findings preventing direct deployment:

1. Mutating operations are attached to generic mapped backends. They are not
   restricted to registry, and mapped open/create follows symlinks. An offline
   disposable fixture compiled against unmodified 40e5041 successfully used a
   symlink inside the mapped root to truncate/write a sentinel outside it.
   `test-write-confinement.c` and `write-confinement-result.txt` reproduce this;
   no real data/device was used. Upstream's existing hexagonfs test passes, so it
   does not cover this confinement property.
2. `apps_std_fsync` returns success without a host fsync; page-cache completion
   is not persistence. Registry commit/reboot durability is not implemented.
3. Mode parsing accepts arbitrary strings containing w/a/+ and silently falls
   back to a read-only open for some failed r+ opens. Validate the actual fopen
   grammar and report access/flush errors honestly.
4. Existing CR trimming is in the common read path. New resolve_parent/create
   passes the final basename directly to mapped_create; simply reapplying the
   old CR patch does not normalize the last write/rename component. Tests must
   cover CR-suffixed reads AND create/unlink/rename, with validation after
   normalization and no path escape.
5. New builder probes `<root>/sensors/persist/registry` as a directory, then maps
   its parent to virtual `/persist/sensors/registry`. It does not provide an
   independent protected `/data/vendor` writable-root option suitable for the
   current read-only vendor root.

The local host tests use only macOS struct-stat field aliases as compiler flags;
upstream sources were not edited. A native Linux/Android build is still needed
before any deployment consideration.

## Narrow implementation path and offline gates

Reuse the upstream protocol operations and lifetime fixes, but add a separate
registry backend/capability. Keep all existing vendor JSON, DSP library, socinfo,
and calibration mappings read-only. Give only virtual
`/persist/sensors/registry` and its existing `/mnt/vendor` alias a dedicated root
such as `/data/vendor/hexagonrpcd/registry-session/`, opened by the server from a
trusted fixed argument. Preserve the expected nested registry directory layout.
Do not use a writable symlink from the vendor tree and do not make every hfs_map
writable. No current fac_cal inputs should be rewritten.

The capability must propagate through opened directories/files and be checked
for create, write, truncate, remove and both sides of rename. Use O_NOFOLLOW and
O_DIRECTORY on traversed directories (or Linux openat2 RESOLVE_BENEATH and
RESOLVE_NO_SYMLINKS); reject special files, hardlink aliases and cross-root
renames. Read descriptors must not acquire write rights just because they use a
mapped backend. Validate modes, size/offset limits and filename normalization.
Implement host fsync and directory fsync for committed rename/removal. Add
specific SELinux data type, init-owned directory and least-required permissions
only after the backend is correct; do not grant write to vendor files.

Before considering any device trial, run a host fixture matrix: ordinary
read/create/append/r+/truncate/rename/remove; short writes and EINTR; malformed
RPC lengths, bad modes and descriptor IDs; CR suffixes; dot/dotdot, repeated
slashes and absolute aliases; terminal/intermediate symlinks; cross-root rename;
read-only config/calibration protection; quota/full-disk/flush errors; crash
between temporary file write and rename. Preserve input JSON hashes and retain
request metadata (operation, virtual path, result) without logging calibration
payloads. The missing RGB groups should remain a separate hypothesis, not be
silently added to a write-support patch. Offline success still does not prove
ALS lux delivery or resolve the historical sensor_process crash.

## Automatic-brightness declaration and honest temporary UI

From the actual R18 framework resource APK, Android
`bool/config_automatic_brightness_available` (0x01110044) is **true**.
`framework-ext-res.apk` maps MIUI 0x11050001
`bool/android_config_automatic_brightness_available` to that same Android value.
This is static capability advertising, not proof a TYPE_LIGHT stream exists.

Actual Settings `MiuiDisplaySettings` creates `brightness_auto_mode_enable`
unconditionally (decompiled lines 454-462). It reads 0x11050001 at 488, but uses
that value to guard an observer update, not to remove/disable the toggle. Its
normal update writes `screen_brightness_mode` at 1216-1220. The only conditional
UI-disable path here concerns virtual-light-sensor devices plus face unlock;
it does not test whether this port provides TYPE_LIGHT. Consequently changing
one framework bool does not demonstrate correct MIUI UI behavior. Do not use
jadx's inferred Android resource names without checking numeric IDs: the AOSP
controller decompilation mislabeled its framework constant.

Until real stable lux exists, the truthful behavior is manual brightness with
an off/disabled automatic toggle explaining “ambient light sensor unavailable”.
Keep the manual slider functional and retain the current manual level. Treat
manual mode/resource correction as temporary capability correction, not ALS
repair; do not substitute CPU/other sensors, synthesize lux, expose a fake LIGHT
sensor, or claim merely hiding the preference fixed hardware. Any eventual ALS
HAL implementation must preserve accel/gyro stability and validate real lux,
activation/deactivation and multiple clients before auto-brightness is enabled.
No settings or capability declarations were changed in this task.

## Offline prototype delivered

A deliberately narrow prototype now exists at
`patches/hexagonrpc-registry-output-restricted.patch`, with repeatable real-C
backend, apps_std dispatch and builder tests under `scripts/ssc/registry-write/`.
It restores default mapped paths to read-only and permits one bounded registry
output file only under an explicit new private root. Unknown output names and
rename remain denied pending evidence. It applies to the pinned fork only, not
the current 435ed1c production tree; minimum backport to that production pin is
the preferred next integration step. No manifest change, Android ABI build,
DSP/device test or ALS success is claimed. See that directory's README and full
upstream dependency list before reviewing or applying the prototype.


## Follow-up: proven filename source and generator defect

The original generator source provides concrete per-group filename rules.
Offline QRD/449 filtering selected 24 of 26 actual JSONs, yielding 141 unique
names (plus the old empty `registry` gives historical 142). Missing RGB groups
were not generated. Per-input isolated generation preserves all source values.
However combined generation corrupts `sns_amd`: matching base and sw_enabled
files both emit that name, and openat lacks O_TRUNC/O_EXCL or merging. Explicit
base→enabled yields invalid JSON tail bytes; reverse order loses enabled. This
is a reproduced tooling bug, not a confirmed explanation of ALS/DSP failure.

`scripts/ssc/registry-schema/` now supplies an independent strict offline
validator/generator and nine tests. Default duplicates fail before output;
explicit disjoint merge retains every original field and rejects type/value/
version conflicts. Fresh-directory-only output avoids tail overwrite. Actual
merged generation has 141 valid files totaling 37,480 bytes, with all input
hashes unchanged. Outputs remain in cache and must not be installed.

Cache `registry-filename-allowlist-candidate.json` enumerates the 141 group paths,
old empty sentinel and historically observed parent `temp.json` (143 files
maximum). Proposed 256 KiB per file / 2 MiB aggregate limits are unvalidated
runtime bounds, not an authorization already added to the backend. The current
single-file backend is only a confinement test prototype; green fixtures do not
mean a usable SEE registry implementation. Exact allowlist/aggregate quota and
legacy temp normalization are concrete next changes; actual rename destinations
and consumption/activation behavior remain unverified. Production 435ed1c's
w/a error code 20 also differs from this fork prototype's 1, blocking deployment
until a minimal backport preserves the default protocol contract.


## Minimal production-pin backport candidate

A separate `patches/hexagonrpc-registry-production-backport.patch` now targets
435ed1c **plus the unchanged existing per-segment CR fix**, with isolated fixtures
and instructions in `scripts/ssc/registry-production/`. It does not import the
fork's 24 commits or change the manifest/library API. Default w/a error 20 and
the original dispatch table are preserved; only explicit opt-in selects new
methods/capabilities. The exact audited group-name set plus empty sentinel and
historical temp name is bounded to 143 files, 256 KiB each and 2 MiB aggregate.
Root-directory locking serializes aggregate checks across concurrent writers.
CLI preflight rejects populated old outputs before FastRPC is opened.

Host tests cover actual old interp2 framing/dispatch, 4096-byte opt-in writes,
short/oversized fetch rejection, extended32/33 refusal without fopen aliasing,
default contract/CR reads, path confinement, real sync/error propagation and
concurrent quota limits. Only the transport is mocked. No data is prepopulated.
This remains incomplete for actual SEE use: truncate RPC32, remove and rename
RPC33/commit semantics are deliberately unsupported; Android ABI, SELinux, real
DSP protocol/recovery and ALS behavior are unverified. A tested bounded file
server is not proof the original activation crash is fixed.
