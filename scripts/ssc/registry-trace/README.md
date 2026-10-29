# Read-only denied-open trace fixture

Independent patch: `patches/hexagonrpc-denied-open-trace.patch`, against production
`435ed1cd68e20f8a9fe4871233ed1f8ce7ccf9e8`. This is not the registry-write backport.
It adds no `-W`, write/create backend, fwrite method or writable mapping.

Only Android's denied `apps_std_fopen_with_env` w/a branch calls liblog. It still
returns `AEE_EUNSUPPORTED` (20) before opening a virtual file. Tag:
`hexagonrpcd-denied`. Fields: fixed method name, escaped request mode (max16
output bytes), escaped virtual request path (max256 output bytes). Input scan is
also bounded by those limits. Nonprintable bytes, backslash and double quote are
encoded as complete `\xHH` units; truncation never splits an escape. No file
contents, RPC payload, environment variable or calibration data are logged.
A process-wide C11 atomic CAS counter saturates at64; it cannot wrap or exceed64
under concurrent calls. Logging is metadata-only and does not grant write access.
Paths at the cap may be truncated and must not be mistaken for full names.

Before NUL-terminal indexing, this method checks input/output array pointers,
context, input1/3/4 pointer/nonzero length, output pointer and length>=uint32.
The method ABI still assumes its declared arrays contain five input buffers and
one output buffer (no array count is passed to the handler). Valid read handling
and valid request return codes are unchanged. Malformed buffers now return
AEE_EBADPARM instead of underflow/dereference. The old unbounded denied-path
stderr print is removed; other read-path diagnostics are unchanged.

Test an isolated cache checkout, never production checkout/device:

```
sh scripts/ssc/registry-trace/run-fixture.sh PATCHED_SOURCE CACHE_BUILD PRISTINE_435ED1C_SOURCE
```

PATCHED_SOURCE must be an archive of435ed1c with this patch and the existing
`scripts/crdroid-tree-fixes.py:patch_hexagonfs_cr` applied. The CR fix is separate
and not embedded in the diagnostic patch. The fixture itself only reads source;
it compiles the actual apps_std.c and original hexagonfs implementation with
ASan/UBSan. `android/log.h` is a host-only boundary stub, not an Android include
replacement; captured liblog messages come from the actual patched method.

Coverage: w/a includingw+ remain20; unavailable-read remains1 against separately
compiled pristine baseline; output word unchanged on failure; null/zero/truncated
RPC buffers; CR/quote/backslash/control escaping; output<=256 even for all-CR
input;8threads×1000 denied requests cap process logs at64; actual mapped read with
CR suffix succeeds using production treefix; existing sentinel contents unchanged
and attempted new file absent. Temp fixture setup/cleanup creates local test
files only. It does not mount or touch device/vendor/registry data.

Observed baseline lifetime caveat: fixture cleanup must close child handles
before the root; closing root first hit an existing435ed1c descriptor-parent
use-after-free. The diagnostic patch intentionally does not alter that lifetime
contract or import unrelated upstream fixes.

Verified cache: `~/.cache/hos-r19-sensors/registry-trace/`, test-results.txt and
verification.json. Host tests passed; full Android liblog linkage and runtime
standard-boot collection are not yet validated. No VM, device install/restart,
ALS activation or image change was performed.
