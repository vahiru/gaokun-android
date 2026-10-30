#!/usr/bin/env python3
"""Execute actual OEM event converter/process with mocked sensor/callback boundaries.

No devices or production ELF edits. Only reviewed code ranges are executable;
raw events are synthetic. The full Fusion estimator is NOT mocked as passing:
this test establishes status gating, unchanged delivery and timestamp behavior.
"""
import argparse
import importlib.util
from pathlib import Path
import platform
import subprocess
import tempfile

spec=importlib.util.spec_from_file_location('gyro_patch',Path(__file__).with_name('patch-fusion-gyro-status-guard.py'))
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)

C=r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
typedef void (*process_fn)(void *,const void *);
typedef void (*convert_fn)(const void *,void *);
static int called;
static float last_dt,last_vec[3];
static int sensor_type(void *ignored){(void)ignored;return 4;}
static int sensor_handle(void *ignored){(void)ignored;return 2;}
static void handle_gyro(void *fusion,const float *v,float dt){
    assert(fusion);++called;last_dt=dt;memcpy(last_vec,v,12);
}
static void hook(unsigned char *map,unsigned at,void *target){
    uint32_t code[]={0x58000050,0xd61f0200}; /* ldr x16,+8; br x16 */
    memcpy(map+at,code,8);memcpy(map+at+8,&target,8);
}
static void *load(const char *file){
    FILE *f=fopen(file,"rb");assert(f);
    unsigned char *map=mmap(NULL,0x90000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(map!=MAP_FAILED && fread(map,1,0x90000,f)==0x90000);fclose(f);
    hook(map,0x7d420,sensor_type);hook(map,0x7d438,sensor_handle);hook(map,0x7d510,handle_gyro);
    sys_icache_invalidate(map,0x90000);
    assert(!mprotect(map,0x90000,PROT_READ));
    assert(!mprotect(map+0x50000,0x4000,PROT_READ|PROT_EXEC));
    assert(!mprotect(map+0x68000,0x4000,PROT_READ|PROT_EXEC));
    assert(!mprotect(map+0x7c000,0x8000,PROT_READ|PROT_EXEC));
    return map;
}
static void put64(unsigned char *p,int64_t value){memcpy(p,&value,8);}
static int64_t get64(const unsigned char *p){int64_t value;memcpy(&value,p,8);return value;}
static void object(unsigned char *p){
    float rate=200;memset(p,0,0x920);p[0x160]=1;p[0x829]=1;
    put64(p+0x8f0,1000000000);memcpy(p+0x8e0,&rate,4);
}
static void event(convert_fn convert,unsigned char *out,int status,const float *v,int64_t ts){
    unsigned char aidl[64]={0};int handle=2,type=4;
    put64(aidl,ts);memcpy(aidl+8,&handle,4);memcpy(aidl+12,&type,4);
    memcpy(aidl+0x18,v,12);memcpy(aidl+0x24,&status,4);
    memset(out,0xa5,104);convert(aidl,out);
    assert(*(uint32_t *)out==104 && *(uint32_t *)(out+8)==4);
    assert(get64(out+16)==ts && !memcmp(out+24,v,12));
    assert((int8_t)out[36]==status); /* Actual OEM converter establishes ABI. */
}
static void process_unchanged(process_fn process,unsigned char *obj,unsigned char *e){
    unsigned char before[104];memcpy(before,e,104);process(obj,e);assert(!memcmp(before,e,104));
}
int main(int argc,char **argv){
    assert(argc==3);unsigned char *base=load(argv[1]),*patched=load(argv[2]);
    process_fn old=(process_fn)(base+0x684c4),guarded=(process_fn)(patched+0x684c4);
    convert_fn convert=(convert_fn)(base+0x50d10);
    _Alignas(16) unsigned char a[0x920],b[0x920],e[104];
    const float extreme[3]={-34.906586f,-34.906586f,-34.906586f};
    const float normal[3]={-.002f,0,-.024f},zero[3]={0,0,0};
    object(a);event(convert,e,0,extreme,1020000000);called=0;
    process_unchanged(old,a,e);assert(called==1 && !memcmp(last_vec,extreme,12));
    object(b);called=0;process_unchanged(guarded,b,e);
    assert(called==0 && get64(b+0x8f0)==1020000000);
    event(convert,e,0,extreme,1040000000);process_unchanged(guarded,b,e);
    assert(called==0 && get64(b+0x8f0)==1040000000);
    event(convert,e,3,normal,1060000000);process_unchanged(guarded,b,e);
    assert(called==1 && fabsf(last_dt-.02f)<1e-7f && !memcmp(last_vec,normal,12));
    for(int status=1;status<=3;++status)for(int v=0;v<2;++v){
        object(a);object(b);event(convert,e,status,v?zero:extreme,1020000000);
        called=0;process_unchanged(old,a,e);assert(called==1);
        float dt=last_dt,oldvec[3];memcpy(oldvec,last_vec,12);
        called=0;process_unchanged(guarded,b,e);
        assert(called==1 && dt==last_dt && !memcmp(oldvec,last_vec,12) && !memcmp(a,b,sizeof(a)));
    }
    object(b);b[0x828]=1;b[0x82a]=1;called=0;
    event(convert,e,0,normal,1020000000);process_unchanged(guarded,b,e);
    assert(called==0 && get64(b+0x8f0)==1020000000);
    event(convert,e,3,zero,1040000000);process_unchanged(guarded,b,e);
    assert(called==3 && fabsf(last_dt-.02f)<1e-7f);
    object(b);called=0;event(convert,e,0,extreme,1200000000);process_unchanged(guarded,b,e);
    event(convert,e,3,normal,1220000000);process_unchanged(guarded,b,e);
    assert(called==1 && fabsf(last_dt-.02f)<1e-7f);
    object(b);called=0;event(convert,e,3,normal,1100000000);process_unchanged(guarded,b,e);
    assert(called==0 && get64(b+0x8f0)==1100000000); /* Existing dt limit unchanged. */
    puts("PASS: actual OEM converter status offset0x24; original consumes accuracy0; candidate skips fusion only, advances time, preserves raw events and all reliable extreme/zero samples");
    munmap(base,0x90000);munmap(patched,0x90000);
}
'''


def main():
    a=argparse.ArgumentParser(description=__doc__)
    a.add_argument('library',type=Path);a.add_argument('--candidate',type=Path,required=True)
    args=a.parse_args()
    if platform.system()!='Darwin' or platform.machine()!='arm64':a.error('requires native macOS arm64')
    original=args.library.read_bytes();expected=p.patch(original);candidate=args.candidate.read_bytes()
    if candidate!=expected:a.error('candidate differs from reviewed R21-pinned patch')
    with tempfile.TemporaryDirectory(prefix='gyro-status-study-') as tmp:
        root=Path(tmp)
        for name,data in [('base',original),('candidate',candidate)]:
            image=bytearray(0x90000)
            for lo,hi in ((0x23e30,0x28bb1),(0x50d10,0x50df8),(0x684c4,0x687b0),
                          (p.GUARD,p.GUARD+p.SIZE)):
                image[lo:hi]=data[lo:hi]
            (root/name).write_bytes(image)
        (root/'runner.c').write_text(C)
        subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',str(root/'runner.c'),
                        '-o',str(root/'runner')],check=True)
        subprocess.run([str(root/'runner'),str(root/'base'),str(root/'candidate')],check=True,timeout=10)
        print('Boundary mocks: sensor type/handle getters and gyro-consumption recorder only; no hardware or complete estimator validation')


if __name__=='__main__':main()
