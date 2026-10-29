# Restricted registry output — offline prototype, not a deployable ALS fix

The patch is `patches/hexagonrpc-registry-output-restricted.patch`. Its **only
base** is FadyAckad/hexagonrpc commit
`40e50412812adce9dddc2be9a4510ebce05eef89` (`sp11-sensors`). It does **not** apply
directly to the production pin `435ed1cd68e20f8a9fe4871233ed1f8ce7ccf9e8`.
No manifest pin, installed file, DSP, device or calibration payload was changed.
No Android build or ABI compatibility is claimed. Future Android integration
should backport the minimum required protocol/write capability onto 435ed1c,
not deploy the fork's unrelated changes implicitly.

**Default DSP error-code compatibility is not preserved by this prototype.**
Production 435ed1c `apps_std_fopen_with_env` rejects w/a immediately with
AEE_EUNSUPPORTED (0x14/20). The fork-based prototype resolves the read-only mapping
and currently reports AEE_EFAILED (1), including ordinary mapped w+/a+ requests.
Readonly protection is not equivalent to the original error contract; firmware
fallback behavior can differ. This is an explicit blocker to direct deployment.
The minimum production backport must test and preserve the original default w/a
code 20 before introducing opt-in registry I/O error mappings. This prototype's
fixture intentionally records the current value 1 rather than disguising it as
production-compatible. No manifest pin or default error policy was changed to
hide this gap.

`upstream-dependencies.txt` lists all 24 full commit hashes between those pins;
`upstream-change-stat.txt` inventories the 49 changed files (new interpreter and
handle APIs are among them). The patch also corrects the fork's stale Android.bp
source lists to match its existing Meson lists; this is source-list consistency,
not proof of an Android build. The fixtures use actual C code on the host.

## Deliberately limited capability

Ordinary mapped, mapped-or-empty and sysfs backends have no write/create/
truncate/rename/unlink operations. `r+`, `w+`, `a+` cannot turn those mappings
writable. No read-only fallback falsely satisfies a requested r+ open.

Only explicit `-W ABSOLUTE_OUTPUT_DIR` selects the new registry backend. It must
be a new independent private output tree, owned by the daemon UID, with mode
0700 directories. Never reuse a vendor config directory or a directory containing
existing JSON/fac_cal inputs. The backend validates this structure:

```
ABSOLUTE_OUTPUT_DIR/              # 0700, exactly one child directory
  registry/                      # 0700, empty or exactly one regular file
    registry                     # optional, single link, <= 4 MiB
```

It maps to `/persist/sensors/registry` and the already-established
`/mnt/vendor/persist/sensors/registry` alias. Consequently the allowed file is
virtual `/persist/sensors/registry/registry/registry`, retaining the existing
nested directory and empty-file layout rather than treating the configured
output directory as a single file. Maximum file count is one and total payload
size is at most 4 MiB. No extra directories or output filenames are creatable.
The old packaged empty registry remains untouched and is used as before when
-W is absent.

Follow-up research found an exact offline group-name source in sscregistrygen
and a historical #37 request for `registry/../temp.json`. See
`../registry-schema/README.md` and the cached precise allowlist candidate.
Current R18 consumption, actual rename destinations and commit ordering remain
unverified; the current one-file code has not yet been expanded using that evidence. Therefore `.tmp`, arbitrary group names,
rename and unlink are rejected, not guessed. This prototype is a confinement test, not a complete usable SEE registry
implementation; a rejection is intentional and must not be
“fixed” by enabling general writes. It is not ready for an ALS activation test.
A future expansion needs request/name evidence, bounded file count/aggregate
quota and source/destination capabilities for any rename protocol.

Safety properties implemented:

- Every trusted output-root component and every backend child opens with
  O_NOFOLLOW; intermediate directories additionally require O_DIRECTORY.
- Dot traversal in mutation paths, special files, unknown entries, multiple
  hard links and unknown mode strings are rejected. Known trailing-CR aliases
  are handled on both parent and final components.
- Registry descriptors preserve read/write modes. A write-open checks type,
  owner, size and obtains an exclusive nonblocking lock before truncation.
- Short writes/EINTR are completed; zero writes, ENOSPC and quota failures return
  failure. Writes are **not atomic**: after the injected partial-write/ENOSPC
  failure, the actual file contains one new byte `a` even though the RPC reports
  status=1 and written=0. The zero count does not mean no bytes reached the file
  or storage. Both backend and apps_std fixtures assert this actual content.
  Any future controlled experiment must preserve/quarantine a failed output tree
  for review and use a fresh isolated output tree afterward; it must not silently
  reuse the failed bytes as a valid registry. No automatic retry, recovery or
  unproven atomic-rename protocol is provided here.
- Actual file fsync and parent-directory fsync failures propagate through
  apps_std_fsync and fclose, including read-only handles referencing the same
  inode as another writer. Those handles perform real fsync too; no permission
  to write is granted. A failed close retains its handle for error handling.
- The writer lock is inode-wide flock and is taken before any O_TRUNC effect.
  The actual builder fixture holds a writer open through one registry alias and
  independently resolves the other alias both in the same FD table and through
  a second root. Both append and truncate opens must return EWOULDBLOCK, while
  original `DATA` remains intact. This does not assume RPC request serialization.
  It does not defend against unrelated local writers that ignore advisory locks.
- A pre-existing upstream root-open error path closing uninitialized data was
  fixed after the denial tests triggered ASan; backport that small correction too.

The dedicated root must not have another local writer. Directory privacy and
FD-relative access protect against untrusted DSP paths, not an unrelated process
running under the same UID that deliberately edits the backing tree. No SELinux
rules or init directories have been added; those require separate reviewed
Android integration.

## Reproduce without a device or VM

From the workspace root, use a fresh scratch checkout (not production sources):

```sh
git clone https://github.com/FadyAckad/hexagonrpc.git /tmp/hos-registry-review
git -C /tmp/hos-registry-review checkout --detach 40e50412812adce9dddc2be9a4510ebce05eef89
git -C /tmp/hos-registry-review apply --check "$PWD/patches/hexagonrpc-registry-output-restricted.patch"
git -C /tmp/hos-registry-review apply "$PWD/patches/hexagonrpc-registry-output-restricted.patch"
scripts/ssc/registry-write/run-fixture.sh /tmp/hos-registry-review /tmp/hos-registry-build
```

Requirements: git, host C compiler and Address/UndefinedBehavior sanitizers.
The script adapts macOS struct-stat field names using compile flags; it does not
edit the supplied source. LeakSanitizer is disabled on macOS because it is not
supported there. ASan/UBSan are enabled. Temporary fixture files are isolated
and removed after success; output executables remain in the selected build dir.

Three executables exercise:

1. Real backend: modes, readonly regression, full/short/EINTR/zero/ENOSPC writes,
   lock-before-truncate, bounded seek/truncate, both fsync stages and close failure,
   traversal, final/intermediate/CLI symlinks, hard links, unexpected files,
   post-open substitution, CR names, denied rename/unlink.
2. Actual `apps_std_procs` dispatch: exact successful byte count; error status and
   zero successful count after short-write failure; malformed length, invalid
   mode, unknown name, quota, fsync and fclose failure propagation; readonly maps.
3. Actual VFS builder: both registry aliases only become writable with opt-in;
   config/calibration sentinel and original empty registry remain unchanged;
   cross-alias and independent-root second writers return EWOULDBLOCK before truncation.

On 2026-10-30 all three passed from a fresh exact-base worktree with the patch
applied. ASan/UBSan reported no error. Backend-only and apps_std tests initially
found the root-open ownership bug and verified its correction. Logs are under
`~/.cache/hos-r18-als/reproducible-fixture-review2.txt`. These tests do not exercise
live FastRPC framing, DSP firmware, Android SELinux, or real ambient-light
measurements. The ALS crash causal link remains unverified.
