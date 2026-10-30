# Sensor producer audit and no-sample candidate

This directory contains offline checks only; neither script accesses devices or
starts services. See `docs/hyperos-r19-sensor-producer-audit.md` for exact binary
identity and the source-provenance limitation.

- `audit-hal-binary.py` pins the actual deployed HAL SHA, decodes its embedded
  mini-debug symbols and checks key ARM64 instructions.
- `test-unavailable-sample.py` extracts real baseline/candidate HAL methods and
  runs ASan/UBSan fixtures. Default source snapshots live under
  `~/.cache/hos-r19-fusion-probe/producer-audit`; use `--baseline/--candidate` to
  point at independent copies.
- `test-empty-event-post.py` extracts the actual sink body, reproduces the old
  empty-vector front() misuse, and verifies no FMQ/wake/wakelock action for an
  empty candidate event list while retaining nonempty zero events.
- The candidate patch is `patches/hos-sensors-no-unavailable-vector-events.patch`.
  It gates event existence on `Sample.valid` using one snapshot, not on nonzero
  numeric values. Valid zero remains a real event.

Historical complete build-input hashes are unavailable. Do not claim the entire
HAL has been byte-reproduced merely because the relevant source methods match
actual ELF behavior. Paired Android baseline/candidate compilation is complete in
`~/.cache/hos-r20-sensors-hal`; the rebuilt baseline is not byte-identical to FFC.
Three-way dynamic ABI checks pass. A fresh-boot device comparison remains pending.

## R21 bounded startup/report observation candidate

`patches/hos-ssc-startup-observation.patch` targets only `SscHub.cpp` from
`~/.cache/hos-r19-fusion-probe/producer-audit/candidate-src/`, baseline SHA256
`c081c6917313ba5ea2b1b3b8af4bc5066e6e51f8936e1869ab7fedd94f0e6ccf`.
Apply from an isolated copy of that HAL source directory:

```
git apply --check /path/to/patches/hos-ssc-startup-observation.patch
git apply /path/to/patches/hos-ssc-startup-observation.patch
```

This diagnostic is separate from the unavailable-vector behavior patch. It does
not modify waits, retries, sessions, activation decisions, DSP messages, sample
values, accuracy handling, cache updates or fusion. No extra sensor is queried.
Open and registry-wait receive begin/end markers, FindSensor accel/gyro receives
begin/end with round, elapsed time, success and bounded160-byte error text.
Successful calls log empty error rather than a stale prior failure string.

After each existing enable attempt (including watchdog re-enable), at most the
first8 parsed measurement vectors of each enabled sensor are logged before cache
storage: original protobuf float32 bit patterns via memcpy (not numeric casts),
raw accuracy, `r.timestamp`, enable time and batch receive time. Nonmeasurement,
malformed or fewer-than3-vector packets retain original skip behavior and do not
consume this vector budget. No numeric range rejection or clamping is added.

`SscReport` was checked in `device/huawei/gaokun3/ssc/ssc_client.h`: msg_id uint32,
timestamp/uid_low/uid_high uint64, payload string. Its timestamp is the DSP's
19.2MHz timer; do not interpret it as host nanoseconds. Host times use
CLOCK_MONOTONIC nanoseconds (excludes suspend), receive time is captured just after
ReadReports returns and shared by reports in that batch, not a kernel socket
arrival timestamp. Logging adds timing overhead and does not measure pure DSP
latency. Match report UID exactly to existing accel/gyro UIDs as before.

All added diagnostics share a thread-safe saturating process budget:511 records
plus one budget-exhausted marker, then silence. Per-enable counters stop at8;
there is no unbounded per-frame logging. Once the process budget is exhausted,
later activations are intentionally not fully observed. Existing production logs
are unchanged and outside this diagnostic budget.

Independent cache candidate/verification record:
`~/.cache/hos-r21-startup-observation/`. Patch SHA256
`4fb4ba7aaa03303c3bcbb735f9aad87cd44acc72b84677a6fcaf3ac7421096d0`.
Application check passed; static before/after counts of Open, WaitForService,
FindSensor, EnableContinuous, Disable and ReadReports calls are identical.
No device source was edited, no device/VM action or remote build performed.
The candidate still requires compilation and independent source review before
any deployment; an application check is not a build/runtime result.

### Isolated Android build completed

The observation candidate was subsequently cross-compiled on the existing A17
build host in `/home/vahiru/hos-r21-startup-observation`. Ninja was used only with
`-t commands`; the selected four compiler commands, linker response file and
strip command were replayed with source paths, object/depfile/output paths,
ThinLTO cache and temporary directory redirected into that independent job.
No regular ninja module build or source/output transaction was required.

Both R21 baseline and observation candidate compiled/linked/stripped. Their
source manifests differ only in `SscHub.cpp`; producer valid-sample gating,
empty-event sink handling, and absence of ALS activation remain unchanged.

Local candidate:
`~/.cache/hos-r21-startup-observation/remote/candidate/module/android.hardware.sensors-service.gaokun3`

- Candidate SHA256: `476a01d2301792c7fcf39e42e8c8285319c4e541742740847f2e88ab92997f64` (394896 bytes).
- Isolated rebuilt baseline SHA256: `e111271c6134520cc7553a2b8696749a48367f2bfcfb04e7f6fe6b43fef57055`.
- CICD current ALS product (not the deployed R21 HAL) remains `99cd5ecb0b0bbb6f248e928e5f2102baf5319f15fcbe14564a4ef1af923e9f0a`.
- Actual deployed R21 HAL: `ab794e19343bfee8e3690d656203ff4b3a737189c8527591dfaa4029b0f548d4`, archived under `~/.cache/hos-r20-sensors-hal/artifacts/candidate/`.

The rebuilt baseline is not byte-identical to actual R21 ab794. All11 baseline
source files exactly match both the R20 candidate build manifest and archived
candidate-src used for R21. The historical static-library input hashes were not
reconstructed, so source identity is not a whole-binary reproduction claim.
Section comparison finds differences in .text, .rodata, .data, .data.rel.ro,
.dynsym, .eh_frame, .eh_frame_hdr, .got, build-id and malloc_hook; this is not merely
a debug-section difference. No functional-equivalence claim follows from that.

Candidate DT_NEEDED, exported dynamic symbol contracts and imported symbol sets
are identical against actual R21 ab794, isolated rebuilt baseline e111, and the
separate CICD ALS product99cd. There are no added/removed dynamic imports. These
are ABI/linkage checks, not execution equivalence or runtime validation.
`remote/abi-comparison.json` now names the inputs `r21_deployed`,
`rebuilt_baseline`, `cicd_current_als_product`, and `candidate`;
`remote/r21-source-identity.json` records the11-file source verification. The
original remote-artifacts.tar is an earlier audit snapshot whose old
`deployed_baseline` label incorrectly meant the CICD product; the corrected JSON
and this distinction supersede that label.

Before/after snapshots cover37 source, SSC, module-cache, derived prebuilt/symbol
mapping and product vendor/symbol/rc/xml entries. Content, mode and mtime match
exactly; no restoration was needed because CICD inputs/outputs were never edited.
Commands, response input, source manifests, compiler logs, ABI audits and
protection snapshots are archived beside the local artifact under `remote/`.
No device deployment, image generation, VM startup or ALS activation occurred.
