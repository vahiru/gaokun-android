# Gaokun Android

Unofficial port of crDroid 13 for Huawei MateBook E Go (gaokun3).

[**中文 → README.zh-CN.md**](README.zh-CN.md)

<p align="center">
  <a href="https://t.me/gaokunAndroid"><img alt="Telegram channel: gaokunAndroid" src="https://img.shields.io/badge/Telegram-gaokunAndroid-26A5E4?logo=telegram&logoColor=white"></a>
  <a href="https://qm.qq.com/q/3VIV2KY6o0"><img alt="QQ group: 920133252" src="https://img.shields.io/badge/QQ%20Group-920133252-12B7F5?logo=qq&logoColor=white"></a>
  <a href="https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.4"><img alt="Latest release: 1.0.0-rc.4" src="https://img.shields.io/badge/Latest-1.0.0--rc.4-yellow"></a>
</p>

## Status

> ### ⚠️ Before you install
> Installing this system can **erase all data on the disk**. Before installing, make sure you can recover a machine that no longer boots, and **back up your important data**.
>
> More known issues are listed under [Known limitations](#known-limitations); common questions (collecting logs, the boot menu, wiping before you sell) are answered in the [FAQ](docs/FAQ.md).

The table below shows how the main features work in the current version. See the [detailed documentation](docs/) for more.

- ✅: works
- ⚠️: usable, with known issues
- ❌: not available

| Feature | State | Notes |
|---|:--:|---|
| Boot and system updates | ✅ | Seamless A/B system updates |
| Factory reset | ⚠️ | Not yet tested on hardware; to wipe the device, reinstall with the graphical installer |
| Display | ✅ | 120 Hz, factory-calibrated colour, wide colour gamut and HDR |
| GPU | ✅ | Hardware acceleration and Vulkan |
| Touchscreen | ✅ | Everyday and game modes can be switched in Settings; everyday mode suppresses accidental palm touches while writing and the like |
| Stylus (M-Pencil) | ⚠️ | Second- and third-generation M-Pencils are supported, with pressure, hover, tilt and the side double-tap; stroke smoothing is adjustable in Settings. Everyday use of the third-generation M-Pencil has not been tested yet |
| Detachable keyboard and touchpad | ✅ | |
| Wi-Fi | ✅ | |
| Wi-Fi hotspot | ⚠️ | Can be turned on; other devices going online through the hotspot has not been tested yet |
| Bluetooth | ⚠️ | The microphone of a Bluetooth headset cannot be used in calls |
| Speakers | ✅ | Bass enhancement or Huawei Histen can be turned on in Settings |
| Microphones | ✅ | Beamforming, with echo cancellation and noise suppression that can be switched per scenario |
| Headphones | ⚠️ | The microphone of a wired headset has not been tested yet |
| USB audio | ⚠️ | USB headsets and USB sound cards have not been tested yet |
| Battery and charging | ⚠️ | Because of the high base power draw of the machine, the battery may drain while on a low-power source, and the tablet may shut down without warning when the battery runs low |
| Standby | ✅ | About 0.5 W in standby, about three days on a full charge |
| Push notifications | ⚠️ | In sleep, FCM can wake the system and deliver messages promptly; apps that do not use FCM receive their messages at the next system wake-up, usually a minute or two late |
| Sensors | ✅ | Accelerometer and ambient light sensor work, with auto-rotate and auto-brightness; there is no magnetometer |
| Games | ⚠️ | Genshin Impact runs smoothly at the highest settings, Arknights, Delta Force and others run; League of Legends: Wild Rift starts. **Playing games that come with ACE framework or other anti-cheat mechanisms on this system may get your account banned** |
| Video playback | ✅ | Hardware decoding, including 10-bit and HDR video |
| Video encoding | ⚠️ | H.264 and HEVC hardware encoding, used for camera recording; the built-in screen recorder still uses software encoding; only one stream at a time uses the hardware, and video processed after the screen turns off may come out incomplete |
| Camera | ✅ | Hardware ISP; both cameras take photos and record up to 1080p video, and the rear camera supports autofocus, digital zoom and exposure compensation |
| USB-C | ⚠️ | File transfer and network sharing with a computer, USB flash drives and external displays work; occasionally the USB-C port does not work after a boot until a restart, the cause is still under investigation |
| Fingerprint | ⚠️ | Enrolment and screen unlock work (Goodix and FocalTech sensors); rated convenience class, so fingerprint payment in apps is not available, and matching on FocalTech sensors has not been tested on hardware |
| Location | ❌ | No GPS hardware, satellite positioning is not available |
| TPM | ❌ | Not supported |
| Video copy protection (DRM) | ❌ | Widevine is not included, paid streaming content such as Netflix does not play |
| Root | ⚠️ | KernelSU is built in; some apps that detect root may refuse to run |
| Chinese input method | ✅ | fcitx5 is preinstalled |

## Hardware

| Component | Specification |
|---|---|
| SoC | Qualcomm Snapdragon 8cx Gen 3 (SC8280XP) |
| Model | HUAWEI GK-W7X |
| GPU | Adreno 690 |
| Display | PPC357DB1-4 (supplied by CSOT or BOE), Himax HX83121A driver IC, 1600×2560, 120 Hz |
| Wi-Fi / Bluetooth | WCN6855 |
| Storage | NVMe |

## Installing

Get the latest version from [**Releases**](https://github.com/vahiru/gaokun-android/releases); the full installation steps are in [`docs/INSTALL.md`](docs/INSTALL.md) (Chinese: [`docs/INSTALL.zh-CN.md`](docs/INSTALL.zh-CN.md)). **Secure Boot must be turned off** before installing.

There are two installers to choose from:

| | Graphical installer (preview) | Command-line installer |
|---|---|---|
| Installation modes | Next to Windows (dual boot); erase the whole disk and install; reinstall Android over the existing partitions (wipes data by default, keeping it is optional); adjust partitions by hand | Erase the whole disk and install |
| Runs from | Its own Linux system, started from a USB stick, or from Windows with `gaokun3-setup.cmd` without a USB stick | An arm64 Linux live USB stick plus a copy of this repository; whether a generic Ubuntu/Debian image boots on this machine has not been verified |
| System image | Carried on the installer medium, or downloaded over the network | Downloaded by hand from the release page: `super.img.zst`, `boot.img` and `install-artifacts.sha256` |
| Where to get it | The `gaokun3-installer-0.2.0-rc.2-*` files on the [v1.0.0-rc.2 release page](https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.2) (v1.0.0-rc.3 did not rebuild the installer; the USB image carries the rc.2 system, so update to rc.3 in Settings after installing) | `scripts/install-gaokun3.sh` in this repository |
| Hardware testing | "Erase the whole disk and install" has not been tested on hardware; errors may still appear after a successful installation | Shares its installation steps with the graphical installer; the partition layout below has so far been created on test disks only |

Default partition layout when the whole disk is erased:

| Partition | Size | Purpose |
|---|---|---|
| `esp` | 300 MiB | Boot menu and the kernel of each slot |
| `misc` | 4 MiB | A/B slot state |
| `metadata` | 32 MiB | Android metadata |
| `super` | 12 GiB | System partitions, A and B |
| `boot_a`, `boot_b` | 64 MiB each | Kernel and boot image, A and B |
| `gk3rescue` | 1 GiB | Rescue system (optional) |
| `userdata` | All remaining space | User data |

**Rescue system:** this machine has no working Android recovery, so the rescue system is the way to repair it when something goes wrong. It is a small Linux system, the same program as the graphical installer, so it can also reinstall Android, and it lives in its own 1 GiB partition. The graphical installer can install it as well (tick *Also install the rescue system*); the command-line installer installs it only when given the rescue system image.

Every boot shows a 5-second boot menu, from which the rescue system can be started; the system does not switch to it on its own when Android hangs. When you need it, hold the power button to force a shutdown, then choose the rescue system in the menu on the next boot with the volume and power buttons (or the arrow keys and Enter on the keyboard cover). To log in to the rescue system over the network (SSH), put your own SSH public key on the installer USB stick before installing; see the [installation guide](docs/INSTALL.md#rescue-system) for how.

**Downloads from mainland China:** when GitHub's download servers are slow or unreachable, the system images (`boot.img`, `super.img.zst`) can be downloaded from a mirror: `https://ota.072172.xyz/install/<build>/<file name>`, where `<build>` is the release's OTA package file name without `.zip`; see [`docs/INSTALL.md`](docs/INSTALL.md#downloads).

**Updating:** OTA updates are installed under Settings → System → System update; the OTA package is downloaded from the mirror above. If the network is poor, you can also get the OTA package another way, put it in the tablet's internal storage and install it with *Install local update* on the same screen.

## Known limitations

The table below lists the limitations to keep in mind in daily use. The full explanation is in [`docs/known-limitations.md`](docs/known-limitations.md).

| Topic | What it means | What to do |
|---|---|---|
| Data encryption | The user data partition is not encrypted: anyone who has the machine can read all data on it, and the lock screen cannot stop that | Don't keep sensitive data on it; before selling or sending it in for repair, reinstall and wipe it with the graphical installer |
| Root | KernelSU is built into the kernel and cannot be turned off; it stays dormant until the manager app is installed. Some apps that detect root may refuse to run; once adb root is enabled in the manager, any computer authorized for USB debugging gets root through adb | Don't enable adb root unless you need it; only authorize computers you trust, and turn USB debugging and wireless debugging off when not in use |
| System signing | The system and its updates are signed with Android's public test keys, so anyone can build an update package or a system app this device will accept | Update only through System update in Settings or this project's release page, and don't install "system components" of unknown origin |
| Boot security | Secure Boot must be off, the boot chain is unlocked, and system partitions are not integrity-checked | This is what running Android on this machine requires; there is no way around it |
| Factory reset | *Erase all data* in Settings has not been tested; don't rely on it | Wipe the device with the graphical installer's *Reinstall Android* |
| Rolling back | Once a new version has booted successfully, the previous version is merged away and cannot be restored; major version updates also upgrade user data | Back up important data before updating |
| Google certification | The device is not Play Protect certified, so the Play Store works properly only after you register it by hand; Play Integrity cannot pass, so apps that depend on it (Google Wallet, some banking apps) do not work | Register it once, following the [installation guide](docs/INSTALL.md#this-device-isnt-play-protect-certified) |
| Use in mainland China | The system ships only Google apps and no Chinese app store; without access to Google, the Play Store, Google services and network location do not work | Download app packages from the apps' official sites or the web versions of app stores |
| Kernel version | To pass some apps' environment checks, apps are told a disguised kernel version | Turn it off or change it in Settings → System → Kernel version spoofing |
| Audio and Bluetooth | Users have reported sound and Bluetooth both stopping after a long uptime; the cause is still under investigation | Restart to recover; when reporting it, attach `/data/vendor/gaokun3/hangdump-*` (reading it needs root) |
| Connecting to a computer | With some computers, the tablet may end up powering the computer, and neither side sees the other | Unplug and plug in again, or use another port on the computer |
| USB-C port | After plugging and unplugging devices (mostly after standby), the USB-C port may stop working until a restart | Restart; until then, use adb and file transfer over Wi-Fi |
| Low-power sources | On a computer's USB port or a small charger, the tablet may show *charging* while the battery still drains, and it switches off hard when the battery is empty | Use a charger with enough power, watch the battery percentage and save your work in time |

## Contributing

You are welcome to help in these ways:

- **Testing and feedback**: if you have tried a feature marked "not yet tested" under [Status](#status), please [open an issue](https://github.com/vahiru/gaokun-android/issues/new/choose) and tell us how it went, whatever the result. Before reporting a problem, please read [issues.md](issues.md).
- **Development**: open tasks and the next step for each are in [`docs/TODO.md`](docs/TODO.md).

Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening an issue or a pull request.

## Community

| | |
|---|---|
| **Telegram** | [t.me/gaokunAndroid](https://t.me/gaokunAndroid) |
| **QQ group** | **920133252** |
| Issues | [GitHub issues](https://github.com/vahiru/gaokun-android/issues) |

## Credits

This project is built on the work of many people. Our thanks go to:

* **The gaokun Linux community**: [linux-gaokun](https://github.com/right-0903/linux-gaokun), [linux-gaokun-buildbot](https://github.com/KawaiiHachimi/linux-gaokun-buildbot), [EGoTouchRev](https://github.com/chiyuki0325/EGoTouchRev-Linux), [matebook-e-go-linux](https://github.com/whitelewi1-ctrl/matebook-e-go-linux) and [matebook-e-go-boot-works](https://github.com/matalama80td3l/matebook-e-go-boot-works), for the kernel, the EC driver, the panel driver, touch reverse-engineering and more.
* **[uup-drivers-sc8280xp](https://github.com/matebook-e-go/uup-drivers-sc8280xp)**, for collecting Huawei's Windows driver packages; the firmware, camera tuning and microphone array parameters used here come from it.
* **[aospm](https://github.com/aospm)**, for showing that AOSP can run on a mainline kernel.
* **Johan Hovold** and everyone who contributed to mainline SC8280XP support.
* **crDroid** and **LineageOS**.
* **Mesa**, for the `freedreno` and `turnip` drivers.
* **@mashen11**, for the microphones, the speaker enhancement and camera autofocus.
* **@Rivarise**, for tuning and testing the stylus on CSOT panels.
* **@dezige131**, for the initial camera hardware ISP pixel-path implementation and fixes to the graphical installer.
* Everyone who helped with testing.

## License

This project is licensed under the GPL-3.0 or later, see [`LICENSE`](LICENSE). Some files keep the license of the project they come from, for example the kernel patches are GPL-2.0-only; see [`NOTICE`](NOTICE) for details.
