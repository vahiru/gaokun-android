#!/usr/bin/env python3
"""Reproduce A17 split-policy compilation offline, with a separate neverallow check.

Path/order reference: refs/a17/system/core/init/selinux.cpp:274-390.
Does not install policy or alter inputs. Each output directory must be new.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, required=True, help="donor root containing system/system_ext/product")
    p.add_argument("--vendor", type=Path, required=True, help="original gaokun3 vendor/etc/selinux")
    p.add_argument("--secilc", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--policy-version", type=int, default=30)
    p.add_argument("--userdebug", action="store_true", help="select donor system_ext userdebug platform policy")
    a = p.parse_args()
    s, e, pr = [a.root.resolve() / x / "etc/selinux" for x in ("system", "system_ext", "product")]
    v = a.vendor.resolve()

    def required(path):
        if not path.is_file():
            p.error(f"missing required input: {path}")
        return path

    def version(name):
        value = required(v / name).read_text().strip()
        if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)?", value):
            p.error(f"invalid version in {name}: {value!r}")
        return value

    ver = version("plat_sepolicy_vers.txt")
    gv = version("genfs_labels_version.txt")
    platform = e / "userdebug_plat_sepolicy.cil" if a.userdebug else s / "plat_sepolicy.cil"
    inputs = [required(platform), required(s / f"mapping/{ver}.cil")]
    optional = [s / f"mapping/{ver}.compat.cil", e / "system_ext_sepolicy.cil",
                e / f"mapping/{ver}.cil", e / f"mapping/{ver}.compat.cil",
                pr / "product_sepolicy.cil", pr / f"mapping/{ver}.cil"]
    inputs += [f for f in optional if f.is_file()]
    inputs += [required(v / "plat_pub_versioned.cil"), required(v / "vendor_sepolicy.cil")]
    odm = a.root.resolve() / "odm/etc/selinux/odm_sepolicy.cil"
    if odm.is_file():
        inputs.append(odm)
    genfs = s / f"plat_sepolicy_genfs_{gv}.cil"
    if int(gv) >= 202504:
        required(genfs)
    if genfs.is_file():
        inputs.append(genfs)
    compiler = required(a.secilc.resolve())
    a.out.mkdir(parents=True, exist_ok=False)
    report = {"compiler": {"path": str(compiler), "sha256": sha(compiler)},
              "vendor_version": ver, "genfs_version": gv, "userdebug": a.userdebug,
              "inputs": [{"path": str(f), "sha256": sha(f)} for f in inputs],
              "skipped_optional": [str(f) for f in optional if not f.is_file()], "runs": {}}
    for mode in ("runtime", "neverallow"):
        output = a.out.resolve() / f"{mode}.policy"
        cmd = [str(compiler), str(inputs[0]), "-m", "-M", "true", "-G"]
        if mode == "runtime":
            cmd.append("-N")  # init uses -N; this run cannot establish neverallow compliance.
        cmd += ["-c", str(a.policy_version), str(inputs[1]), "-o", str(output), "-f", os.devnull]
        cmd += [str(f) for f in inputs[2:]]
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (a.out / f"{mode}.log").write_bytes(result.stdout)
        ok = result.returncode == 0 and output.is_file() and output.stat().st_size > 0
        report["runs"][mode] = {"argv": cmd, "returncode": result.returncode, "passed": ok}
        if ok:
            report["runs"][mode].update(size=output.stat().st_size, sha256=sha(output))
        print(f"{mode}: {'PASS' if ok else 'FAIL'} (rc={result.returncode})")
    (a.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if all(r["passed"] for r in report["runs"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
