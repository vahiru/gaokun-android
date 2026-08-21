# Android on the Huawei MateBook E Go (SC8280XP / `gaokun3`)

**crDroid 16.0 (Android 16) on a mainline Linux kernel, with hardware Vulkan on
the Adreno 690.**

Qualcomm never shipped an Android BSP for the 8cx family — only Windows and
Linux drivers. There is no stock ROM to lift vendor blobs from, no `fastboot`,
no A/B slots, no recovery partition and no serial console. So this is not a
normal device port: it is *AOSP on mainline*, with every HAL built on top of
upstream drivers.

> ### ⚠️ Alpha. Read this first.
> Games run well, and as of v0.3.0-alpha the machine sleeps and wakes properly.
> **There is still no working recovery** — see [Known issues](#known-issues).
> Installing erases the internal disk. You need to be comfortable recovering a
> machine that will not boot. No warranty of any kind.

[**中文说明 → README.zh-CN.md**](README.zh-CN.md)

**Talk to us:** [Telegram](https://t.me/gaokunAndroid) · QQ group **920133252**

---

## Status

Everything below was measured on hardware, not inferred. The evidence is in
[`docs/`](docs/).

| Area | State | Notes |
|---|:--:|---|
| Boot (UEFI + systemd-boot, internal disk) | ✅ | No USB media required |
| Display 1600×2560 @ 120 Hz | ✅ | The framework default pinned rendering to 60; overridden, measured 8.33 ms vsync |
| GPU — Adreno 690, hardware Vulkan | ✅ | Mesa 26.0.3 `turnip`; zero SMMU faults over a 22-minute soak |
| Touchscreen | ✅ | Himax HX83121A; needs the gpio174 patch in `patches/` |
| Detachable keyboard + touchpad | ✅ | USB HID `12d1:10b8` |
| Wi-Fi | ✅ | ath11k / WCN6855. Measured 61.7 MB/s pulling 200 MB over the LAN. ⚠️ Downloads *from the internet* run at only 1–2 MB/s on this machine while a PC on the same network gets 36.9 MB/s from the same URL — cause not established, and **not** the Wi-Fi: ping to the gateway is 0% loss at 1400 bytes. It does make an in-system OTA slow. [#44](docs/stage4-findings.md) |
| Bluetooth | ⚠️ | Works — `hci_qca`, adapter `ON`, zero crashes at boot. **But it can deadlock after long uptime**, together with audio; see [#38](docs/stage4-findings.md) |
| Speakers | ⚠️ | Works — user-confirmed, WSA883x via audioreach. **But audio can deadlock after long uptime**, together with Bluetooth; see [#38](docs/stage4-findings.md) |
| Headphone jack | ✅ | **Fixed, user-confirmed.** Three separate blockers, none of them exotic: the RX macro's interpolator stage was never wired up (input mux and demodulator mux both at reset values), so the DAPM route was incomplete and the backend refused to open — with no kernel message at all. Then the audio policy declared no wired output, and `WiredAccessoryManager` was watching `/sys/class/switch/h2w`, which does not exist on mainline. Measured after the fix: PCM 0 `RUNNING`, DMA consuming 48960 frames/s. [#40](docs/stage4-findings.md) |
| Microphones | ⚠️ | **The internal microphone had never worked and nobody had noticed** — the audio policy pointed at `CARD_0_DEV_3` but that PCM would not open, because the front-end mixer and the VA macro's DMIC sequence were never set. Fixed and measured: 384000 frames captured, RMS −30.5 dBFS in a quiet room. The headset microphone path streams correctly but reads as an open input with nothing plugged in, so it still needs someone with a headset. ⚠️ Both capture PCMs are stereo-only; asking for mono gets you `cannot set hw params`. [#40](docs/stage4-findings.md) |
| Battery, charging, lid switch | ✅ | Huawei EC driver |
| **Gaming** | ✅ | Genshin Impact at max graphics, smooth. GPU idles at 270 MHz, peaks 690 MHz, 50 °C |
| CPU thermal throttling | ✅ | Mainline DTS has **no** CPU cooling maps at all — fixed in [`patches/0009`](patches/) |
| **Suspend / standby** | ✅ | **Fixed 2026-08-22 — and the cause was ours, not the kernel's.** Real suspend and resume, no resets. ⚠️ One trade-off: USB adb drops while the screen is off. [#52](docs/stage4-findings.md), [#57](docs/stage4-findings.md) |
| Sensors (accel + gyro) | ✅ | **Auto-rotate works, confirmed on device.** A sensors HAL written for this port feeds real accelerometer and gyroscope data to SensorService, and the framework derives Game Rotation Vector / Gravity / Linear Acceleration from them. The factory mount matrix is all zeros (that calibration data died with Windows), but the sensor frame turns out to match the panel, so no correction was needed. This machine has **no magnetometer** (so no compass), and enabling the ALS poisons the DSP session. The ALS failure is now narrowed to one difference: a field-by-field comparison against the working accelerometer rules out the PMIC rail (both use the same one) and chip-pin interrupts (both use one), leaving the SLPI-side I²C instance — 1 for the accelerometer, 5 for the light sensor. See [#37](docs/stage4-findings.md), [#43](docs/stage4-findings.md) |
| Hardware video decode | ⚠️ | **Kernel half done** — `/dev/video0` and `/dev/video1` are `qcom-venus-decoder` / `qcom-venus-encoder`, all suppliers resolved, no firmware errors. As far as we know a first for SC8280XP. Android still needs a Codec2 component to talk to V4L2, so all 66 decoders remain software. [#41](docs/stage4-findings.md) |
| Camera | ❌ | Not started |
| USB-C DisplayPort / UCSI | ❌ | UCSI PPM init times out — a known mainline defect on this machine |
| Fingerprint, TPM | ❌ | No driver exists |
| SELinux | ⚠️ | `permissive` |

### Two things that will surprise you

**The mainline device tree had no CPU thermal throttling at all.**
`sc8280xp.dtsi` contains exactly one `cooling-maps` block and it is under
`gpu-thermal`. Every CPU zone had a single 110 °C *critical* trip and nothing
else — so the CPUs ran flat out until the kernel performed an emergency
shutdown, with no gradual throttling in between. On a fanless tablet that is
reachable.

[`patches/0009`](patches/) fixes it in the device tree: a passive trip at 85 °C
on each of the eight per-core zones, bound to that core's cluster cpufreq
cooling device. Measured on the same machine across a DTB swap: cooling devices
bound per zone 0 → 1, trip points 1 → 2. Worth knowing if you run any other
sc8280xp machine on mainline — the gap is not specific to this device.

**Standby was broken for a whole stage, and the culprit was a line of device
tree we wrote ourselves.** It looked like a kernel or EC defect: the machine
would suspend and then reset itself seconds later, and **it reproduced
identically under Ubuntu on the same kernel** — which is exactly the evidence
you would use to rule Android out. It did rule Android out. It also pointed at
the wrong layer entirely.

The real cause was added in Stage 2 to get USB device-mode adb: we set the
second USB controller to `dr_mode = "otg"` with `usb-role-switch`, where
upstream has plain `host`. This machine's UCSI is broken, so nothing ever
assigns a role, and the controller sits in `device` with no gadget and no xHCI
child. Powering that half-initialised state down — a system suspend does it,
and so does simply unbinding the driver — **resets the whole board, with
nothing in any log**.

Two things made it take so long. There were **two independent blockers stacked**
(the second, an EC `suspend_noirq` timeout, was already fixed by a patch we were
carrying), so every single-variable experiment came back negative. And the
**per-attempt failure rate was about 93%**, which makes "change one thing, try
once, it died" the expected outcome for *any* configuration — several rounds
went into chasing noise. The Android fix switches the role to `host` before the
machine sleeps and back to `device` when the screen comes on, which is why USB
adb drops while the screen is off. [#52](docs/stage4-findings.md),
[#57](docs/stage4-findings.md)

---

## Hardware

| | |
|---|---|
| SoC | Qualcomm Snapdragon 8cx Gen 3 (SC8280XP) |
| Model | HUAWEI GK-W7X, 2022, CSOT panel |
| **BIOS** | **2.16 — do not upgrade to 2.17.** The touch SPI bus and GPIO numbering differ between the two, and the upstream driver targets 2.16 |
| GPU | Adreno 690 |
| Panel | Himax HX83121A, MIPI-DSI, 1600×2560 — the same panel as the Galaxy Tab S7 FE |
| Wi-Fi / BT | WCN6855 |
| Storage | NVMe |
| Firmware | UEFI. Secure Boot must be disabled |

---

## Installing

Take the latest [**Release**](../../releases) and follow
[`docs/INSTALL.md`](docs/INSTALL.md).

Installation **erases the internal disk**. The layout it creates:

| Partition | Size | Purpose |
|---|---|---|
| ESP | 300 MiB | systemd-boot, kernels, ramdisks |
| `userdata` | rest of the disk | `/data` |
| `super` | 12 GiB | system / system_ext / product / vendor |
| `metadata` | 32 MiB | |
| rescue | ~25 GiB | A full Ubuntu, reachable over SSH |

That last partition is deliberate. This machine has no recovery partition and
no serial console, so an ordinary Linux install *is* the recovery environment.
It is the default boot entry, which means a hung Android is one power-button
press away from a system you can SSH into and repair remotely — without being
anywhere near the machine.

---

## Building

A Linux host with roughly 16 GB of RAM and 400 GB of disk.

```sh
repo init -u https://github.com/crdroidandroid/android.git -b 16.0
# add manifests/local_manifest_gaokun3.xml to .repo/local_manifests/
repo sync -c -j"$(nproc)"

python3 scripts/crdroid-tree-fixes.py <tree>     # read the script for why
source build/envsetup.sh
lunch lineage_gaokun3-bp4a-userdebug
m
m superimage
```

Proprietary Huawei firmware is **not** in this repository. See
[`device/huawei/gaokun3/firmware/README.md`](device/huawei/gaokun3/firmware/README.md)
for how to obtain it from your own machine.

The kernel is built separately, from
[`linux-gaokun-buildbot`](https://github.com/KawaiiHachimi/linux-gaokun-buildbot).
The Android-specific configuration assertions are in
[`scripts/kernel-config-android.sh`](scripts/kernel-config-android.sh) and the
extra patches in [`patches/`](patches/).

---

## Repository layout

| Path | Contents |
|---|---|
| `device/huawei/gaokun3/` | The device tree |
| `patches/` | Kernel and Mesa patches that are not upstream |
| `scripts/` | Build, deploy, forensics and installer tooling |
| `docs/` | **The engineering record.** Every finding, with evidence |
| `manifests/` | `repo` local manifest |

`docs/` is not an afterthought. Nothing about this platform exists in any wiki
or in any model's training data, so the findings files are a primary artifact:
they record what was measured, what turned out to be wrong, and which earlier
conclusions were later overturned. Several of them were.

---

## Known issues

| Issue | Where |
|---|---|
| **No working recovery.** The image builds and ships, but booting it reset-loops the machine, so the boot entry is not created. Costs: no `adb sideload`, no `fastbootd`, and *Erase all data* in Settings probably does nothing (it asks the bootloader for recovery, and systemd-boot does not read that request) | [#39](docs/stage4-findings.md) |
| **Audio and Bluetooth can deadlock after long uptime** — reported on device, not yet reproduced or diagnosed. Both ride the same QRTR/FastRPC path to the DSPs, where we have already measured session-level lockups | [#38](docs/stage4-findings.md) |
| Enabling the ambient light sensor returns no readings *and* poisons the whole DSP session, so there is no auto-brightness (#37) | [`docs/stage4-findings.md`](docs/stage4-findings.md) |
| USB adb drops after an unplug (#27), and now also whenever the screen turns off — that is the suspend fix switching the controller to host mode. adb over TCP on 5555 is on by default and is unaffected | [`docs/stage4-findings.md`](docs/stage4-findings.md) |
| GPU SMMU raises SPI 675/680 while the DT declares 678/679 | [`docs/stage5-freedreno.md`](docs/stage5-freedreno.md) D6 |
| The thermal HAL is the AOSP mock, and its SHUTDOWN threshold is 36 °C | [`docs/stage6-crdroid.md`](docs/stage6-crdroid.md) §M4 |

---

## Help wanted

The full backlog — with the concrete first step for each item, and the reasons
behind everything that is parked — lives in [`docs/TODO.md`](docs/TODO.md).
What follows is the curated subset worth someone's weekend.


Concrete, well-scoped work, roughly easiest first:

1. **Hardware video decode — the Android half.** The kernel half is done:
   `/dev/video0` and `/dev/video1` are a working Venus decoder and encoder.
   What is missing is a Codec2 component that talks to V4L2, so all 66 codecs
   are still software. `external/v4l2_codec2` is already in the manifest, and
   three prerequisites are confirmed on device: the `IComponentStore/default`
   instance it wants is free, `media.c2.hal.selection` is already `aidl`, and
   ⚠️ the poolmask must be BLOB `0xfc0000`, **not** the `0xf50000` its README
   suggests — that value is for ION, and this kernel has none.
2. **GPU SMMU interrupt fix.** The SMMU asserts SPI 675/680; the device tree
   declares 678/679, so context faults never reach the CPU. A DTB change should
   remove the need for the `smmu-nostall.sh` polling workaround entirely.
3. **A real thermal HAL** reading `/sys/class/thermal`. ⚠️ Raise the SHUTDOWN
   thresholds at the same time — the AOSP mock reports 36 °C, and
   `ThermalManagerService` will power the machine off when it sees that.
4. **SELinux enforcing.** Two services need policy written.
5. **Sensors — SELinux policy and a kernel flag.** The sensor stack itself is
   done: accelerometer and gyroscope run through the SLPI DSP into a
   purpose-written HAL, and auto-rotate works. As far as we know no other
   SC8280XP device has this working, the ThinkPad X13s included — the protocol
   is written up in
   [`docs/sensors-ssc-protocol.md`](docs/sensors-ssc-protocol.md) if you want
   it for yours. One loose end here: no sepolicy has been written for the HAL
   or for `hexagonrpcd`, so both still run under `permissive`. The
   **ambient light sensor** is a harder, separate problem: enabling it returns
   no readings *and* poisons the whole SSC session, so there is no
   auto-brightness.
6. **Recovery that boots.** The image is built and delivered; it reset-loops.
   ★ The useful first move is not more blind reboots — it is getting **USB adb
   working inside recovery**, the only channel that can see anything (there is no
   serial port, recovery has no network stack, and `init_fatal_panic` provably
   cannot catch this class of failure on this device). [#39](docs/stage4-findings.md)
   spells out what was already ruled out. Fixing this also gets `fastbootd` for
   free and makes *Erase all data* work.
7. **Camera.** Untouched.

If you have a MateBook E Go and want to test, open an issue — reports of what
breaks are as useful as patches. Please include your BIOS version and SKU.

---

## Community

| | |
|---|---|
| **Telegram** | [t.me/gaokunAndroid](https://t.me/gaokunAndroid) |
| **QQ group** | **920133252** |
| Issues | [GitHub issues](../../issues) — the right place for anything that needs a paper trail |

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

## License

GNU General Public License v3.0 or later — see [`LICENSE`](LICENSE) and
[`NOTICE`](NOTICE). A few files adapted from AOSP keep their Apache-2.0
headers, and the kernel patches under [`patches/`](patches/) stay GPL-2.0-only
because they are derivative works of Linux.
Kernel patches under `patches/` are GPL-2.0-only as derivative works of Linux;
Mesa patches are MIT, matching upstream.
