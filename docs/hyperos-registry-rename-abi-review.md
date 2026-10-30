# Registry backport: rename ABI and durability review

Offline review, 2026-10-30. No device/DSP/ALS session or VM activity.
The production-pin candidate still refuses rename; this review does not add a
rename capability or seed any registry/calibration payload.

## Independently verified ABI

Qualcomm's official `quic/fastrpc` sources were fetched read-only and verified
again at commit `a56e9d4de3614a8d8f4b0e15ff561350c4b686e3`:

- `inc/apps_std.h`: signatures for fwrite, fsync, ftrunc and frename.
- `src/apps_std_skel.c:571-620,658-666,1442-1445`: actual generated unmarshalling.
- `src/apps_std_imp.c:569,1428,1746,1768`: reference operation semantics.

Pinned copies and SHA256s are in
`~/.cache/hos-r18-als/apps-std-abi-source-audit.json`.
They match the initially inspected unpinned snapshots byte-for-byte. These are
reference ABI sources, not proof of the exact bundled Huawei SEE implementation.

| Operation | Scalar method | Primary words | Input/output buffers |
|---|---|---|---|
| stat | 31 escape | `[31, path_length]` | 2 / 1 |
| ftrunc | 31 escape | `[32, fd, signed_int32_length]` | 1 / 0 |
| frename | 31 escape | `[33, old_length, new_length]` | 3 / 0 |

Rename's other two input buffers are positive-length, NUL-terminated strings;
lengths include their terminal NUL. A future old-interp2 definition would need
one extended-method word plus two string-length words, not two ordinary strings
without the extended word. Current code rejects the full 33 before indexing.
The reference uses minimum buffer lengths; the current legacy decoder requires
exact counts/lengths. Do not casually change that existing contract while adding
rename. Truncate's signed length must not be interpreted as a huge positive
uint32 value when method32 is eventually implemented.

A new fixture constructs the **complete official method33 packet**, passes it
through the actual old framing/decoder/dispatcher, and checks result 20. A
fixture source containing `STAGE` and destination containing 4096 known bytes
both remain intact. No synthetic rename success is returned.

## A real encoding defect found by the correct packet

The variable-length old/new strings make a later buffer-size field unaligned.
Original `hexagonrpcd/iobuffer.c:219` stored via `*(uint32_t *)ptr`, triggering
UBSan's misaligned-store error. Earlier aligned 4096-byte payload tests did not
cover this. UBSan normally recovers, so a later PASS line alone was insufficient.

The backport now writes the same little-endian four bytes with `memcpy`.
No wire format, method permission or library API changes. The production fixture
runner now sets `UBSAN_OPTIONS=halt_on_error=1`; the complete packet regression
and all existing tests pass without sanitizer diagnostics. This is a minimal
marshalling correction, not an implementation of rename.

## ABI success does not imply durable registry commit

The official `apps_std_fsync` implementation invokes **fflush**, and its comment
explicitly disclaims physical disk persistence. The fork's unbuffered-file
no-op therefore cannot be called an ABI violation merely because it lacks host
fsync. Our actual file/parent-directory fsync is a stronger project durability
requirement. It may expose errors/timing the reference implementation does not.
The official rename wrapper calls `rename()` and returns an error on failure;
it does not fsync parent directories or establish a durable transaction.

Reference fwrite can return a positive partial byte count with success; our
opt-in policy instead completes short writes or returns failure/written=0 on
error. The latter must never be interpreted as no partial file change. Error
mapping also needs review: reference operations use errno-derived failures,
whereas current new operations generally return AEE_EFAILED. Default production
w/a=20 is preserved, but that does not prove all opt-in failure semantics match
what SEE expects. Never infer protocol compatibility solely from nonzero checks.

The candidate currently requires a fresh private output tree on every new
-W daemon run. It flushes bytes to storage, but deliberately refuses automatically
reusing populated output after restart. It is a bounded capture candidate, not
a finished persistent-registry service. The successful fixtures establish
confinement/transport behavior, not restart/recovery compatibility.

## Evidence still needed before allowing a commit operation

Historical #37 proves writes to
`/persist/sensors/registry/registry/../temp.json`. The generator proves a finite
set of group filenames. Neither establishes which actual destination is passed
to rename, whether replacement is intended, or the fopen/write/flush/close/
rename order. Do not authorize every pair of reviewed names from those facts.
Existing firmware source or an already captured RPC trace can resolve this
without enabling ALS; a future non-ALS capture still requires separate approval
and recovery preparation. No such live capture was performed here.

For each observed rename pair, establish:

1. Exact source/destination spelling, aliases/CR handling, and source content
   format. Determine whether one shared temp file is reused serially or multiple
   writers exist. Preserve all original calibration/config inputs read-only.
2. Whether the source FD is closed before rename and whether destination FDs can
   remain open. POSIX rename does not invalidate an open writer: later writes
   through that FD can mutate the newly published inode. An existing flock is
   advisory and pathname rename does not automatically honor it.
3. Which failures SEE retries or treats as fatal, including missing source,
   existing destination, quota failure, fsync failure and partial writes. Do not
   introduce a fabricated success or silently reset a failed output tree.

## Necessary bounded implementation once that evidence exists

- Add explicit method33 decoding only to the opt-in table, retaining default
  unsupported behavior. Validate lengths/NULs and full extended ID. Method32
  remains a separate explicit decision.
- Resolve both paths with the registry capability, NOFOLLOW and the exact pair
  policy. Restrict to one captured output root; reject unknown/cross-root paths,
  symlinks, special files and unexpected hard links. Do not enable general '..'.
- Serialize the commit under the same root quota lock. Validate source and any
  replaced destination under that lock. File locks and descriptor tracking must
  prevent a still-open writer from changing a published inode after commit;
  requiring closed FDs is only valid if the observed protocol permits it.
- Flush the source data, perform the permitted atomic pathname replacement,
  then fsync **both** relevant parents when moving root/temp.json into a group
  under root/registry/. Merely fsyncing the file or one parent is insufficient
  for a claimed durable cross-directory commit.
- If rename succeeds but a later fsync fails, the pathname may already have
  changed. Report failure and quarantine the trial output; do not assert an
  atomic rollback or automatically repeat the operation. A crash matrix must
  distinguish pre-rename, post-rename/pre-fsync and fully committed states.
- Define a validated reopen/recovery policy for later boots separately from
  the initial fresh-output restriction. The offline generator's historical
  corruption and unsupported SEE schema forbid using generated data as a shortcut.

Required future offline fixtures include exact observed pairs, active writer
and reader lifetimes, same/cross-root cases, quota replacement accounting,
crashes at each commit boundary, both parent-fsync errors, and reboot/reopen
validation. Until these are implemented and independently checked, rename stays
unsupported and the candidate must not be called a complete ALS repair.
