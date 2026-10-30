#!/usr/bin/env python3
"""Offline study of a guard around the actual pinned HyperOS Fusion function.

No on-device actions and no patched production ELF is written. Execute only
reviewed initialization routines in isolated native arm64 host subprocesses.
"""
import argparse
import hashlib
from pathlib import Path
import platform
import struct
import subprocess
import tempfile

PIN = 'a4ba29620bc241c8cc4ab07b9dee05fd3e29e170855a94f0bddbd8ab8594dcaf'
ENTRY = 0x567d8
GUARD = 0x809c0
ASM = r'''
.text
.global guard_start
guard_start:
    cmp w1, #1
    b.ne pass
    ldr d16, [x2]
    ldr s17, [x2, #8]
    fmul v16.2s, v16.2s, v16.2s
    faddp s18, v16.2s
    fmul s17, s17, s17
    fadd s18, s18, s17
    fmov w16, s18
    ubfx w16, w16, #23, #8
    cmp w16, #255
    b.eq reject
    fcmp s18, #0.0
    b.le reject
pass:
    ldr w10, [x0, #0x1f4]
    .inst 0x14000000
reject:
    mov w0, #0
    ret
'''
C = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
typedef void (*init_fn)(void *, int);
typedef int (*check_fn)(void *, int, const float *, float);
static void *load(const char *path) {
    FILE *f=fopen(path,"rb"); assert(f);
    void *p=mmap(NULL,0x90000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(p!=MAP_FAILED && fread(p,1,0x90000,f)==0x90000); fclose(f);
    sys_icache_invalidate(p,0x90000);
    assert(!mprotect(p,0x90000,PROT_READ));
    assert(!mprotect((char*)p+0x54000,0x4000,PROT_READ|PROT_EXEC));
    assert(!mprotect((char*)p+0x7c000,0x8000,PROT_READ|PROT_EXEC));
    return p;
}
static int finite_q(const unsigned char *obj) {
    const float *q=(const float *)obj;
    return isfinite(q[0])&&isfinite(q[1])&&isfinite(q[2])&&isfinite(q[3]);
}
int main(int argc,char **argv) {
    assert(argc==3);
    void *old=load(argv[1]),*guarded=load(argv[2]);
    init_fn init0=(init_fn)((char*)old+0x56644),init1=(init_fn)((char*)guarded+0x56644);
    check_fn check0=(check_fn)((char*)old+0x567d4),check1=(check_fn)((char*)guarded+0x567d4);
    _Alignas(16) unsigned char a[0x240],b[0x240],before[0x240];
    const float normal[3]={-.185f,-.123f,9.873f},gyro[3]={-.003f,-.0008f,-.024f};
    const float bad[][3]={{0,0,0},{NAN,0,9},{INFINITY,0,9},{-INFINITY,1,0},
                         {1e-30f,0,0},{1e30f,1,1}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        memset(b,0,sizeof(b));init1(b,1);memcpy(before,b,sizeof(b));
        assert(check1(b,1,bad[i],.02f)==0 && !memcmp(before,b,sizeof(b)));
        check1(b,4,gyro,.02f);check1(b,1,normal,.02f);
        assert(finite_q(b));
        memcpy(before,b,sizeof(b));
        assert(check1(b,1,bad[i],.02f)==0 && !memcmp(before,b,sizeof(b)));
    }
    /* Valid samples in all modes must preserve the exact original object. */
    for(int mode=0;mode<3;++mode) for(int what=1;what<=4;what*=2) {
        memset(a,0,sizeof(a));memset(b,0,sizeof(b));init0(a,mode);init1(b,mode);
        assert(check0(a,what,normal,.02f)==check1(b,what,normal,.02f));
        assert(!memcmp(a,b,sizeof(a)));
    }
    memset(a,0,sizeof(a));memset(b,0,sizeof(b));init0(a,1);init1(b,1);
    check0(a,1,normal,.02f);check1(b,1,normal,.02f);
    check0(a,4,gyro,.02f);check1(b,4,gyro,.02f);
    assert(finite_q(a) && finite_q(b) && !memcmp(a,b,sizeof(a)));
    memset(a,0,sizeof(a));init0(a,1);
    check0(a,1,bad[0],.02f);check0(a,4,gyro,.02f);
    assert(!finite_q(a));
    puts("PASS: actual OEM initializer reproduces zero-input NaN; guard preserves valid state byte-for-byte and rejects invalid accel without mutation");
    munmap(old,0x90000);munmap(guarded,0x90000);
}
'''


def sections(data):
    off = struct.unpack_from('<Q', data, 40)[0]
    size, count, names_index = struct.unpack_from('<HHH', data, 58)
    rows = [struct.unpack_from('<IIQQQQIIQQ', data, off+i*size) for i in range(count)]
    names = rows[names_index]
    strings = data[names[4]:names[4]+names[5]]
    return {strings[r[0]:strings.find(b'\0', r[0])].decode(): r for r in rows}


def branch(at, to):
    delta = to - at
    assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27)
    return struct.pack('<I', 0x14000000 | ((delta // 4) & 0x3ffffff))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('library', type=Path)
    ap.add_argument('--candidate', type=Path,
                    help='optional offline ELF candidate; verify exact permitted changes and execute its guard bytes')
    args = ap.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        ap.error('native validation requires macOS arm64')
    data = args.library.read_bytes()
    if hashlib.sha256(data).hexdigest() != PIN:
        ap.error('library hash mismatch')
    assert data[ENTRY:ENTRY+4] == bytes.fromhex('0af441b9')
    image = bytearray(0x90000)
    # No library constructors, imports, service code or unrelated instructions.
    for lo, hi in ((0x23e30,0x28bb1),(0x56644,0x56c44)):
        image[lo:hi] = data[lo:hi]
    # Resolve the two internal PLT calls in this isolated harness only.
    # No external imports are permitted or copied.
    for at, target in ((0x7d4c8,0x566b4),(0x7d4f8,0x56aec)):
        image[at:at+4] = branch(at,target)
    with tempfile.TemporaryDirectory(prefix='hos-fusion-guard-study-') as tmp:
        p = Path(tmp)
        (p/'guard.S').write_text(ASM)
        subprocess.run(['clang','-target','aarch64-linux-gnu','-c',str(p/'guard.S'),
                        '-o',str(p/'guard.o')],check=True)
        obj=(p/'guard.o').read_bytes(); text=sections(obj)['.text']
        guard=bytearray(obj[text[4]:text[4]+text[5]])
        pos=guard.find(struct.pack('<I',0x14000000))
        assert pos>=0 and guard.count(struct.pack('<I',0x14000000))==1
        guard[pos:pos+4]=branch(GUARD+pos,ENTRY+4)
        assert len(guard)<=0x80 and not any(data[GUARD:GUARD+len(guard)])
        patched=bytearray(image)
        patched[GUARD:GUARD+len(guard)]=guard
        patched[ENTRY:ENTRY+4]=branch(ENTRY,GUARD)
        if args.candidate:
            actual=args.candidate.read_bytes()
            expected=bytearray(data)
            expected[GUARD:GUARD+len(guard)]=guard
            expected[ENTRY:ENTRY+4]=branch(ENTRY,GUARD)
            struct.pack_into('<QQ',expected,0xd0,0x449c0+len(guard),0x449c0+len(guard))
            if actual != expected:
                raise SystemExit('candidate differs outside exact reviewed patch or contains a different guard')
            patched[ENTRY:ENTRY+4]=actual[ENTRY:ENTRY+4]
            patched[GUARD:GUARD+len(guard)]=actual[GUARD:GUARD+len(guard)]
        (p/'original-map').write_bytes(image);(p/'guard-map').write_bytes(patched)
        (p/'runner.c').write_text(C)
        subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',str(p/'runner.c'),
                        '-o',str(p/'runner')],check=True)
        subprocess.run([str(p/'runner'),str(p/'original-map'),str(p/'guard-map')],
                       check=True,timeout=10)
        print(f'Guard study only: {len(guard)} bytes, BTI retained, entry before PAC/stack; production ELF unchanged')


if __name__ == '__main__':
    main()
