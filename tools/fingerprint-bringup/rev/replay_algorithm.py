#!/usr/bin/env python3
"""Offline full-image replay of the original TA enrol path.

Loads every PT_LOAD segment of fingerpr.mbn, applies the RELATIVE relocations,
binds the dynamic imports to host shims (qsee_* SFS/SPI/log/alloc plus the
GPAppLib/cmnlib entry points), then runs the original tz_app_init, builds the
124-byte sensor config that focal_SensorResourceCreate would allocate, calls
focal_CreateFocalImgeBufInstance (0x9746c) and finally the enrol function
0x9b57c on a synthetic 8-bit image.

No device, no SPI/SFS, no vendor template and no real image are used.  SFS and
SPI imports fail deterministically so the original error paths run instead.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from replay_preprocess import HERE, TA_PIN, runner
from ta_image_relocs import parse, R_RELATIVE, R_ABS64, R_GLOB_DAT, R_JUMP_SLOT

MAP_SIZE = 0x37d000
ENROL_CONFIG_BYTES = 124
IMP = {'ok':0,'malloc':1,'realloc':2,'free':3,'log':4,'sfs':5,'spi':6,'fatal':7}
ABS = {'app_heap_base':0,'app_heap_limit':1,'app_stack_base':2,'app_stack_limit':3}


def import_class(name):
    if name=='qsee_malloc':return IMP['malloc']
    if name=='qsee_realloc':return IMP['realloc']
    if name=='qsee_free':return IMP['free']
    if name in ('qsee_log','qsee_printf'):return IMP['log']
    if name=='qsee_err_fatal':return IMP['fatal']
    if name.startswith('qsee_sfs_'):
        return IMP['ok'] if name in ('qsee_sfs_is_anti_rollback_enabled','qsee_sfs_error') else IMP['sfs']
    if name.startswith('qsee_spi_'):return IMP['spi']
    return IMP['ok']


def loader_source(blob):
    loads,relocations=parse(blob)
    relative=[r for r in relocations if r['kind']==R_RELATIVE]
    imported=[r for r in relocations if r['kind'] in (R_JUMP_SLOT,R_GLOB_DAT)]
    absolute=[r for r in relocations if r['kind']==R_ABS64]
    names=[];bindings=[]
    for r in imported:
        name=r['name']
        if name not in names:names.append(name)
        bindings.append((r['target'],names.index(name)))
    abs_bindings=[]
    for r in absolute:
        if r['name'] not in ABS:raise ValueError(f'unhandled ABS64 symbol {r["name"]}')
        abs_bindings.append((r['target'],ABS[r['name']]))
    lines=['/* generated loader data */']
    for i in range(len(names)):
        lines.append('static uint64_t imp_%d(uint64_t a,uint64_t b,uint64_t c,uint64_t d){'
                     'return import_hit(%d,a,b,c,d);}'%(i,i))
    lines.append('static const char *const import_names[]={%s};'%(
        ','.join(json.dumps(n) for n in names)))
    lines.append('static const unsigned char import_class[]={%s};'%(
        ','.join(str(import_class(n)) for n in names)))
    lines.append('static const unsigned import_count=%d;'%len(names))
    lines.append('static const import_binding_t import_bindings[]={%s};'%(
        ','.join('{0x%x,(uintptr_t)imp_%d}'%(slot,idx) for slot,idx in bindings)))
    lines.append('static const relative_reloc_t relative_relocs[]={%s};'%(
        ','.join('{0x%x,0,0x%x}'%(r['target'],r['addend']) for r in relative)))
    lines.append('static const unsigned relative_reloc_count=%d;'%len(relative))
    lines.append('static const abs_binding_t abs_bindings[]={%s};'%(
        ','.join('{0x%x,%d,0}'%(slot,code) for slot,code in abs_bindings)))
    lines.append('static const unsigned abs_count=%d;'%len(abs_bindings))
    image=bytearray(MAP_SIZE)
    for seg in loads:
        start,end=seg['vaddr'],seg['vaddr']+seg['filesz']
        if end>MAP_SIZE:raise ValueError('segment exceeds map size')
        image[start:end]=blob[seg['offset']:seg['offset']+seg['filesz']]
    return image,'\n'.join(lines)+'\n',{
        'load_segments':len(loads),'relative_relocations':len(relative),
        'import_bindings':len(bindings),'import_names':len(names),
        'absolute_bindings':len(abs_bindings)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--private-encoded',type=Path,
                        help='optional 15680-byte encoded contact frame; decoded '
                             'locally and fed to the real-image enrol case')
    args=parser.parse_args()
    real_plain=None
    if args.private_encoded:
        try:
            from decode_sc_image import decode
        except ModuleNotFoundError as exc:
            raise SystemExit('--private-encoded needs PyCryptodome (Crypto.Cipher); '
                             'in this workspace run with /usr/bin/python3') from exc
        real_plain,real_stats=decode(args.private_encoded.read_bytes(),
                                     HERE/'cabs/ftwbio/FtWbioDriverUmdf.dll')
        assert len(real_plain)==15680
        print(json.dumps({'real_frame':str(args.private_encoded),
                          'decoded_bytes':len(real_plain),
                          'stats':real_stats}),flush=True)
    blob=(HERE/'cabs/qctree/fingerpr.mbn').read_bytes()
    assert hashlib.sha256(blob).hexdigest()==TA_PIN
    args.out.mkdir(parents=True,exist_ok=False)
    image,data,counts=loader_source(blob)
    tail=(HERE/'algorithm_replay_tail.c').read_text()
    assert '/*__LOADER_DATA__*/' in tail
    tail=tail.replace('/*__LOADER_DATA__*/',data)
    store=HERE.parent/'host-store'
    store_hdr=(store/'tpl_store.h').read_text()
    store_impl=(store/'tpl_store.c').read_text().replace('#include "tpl_store.h"\n','')
    records=[]
    with tempfile.TemporaryDirectory(prefix='mapped-',dir=args.out) as temporary:
        root=Path(temporary)
        (root/'ta.map').write_bytes(image)
        (root/'windows.map').write_bytes(b'')
        source=root/'algorithm.c'
        source.write_text('#define main unused_preprocess_main\n'+
                          (HERE/'preprocess_replay_runner.c').read_text()+'\n'+
                          store_hdr+'\n'+store_impl+'\n'+tail)
        with runner(root,'linux-container',source) as (command,scope):
            cases=[('init',20),('config',30),
                            ('quality96',60),('enroll96:13:40',60),
                            ('loop96:13:40:0:1',120),
                            ('mutate96:13:40:0:0',180),
                            ('quality40x196dimsfix',60),('enroll40x196dimsfix:13:40',60),
                            ('loop40x196dimsfix:13:40:0:1',120),
                            ('quality196x40dimsfix',60),('enroll196x40dimsfix:13:40',60),
                            ('loop196x40dimsfix:13:40:0:1',120),
                            ('mutate196x40dimsfix:13:40:0:0',180),
                            ('mutate196x40dimsfix:13:40:2:0',180),
                            ('loop40x196dimsfix:13:40:0:0',120),
                            ('mutate40x196dimsfix:13:40:0:0',180),
                            ('mutate40x196dimsfix:13:40:2:0',180),
                            ('multi40x196dimsfix:13:40',180),
                            ('multipersist40x196dimsfix:13:40:0:1',180),
                            ('persistload40x196dimsfix:13:40:0:0',120),
                            ('multi196x40dimsfix:13:40',180),
                            ('multipersist196x40dimsfix:13:40:0:1',180),
                            ('persistload196x40dimsfix:13:40:0:0',120),
                            ('dualsave40x196dimsfix:13:40',180),
                            ('dualload40x196dimsfix:13:40:0:0',180),
                            ('bload40x196dimsfix:13:40:0:0',180),
                            ('storesave40x196dimsfix:13:40:0:0',180),
                            ('storeload40x196dimsfix:13:40:0:0',180),
                            ('storeload196x40dimsfix:13:40:0:0',120),
                            ('storesave196x40dimsfix:13:40:0:0',180),
                            ('storeload196x40dimsfix:13:40:0:0',180),
                            ('storesave96:13:40:0:0',180),
                            ('storeload96:13:40:0:0',180),
                            ('storesave40x196dimsfix:13:40:0:0:0',180),
                            ('storesave40x196dimsfix:4:0:0:0:1',180),
                            ('storeloadmulti40x196dimsfix:13:40:0:0',180),
                            ('persistsave40x196dimsfix:13:40',120),
                            ('persistload40x196dimsfix:13:40:0:1',120),
                            ('persisttamperpayload40x196dimsfix:13:40:0:0',120),
                            ('persisttampermagic40x196dimsfix:13:40:0:0',120),
                            ('persisttampercrc40x196dimsfix:13:40:0:0',120),
                            ('persisttampertrunc40x196dimsfix:13:40:0:0',120),
                            ('persistload40x196dimsfix:13:40:4:0',120),
                            ('persistload40x196dimsfix:4:0',120),
                            ('persistsave40x196dimsfix:13:40:0:0:1',120),
                            ('persistload40x196dimsfix:13:40:0:0:1',120),
                            ('persistload196x40dimsfix:13:40:0:0',120),
                            ('persistsave196x40dimsfix:13:40',120),
                            ('persistload196x40dimsfix:13:40:0:1',120),
                            ('persistsave96:13:40',120),
                            ('persistload96:13:40:0:0',120)]
            if real_plain is not None:
                cases.append(('realimg40x196dimsfix:13:40:0:0',180))
                cases.append(('realstoresave40x196dimsfix:13:40:0:0',180))
                cases.append(('realstoreload40x196dimsfix:13:40:0:0',180))
            for case,timeout in cases:
                try:
                    result=subprocess.run(command+['unused','0',case,'/build/tpl.bin'],
                                          capture_output=True,timeout=timeout,
                                          input=real_plain if case.startswith(('realimg','realstore'))
                                                else b'')
                    item={'case':case,'exit_code':result.returncode}
                    if result.stdout:
                        try:item['result']=json.loads(result.stdout)
                        except ValueError:item['stdout']=result.stdout.decode('utf-8','replace')[:2000]
                    if result.stderr:item['stderr']=result.stderr.decode('utf-8','replace')[:12000]
                except subprocess.TimeoutExpired:
                    item={'case':case,'timeout':True}
                records.append(item)
                print(json.dumps(item),flush=True)
    (args.out/'results.json').write_text(json.dumps(records,indent=2)+'\n')
    scope_data={
        'runtime':scope,'ta_sha256':TA_PIN,'device_actions':False,
        'map_bytes':MAP_SIZE,'counts':counts,
        'executed_original':{'app_init':'0xaccc','image_instance':'0x9746c',
                             'image_quality':'0xe6b64','enrol':'0x9b57c',
                             'template_export':'0xdfd20','template_import':'0xe0460',
                             'template_valid_flag':'0xe83b4'},
        'template_persistence':{
            'tpl_type':121,'magic':'0x1415927d','crc':'crc16 over [0, header_end)',
            'crc16':{'poly':'0x8005 reflected (0xa001)','init':0,'refin':True,
                     'refout':False,'xorout':0,'coverage':'[0, len-2)',
                     'storage':'little-endian u16 at [len-2, len)',
                     'observed':{'40x196':'0xb785','196x40':'0xcec0','96x96':'0x645a'}},
            'crc_offset':'header-derived end offset, not tplSize; a short buffer '
                         'makes the loader read the 2 CRC bytes past the declared length',
            'tamper_expectations':{'payload_flip':-3,'crc_flip':-3,'truncate2':-3,
                                   'magic_flip':-2},
            'tamper_note':'on -3 the TA has already populated the DB (count=1, flag=1) '
                          'and identify still matches, so the host must pre-validate '
                          'magic/geometry/crc and treat any non-zero load rc as fatal',
            'geometry_binding':'the 216-byte header is byte-identical for 40x196 '
                               'and 196x40; width/height and the 124-byte config are '
                               'not stored in the blob, so the host store must persist '
                               '{width,height,config-hash} alongside it',
            'identify_mutation':'default config: export hash/size identical before '
                                'and after a successful match (same image and 2 px '
                                'shift) for 40x196, 196x40 and 96x96',
            'host_store':{'module':'../host-store','container':'FPT1 + 64-byte header + blob',
                          'verified':['storesave/storeload 40x196, 196x40, 96x96',
                                      '40x196 container rejected when read as 196x40',
                                      'two-finger store restore: probes hit id 0 and 1, '
                                      'unknown pattern -2']},
            'observed_size_bytes':{'40x196':238970,'196x40':239938,'96x96':273962},
            'layout':'4 magic + 212 header + 96*(489 fixed + bin 984 + mask 981) + feat + 2 crc',
            'note':'import restores count=1 and db+50118=1 by itself; identify succeeds '
                   'in a fresh process with an identical image'},
        'research_only_case':'enroll_forced zeroes cfg quality/area thresholds; '
                             'not a real configuration',
        'config':'124-byte struct replicated from focal_SensorResourceCreate (0x955c4) fields',
        'host_boundaries':['qsee_malloc/realloc/free -> zeroed 128 MiB arena',
                           'qsee_sfs_* -> -1','qsee_spi_* -> -1',
                           'qsee_log/printf -> counted','qsee_err_fatal -> stop',
                           'GPAppLib/cmnlib entry points -> 0',
                           'app_heap_*/app_stack_* -> arena/stack marker'],
        'not_executed':['focal_SensorResourceCreate SPI path','real templates',
                        'real images','any 0x80xx wire command'],
        'template_abi':{'db_array':'off_361540','count_field':'db+50116',
                        'record_bytes':520,'data_offset':94},
        'note':'SFS/SPI errors make the original init fail closed; a non-zero return is a '
               'finding, not a harness success'
    }
    if real_plain is not None:
        scope_data['not_executed'].remove('real images')
        real={r['case'].split(':')[0]:r['result'] for r in records
              if r.get('case','').startswith(('realimg','realstore'))
              and 'result' in r}
        scope_data['real_image']={
            'source':'private encoded contact frame, decoded locally (AES key/iv '
                     'from FtWbioDriverUmdf.dll 0x24060/0x24070)',
            'encoded_bytes':15680,'decoded_bytes':len(real_plain),
            'decode_stats':real_stats,
            'cases':real,
            'note':'one real contact frame through original preprocess (0x1c68), '
                   'quality (0xe6b64), enrol (0x9b57c), export (0xdfd20) and '
                   'identify (0x9e1f0), plus host-store save/restore across two '
                   'processes; proves the offline algorithm and storage path accept '
                   'a real sensor frame, not yet the live SPI/SFS wire path'}
    (args.out/'scope.json').write_text(json.dumps(scope_data,indent=2)+'\n')


if __name__=='__main__':main()
