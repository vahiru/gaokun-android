# AGENTS.md

This repository brings Android (crDroid) to the **Huawei MateBook E Go (`gaokun3`, Snapdragon 8cx Gen 3 / `SC8280XP`)** on a **mainline Linux kernel and UEFI**.

## 1. Repository Layout & Boundaries

This repository contains the Android device tree, custom HALs and system services, patch queues for upstream trees, and standalone UEFI boot and installer tools:

- [`device/huawei/gaokun3/`](device/huawei/gaokun3/): Android device configuration, custom AIDL HALs (camera, sensors, light, USB, boot control, audio hooks/mic array/NPU denoise/speaker effects), stylus daemon, Parts settings app, and SELinux policies.
- [`patches/`](patches/README.md): Patch queues organized by target tree and subsystem (`kernel/`, `android/`, `mesa/`, `libcamera/`, `resukisu/`), applied by [`scripts/kernel-apply-patches.sh`](scripts/kernel-apply-patches.sh), [`scripts/crdroid-tree-fixes.py`](scripts/crdroid-tree-fixes.py), [`scripts/mesa-a17/regen.sh`](scripts/mesa-a17/regen.sh), [`scripts/camera/setup-libcamera-tree.sh`](scripts/camera/setup-libcamera-tree.sh), and [`scripts/kernel-setup-resukisu.sh`](scripts/kernel-setup-resukisu.sh).
- [`scripts/crdroid-tree-fixes.py`](scripts/crdroid-tree-fixes.py): Modifications applied to other AOSP/crDroid repositories. Register any AOSP change outside `device/huawei/gaokun3/` here.
- [`tools/`](tools/) and [`live/`](live/): Self-contained subprojects — [`tools/gk3boot/`](tools/gk3boot/README.md) (UEFI boot manager `gk3boot.efi` and `fastboot` executor), [`tools/gk3-installer/`](tools/gk3-installer/Cargo.toml) (Rust installer backend), and [`live/installer-flutter/`](live/installer-flutter/README.md) (Flutter graphical installer frontend, built via [`scripts/live/`](scripts/live/README.md)).
- `refs/` (git-ignored, fetched via `bash scripts/clone-refs.sh`): Read-only reference trees (`linux-gaokun`, `gaokun-buildbot`, `aospm-*`, `lineage-sepolicy`).

## 2. Patch Queue Workflow & Format (`patches/`)

Modifications to upstream source trees (Linux kernel, AOSP/crDroid repositories, Mesa, `libcamera`, `ReSukiSU`) are maintained as declarative **patch queues** under [`patches/`](patches/README.md) and applied onto pinned upstream baselines by their consumer scripts.

### Organization

- **Hierarchy by target tree and subsystem**:
  - [`patches/kernel/<subsystem>/`](patches/README.md) (`core`, `iommu`, `pcie-pm`, `wifi`, `ec-usb`, `drm`, `iris`, `camera`, `audio`, `input`, `dts`, `fingerprint`): Kernel patches (`KPATCHES`).
  - [`patches/android/<project>/`](patches/README.md) (`hardware-interfaces`, `drm-hwcomposer`, `v4l2-codec2`, `tinyalsa`, `frameworks-base`): Patches applied to AOSP/crDroid subtrees.
  - [`patches/mesa/`](patches/mesa/), [`patches/libcamera/`](patches/libcamera/), [`patches/resukisu/`](patches/resukisu/): Patches for their respective upstream trees.
- **Local `0001..NNNN` numbering per leaf directory**: Each leaf directory numbers its patches independently from `0001` (`NNNN-<topic>.patch`). Do not use a global cross-directory number sequence.
- **Subsystem orthogonality**: Different `patches/kernel/<subsystem>/` directories must modify disjoint sets of kernel source files. Patches that touch the same file or depend on each other's context must live in the same subsystem directory in application order.

### What Belongs in `patches/` and What Does Not

- **Keep**: Active patches applied to the current target baselines, registered in their consumer script and [`patches/README.md`](patches/README.md).
- **Do NOT commit**:
  - **Retired, superseded, or already-upstreamed patches**: Delete them directly when upgrading baselines or replacing fixes (Git history preserves them; never create `archive/` or `old/` folders).
  - **Diagnostic or `printk`/trace probes**: Do not commit one-off debugging or tracing patches to `patches/`.
  - **Unconsumed WIP snapshots or duplicate mailing-list copies**: Every patch in `patches/` must be wired into a consumer script; do not commit unreferenced work-in-progress patches or separate copies of patches already in the queue.

### Patch Format Guidelines

- **Generate with Git (`git format-patch -N`)**: Never hand-write diff headers or line counts. Always pass `-N` (`--no-numbered`) so `Subject:` uses `[PATCH]` rather than `[PATCH N/M]` series counters (avoiding cross-file churn when adding or removing patches).
- **Standard English mbox header**:
  - `From: Name <email>`: Real author identity (never placeholders like `<local>` or `<dev@localhost>`).
  - `Subject: [PATCH] <subsystem>: <concise summary>` in English.
  - **Commit message body in English**: Explain the symptom, underlying mechanism or hardware/spec constraint, and why the change is made. Do not include chronological debugging diaries, session logs, or references to deleted/internal planning docs.
- **Provenance & upstream status trailers**: Immediately before the `---` separator (alongside any `Fixes:` / `Signed-off-by:` trailers), every patch must carry DEP-3 / Yocto trailers:
  - `Origin: local` | `Origin: backport, <commit / MR / URL>` | `Origin: other, <URL>`
  - `Upstream-Status: Pending` | `Upstream-Status: Backport [<ref>]` | `Upstream-Status: Inappropriate [<reason>]` (e.g., `[Android-specific]`, `[firmware workaround]`, `[device-specific policy]`)

## 3. Development Workflow & Verification

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

## 4. Language & Documentation Conventions

### Language

- **Developer communication, developer docs (`docs/dev/`, component `README.md`, `docs/TODO.md`), code comments in our own modules, and repository commit messages**: Simplified Chinese.
- **Patch headers and descriptions (`patches/**/*.patch`) and runtime log messages (`dmesg` / `logcat`)**: English, following upstream commit format (`Subject: [PATCH] <subsystem>: <summary>` with `Origin:` and `Upstream-Status:` trailers).
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
   - **Chinese formatting**: Do not insert spaces between Chinese characters and inline English words, numbers, or `` `code` `` spans.
   - Always run `python3 -I scripts/check-doc-links.py` after adding, moving, or editing documentation.
