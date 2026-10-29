#!/usr/bin/env python3
"""R9 fixed-device HyperOS streaming installer; preview by default.

Run in the verified gaokun3 rescue system. --apply reads exactly 12 GiB from
stdin and replaces only the UUID-bound experimental partitions after GPT and
asset checks. Never boot after an exception; stream-started.json blocks retries.
Paths and expected super hash bind this to the R9 experiment, not a general flasher.
"""
import hashlib,importlib.util,json,os,shutil,stat,struct,subprocess,sys,uuid
from pathlib import Path
base=Path('/home/user/hos-r6/install-bundle')
root=Path('/home/user/hos-r9-hyperos')
plan=json.loads((base/'partition-plan.json').read_text())
spec=importlib.util.spec_from_file_location('installer',base/'install-baseline.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
assert os.geteuid()==0
assert hashlib.sha256((base/'partition-plan.json').read_bytes()).hexdigest()=='0adc8eb05714e651118c9a28de8c76aecea8b3d8b024c7b5a0b26a2360a678de'
_,_,header,entries=mod.read_gpt(Path('/dev/nvme0n1'),1000215216)
assert header['guid']==plan['disk_guid']
old=(base/'installation-backup/primary.bin').read_bytes()[1024:]
for item in plan['existing_partitions_preserved']:
 n=item['number'];assert entries[n]==old[(n-1)*128:n*128]
assert set(entries)=={i['number'] for i in plan['existing_partitions_preserved']+plan['new_partitions']}
devices={}
for item in plan['new_partitions']:
 n=item['number'];e=entries[n]
 assert str(uuid.UUID(bytes_le=e[16:32]))==item['guid']
 assert e[56:128].decode('utf-16-le').split('\0')[0]==item['name']
 assert struct.unpack_from('<QQ',e,32)==(item['start_sector'],item['end_sector'])
 node=Path('/dev/disk/by-partuuid')/item['guid']
 assert node.resolve()==Path('/dev/nvme0n1p'+str(n)) and stat.S_ISBLK(node.stat().st_mode)
 assert int(subprocess.check_output(['blockdev','--getsize64',str(node)]))==item['size']
 assert subprocess.run(['findmnt','-rn','--source',str(node)],stdout=subprocess.DEVNULL).returncode==1
 devices[item['name']]=node
esp=Path('/boot/efi');pkg=root/'esp-package'
assert subprocess.check_output(['findmnt','-nro','SOURCE','--target',str(esp)],text=True).strip()=='/dev/nvme0n1p1'
manifest=json.loads((pkg/'manifest.json').read_text())
assert set(manifest)=={'gaokun3-hos/r9-hyperos/ramdisk.img','loader/entries/gaokun3-hos-hyperos-r9.conf'}
for name,item in manifest.items():
 assert not (esp/name).exists()
 assert (pkg/name).stat().st_size==item['size'] and mod.sha(pkg/name)==item['sha256']
assert shutil.disk_usage(esp).free>sum(v['size'] for v in manifest.values())+1024*1024
assert not (root/'stream-started.json').exists()
if '--apply' not in sys.argv:
 print('PREVIEW PASS: original GPT entries intact, isolated targets verified, ESP assets verified');sys.exit(0)
(root/'stream-started.json').write_text(json.dumps({'expected_sha256':'1bfa8cdc00ec1dc617cfaf03ad42bc85a01db456086ceac5d616389d8c287755','size':12884901888})+'\n')
with devices['misc_hos'].open('rb') as f:(root/'misc-before-hyperos.img').write_bytes(f.read())
expected='1bfa8cdc00ec1dc617cfaf03ad42bc85a01db456086ceac5d616389d8c287755';total=0;h=hashlib.sha256()
print('STREAM READY',flush=True)
with devices['super_hos'].open('r+b',buffering=0) as f:
 while total<12884901888:
  b=sys.stdin.buffer.read(min(4*1024*1024,12884901888-total))
  if not b:raise RuntimeError('truncated stream; do not boot')
  h.update(b);view=memoryview(b)
  while view:
   n=f.write(view)
   if not n:raise RuntimeError('short write')
   view=view[n:]
  total+=len(b)
  if total%(1024**3)==0:print(f'written {total//1024**3} GiB',flush=True)
 assert sys.stdin.buffer.read(1)==b''
 os.fsync(f.fileno())
assert h.hexdigest()==expected,'stream hash mismatch'
assert mod.sha(devices['super_hos'])==expected,'disk readback mismatch'
print('SUPER READBACK VERIFIED',flush=True)
for name in ['metadata_hos','userdata_hos']:
 subprocess.run(['mkfs.ext4','-F','-b','4096','-L',name,'-E','lazy_itable_init=0,lazy_journal_init=0',str(devices[name])],check=True)
 subprocess.run(['e2fsck','-f','-n',str(devices[name])],check=True)
with devices['misc_hos'].open('r+b',buffering=0) as f:
 f.write(bytes(4*1024*1024));os.fsync(f.fileno())
for name in sorted(manifest,key=lambda s:s.endswith('.conf')):
 target=esp/name;target.parent.mkdir(parents=True,exist_ok=True)
 with target.open('xb') as f:f.write((pkg/name).read_bytes());f.flush();os.fsync(f.fileno())
 assert mod.sha(target)==manifest[name]['sha256']
os.sync()
(root/'installed.json').write_text(json.dumps({'super_sha256':expected,'installed':True,'boot_tested':False,'default_changed':False},indent=2)+'\n')
print('HYPEROS INSTALL VERIFIED; no reboot or oneshot change',flush=True)
