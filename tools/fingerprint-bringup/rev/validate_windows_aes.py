#!/usr/bin/env python3
"""Compare original ARM64 CBC against AES on synthetic or optional private data.

macOS arm64 only. No DLL entry point or device access. Proprietary key/IV bytes
are read locally, never printed or written into source/report files.
"""
import hashlib
import argparse
import os
from pathlib import Path
import platform
import struct
import subprocess
import tempfile

from Crypto.Cipher import AES
from pe import pe, off_of

DLL = Path(__file__).parent / 'cabs/ftwbio/FtWbioDriverUmdf.dll'
PIN = '90dfeb2c23523a382f9310c16e6ad5d4ed33247b7868a955521ce7b70a710fb0'

C_SOURCE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
extern void call_windows_decoder(void *, void *, unsigned, void *);
static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END) || (*size = (size_t)ftell(f)) > 0x1000000 ||
        fseek(f, 0, SEEK_SET)) exit(20);
    unsigned char *p = malloc(*size);
    if (!p || fread(p, 1, *size, f) != *size) exit(21);
    fclose(f); return p;
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    umask(0077);
    size_t image_size, size;
    unsigned char *image = read_file(argv[1], &image_size);
    unsigned char *data = read_file(argv[2], &size);
    if (image_size != 0x58000 || !size || size % 16 || size > 0x100000) return 3;
    void *map = mmap(NULL, image_size, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANON, -1, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 4; }
    __builtin_memcpy(map, image, image_size);
    sys_icache_invalidate(map, image_size);
    /* Only the first 16 KiB page contains reviewed instructions. All tables
       and data pages are read-only, non-executable. Uncopied code is zero. */
    if (mprotect(map, image_size, PROT_READ) ||
        mprotect(map, 0x4000, PROT_READ|PROT_EXEC)) { perror("mprotect"); return 6; }
    /* The original routine is in-place in GetImage. No imports on valid
       buffers; its local stack-cookie helpers remain in the mapped image. */
    call_windows_decoder(data, data, (unsigned)size, (char *)map + 0x1c10);
    FILE *out = fopen(argv[3], "wb");
    if (!out || fwrite(data, 1, size, out) != size || fclose(out)) return 5;
    munmap(map, image_size); free(data); free(image); return 0;
}
'''
ASM_SOURCE = r'''
.text
.globl _call_windows_decoder
.p2align 2
_call_windows_decoder:
    stp x29, x30, [sp, #-32]!
    str x18, [sp, #16]
    mov x29, sp
    blr x3
    ldr x18, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-payload', type=Path,
                        help='optional fixed 15680-byte private capture; only compare, never print bytes')
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise SystemExit('requires a macOS arm64 host; no emulated claims')
    data, base, sections = pe(DLL)
    if hashlib.sha256(data).hexdigest() != PIN:
        raise SystemExit('DLL SHA mismatch')
    key_offset, _ = off_of(sections, base + 0x24060)
    iv_offset, _ = off_of(sections, base + 0x24070)
    key, iv = data[key_offset:key_offset + 16], data[iv_offset:iv_offset + 16]
    image = bytearray(0x58000)
    # Only CBC, AES key/block transforms, two local stack-cookie helpers and
    # referenced table/data ranges. No PE entry point or IAT is copied.
    ranges = ((0x14a0, 0x14b8), (0x14c0, 0x14e0), (0x1c10, 0x2720),
              (0x19000, 0x1c000), (0x24038, 0x24040), (0x24060, 0x24080))
    for start, end in ranges:
        offset, _ = off_of(sections, base + start)
        last, _ = off_of(sections, base + end - 1)
        if offset is None or last != offset + end - start - 1:
            raise SystemExit('reviewed range is not contiguous in pinned DLL')
        image[start:end] = data[offset:offset + end - start]
    with tempfile.TemporaryDirectory(prefix='fp-windows-aes-') as tmp:
        p = Path(tmp)
        (p / 'runner.c').write_text(C_SOURCE)
        (p / 'wrapper.S').write_text(ASM_SOURCE)
        (p / 'mapped-image.bin').write_bytes(image)
        subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', str(p / 'runner.c'),
                        str(p / 'wrapper.S'), '-o', str(p / 'runner')], check=True)
        for size in (16, 32, 256, 15680):
            plaintext = bytes((i * 29 + i // 7 + 3) & 255 for i in range(size))
            ciphertext = AES.new(key, AES.MODE_CBC, iv).encrypt(plaintext)
            (p / 'ciphertext.bin').write_bytes(ciphertext)
            subprocess.run([str(p / 'runner'), str(p / 'mapped-image.bin'),
                            str(p / 'ciphertext.bin'), str(p / 'decoded.bin')],
                           check=True, timeout=10)
            actual = (p / 'decoded.bin').read_bytes()
            if actual != plaintext:
                raise SystemExit(f'FAIL: original DLL decoder differs at length {size}')
            print(f'PASS: original DLL decoder == AES-128-CBC, synthetic {size} bytes')
        if args.private_payload:
            captured = args.private_payload.read_bytes()
            if len(captured) != 15680:
                raise SystemExit('private capture must be exactly 15680 bytes')
            expected = AES.new(key, AES.MODE_CBC, iv).decrypt(captured)
            cipher_path = p / 'private-cipher.bin'
            fd = os.open(cipher_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'wb') as file:
                file.write(captured)
            subprocess.run([str(p / 'runner'), str(p / 'mapped-image.bin'),
                            str(cipher_path), str(p / 'private-decoded.bin')],
                           check=True, timeout=10)
            if (p / 'private-decoded.bin').read_bytes() != expected:
                raise SystemExit('FAIL: original DLL and AES differ on private capture')
            print('PASS: private capture original DLL output exactly matches AES-128-CBC; no bytes disclosed')


if __name__ == '__main__':
    main()
