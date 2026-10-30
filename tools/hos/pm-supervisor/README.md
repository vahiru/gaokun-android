# Fixed-boot device-role suspend observation supervisor (candidate)

This is an opt-in short diagnostic, not a persistent suspend-policy fix. No device
operation has been performed during preparation. It never changes USB role,
properties, pm_test, mem_sleep or an RTC sysfs alarm. It does not infer PASS from
completion: assess recorded suspend counters, actual time and wake sources.

The fixed target is boot64b3ea5c-0428-4d05-ba5a-bda6586d4dd3 with super_hos,
allow_suspend=0, device_only=true, no loaded modules, the confirmed vreg_l9d DT
always-on property, role=device, frameworkAsleep, pm_test[none], mem_sleep[s2idle].
These are checked before READY and again after gate approval. The fixed adbd PID
is845; a different boot/PID or unsupported layout requires a new review.

## Lifecycle

1. `--prepare` creates a fresh root-owned0700 diagnostic directory
   `/data/local/tmp/hos-r22-device-pm-native` (no reuse). A forked supervisor blocks
   HUP/INT/TERM/QUIT for signalfd processing, calls setsid, creates the new cgroup2
   `/sys/fs/cgroup/hos-r22-pm-test`, and migrates itself via cgroup.procs. It checks
   cgroup2 magic, own SID/PGID, exact sole v2 membership, and different membership
   from adbd845. Mixed v1 lines are retained in the log, not mistaken for v2.
2. It creates an unarmed CLOCK_BOOTTIME_ALARM timerfd. Unsupported/permission
   failures refuse; there is no fallback to ordinary sleep or RTC alarm writes.
3. It confirms the three original locks, acquires and reads back its own unique
   `hos_r22_pm_test_native` lock, and writes READY with boot/PID/SID/token. Only
   then does it acknowledge the launcher and let the launcher exit; stdin/stdout/
   stderr were detached to /dev/null. No original lock has been released.
4. Main must independently verify the supervisor is still alive, separate from
   adbd's process group/v2 cgroup, and all four locks present. Main explicitly
   creates root-owned0600 regular single-link `GO` under the output directory,
   containing exactly the32-character READY token, without a newline. Symlinks,
   nonregular files, wrong owner/mode/size/token refuse. This gate is internal
   coordination under the existing task authorization, not a new user approval.
   Gate wait is at most300 BOOTTIME seconds; a signal/error/timeout restores and
   exits without arming or releasing original locks.
5. After gate and precondition/isolation revalidation, capture counters. Arm an
   absolute timerfd deadline20 BOOTTIME seconds ahead. Only after successful arm
   does it release the three known locks and its own lock, then read back absence.
   Other kernel/framework blockers remain; no manual `echo mem` is used. This
   allows the existing natural SystemSuspend path for a bounded observation.
6. Timer expiry, handled signal, partial release or other failure uses one cleanup
   path: acquire own lock, re-acquire all three originals, read back all four,
   then and only then unlock own. Any failure preserves/attempts protection and
   returns failure; it never knowingly releases its own lock without readback.
   Boot/role/properties are checked at finish; unexpected changes cause failure
   and re-acquisition of own protection. Events contain actual elapsed time,
   before/after suspend_stats, wakeup_sources, locks and role, plus exit reason.

No root-lock restoration guarantee is possible after SIGKILL, process/kernel
crash or power loss. The session/cgroup separation specifically addresses adbd
transport/process-group cleanup, not arbitrary root killing the independent
cgroup. The timerfd belongs to this process and is closed if it is killed: it is
not a separate failsafe daemon. Main must not kill this supervisor during a
window. Signalled/early-aborted tests are reported as aborted, never as a20-second
sleep PASS. If recovery cannot acquire/read back locks, leave the logged state
for manual recovery; do not delete locks or retry blindly.

The cgroup/output directory remain as audit evidence after exit. No autonomous
retry, old-directory removal or role switch exists. Vendor permanent device-only
protection is unchanged. A completed diagnostic is not authorization to remove
those locks from production or to switch automatically tohost.

## Host checks and build command

The actual shared lock-contract header is compiled in host failure tests:

```
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/hos/pm-supervisor/test_lock_contract.c -o /tmp/hos-pm-lock-test
/tmp/hos-pm-lock-test
```

Tests cover arm failure before any release; each partial-release failure followed
by recovery; each restoration write failure; read failure before/after release;
successful writes with missing readback; missing initial locks; exact lock tokens;
mixed cgroup hierarchies, duplicate/missing/malformed0::, output bounds and fake
path-prefix match. ASan/UBSan passed. This does not claim real cgroup/signal/alarm
integration was tested on a device.

Android NDK28.2/API35 syntax-only -Wall/-Wextra/-Werror check passed. Prepare a
small binary in a new cache path using (not executed for deployment here):

```
/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang \
  -std=c11 -O2 -Wall -Wextra -Werror \
  tools/hos/pm-supervisor/device_pm_supervisor.c \
  -o /NEW_CACHE/device-pm-supervisor
```

No privileged execution, installation, big image build or VM startup is part of
this preparation. Candidate must be independently reviewed before preparing a
READY process on the device.

### Prepared arm64 candidate

The small Android binary has now been built locally with the command above:
`~/.cache/hos-r22-pm-supervisor/device-pm-supervisor` (27776 bytes), API35,
SHA256 `884560f08d919904498ba1024485c309c9f4e5af67e95721925013a8178d1d32`.
Its adjacent manifest records exact source/header/test hashes and compiler argv;
elf-audit.txt and host-tests.txt preserve validation output. No deployment,
READY process or GO gate has been performed. Main will review and conduct any
READY-only survival/cgroup check separately before considering GO.
