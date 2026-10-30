#!/usr/bin/env python3
"""Offline original app-init/dispatch trace; STOP before enrol algorithm."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from replay_preprocess import HERE, TA_PIN, runner


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    blob=(HERE/'cabs/qctree/fingerpr.mbn').read_bytes()
    assert hashlib.sha256(blob).hexdigest()==TA_PIN
    args.out.mkdir(parents=True,exist_ok=False)
    mapping=bytearray(0x364000)
    # Original init and dispatcher; constants include the jump table, no keys.
    for lo,hi in ((0xaccc,0xad60),(0xadfc,0xc954),(0xd63c,0xd648),
                  (0x1e64b0,0x1e6508)):
        mapping[lo:hi]=blob[lo+0x3000:hi+0x3000]
    records=[]
    with tempfile.TemporaryDirectory(prefix='mapped-',dir=args.out) as temporary:
        root=Path(temporary)
        (root/'ta.map').write_bytes(mapping)
        source=root/'workspace.c'
        source.write_text('#define main unused_preprocess_main\n'+
                          (HERE/'preprocess_replay_runner.c').read_text()+'\n'+
                          (HERE/'workspace_replay_tail.c').read_text())
        with runner(root,'linux-container',source) as (command,scope):
            for case in ('direct-8026','8031-then-8026'):
                result=subprocess.run(command+['unused','0',case],capture_output=True,timeout=15)
                item={'case':case,'exit_code':result.returncode}
                if result.stdout:item['trace']=json.loads(result.stdout)
                records.append(item)
                print(json.dumps(item),flush=True)
    (args.out/'results.json').write_text(json.dumps(records,indent=2)+'\n')
    (args.out/'scope.json').write_text(json.dumps({
        'runtime':scope,'ta_sha256':TA_PIN,'device_actions':False,
        'mocked':['log configuration','logs','4e24 message setup','9280 FocalTech initialization',
                  '8cf0 file lookup returns missing','98888 algorithm setting'],
        'stop_at':'9a94 enrol entry; never execute algorithm',
        'allocator':'original d63c shim; qsee_realloc(NULL,size) replaced by tracked host allocation',
        'scope_limit':'constructed empty session with boundary mocks; not all initialization paths'
    },indent=2)+'\n')


if __name__=='__main__':main()
