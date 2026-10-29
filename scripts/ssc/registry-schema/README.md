# Offline SSC registry schema validator and safe generator

This tool is independent of the registry write-backend patch. It reads existing
configuration files; it does not change them, create missing RGB calibration,
contact a DSP, or establish SEE compatibility. Generated results must not be
installed on the device: the historical prepopulation experiment broke accel.

The upstream `tools/sscregistrygen.c` at 40e5041 recursively emits each top-level
and dot-prefixed child group into a filename equal to its full group name, with
child references encoded as `{type: grp, ver: "0", data: ""}`. Filtering is by
`config.hw_platform` and `config.soc_id`, not by directory naming. Existing R18
QRD/449 inputs select 24 of 26 JSONs, excluding the two disabled configurations
with `soc_id: [""]`. They imply 141 unique group files. With the old empty file
named `registry`, that explains the recorded historical total of 142.

A reproduced defect makes upstream combined output unsuitable even before
firmware testing: both `sns_amd.json` and `sns_amd_sw_enabled.json` match and emit
`sns_amd`. The upstream generator neither merges nor uses O_TRUNC/O_EXCL.
Base-then-enabled leaves trailing bytes and invalid JSON. Enabled-then-base
produces valid JSON but loses `enabled`. Adding O_TRUNC alone fixes the tail but
still loses fields. This is a concrete tooling defect, not proof of the cause
of the historical SLPI/ALS failure.

`registry_schema.py` validates all inputs and serializes everything before
creating an output directory. By default **any duplicate output group fails**.
Explicit `--merge-disjoint` accepts only identical shared field values and
non-overlapping fields within the group. It never recursively combines parts of
a typed item, chooses a higher version, or guesses calibration precedence.
Different type/value/version is an error. For current `sns_amd`, only `enabled`
is added to the original owner/param1/param2/sample_rate group; all original
selected-input fields are retained exactly. This is tooling value preservation,
not proof that SEE accepts this representation.

Output is optional. When requested, the entire directory must be new; it is
created privately, every file uses O_EXCL, and files/directory are fsynced. No
existing or partial output is overwritten. An I/O failure can leave a partial
directory; it must be kept separate and never automatically retried as valid.
The stdout report stays outside the output directory and includes source hashes,
filter exclusions, explicit merges and output hashes. Files contain no injected
calibration defaults. Missing `tcs3701.rgb.config` and
`tcs3701_platform.rgb.fac_cal` remain missing.

```sh
python3 -m unittest discover -s scripts/ssc/registry-schema -p 'test_*.py' -v

# Validation only: current inputs intentionally fail on duplicate sns_amd.
python3 scripts/ssc/registry-schema/registry_schema.py "$CONFIG_DIR" --platform QRD --soc-id 449

# Explicit semantics audit; output directory must not already exist.
python3 scripts/ssc/registry-schema/registry_schema.py "$CONFIG_DIR" \
  --platform QRD --soc-id 449 --merge-disjoint --output "$NEW_CACHE_DIR" \
  > "$REPORT_OUTSIDE_OUTPUT_DIR"
```

Nine fixture tests cover value-preserving flatten/filter, default duplicate
rejection, explicit disjoint merge, type/value/version conflicts, unsafe names,
duplicate JSON keys/nonfinite values, nested-name collisions, fresh output only,
symlink refusal and partial I/O failure without retry.

Current evidence is cached at `~/.cache/hos-r18-als/`:

- `generated-registry-audit.json`: 26 input hashes, per-input exact semantics,
  selected/excluded files, 141 names and combined-generator corruption.
- `generator-order-evidence.json`: both explicit collision orders reproduced.
- `registry-filename-allowlist-candidate.json`: 141 group paths, empty sentinel
  and historical `temp.json`; **candidate only, not backend authorization**.
- `strict-generator-value-proof.json`: merged generation retains every selected
  input group field, adds no RGB group, and leaves source hashes unchanged.

The old #37 record at `docs/stage4-findings.md:641` already documents the DSP
request `/persist/sensors/registry/registry/../temp.json`. That establishes a
historical temporary filename and parent-directory need, not current R18
observation, rename destinations/order, or ALS causality. A future bounded
write-backend revision can use the exact audited group-name set plus an explicit
normalization of that legacy temp path. It still needs count and aggregate-byte
limits, atomic/error protocol review, and default DSP error-code preservation.
Do not enable arbitrary `..`, unknown names, or general rename based on this
finding. Do not prepopulate even the repaired outputs on the device.
