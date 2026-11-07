# AGENTS.md

This repository brings Android (crDroid) to the **Huawei MateBook E Go (`gaokun3`, Snapdragon 8cx Gen 3 / `SC8280XP`)** on a **mainline Linux kernel and UEFI**.

## 1. Repository Layout & Boundaries

This repository is not a full AOSP tree or Linux kernel tree; it holds the device configuration, custom HALs/services, patch stacks, and standalone boot/installer tools:

- [`device/huawei/gaokun3/`](device/huawei/gaokun3/): Android device configuration, custom AIDL HALs (camera, sensors, light, USB, boot control, audio hooks/mic array/NPU denoise/speaker effects), stylus daemon, Parts settings app, and SELinux policies.
- [`patches/`](patches/README.md): Out-of-tree patches organized by target tree and subsystem (`kernel/`, `android/`, `mesa/`, `libcamera/`, `resukisu/`), applied by [`scripts/kernel-apply-patches.sh`](scripts/kernel-apply-patches.sh), [`scripts/crdroid-tree-fixes.py`](scripts/crdroid-tree-fixes.py), and [`scripts/mesa-a17/regen.sh`](scripts/mesa-a17/regen.sh).
- [`scripts/crdroid-tree-fixes.py`](scripts/crdroid-tree-fixes.py): Out-of-tree modifications applied to other AOSP/crDroid repositories. Any change outside `device/huawei/gaokun3/` in the AOSP tree must be registered here rather than left uncommitted in a build tree.
- [`tools/`](tools/) and [`live/`](live/): Self-contained subprojects — [`tools/gk3boot/`](tools/gk3boot/README.md) (UEFI boot manager `gk3boot.efi` and `fastboot` executor), [`tools/gk3-installer/`](tools/gk3-installer/Cargo.toml) (Rust installer backend), and [`live/installer-flutter/`](live/installer-flutter/README.md) (Flutter graphical installer frontend, built via [`scripts/live/`](scripts/live/README.md)).
- `refs/` (git-ignored, fetched via `bash scripts/clone-refs.sh`): Read-only reference trees (`linux-gaokun`, `gaokun-buildbot`, `aospm-*`, `lineage-sepolicy`).

## 2. Development Workflow & Verification

- **Read subsystem docs first**: Before modifying a component, read its local `README.md` or the corresponding architecture doc under [`docs/dev/`](docs/dev/) (full index in [`docs/README.md`](docs/README.md), open tasks in [`docs/TODO.md`](docs/TODO.md)).
- **Verify symbols from source**: Always `grep` this repo or `refs/` for exact kernel `CONFIG_*` names, AOSP properties, HAL interfaces, SELinux types, and file paths; never guess from memory.
- **Respect module boundaries**: Place fixes in the driver or module where the problem originates rather than layering workarounds elsewhere. Keep code, scripts, and configs host-agnostic (never hardcode personal file paths, hostnames, network addresses, or credentials).
- **Hardware safety**: Writing `boot` partitions, altering ESP boot entries, writing EC charging thresholds, or issuing untested TrustZone SMCs requires explicit user confirmation beforehand.
- **Local verification commands**:
  - Boot manager & fastboot: `make -C tools/gk3boot/core test`; container tests: `bash scripts/gk3boot/test-{boot,probe,fastbootd,executor}.sh`
  - Installer backend: `bash tools/gk3-installer/build.sh test` and `lint`; container differential tests: `bash scripts/live/test-in-container.sh scripts/live/test-{duel,apply}.sh`
  - Installer frontend: `flutter test` inside `live/installer-flutter/`
  - Kernel patches & config: `bash scripts/kernel-apply-patches.sh <kernel-tree> --check`, `bash scripts/kernel-config-android.sh <kout>`
  - Documentation links: `python3 -I scripts/check-doc-links.py`
  - On-device release acceptance: `SER=gaokun3 bash scripts/accept.sh` (see [`docs/dev/release-checklist.md`](docs/dev/release-checklist.md))

## 3. Language & Documentation Conventions

### Language

- **Developer communication, developer docs (`docs/dev/`, component `README.md`, `docs/TODO.md`), code comments, and repository commit messages**: Simplified Chinese.
- **Kernel patches (`patches/kernel/**/*.patch`, `docs/dev/upstream/`) and runtime log messages (`dmesg` / `logcat`)**: English, following Linux kernel coding style and commit format (`subsystem: summary`).
- **User-facing documentation (`README*`, `docs/INSTALL*`, `docs/FAQ*`, `docs/known-limitations*`, `docs/relnotes/`)**: Maintained in both English (`.md`) and Simplified Chinese (`.zh-CN.md`).

### Documentation Structure & Style

1. **Document hierarchy**:
   - [`docs/`](docs/) top level: User- and contributor-facing documents only (`INSTALL`, `FAQ`, `known-limitations`, `BUILDING`, `TODO`, `relnotes/`). Do not place developer design docs or planning files at the top level of `docs/`.
   - Component `README.md` (`device/huawei/gaokun3/<module>/`, `tools/<tool>/`, `scripts/<dir>/`, `live/`): Document single-component usage, interfaces, parameters, and verification next to the code.
   - [`docs/dev/`](docs/dev/): Cross-layer and subsystem architecture documentation. Prefer updating existing files; create a new file under `docs/dev/` only when introducing a distinct new subsystem, and register it in [`docs/README.md`](docs/README.md).
2. **Content & tone**:
   - Document **current architecture, design rationale (why), build/verification steps, and known limitations**. Code comments should explain non-obvious hardware or framework constraints, not change history.
   - **No raw work logs in the repo**: Do not commit debugging diaries, chronological trial-and-error logs, stage plans (`*-plan.md`), or raw experiment dumps (historical context belongs in commit messages and PR descriptions).
   - **No ad-hoc internal IDs**: Do not invent custom tracking codes in docs, comments, or commit messages; reference GitHub Issue/PR numbers instead.
   - **Chinese formatting**: Do not insert spaces between Chinese characters and inline English words, numbers, or `` `code` `` spans. State untested changes as "尚未经实测" or "尚未上机测试".
   - Always run `python3 -I scripts/check-doc-links.py` after adding, moving, or editing documentation.
