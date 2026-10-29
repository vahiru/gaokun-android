# Optional port0 device-only policy

`persist.vendor.gaokun3.usbrole.device_only=true` (also `1`) keeps the
`a600000.usb` / port0 data role in device mode. Missing, `false`, or `0`
retains/restores normal automatic role selection. No shipping default is
changed. The property uses the existing `persist.vendor.gaokun3.` SELinux
property prefix, so no new property or policy type is needed.

The USB HAL stays running and continues port-status and android_usb gadget
connection reporting. It waits for property serial changes rather than
restarting or periodically polling. Host requests from Settings report
`NOT_SUPPORTED`, `canChangeMode` becomes false, and the final HAL role-write
boundary rejects host even if the policy changes during an automatic
selection. A pending host transaction fails once the policy is enabled.
RoleFollow discards a previous Settings choice on a policy transition, so
restoring false resumes automatic behavior instead of resurrecting a stale
host request. Normal Type-C/L9d safety prerequisites for HAL role control
remain intact; boot and property-triggered shell service handle device
selection when those prerequisites are absent.

The screen-off script rechecks policy while waiting for unplug, before
writing a role, while waiting for the controller, and before releasing its
suspend lock. Existing downstream devices no longer keep port0 in host
when device-only is enabled. The init trigger acquires an additional
`gaokun3_usb_device_only` lock before stopping in-flight role services and
starting device selection. This lock protects against a concurrent host
oneshot crossing its final property check. On false/0, init acquires the
ordinary role lock before releasing the extra lock; screen-off with
allow_suspend=1 resumes the normal verified-host path. The property does
not change allow_suspend itself. Device-only deliberately keeps suspend
blocked, even with allow_suspend=1.

All shipping role writes were reviewed: Follow's source-power decision,
no-data probing/charger fallback, Settings restoration and detach path
use Port.cpp::WriteRole. Boot and screen changes call the same vendor
role script. Gadget UDC bindings and soft-connect writes do not choose a
host role. No other USB controller or physical port is changed. Manually
invoked historical scripts under scripts/s2idle and arbitrary root sysfs
writes are outside this policy's enforcement boundary.

## Validation and deployment

`python3 scripts/usb/test-device-only.py` compiles actual Follow.cpp with
mock hardware I/O and the actual Port.cpp role-write function, then runs
the real shell control flow against temporary sysfs and Android command
fixtures. It covers missing/false/0 defaults, true/1 behavior, source and
sink partners, idle charger probing, Settings choice clearing, detach
pullup reset, port0 downstream devices, and policy activation during
unplug waiting or host settling. This is host-side validation, not an
Android HAL build, SELinux boot test, or physical USB acceptance test.

Minimum installed files to update together:

- `/vendor/bin/hw/android.hardware.usb-service.gaokun3` (rebuild the USB HAL).
- `/vendor/bin/gaokun3-usbrole.sh`.
- `/vendor/etc/init/usbrole.rc` (init reads this at boot).

No kernel, DTB, system framework, property_contexts or sepolicy change is
required. A persistent deployment needs the updated vendor files and a
boot that imports the new init actions. Merely replacing the init file in
a running system does not activate its new triggers. After that, changing
the property should work without stopping/restarting the HAL. Setting
false restores role policy but does not revert an independently configured
allow_suspend=0. Runtime acceptance should check port and gadget callbacks,
USB enumeration, screen-off behavior, reboot persistence and false restore.

## Android build verification

Both this USB HAL and the independent Thermal HAL candidate subsequently
compiled successfully against the existing A17 crDroid tree with
`lunch lineage_gaokun3-cp2a-userdebug` and a targeted `m -j16` build. No
source changes were needed for cross-compilation. The USB aarch64 binary
SHA256 is `b123b2269a51297d19d25552a3066f25a89f74c2553961ce70d15d9189b74598`.
Artifacts, input hashes, build log and original-source restoration evidence
are in `/Users/vahiru/.cache/hos-r17-hal/`. The original remote source files
were backed up and restored byte-for-byte after the build. This establishes
build compatibility; runtime USB and init-action acceptance remains separate.
