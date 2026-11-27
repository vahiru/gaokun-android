# Gaokun Android

Unofficial port of crDroid 13 for Huawei MateBook E Go (gaokun3).

[**中文 → README.zh-CN.md**](README.zh-CN.md)

<p align="center">
  <a href="https://t.me/gaokunAndroid"><img alt="Telegram channel: gaokunAndroid" src="https://img.shields.io/badge/Telegram-gaokunAndroid-26A5E4?logo=telegram&logoColor=white"></a>
  <a href="https://qm.qq.com/q/3VIV2KY6o0"><img alt="QQ group: 920133252" src="https://img.shields.io/badge/QQ%20Group-920133252-12B7F5?logo=qq&logoColor=white"></a>
  <a href="https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.5"><img alt="Latest release: 1.0.0-rc.5" src="https://img.shields.io/badge/Latest-1.0.0--rc.5-yellow"></a>
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
| Factory reset | ⚠️ | Not yet tested on hardware; to wipe the device, reinstall with the Live installer |
| Display | ✅ | 120 Hz, factory-calibrated colour, wide colour gamut, HDR and experimental force activity embedding |
| GPU | ✅ | Hardware acceleration and Vulkan |
| Touchscreen | ✅ | Accidental touch protection can be enabled under Settings → Display to suppress accidental palm touches while writing with the stylus |
| Stylus (M-Pencil) | ✅ | Compatible with second- and third-generation M-Pencils; supports pressure, hover, tilt and the side double-tap, with adjustable stroke smoothing and 4096 pressure levels |
| Detachable keyboard and touchpad | ✅ | |
| Wi-Fi | ✅ | |
| Wi-Fi hotspot | ✅ | |
| Wi-Fi Direct | ✅ | |
| Bluetooth | ✅ | |
| Speakers | ✅ | Bass enhancement or Huawei Histen can be turned on in Settings |
| Microphones | ✅ | Beamforming, with echo cancellation and noise suppression that can be switched per scenario |
| Headphones | ⚠️ | The microphone of a wired headset has not been tested yet |
| Battery and charging | ✅ | |
| Standby | ✅ | About 0.5 W in standby, about three days on a full charge |
| Push notifications | ⚠️ | In sleep, FCM can wake the system and deliver messages promptly; apps that do not use FCM (such as WeChat and QQ) receive their messages at the next system wake-up, usually a minute or two late |
| Sensors | ✅ | Accelerometer, gyroscope and ambient light sensor work, with auto-rotate and auto-brightness; there is no magnetometer |
| Games | ⚠️ | Genshin Impact runs smoothly at the highest settings, Arknights, Delta Force and others run; League of Legends: Wild Rift starts; gamepads are supported. **Playing games that come with ACE framework or other anti-cheat mechanisms on this system may get your account banned** |
| Thermals and CPU scheduling | ✅ | |
| Video playback | ✅ | Hardware decoding for H.264, HEVC and VP9 |
| Video encoding | ✅ | Hardware encoding for H.264 and HEVC |
| Camera | ✅ | |
| USB-C | ⚠️ | Currently only the left USB-C port supports device role; the right port always stays in host mode |
| Fingerprint | ⚠️ | Enrolment, screen unlock and verification in apps work (Goodix and FocalTech sensors); fingerprint payment in WeChat Pay, Alipay and similar apps does not work |
| Location | ❌ | No GPS hardware |
| TPM | ❌ | Not supported |
| Video copy protection (DRM) | ❌ | Widevine is not included, paid streaming content such as Netflix does not play |
| Root | ⚠️ | ReSukiSU is built into the kernel |
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

Download the latest version from the [**Releases**](https://github.com/vahiru/gaokun-android/releases) page; the full installation steps are in [`docs/INSTALL.md`](docs/INSTALL.md) (Chinese: [`docs/INSTALL.zh-CN.md`](docs/INSTALL.zh-CN.md)). **Secure Boot must be disabled** before installing.

There are two installers to choose from:

| | Windows installer | Live installer |
|---|---|---|
| What it does | Installs into unallocated space on the system disk (dual boot with Windows); uninstalls Android; creates standalone installation media (Live installer) | Keep existing system (dual boot); erase entire disk; reinstall Android (user data is erased by default and can be kept); delete partitions; delete installer partition |
| How it runs | Inside the Windows installed on the machine | Started from a USB drive or from the installer partition on the internal disk |
| System installation files | The latest release is downloaded, or files you have already downloaded are used | Carried on the installation media; no network is used during installation |
| Where to get it | The `Windows installer` releases on the [Releases](https://github.com/vahiru/gaokun-android/releases) page | The `Live installer` releases on the [Releases](https://github.com/vahiru/gaokun-android/releases) page |
| Latest version | [Windows installer 0.2.1](https://github.com/vahiru/gaokun-android/releases/tag/windows-installer-v0.2.1) | [Live installer 0.2.0 (v1.0.0-rc.5)](https://github.com/vahiru/gaokun-android/releases/tag/live-installer-v0.2.0-for-v1.0.0-rc.5) |

Default partition layout after erasing the entire disk:

| Partition | Size | Purpose |
|---|---|---|
| `esp` | 300 MiB | Boot menu and the kernel of each slot |
| `misc` | 4 MiB | A/B slot state |
| `metadata` | 32 MiB | Android metadata |
| `boot_a`, `boot_b` | 64 MiB each | Kernel and boot image, A and B copies |
| `super` | 12 GiB | System partitions, A and B copies |
| `userdata` | All remaining space | User data |

**Repair:** this machine has no working Android recovery. If Android does not start, reinstall Android with the Live installer; you can keep your user data.

**Downloads from mainland China:** when GitHub's download servers are slow or unreachable, the system images (`boot.img`, `super.img.zst`) can be downloaded from a mirror: `https://ota.072172.xyz/install/<build>/<file name>`, where `<build>` is the release's OTA package file name without `.zip`; see [`docs/INSTALL.md`](docs/INSTALL.md#downloads). In the Windows installer, you can switch the download source to "R2 mirror".

**Updating:** OTA updates are installed under Settings → System → System update; the OTA package is downloaded from the mirror above. If the network is poor, you can also get the OTA package another way, put it in the tablet's internal storage and install it with *Install local update* on the same screen.

## Known limitations

The table below lists the limitations to keep in mind in daily use. The full explanation is in [`docs/known-limitations.md`](docs/known-limitations.md).

| Topic | What it means | What to do |
|---|---|---|
| Data encryption | `/data` is not encrypted: anyone who has the machine can read all data on it | Do not store sensitive data on it |
| Root | ReSukiSU is built into the kernel; once the manager app is installed, some apps that detect root may refuse to run | Use at your own discretion |
| Boot security | Secure Boot must be disabled | This is required to run a third-party OS on this machine; there is no way around it |
| Google certification | The device is not Play Protect certified, so the Play Store works properly only after you register it manually | Register it once, following the [installation guide](docs/INSTALL.md#this-device-isnt-play-protect-certified) |
| Use in mainland China | The system ships only Google apps and no Chinese app store | Use a third-party app store such as CoolApk |
| Connecting to a computer | With some computers, the tablet may end up powering the computer, and neither side sees the other | Unplug and plug in again, or use another port on the computer |

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
