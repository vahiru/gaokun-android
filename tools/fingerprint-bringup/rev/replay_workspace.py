#!/usr/bin/env python3
"""Offline original app-init/dispatch trace; the enrol algorithm is never run.

Runs 0x8026 with the allocation the TA really requests (0xf8000), then with a
guarded host workspace large enough for the qword_2A4638+0x950000 window the
dispatcher passes to sub_9A94.  The clear stage executes the original 0x9a94
and 0xd350 and stops at the algorithm entry 0x9b57c.  A -gate case also seeds
the real static index bound at unk_2261B0+8 so that the original code takes the
algorithm call; the stop hook then records the entry arguments without
executing any algorithm byte.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from replay_preprocess import HERE, TA_PIN, runner

# qword_2A4638+0x950000 .. +0xa2c00 is the extent sub_9A94 clears.
ENROL_WORKSPACE_OFFSET = 0x950000
ENROL_CLEAR_BYTES = 666624
ENROL_HIGH_WATER = ENROL_WORKSPACE_OFFSET + ENROL_CLEAR_BYTES
# Real static value sub_9A94 reads as the index bound (bytes at unk_2261B0+8).
INDEX_BOUND_VA = 0x2261B8


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--workspace-bytes',type=lambda s:int(s,0),
                        default=0x1000000,
                        help='minimum workspace for the enlarged case (default 16 MiB); 0 skips it')
    args=parser.parse_args()
    if args.workspace_bytes and args.workspace_bytes < ENROL_HIGH_WATER:
        parser.error(f'--workspace-bytes must cover the {ENROL_HIGH_WATER:#x} high-water mark')
    blob=(HERE/'cabs/qctree/fingerpr.mbn').read_bytes()
    assert hashlib.sha256(blob).hexdigest()==TA_PIN
    index_bound=struct.unpack_from('<I',blob,INDEX_BOUND_VA+0x3000)[0]
    assert index_bound,'expected a non-zero static index bound'
    args.out.mkdir(parents=True,exist_ok=False)
    mapping=bytearray(0x364000)
    # Original init and dispatcher; constants include the jump table, no keys.
    # The clear stage also needs the enrol wrapper (9a94..9b53) and the original
    # memset (d350..d45f); its unreachable state helpers are stubbed, not mapped.
    for lo,hi in ((0xaccc,0xad60),(0xadfc,0xc954),(0xd63c,0xd648),
                  (0x1e64b0,0x1e6508),(0x9a94,0x9b54),(0xd350,0xd460)):
        mapping[lo:hi]=blob[lo+0x3000:hi+0x3000]
    modes=[('original',None,(('direct-8026',0),('8031-then-8026',0)))]
    if args.workspace_bytes:
        modes.append(('enlarged',args.workspace_bytes,
                      (('direct-8026',0),('8031-then-8026',0),
                       ('direct-8026-clear',0),('8031-then-8026-clear',0),
                       ('direct-8026-clear-gate',index_bound),
                       ('8031-then-8026-clear-gate',index_bound))))
    records=[]
    with tempfile.TemporaryDirectory(prefix='mapped-',dir=args.out) as temporary:
        root=Path(temporary)
        (root/'ta.map').write_bytes(mapping)
        source=root/'workspace.c'
        source.write_text('#define main unused_preprocess_main\n'+
                          (HERE/'preprocess_replay_runner.c').read_text()+'\n'+
                          (HERE/'workspace_replay_tail.c').read_text())
        with runner(root,'linux-container',source) as (command,scope):
            for mode,workspace,cases in modes:
                for case,gate in cases:
                    argv=['unused','0',case]+([str(workspace)] if workspace else [])
                    if gate:argv.append(str(gate))
                    result=subprocess.run(command+argv,capture_output=True,timeout=15)
                    item={'allocation_mode':mode,'workspace_min_bytes':workspace or 0,
                          'case':case,'index_bound_seed':gate,'exit_code':result.returncode}
                    if result.stdout:item['trace']=json.loads(result.stdout)
                    records.append(item)
                    print(json.dumps(item),flush=True)
    (args.out/'results.json').write_text(json.dumps(records,indent=2)+'\n')
    (args.out/'scope.json').write_text(json.dumps({
        'runtime':scope,'ta_sha256':TA_PIN,'device_actions':False,
        'mocked':['log configuration','logs','4e24 message setup','9280 FocalTech initialization',
                  '8cf0 file lookup returns missing','98888 algorithm setting',
                  'b8300/c3204/c3208 state reset and logging helpers (clear stage only)'],
        'stop_at':{'entry':'9a94 enrol entry; never execute algorithm',
                   'clear':'9b57c enrol algorithm gate; original 9a94 clear runs first'},
        'allocator':'original d63c shim; qsee_realloc(NULL,size) replaced by tracked host allocation, '
                    'or by a guarded mmap granting max(requested,workspace_min) when enlarged',
        'enrol_workspace':{'base_global':'0x2a4638','offset':ENROL_WORKSPACE_OFFSET,
                           'clear_bytes':ENROL_CLEAR_BYTES,'high_water':ENROL_HIGH_WATER},
        'clear_observation':'guarded workspace pre-filled with 0xa5; original d350 memset must zero '
                            'exactly the 666624-byte window and leave the bytes outside untouched',
        'index_gate':{'va':'0x2261b8','static_value':index_bound,
                      'gate_case':'seed the real static bound so original 9a94 calls 9b57c; '
                                  'the stop hook records entry arguments, no algorithm byte runs'},
        'scope_limit':'constructed session with boundary mocks; without the -gate seed the map leaves '
                      'unk_2261B0 zero so the index gate stays closed'
    },indent=2)+'\n')


if __name__=='__main__':main()
