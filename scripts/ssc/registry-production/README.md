# Minimal 435ed1c registry-output backport — offline candidate

Patch: `patches/hexagonrpc-registry-production-backport.patch`.
Base: **435ed1cd68e20f8a9fe4871233ed1f8ce7ccf9e8 plus the existing production
CR-segment fix** from `scripts/crdroid-tree-fixes.py::patch_hexagonfs_cr`.
`prepare-baseline.py` applies only that existing fix and requires the exact pin.
The new patch intentionally does not duplicate it. Reapplying the original
helper after the new patch reports already applied and changes nothing.

This is separate from the fork-based prototype. It does not change the manifest,
libhexagonrpc, its public headers, the interp2 API, or existing mapped backend
source. It does not pull the fork's 24 commits or migrate to interp4/handle APIs.
No generated registry values, original calibration values, firmware, image or
device state were changed. No VM or ALS session was used. **Android compilation,
SELinux integration and real DSP compatibility are not verified. Do not deploy
or enable ALS from the host test results.**

## Default contract and opt-in scope

Without `-W`, the original 32-entry apps_std dispatch table is byte-identical.
`fopen_with_env` w/wb/w+/a/a+ (and old w-prefixed strings) still return
AEE_EUNSUPPORTED **20** before filesystem operations. Historically accepted r+
remains a read-only open, with no write hook. New fopen(0), fwrite(5), fsync(23)
remain absent from the default table. Default large-buffer behavior is retained.
Read-only stat and CR-qualified config reads are regression-tested.

Only explicit `-W NEW_PRIVATE_OUTPUT_DIR` selects registry capabilities and
adds methods 0, 5, 23 using **old interp2 definitions**. All ordinary mapped,
mapped-or-empty and sysfs paths remain read-only even in this mode. Failed
ordinary w/a opens continue to return 20 when the backend lacks create support.
Opt-in malformed modes are rejected, never downgraded to read-only success.

The output root must be independently created, owned by the daemon UID and
0700, with a pre-existing private child directory `registry`. It must not be
an existing config/calibration directory. Before opening FastRPC, the CLI
preflight rejects any populated output: only the private `registry` child and
an optional zero-byte `registry` sentinel are allowed. Generated groups/temp or
nonempty partial output cannot be reused on a new daemon run. Within one running
session the backend naturally reopens its own outputs. A zero-length failed
attempt is not distinguishable from an empty prepared tree, so the operational
requirement to quarantine every failed trial still applies. No file payload is
seeded by this patch. The finite output-name set is:

- `registry/<group>`: 141 exact names derived from the 24 QRD/449-selected JSONs.
- `registry/registry`: the existing empty sentinel filename, optional.
- `temp.json`: the historical sibling temporary filename recorded in #37.

`name-provenance.json` pins all 26 original input hashes and group origins;
`verify-names.py` checks the exact header set. No missing RGB group is invented.
The backend permits at most 143 files, 256 KiB per file, 2 MiB total logical
payload bytes. These conservative limits are host-validated limits, not measured
maximum real DSP requirements. Unknown files, directories or groups fail closed.

Mappings retain `/persist/sensors/registry` and the existing `/mnt/vendor` alias.
Only the two exact historical absolute spellings ending
`registry/registry/../temp.json` normalize to the sibling temp path. Existing
per-component trailing CR normalization also applies, including `..\r` in that
specific spelling. Other dot/dotdot traversal is rejected; it is not generally
enabled. Intermediate/final/CLI-root symlinks, special files and multiple hard
links are refused before truncation. Existing vendor data stays untouched.

## Quota, durability and failure semantics

A file's exclusive nonblocking flock is acquired before O_TRUNC. Actual alias
and independently opened-root fixtures prove a second append/truncate writer
returns EWOULDBLOCK with the original bytes intact. A separate fresh directory
open-description lock serializes **every** create/write/truncate quota check
across files/aliases/threads. It is not a dup of the same locked description.
The 16-race quota fixture leaves exactly one successful competing 256 KiB write
when only 256 KiB aggregate capacity remains; total never exceeds 2 MiB.

File fsync plus parent-directory fsync are real, including explicit sync through
a read-only file handle. File-close sync failure returns failure and retains the
handle. Temporary directory lookup cleanup has no pending buffered writes;
explicit directory fsync still performs the operation. Ordinary mappings gain
no write/sync capability. Directory and file descriptors in the registry backend
own independent host FDs, so closing a directory before an opened child does not
leave a dangling traversal pointer. Necessary root-init/lifetime corrections
are included rather than copying unrelated library changes.

Writes are **not atomic**. A short write followed by ENOSPC returns a failed RPC
with written=0, while the file already contains new partial bytes. Fixtures
assert the actual one-byte partial file. Neither failure nor written=0 proves
nothing reached storage. A future controlled trial must retain/quarantine any
failed output tree and must not automatically reuse it as a valid registry.
There is no unproven automatic retry or atomic rename protocol here.

## Listener and explicitly unsupported protocol

Only opt-in mode fetches frames beyond the legacy 256-byte inline buffer, using
`adsp_listener_get_in_bufs2` method 5 represented in the existing interp2 API.
The implementation follows the fetch shape in quic/fastrpc
`src/listener_android.c` and `inc/adsp_listener.h`; cached source snapshots live
under `~/.cache/hos-r18-als/`. Frames are capped at 256 KiB + 4096 bytes, fetched
length must match the previously declared complete length, and every embedded
blob length is validated before the old decoder allocates. Short/error/oversize
responses fail; zero-filled remainder is never passed off as a complete fetch.
The successful 4096-byte fixture uses the actual old encoder, decoder, argument
validation, dispatch and backend; only transport is mocked.

Inline method 31 is treated as the extended-ID marker. Its full uint32 ID is
checked **before indexing**; only existing stat(31) is supported. Method32
(ftrunc) and method33 (rename) are explicitly rejected with 20, not masked into
method0. A malformed producer masking 32 to inline0 cannot use a ftrunc-shaped
buffer as fopen: count/size validation rejects it before any operation. Bits
already discarded by a producer cannot be recovered; a byte-identical legitimate
fopen packet cannot be distinguished by guessing its former method number.
The fixtures prove conforming extended32/33 and malformed masked32 do not change
the file. In opt-in mode, unknown-method rejection is actually returned on the
next listener invoke; default unknown-call control flow remains unchanged.

**Not supported/verified:** RPC method32 truncate, remove, rename, real rename
destinations/commit order, the real SEE large-fetch response contract, Android
ABI build, SELinux labeling, DSP recovery and stable ALS readings. Backend-only
truncate exists for bounded mode/quota tests, but is not advertised as RPC32.
The finite group capability is more than a single-file toy; it is still not a
complete operational SEE registry implementation. In particular historical temp
writes do not prove a safe rename destination or justify arbitrary rename.
The known prior prepopulated registry failure forbids treating any generated
files as a working on-device configuration.

## Source references, not imported dependency commits

Selected ideas were inspected in fork commits `518fdf0` (filesystem operations),
`7543f33` (apps_std writes), `f0dfd50` (large reverse-RPC fetch), `7f2fc1d`
(parent-directory layout), `40e5041` (lifetime/error handling), and `78e2a94`
(extended-ID convention). None is cherry-picked wholesale. The code is adapted
to the production interfaces; default table and generic mapped source are
independently byte-compared against 435ed1c. The new source list adds one private
C file and does not replace the old interfaces.c or public library sources.

## Reproduce on an isolated checkout

From the workspace root, with a host C compiler supporting ASan/UBSan:

```sh
git clone https://github.com/linux-msm/hexagonrpc.git /tmp/hos-registry-production
git -C /tmp/hos-registry-production checkout --detach 435ed1cd68e20f8a9fe4871233ed1f8ce7ccf9e8
python3 scripts/ssc/registry-production/prepare-baseline.py /tmp/hos-registry-production
git -C /tmp/hos-registry-production apply --check "$PWD/patches/hexagonrpc-registry-production-backport.patch"
git -C /tmp/hos-registry-production apply "$PWD/patches/hexagonrpc-registry-production-backport.patch"
python3 scripts/ssc/registry-production/prepare-baseline.py /tmp/hos-registry-production
scripts/ssc/registry-production/run-fixture.sh /tmp/hos-registry-production /tmp/hos-registry-production-build
```

Five actual-C fixtures cover apps_std default and opt-in contracts, backend
paths/FDs/sync/short writes, actual VFS aliases and CR compatibility, finite
multi-file concurrent quotas, and actual old listener framing/dispatch/error
reply. Host-only endian/stat shims are never installed. On macOS LeakSanitizer
is unavailable, so it is disabled; ASan and UBSan remain enabled. Temporary data
is isolated and removed after passing tests. Production-source evidence and
fresh-checkout logs are under `~/.cache/hos-r18-als/production-source-audit.json`
and `production-final-fixture.txt`. The manifest pin remains unchanged.
