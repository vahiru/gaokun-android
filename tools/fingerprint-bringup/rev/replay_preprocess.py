#!/usr/bin/env python3
"""Offline original TA/Windows preprocess replay; no commands, pixels or key output."""
import argparse
import contextlib
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess
import tempfile
import os
import uuid

from pe import pe, off_of

HERE=Path(__file__).parent
TA_PIN='b081543c7b6ae4360969007836747601e8c48ec48a8335a5479e45ec0e34355a'
DLL_PIN='90dfeb2c23523a382f9310c16e6ad5d4ed33247b7868a955521ce7b70a710fb0'


@contextlib.contextmanager
def runner(root, runtime, source=None):
    source = Path(source) if source else HERE/'preprocess_replay_runner.c'
    if runtime == 'macos-experimental':
        subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',
                        str(source),str(HERE/'preprocess_replay_bridge.S'),
                        '-o',str(root/'runner')],check=True)
        yield ([str(root/'runner'),str(root/'ta.map'),str(root/'windows.map')],
               {'runtime':runtime,'unreliable_for_ta_x18':True})
        return
    image='gk3-flutter-base:e7b94444ab16'
    identity=subprocess.check_output(['docker','image','inspect','--format','{{.Id}} {{.Architecture}}',image],text=True).strip()
    if not identity.endswith(' arm64'):
        raise ValueError('reviewed existing image must be arm64; never pull an image')
    name='gaokun3-fp-preprocess-'+str(os.getpid())+'-'+uuid.uuid4().hex[:8]
    command=['docker','run','--pull=never','--rm','-d','--name',name,'--network','none',
             '--read-only','--tmpfs','/build:rw,exec,size=32m','-e','TMPDIR=/build',
             '--mount',f'type=bind,src={root.resolve()},dst=/maps,readonly',
             '--mount',f'type=bind,src={source.resolve()},dst=/runner.c,readonly',
             '--mount',f'type=bind,src={(HERE/"preprocess_replay_bridge_linux.S").resolve()},dst=/bridge.S,readonly',
             '--entrypoint','/bin/sh',image,'-c','exec sleep infinity']
    container=subprocess.check_output(command,text=True).strip()
    try:
        subprocess.run(['docker','exec',container,'clang','-std=c11','-Wall','-Wextra','-Werror',
                        '/runner.c','/bridge.S','-o','/build/runner'],check=True,timeout=30)
        kernel=subprocess.check_output(['docker','exec',container,'uname','-srmo'],text=True).strip()
        yield (['docker','exec','-i',container,'timeout','10s','/build/runner',
                '/maps/ta.map','/maps/windows.map'],
               {'runtime':'existing-arm64-linux-container','image':identity,'kernel':kernel,
                'network':'none','rootfs':'read-only','build_storage':'32MiB tmpfs','container_name':name})
    finally:
        # Only the exact container created above; never touch other containers/volumes.
        subprocess.run(['docker','rm','-f',container],capture_output=True)


def maps():
    ta=(HERE/'cabs/qctree/fingerpr.mbn').read_bytes()
    win,base,sections=pe(HERE/'cabs/ftwbio/FtWbioDriverUmdf.dll')
    if hashlib.sha256(ta).hexdigest()!=TA_PIN or hashlib.sha256(win).hexdigest()!=DLL_PIN:
        raise ValueError('unreviewed binary hash')
    a=bytearray(0x1e4000)
    for lo,hi in ((0x1c68,0x29c8),(0x1e1ad0,0x1e1ae0)):
        a[lo:hi]=ta[lo+0x3000:hi+0x3000]
    b=bytearray(0x28000)
    for lo,hi in ((0x14a0,0x14b8),(0x14c0,0x14e0),(0x6850,0x76e8),
                  (0x1d000,0x1e000),(0x24038,0x24040)):
        offset,_=off_of(sections,base+lo)
        b[lo:hi]=win[offset:offset+hi-lo]
    return a,b


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True,help='new cache report directory')
    p.add_argument('--private-encoded',type=Path,help='optional15680-byte encrypted capture; plaintext stays in memory/pipe')
    p.add_argument('--runtime',choices=('linux-container','macos-experimental'),default='linux-container')
    args=p.parse_args()
    if platform.system()!='Darwin' or platform.machine()!='arm64':p.error('requires macOS arm64')
    args.out.mkdir(parents=True,exist_ok=False)
    ta,win=maps()
    cases={
        'constant': [1000]*7840,
        'gradient': [500+(i*17)%3000 for i in range(7840)],
        'boundary': [0 if i%2 else 4096 for i in range(7840)],
        '101_overrange': [5000]*101+[500+(i*17)%3000 for i in range(7739)],
    }
    records=[]
    with tempfile.TemporaryDirectory(prefix='mapped-',dir=args.out) as tmp:
        root=Path(tmp)
        (root/'ta.map').write_bytes(ta);(root/'windows.map').write_bytes(win)
        with runner(root,args.runtime) as (run_command,runtime_scope):
          def run(name,plain,algorithm='both',fail=0):
            item={'case':name,'algorithm':algorithm,'failure_injection':fail}
            try:
                r=subprocess.run(run_command+[algorithm,str(fail)],input=plain,capture_output=True,timeout=15)
                item['exit_code']=r.returncode
                if r.returncode==0:item['result']=json.loads(r.stdout)
                else:
                    item['runner_failed']=True
                    if r.stdout:
                        try:item['fault_location']=json.loads(r.stdout)
                        except (ValueError,UnicodeDecodeError):pass
                    item['fault_recorded']='fault_location' in item
            except subprocess.TimeoutExpired:
                item['timeout']=True
            records.append(item)
            (args.out/'results.json').write_text(json.dumps(records,indent=2)+'\n')
            print(json.dumps(item),flush=True)
          for name,values in cases.items():
            run(name,struct.pack('<7840H',*values))
          gradient=struct.pack('<7840H',*cases['gradient'])
          for algorithm in ('ta','windows'):
            for fail in range(1,7):run('gradient-allocation-failure',gradient,algorithm,fail)
          if args.private_encoded:
            # Existing local decoder verifies the same reviewed DLL hash.
            # Neither the input plaintext nor either output is written to disk.
            from decode_sc_image import decode
            plain,_=decode(args.private_encoded.read_bytes(),HERE/'cabs/ftwbio/FtWbioDriverUmdf.dll')
            run('private-contact',plain)
    (args.out/'scope.json').write_text(json.dumps({
        'ta_sha256':TA_PIN,'dll_sha256':DLL_PIN,'runtime':runtime_scope,
        'executed_original_algorithms':['TA1c68..29c7','Windows6850..76e7'],
        'boundary_mocks':['malloc/free with failure injection and redzones','memcpy/memset','logs/log-only path lookup'],
        'original_runtime_helpers':['Windows14a0/14c0 stack cookie'],
        'platform_x18_restored_at_each_host_boundary':True,
        'pixel_or_key_output':False,'plaintext_persisted':False,'device_actions':False,
        'interpretation':'faults and differences are findings, not a claim every algorithm passed'
    },indent=2)+'\n')


if __name__=='__main__':main()
