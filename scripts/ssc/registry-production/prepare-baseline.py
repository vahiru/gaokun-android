#!/usr/bin/env python3
"""Apply only the existing production CR fix to an isolated 435ed1c checkout."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

checkout=Path(sys.argv[1]).resolve()
pin='435ed1cd68e20f8a9fe4871233ed1f8ce7ccf9e8'
if subprocess.check_output(['git','-C',str(checkout),'rev-parse','HEAD'],text=True).strip()!=pin:
    raise SystemExit('Wrong base: require exact production 435ed1c')
repo=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('existing_tree_fixes',repo/'scripts/crdroid-tree-fixes.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix='hos-cr-baseline-') as tmp:
    root=Path(tmp);(root/'external').mkdir();(root/'external/hexagonrpc').symlink_to(checkout,target_is_directory=True)
    result=module.patch_hexagonfs_cr(root)
    print(result)
    if '已加上' not in result and '已打过' not in result:
        raise SystemExit('Existing CR fix failed')
