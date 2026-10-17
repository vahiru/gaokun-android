# Android on the Huawei MateBook E Go (SC8280XP / `gaokun3`)

**crDroid 16.0 (Android 16) on a mainline Linux kernel, with hardware Vulkan on
the Adreno 690.**

Qualcomm never shipped an Android BSP for the 8cx family — only Windows and
Linux drivers. There is no stock Android ROM to lift vendor blobs from, and the
machine has none of the usual Android plumbing: no `fastboot`, no Android
bootloader, no recovery partition and no serial console — just UEFI. So this is
not a normal device port: it is *AOSP on mainline*, with every HAL built on top
of upstream drivers.

> ### ⚠️ Alpha. Read this first.
> Latest release: **v0.7.1-alpha**. Games run well, and the machine sleeps and
> wakes properly.
> **There is still no working recovery and no working factory reset** — see
> [Known limitations](#known-limitations).
> The command-line installer (and the graphical installer's *Erase the whole
> disk*) **erases the internal disk, Windows included**; only the graphical
> installer's dual-boot mode (preview) keeps Windows. No image of the factory
> state exists anywhere. You need to be comfortable recovering a machine that
> will not boot. No warranty of any kind.
>
> Before you install, also know: **`/data` is not encrypted**, **root is built
> into the kernel**, the images are **signed with Android's public test keys**,
> and up to v0.7.1 **adb over the network is open without authorization**.
> Details in [Known limitations](#known-limitations); common questions (logs,
> the boot menu, wiping before you sell) in the [FAQ](docs/FAQ.md).

[**中文说明 → README.zh-CN.md**](README.zh-CN.md)

**Talk to us:** [Telegram](https://t.me/gaokunAndroid) · QQ group **920133252**

---

## Status

As of v0.7.1-alpha. Every ✅ was measured on hardware, not inferred; whatever
has not been tested says so. The evidence is in [`docs/`](docs/), the case
numbers (#NN) are in [`docs/stage4-findings.md`](docs/stage4-findings.md).

| Area | State | Notes |
|---|:--:|---|
| Boot (UEFI + systemd-boot, internal disk) | ✅ | No USB media required. A/B slots with Virtual A/B; system updates (kernel included) install from Settings |
| Display 1600×2560 @ 120 Hz | ✅ | The framework default pinned rendering to 60; overridden, measured 8.33 ms vsync |
| GPU — Adreno 690, hardware Vulkan | ✅ | Mesa 26.0.3 `turnip`; zero SMMU faults over a 22-minute soak |
| Touchscreen | ⚠️ | Works — Himax HX83121A, needs the gpio174 patch in `patches/` ([#26](docs/stage4-findings.md#26)). **Since v0.6.2 touch is tuned from measurements, not feel**: (1) the jump-detection threshold in the shipped preset was in effect a 1.0 m/s speed limit above which the driver reported nothing — one fast flick arrived as a dozen touches; the test is now against the predicted position ([#114](docs/stage4-findings.md#114)); (2) coordinate `fuzz` 8 → 0: measured centroid jitter is under half an output unit, while fuzz 8 was discarding a quarter of real motion ([#116](docs/stage4-findings.md#116)); (3) press latency 25 → 17 ms; (4) contact size and pressure are reported, for apps that read them (Android's own palm rejection is off on this device, so it does not use them). Six driver defects fixed plus per-stage counters and raw capacitance-frame dumps ([#115](docs/stage4-findings.md#115)). ⚠️ A palm on the panel fragments into several contacts (taps still work) |
| Detachable keyboard + touchpad | ✅ | USB HID `12d1:10b8`; can be switched off in *Settings › System › Detachable keyboard* |
| Keyboard cover as a lid | ⚠️ | The EC reports the lid switch, but Android does nothing with it: closing the cover does not turn the screen off — the screen-off timeout does |
| Wi-Fi | ✅ | ath11k / WCN6855. Measured 61.7 MB/s pulling 200 MB over the LAN ([#44](docs/stage4-findings.md#44)). Downloads from far-away servers (~300 ms) used to cap at about 3.5 MB/s per connection — Android's default TCP buffers; raised in v0.7.0, measured 3.5 → 9.5 MB/s ([#119](docs/stage4-findings.md#119)). WPA3-SAE connects ([#107](docs/stage4-findings.md#107)); Android's automatic WPA2 → WPA3 upgrade on mixed-mode routers is off since v0.7.1, because some of them rejected it. [Issue #2](https://github.com/vahiru/gaokun-android/issues/2) (pure WPA3, kicked after association on a ZTE router) does not reproduce here. ⚠️ The MAC address changes on every boot |
| Wi-Fi hotspot | ⚠️ | Works since v0.7.1 ([#11](https://github.com/vahiru/gaokun-android/issues/11)) — the hotspot comes up; a phone actually joining it has not been tested. ⚠️ Turning it on disconnects the tablet from Wi-Fi (no Wi-Fi + hotspot combination is configured yet), so there is no uplink to share |
| Bluetooth | ⚠️ | Works — `hci_qca`, adapter `ON`, zero crashes at boot. ⚠️ Playback to Bluetooth headphones (A2DP) has **not been verified** on hardware. The audio policy has no Bluetooth SCO path, so **a Bluetooth headset's microphone cannot be used for calls**. Can deadlock after long uptime, together with audio ([#38](docs/stage4-findings.md#38)) |
| Speakers | ⚠️ | Work — WSA883x via audioreach. The gain staging used to clip (THD −20 dB on −6 dBFS material); since v0.6.0 digital gain is pinned at unity and loudness comes from the PA, about 20 dB cleaner ([#86](docs/stage4-findings.md#86)). Optional *Speaker enhancement (experimental)* in *Settings › Sound*, off by default (v0.7.0, thanks @mashen11). Since v0.7.1 playback is paced by the sound hardware and nothing is dropped under load, so rhythm games stay in sync ([#130](docs/stage4-findings.md#130)). ⚠️ Audio can deadlock after long uptime, together with Bluetooth ([#38](docs/stage4-findings.md#38)) |
| Headphone jack | ✅ | **Fixed, user-confirmed.** Three separate blockers: the RX macro's interpolator stage was never wired up (so the backend refused to open — with no kernel message at all), the audio policy declared no wired output, and `WiredAccessoryManager` was watching `/sys/class/switch/h2w`, which does not exist on mainline ([#40](docs/stage4-findings.md#40)) |
| Microphones | ⚠️ | **Built-in microphones record since v0.7.0** ([PR #10](https://github.com/vahiru/gaokun-android/pull/10), thanks @mashen11; [#127](docs/stage4-findings.md#127)). They had never worked: the front-end mixer and DMIC sequence were never set, the HAL overwrote the microphone's address, the policy offered a rate the hardware refuses, and a pipe throttle dropped about one block in eight. ⚠️ **The microphone of a wired headset is not used** — calls and recordings with a 4-pole headset still take the built-in microphones |
| USB audio | ❌ | USB headsets and USB sound cards are not routed (the kernel driver is there, the audio HAL has no USB module) |
| Battery, charging | ⚠️ | Huawei EC driver. ⚠️ On a low-power source (a computer's USB port, a small phone charger) the battery can show *charging* while it drains, and Android then never does its low-battery shutdown — the machine switches off hard at 0%. Battery temperature reads 0 |
| **Gaming** | ✅ | Genshin Impact at max graphics, smooth. GPU idles at 270 MHz, peaks 690 MHz, 50 °C. v0.7.1 smoke test: Arknights, Delta Force, Strinova, Phigros, Arcaea run. ⚠️ *League of Legends: Wild Rift* closes right after launch (reported, no log yet) |
| CPU thermal throttling | ✅ | Mainline DTS has **no** CPU cooling maps at all — fixed in [`patches/0009`](patches/) |
| **Suspend / standby** | ✅ | **Fixed 2026-08-22 — and the cause was ours, not the kernel's** ([#52](docs/stage4-findings.md#52), [#57](docs/stage4-findings.md#57)). Real suspend and resume, no resets. v0.7.1 fixed a hang on waking after a long uptime ([#16](https://github.com/vahiru/gaokun-android/issues/16), [#131](docs/stage4-findings.md#131)). With a computer attached over USB the screen turns off but the machine stays awake, so USB adb keeps working. Not measured yet: battery drain over a night of standby |
| Sensors (accel + gyro) | ✅ | **Auto-rotate works, confirmed on device.** A sensors HAL written for this port feeds real accelerometer and gyroscope data to SensorService; the framework derives Game Rotation Vector / Gravity / Linear Acceleration from them. The factory mount matrix is all zeros (that calibration data died with Windows), but the sensor frame matches the panel, so no correction was needed. **No magnetometer** (so no compass). The light sensor answers on the bus, but activating it crashes the sensor DSP, so it stays off — no auto-brightness ([#121](docs/stage4-findings.md#121)). ⚠️ If that DSP ever crashes and restarts, the sensors are gone until a reboot ([#121](docs/stage4-findings.md#121)) |
| Hardware video decode | ✅ | **`qcom-iris` since v0.7.0** (was `qcom-venus`): H.264, HEVC and VP9 through `c2.v4l2.*.decoder`, including stop, seek, replay and mid-stream resolution changes, after fixes to the upstream driver ([#128](docs/stage4-findings.md#128)). VP8 is decoded in software. The two `external/v4l2_codec2` portability bugs found when decode was first brought up are in [#41](docs/stage4-findings.md#41) |
| Hardware video encode | ❌ | Deliberately off: `v4l2_codec2`'s encoder does not convert the RGBX frames it is handed into the NV12 the hardware takes, and if enabled it would make apps fail instead of falling back. Apps use the software encoders |
| Camera | ✅ | **Both cameras work** (since v0.6.1). Front is a Hynix hi846, rear an **OmniVision OV13B10** — identified by recovering the power sequence from Huawei’s Windows driver package ([#106](docs/stage4-findings.md#106)). Path: mainline `camss` → libcamera *simple* pipeline with the software ISP → libyuv → an AIDL HAL written for this port. Flash works; the rear camera has **autofocus** (v0.7.0, thanks @mashen11); photos are rotated the way the app asks. The power-domain defect that made every second capture fail is fixed ([`patches/0031`](patches/), [#105](docs/stage4-findings.md#105)). ⚠️ At most 15 fps, no zoom, no exposure compensation. Image quality is not tuned: no colour matrix, dim scenes are noisy, flash shots overexpose; autofocus in the dark is slow. **Video recording has not been verified** on hardware |
| USB-C | ⚠️ | UCSI comes up and both connectors register ([#112](docs/stage4-findings.md#112)). The data role follows what is on the other end — a computer gets us as a device; a hub or flash drive should get us as the host, which is designed for but not yet tried with real hardware ([`patches/0048`](patches/), v0.7.0) — and since v0.7.1 Android's USB service runs, so apps can use USB devices ([#13](https://github.com/vahiru/gaokun-android/issues/13)). ⚠️ **After replugging (seen after standby) the port can stop working until a reboot** — and, going by the code, the machine then also stays out of standby until that reboot. File transfer to a computer works (MTP/PTP: choose it under Settings > Connected devices > USB; USB 2.0 speed). ⚠️ **USB flash drives are not mounted** (Android has no removable-storage configuration yet). DisplayPort alt-mode is untested |
| Fingerprint | ❌ | In progress: Huawei's signed fingerprint app loads into the secure world on this machine ([#125](docs/stage4-findings.md#125)); there is no driver or HAL yet |
| Stylus (M-Pencil), TPM | ❌ | No support |
| Root | ⚠️ | KernelSU (the ReSukiSU fork) is **built into every kernel** and cannot be switched off. It stays dormant until you install the ReSukiSU manager app; then only apps you approve there get root. Apps that look for root or an unlocked boot chain may refuse to run ([details](docs/known-limitations.md#root-is-built-in-kernelsu--resukisu)) |
| DRM (Widevine) | ❌ | No DRM module at all: Netflix, Disney+, Prime Video and the like do not play their shows ([details](docs/known-limitations.md#no-drm-protected-video-no-widevine)) |
| SELinux | ⚠️ | `permissive`. Seven rounds of policy work towards enforcing; in enforcing trial runs the main functions work, the camera included ([#129](docs/stage4-findings.md#129)) |

## Hardware

| | |
|---|---|
| SoC | Qualcomm Snapdragon 8cx Gen 3 (SC8280XP) |
| Model | HUAWEI GK-W7X, 2022, CSOT panel — the only model this has been built and tested on |
| BIOS | Any version. (Earlier versions of this page said 2.16 only and warned against 2.17; that restriction was lifted on 2026-09-25 — it has been verified not to depend on the BIOS version. Please do not downgrade.) |
| GPU | Adreno 690 |
| Panel | Himax HX83121A, MIPI-DSI, 1600×2560 — the same panel as the Galaxy Tab S7 FE |
| Wi-Fi / BT | WCN6855 |
| Storage | NVMe |
| Firmware | UEFI. Secure Boot must be disabled |

---

## Installing

Take the latest [**Release**](https://github.com/vahiru/gaokun-android/releases)
and follow [`docs/INSTALL.md`](docs/INSTALL.md) (English; 中文:
[`docs/INSTALL.zh-CN.md`](docs/INSTALL.zh-CN.md)). Any BIOS version
works; Secure Boot must be off. There are two installers:

| | Graphical installer (preview) | Command-line installer |
|---|---|---|
| What it can do | **Install next to Windows** (dual boot), erase the whole disk, **reinstall Android** in place (wiping data by default, or keeping it), adjust partitions by hand | Erase the whole disk and install |
| Runs from | Its own small Linux: a USB stick, or — without any USB stick — started from Windows with `gaokun3-setup.cmd` | An arm64 Linux live USB plus a checkout of this repository (a generic Ubuntu/Debian image booting on this machine has not been verified) |
| System image | Carried on the medium, or downloaded over Wi-Fi | `super.img.zst` + `boot.img` + `install-artifacts.sha256` from the release |
| Files | `gaokun3-installer-0.1.0-preview-*` on the [v0.7.0-alpha release](https://github.com/vahiru/gaokun-android/releases/tag/v0.7.0-alpha) | `scripts/install-gaokun3.sh` |
| Tested on hardware | Only *Reinstall Android, keeping data*. Dual boot, erasing the disk and adjusting partitions have run on test disks only; the Windows script only in a virtual machine; the USB image has not been booted yet | The original install path. Since 2026-09-24 it runs on the graphical installer's backend, and the layout below has so far been created on test disks only, not on a real machine |

Erasing the whole disk creates this layout:

| Partition | Size | Purpose |
|---|---|---|
| `esp` | 300 MiB | systemd-boot, plus the kernel / DTB / ramdisk it loads, one directory per slot |
| `misc` | 4 MiB | A/B slot state |
| `metadata` | 32 MiB | Android metadata |
| `super` | 12 GiB | system / system_ext / product / vendor, A/B |
| `boot_a`, `boot_b` | 64 MiB each | Android boot images; OTA updates write these, and the ESP copies are unpacked from them |
| `gk3rescue` | 1 GiB | Optional rescue system — see below |
| `userdata` | rest of the disk | `/data` |

**The rescue system.** This machine has no working Android recovery and no
serial console, so a small Linux you can boot from the menu is how you repair
it. The installer puts it on its own 1 GiB partition, as a **non-default** entry
in the boot menu (every boot shows the menu for 15 seconds): if Android hangs,
hold the power button and pick the rescue entry. Nothing falls back to it on its
own. The graphical installer installs it; the command-line installer only if it
finds the rescue image — releases do not include it, so from a
generic live USB you get Android only.

> Earlier versions of this README described a ~25 GiB Ubuntu rescue partition
> as the default boot entry. That is gone (2026-09-24); see
> [`docs/INSTALL.md`](docs/INSTALL.md#about-the-rescue-system).

**Logging in to the rescue system over SSH** needs your public key, put on the
installer stick before installing — the published image carries nobody's key.
See
[`docs/INSTALL.md`](docs/INSTALL.md#about-the-rescue-system).

**Downloading from mainland China:** if GitHub's download servers are slow or
unreachable, the system images (`boot.img`, `super.img.zst`,
`install-artifacts.sha256`) are mirrored at
`https://ota.072172.xyz/install/<build>/<file>`, where `<build>` is the name of
that release's OTA package without `.zip` — see
[`docs/INSTALL.md`](docs/INSTALL.md#downloads). The graphical installer
downloads from this mirror itself; the installer's own files are on GitHub only
for now.

**Updating:** from v0.2.x onward, update in Settings — the in-system updater
installs into the inactive slot and the new version starts on the next reboot.

---

## Known limitations

The full list, with workarounds, is in
[`docs/known-limitations.md`](docs/known-limitations.md); the latest release
notes ([v0.7.1-alpha](docs/relnotes/v0.7.1-alpha.md)) list what changed. In short:

* **Security and privacy.** `/data` is **not encrypted** — anyone with the
  machine in hand can read everything on it; the lock screen does not change
  that. Root (ReSukiSU, a KernelSU fork) is **built into every kernel**; it
  stays dormant until you install its manager app, but apps that look for root
  or an unlocked boot chain may refuse to run. Images and updates are signed
  with **Android's public test keys**, so anyone can sign an update or system
  app this device will accept. Up to v0.7.1, **adb over Wi-Fi (port 5555) is on
  with no authorization prompt** — anyone on the same network can get a root
  shell; release builds will turn this off (planned for 1.0). SELinux is
  `permissive`.
* **No working recovery**, so *Erase all data* in Settings does nothing. To wipe
  the device, use the graphical installer's *Reinstall Android*.
* **Google.** Release images include Google apps (MindTheGapps); there is no
  build without them. The Play Store reports the device as uncertified until
  you register it ([INSTALL §4](docs/INSTALL.md#this-device-isnt-play-protect-certified)),
  and Play Integrity fails. No Widevine, so Netflix-style paid streaming does
  not play. No GPS, and network location comes from Google services. No
  Chinese input method is preinstalled.
* **Not supported:** fingerprint (in progress), stylus, TPM, USB flash drives,
  USB audio, hardware video encoding, the Bluetooth headset microphone, the
  wired headset microphone, auto-brightness.
* **Known bugs:** audio and Bluetooth can deadlock after long uptime (a watchdog
  saves evidence under `/data/vendor/gaokun3/hangdump-*` — please attach it);
  plugged into a computer, the tablet may end up powering the computer and
  neither side sees the other (replug); the USB-C port can stop working after
  replugging until a reboot; a palm
  fragments into several touches; the hotspot drops Wi-Fi; *Wild Rift* closes
  on launch; the battery can show *charging* while draining on low-power
  sources.
* **Proprietary components.** Release images contain Huawei firmware and the
  Huawei Histen audio library, which are **not** in this repository.

---

## Building

A Linux host with at least 64 GB of RAM (AOSP's own recommendation; we never
build with less — [`docs/build-machine.md`](docs/build-machine.md)) and about
400 GB of disk.

```sh
repo init -u https://github.com/crdroidandroid/android.git -b 16.0 --git-lfs
cp <this repo>/manifests/local_manifest_gaokun3.xml .repo/local_manifests/
repo sync -c -j"$(nproc)"

cp -a <this repo>/device/huawei/gaokun3 device/huawei/gaokun3
python3 <this repo>/scripts/crdroid-tree-fixes.py .   # read the script for why
source build/envsetup.sh
lunch lineage_gaokun3-bp4a-userdebug
m bacon superimage
```

* Run `bacon` and `superimage` in **one** `m` invocation: two invocations give
  the OTA package and `super.img` different build stamps, and they no longer
  recognise each other ([`scripts/release.sh`](scripts/release.sh)).
* Build **`userdebug`**, not `user`: a `user` build forces SELinux enforcing,
  and this port's policy cannot boot that yet.
* By default this is a **release build** (still the `userdebug` variant): adb
  is off until you enable it and asks to authorize each computer, nothing
  listens on TCP 5555, `ro.debuggable` is 0 and no adb key is built in.
  `GAOKUN3_DEV_BUILD=1 m bacon superimage` gives a **development build** with
  the old conveniences (adb without authorization, TCP 5555,
  `ro.debuggable=1`, your key built in) — never publish one. The switch is in
  [`lineage_gaokun3.mk`](device/huawei/gaokun3/lineage_gaokun3.mk); it is new
  since v0.7.1-alpha.
* Some build inputs are **not** in this repository; each directory's README
  says how to produce them: `firmware/` (Huawei firmware, from your own
  machine — [`firmware/README.md`](device/huawei/gaokun3/firmware/README.md)),
  `hexagonrpcd-root/` (sensor DSP files), `prebuilt-boot/` (the kernel, below),
  and `effects/prebuilt/` (the Histen library; without it the speaker
  enhancement is silently absent). Development builds also need `adb_keys`
  (your own adb public key:
  `cp ~/.android/adbkey.pub device/huawei/gaokun3/adb_keys`); release builds
  do not use it.

The kernel is built separately, in layers: mainline **v7.2-rc2**
(`8cdeaa50eae8dad34885515f62559ee83e7e8dda`), the
[`linux-gaokun-buildbot`](https://github.com/KawaiiHachimi/linux-gaokun-buildbot)
patches on top, then this repository's [`patches/`](patches/)
(`scripts/kernel-apply-patches.sh <tree>`, idempotent) and ReSukiSU
(`scripts/kernel-setup-resukisu.sh <tree>`). Exactly which sources went into a
given release's kernel is listed in that release's `kernel-source.txt`
attachment (for v0.7.1-alpha, which predates it:
[`docs/relnotes/v0.7.1-alpha-sources.md`](docs/relnotes/v0.7.1-alpha-sources.md)).
`scripts/clone-refs.sh` checks out the buildbot's `main` branch as a reference
only; that is not necessarily what a release was built from. The
Android-specific configuration is asserted by [`scripts/kernel-config-android.sh`](scripts/kernel-config-android.sh);
how to build `vmlinuz.efi` and the DTB and where to put them is in
[`prebuilt-boot/README.md`](device/huawei/gaokun3/prebuilt-boot/README.md).

---

## Repository layout

| Path | Contents |
|---|---|
| `device/huawei/gaokun3/` | The device tree |
| `patches/` | Kernel, Mesa and AOSP patches that are not upstream |
| `scripts/` | Build, release, deploy, forensics and installer tooling (`scripts/live/` builds the graphical installer's Linux) |
| `live/installer-flutter/` | The graphical installer (Flutter) |
| `tools/` | Bring-up tools (fingerprint, camera, debugging) |
| `docs/` | **The engineering record.** Every finding, with evidence. Start from the index: [`docs/README.md`](docs/README.md) |
| `manifests/` | `repo` local manifest |

`docs/` is not an afterthought. Nothing about this platform exists in any wiki
or in any model's training data, so the findings files are a primary artifact:
they record what was measured, what turned out to be wrong, and which earlier
conclusions were later overturned. Several of them were.

---

## Help wanted

The full backlog — with the concrete first step for each item, and the reasons
behind everything that is parked — lives in [`docs/TODO.md`](docs/TODO.md).
What follows is the curated subset worth someone's weekend, roughly easiest
first:

1. **GPU SMMU interrupt fix.** The SMMU asserts SPI 675/680; the device tree
   declares 678/679, so context faults never reach the CPU. A DTB change should
   remove the need for the `smmu-nostall.sh` polling workaround entirely
   ([`docs/stage5-freedreno.md`](docs/stage5-freedreno.md) D6).
2. **Palm rejection.** A palm on the panel fragments into several contacts;
   three threshold-based fixes were measured and all failed, so the next step
   is a cross-frame shape test in the touch driver. The driver already reports
   contact size; there is no touch IDC file yet. [#116](docs/stage4-findings.md#116),
   [`scripts/touch/README.md`](scripts/touch/README.md).
3. **Hardware video *encode*.** Off on purpose (see the status table). Making
   it work means teaching `v4l2_codec2`'s encode component to convert RGBX to
   NV12 — the notes in `device/huawei/gaokun3/device.mk` list the three places
   to change for a re-test.
4. **SELinux enforcing.** Seven rounds are done and an enforcing trial run
   works; what remains is real standby, an enforcing-to-enforcing OTA and a
   fresh install under enforcing ([#129](docs/stage4-findings.md#129)).
5. **The ambient light sensor.** Activating it crashes the SLPI's
   sensor process; Windows uses the same configuration, so the difference is in
   the DSP's own registry ([#121](docs/stage4-findings.md#121)). The
   accelerometer / gyroscope stack works — as far as we know a first on
   SC8280XP, the ThinkPad X13s included — and the protocol is written up in
   [`docs/sensors-ssc-protocol.md`](docs/sensors-ssc-protocol.md) if you want
   it for your machine.
6. **Camera image quality.** What is left is quality, not plumbing: no
   colour-correction matrix (that needs a colour chart), the software ISP has
   no denoise of its own, and flash-lit shots blow out the highlights.
   Concrete next steps are in [`docs/TODO.md`](docs/TODO.md) T3.

Factory reset and `fastboot` now go through a unified boot entry
([`docs/boot-entry-design.md`](docs/boot-entry-design.md), code in
[`tools/gk3boot/`](tools/gk3boot/README.md)). It is in the 1.0 development
builds, not in a release yet, and factory reset through it has not been tested
on hardware — please talk to us before starting on recovery.

**If you have a MateBook E Go and want to test**, these have never been tried on
hardware: Bluetooth headphones, an external display over USB-C, a phone joining
the hotspot, the v0.7.1 change for WPA2/WPA3 mixed-mode routers, the graphical installer's dual-boot
and erase-disk modes on a real disk, and the Windows script on a real Windows.
Open an issue — reports of what breaks are as useful as patches. Please include
your BIOS version and SKU.

---

## Community

| | |
|---|---|
| **Telegram** | [t.me/gaokunAndroid](https://t.me/gaokunAndroid) |
| **QQ group** | **920133252** |
| Issues | [GitHub issues](https://github.com/vahiru/gaokun-android/issues) — the right place for anything that needs a paper trail |

Chat is good for "is this normal?"; open an issue for anything reproducible, so
it does not get lost in scrollback.

---

## Credits

* The **gaokun Linux community** —
  [linux-gaokun](https://github.com/right-0903/linux-gaokun),
  [linux-gaokun-buildbot](https://github.com/KawaiiHachimi/linux-gaokun-buildbot),
  [EGoTouchRev](https://github.com/chiyuki0325/EGoTouchRev-Linux) — for the
  kernel, the EC driver and the touch reverse-engineering this port stands on.
* **[aospm](https://github.com/aospm)**, for showing that AOSP on a mainline
  kernel is a workable shape at all.
* **Johan Hovold** and everyone who brought SC8280XP support upstream.
* **crDroid** and **LineageOS**.
* **Mesa** — `freedreno` and `turnip`.
* **@mashen11** — microphone, speaker enhancement and camera autofocus.

## License

GNU General Public License v3.0 or later — see [`LICENSE`](LICENSE) and
[`NOTICE`](NOTICE). A few files adapted from AOSP keep their Apache-2.0
headers. The kernel patches under [`patches/`](patches/) stay GPL-2.0-only,
because they are derivative works of Linux; the Mesa patches are MIT, matching
upstream.
