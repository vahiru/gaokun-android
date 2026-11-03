/* SPDX-License-Identifier: GPL-2.0 */
/* Isolated host execution of reviewed algorithm bytes. No QSEE device IO. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <openssl/sha.h>

#define MAP_SIZE 0x37d000UL
#define HEAP_BYTES (128UL * 1024 * 1024)
typedef struct { uint32_t slot, pad; uint64_t addend; } relative_reloc_t;
typedef struct { uint64_t slot; uintptr_t fn; } import_binding_t;
typedef struct { uint64_t slot; uint32_t code, pad; } abs_binding_t;
enum { IMP_OK, IMP_MALLOC, IMP_REALLOC, IMP_FREE, IMP_LOG, IMP_SFS, IMP_SPI, IMP_FATAL };
enum { ABS_HEAP_BASE, ABS_HEAP_LIMIT, ABS_STACK_BASE, ABS_STACK_LIMIT };
static unsigned char *ta_map, *heap_arena, *heap_mapping;
static size_t heap_used, page_bytes;
static unsigned block_count, spi_calls, sfs_calls;
static unsigned char valid_fingers[6];
static struct { void *ptr; size_t size; } blocks[4096];
static void *arena_alloc(size_t n);
static uint64_t import_hit(unsigned idx, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
extern uint64_t call_algorithm(uint64_t, uint64_t, uint64_t, uint64_t, void*);
extern void engine_bridge_log(void), engine_bridge_zero(void);
extern void engine_bridge_failed(int);
/* Generated only from the pinned local TA; kept outside the repository. */
#include "engine-loader-data.h"

void engine_failed(int signal_number)
{
    static const char message[] = "Native algorithm boundary failed; no data exported\n";
    (void)signal_number;
    (void)write(2, message, sizeof(message) - 1);
    _Exit(73);
}
#define failed engine_failed
static void *arena_alloc(size_t n)
{
    if (!n) n = 1;
    if (n == 0xf8000) n = 16UL * 1024 * 1024;
    if (n > SIZE_MAX - 15) return NULL;
    n = (n + 15) & ~(size_t)15;
    if (!heap_arena || n > HEAP_BYTES - heap_used || block_count >= 4096) return NULL;
    void *ptr = heap_arena + heap_used;
    heap_used += n;
    memset(ptr, 0, n);
    blocks[block_count].ptr = ptr;
    blocks[block_count++].size = n;
    return ptr;
}
uint64_t engine_log(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    (void)a; (void)b; (void)c; (void)d;
    return 0; /* Never format algorithm logs containing scores or features. */
}
uint64_t engine_zero(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return engine_log(a, b, c, d);
}
static uint64_t import_hit(unsigned idx, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    (void)c; (void)d;
    if (idx >= import_count) failed(0);
    switch (import_class[idx]) {
    case IMP_MALLOC: return (uintptr_t)arena_alloc((size_t)a);
    case IMP_REALLOC: {
        void *ptr = arena_alloc((size_t)b);
        if (ptr && a) {
            size_t bytes = 0;
            for (unsigned i = 0; i < block_count; ++i)
                if (blocks[i].ptr == (void*)(uintptr_t)a) bytes = blocks[i].size;
            if (!bytes) failed(0);
            memcpy(ptr, (void*)(uintptr_t)a, bytes < b ? bytes : (size_t)b);
        }
        return (uintptr_t)ptr;
    }
    case IMP_FREE: return 0; /* Bounded monotonic diagnostic arena. */
    case IMP_LOG: return 0;
    case IMP_SFS: ++sfs_calls; return (uint64_t)-1;
    case IMP_SPI: ++spi_calls; return (uint64_t)-1;
    case IMP_FATAL: failed(0); return (uint64_t)-1;
    default: return 0;
    }
}
static uint64_t invoke(unsigned offset, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return call_algorithm(a, b, c, d, ta_map + offset);
}
static void hook(unsigned offset, void *fn)
{
    const uint32_t branch[] = { 0x58000050, 0xd61f0200 };
    memcpy(ta_map + offset, branch, sizeof(branch));
    memcpy(ta_map + offset + sizeof(branch), &fn, sizeof(fn));
}
void audit_engine_close(void)
{
    if (ta_map) munmap(ta_map, MAP_SIZE);
    if (heap_mapping) munmap(heap_mapping, HEAP_BYTES + 2 * page_bytes);
    ta_map = heap_arena = heap_mapping = NULL;
}
int audit_engine_open(unsigned width, unsigned height)
{
    const char *path = "/data/local/tmp/gaokun3-fp-template-audit-20261103/native-engine.map";
    unsigned char digest[32];
    struct rlimit limit = {0, 0};
    (void)import_names;
    if (ta_map || width * height != 7840 || setrlimit(RLIMIT_CORE, &limit)) return -1;
    signal(SIGSEGV, engine_bridge_failed); signal(SIGBUS, engine_bridge_failed);
    signal(SIGILL, engine_bridge_failed); signal(SIGABRT, engine_bridge_failed);
    signal(SIGFPE, engine_bridge_failed);
    heap_used = block_count = spi_calls = sfs_calls = 0;
    memset(valid_fingers, 0, sizeof(valid_fingers));
    ta_map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ta_map == MAP_FAILED) { ta_map = NULL; return -1; }
    FILE *file = fopen(path, "rb");
    if (!file) goto fail;
    size_t got = fread(ta_map, 1, MAP_SIZE, file);
    int extra = fgetc(file);
    fclose(file);
    if (got != MAP_SIZE || extra != EOF) goto fail;
    SHA256(ta_map, MAP_SIZE, digest);
    if (memcmp(digest, engine_pin, sizeof(digest))) goto fail;
    page_bytes = (size_t)sysconf(_SC_PAGESIZE);
    heap_mapping = mmap(NULL, HEAP_BYTES + 2 * page_bytes, PROT_NONE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (heap_mapping == MAP_FAILED) { heap_mapping = NULL; goto fail; }
    heap_arena = heap_mapping + page_bytes;
    if (mprotect(heap_arena, HEAP_BYTES, PROT_READ | PROT_WRITE)) goto fail;
    for (unsigned i = 0; i < relative_reloc_count; ++i)
        *(uintptr_t*)(ta_map + relative_relocs[i].slot) = (uintptr_t)ta_map + relative_relocs[i].addend;
    for (unsigned i = 0; i < sizeof(import_bindings)/sizeof(import_bindings[0]); ++i)
        *(uintptr_t*)(ta_map + import_bindings[i].slot) = import_bindings[i].fn;
    uintptr_t stack_marker = (uintptr_t)&stack_marker;
    for (unsigned i = 0; i < abs_count; ++i) {
        uintptr_t value = abs_bindings[i].code == ABS_HEAP_BASE ? (uintptr_t)heap_arena :
                          abs_bindings[i].code == ABS_HEAP_LIMIT ? (uintptr_t)heap_arena + HEAP_BYTES :
                          abs_bindings[i].code == ABS_STACK_BASE ? stack_marker : stack_marker - 0x10000;
        *(uintptr_t*)(ta_map + abs_bindings[i].slot) = value;
    }
    hook(0x4e24, engine_bridge_zero); hook(0x4ef0, engine_bridge_log);
    __builtin___clear_cache((char*)ta_map, (char*)ta_map + MAP_SIZE);
    if (mprotect(ta_map, 0x225000, PROT_READ | PROT_EXEC)) goto fail;
    /* tz_app_init's observed return register is 31, not a status ABI. */
    (void)invoke(0xaccc, 0, 0, 0, 0);
    uintptr_t s1 = *(uintptr_t*)(ta_map + 0x3614c8);
    uintptr_t s2 = *(uintptr_t*)(ta_map + 0x3614d0);
    uintptr_t s3 = *(uintptr_t*)(ta_map + 0x3614d8);
    if (s1) *(unsigned char*)s1 = 1;
    if (s2) *(unsigned*)s2 = 0;
    if (s3) *(uintptr_t*)s3 = 0;
    unsigned char *cfg = build_sensor_config();
    if (!cfg) goto fail;
    *(unsigned*)cfg = width; *(unsigned*)(cfg + 4) = height;
    *(uintptr_t*)(ta_map + 0x346368) = (uintptr_t)cfg;
    uintptr_t dims = *(uintptr_t*)(ta_map + 0x361530);
    if (!dims) goto fail;
    *(unsigned*)dims = width; *(unsigned*)(dims + 4) = height;
    if (invoke(0x9746c, 0, 0, 0, 0)) goto fail;
    return 0;
fail:
    audit_engine_close(); return -1;
}
static void guard(const unsigned char *image)
{
    for (unsigned i = 0; i < 64; ++i)
        if (image[i] != 0xc3 || image[64 + 7840 + i] != 0xc3) failed(0);
}
int audit_engine_add_sample(unsigned finger, unsigned sample, const unsigned char *pixels)
{
    unsigned char image[7840 + 128];
    if (!ta_map || finger >= 6 || sample >= 96) return -99;
    memset(image, 0xc3, sizeof(image)); memcpy(image + 64, pixels, 7840);
    int result = (int)invoke(0x9b57c, finger, sample, (uintptr_t)(image + 64), 0);
    guard(image);
    return result;
}
int audit_engine_finish(unsigned finger)
{
    if (!ta_map || finger >= 6) return -99;
    uintptr_t databases = *(uintptr_t*)(ta_map + 0x361540);
    uintptr_t database = databases ? ((uintptr_t*)databases)[finger] : 0;
    if (!database || !*(uint16_t*)(database + 50116)) return -98;
    (void)invoke(0xe83b4, finger, 0, 0, 0);
    if (!*(uint16_t*)(database + 50118)) return -98;
    valid_fingers[finger] = 1;
    return 0;
}
int audit_engine_enroll(unsigned finger, const unsigned char *pixels)
{
    int result = audit_engine_add_sample(finger, 0, pixels);
    return result ? result : audit_engine_finish(finger);
}
int audit_engine_identify(const unsigned char *pixels)
{
    unsigned char image[7840 + 128], id = 0xee, extra = 0xee;
    if (!ta_map) return -99;
    memset(image, 0xc3, sizeof(image)); memcpy(image + 64, pixels, 7840);
    int result = (int)invoke(0x9e1f0, (uintptr_t)(image + 64), 1, (uintptr_t)&id, (uintptr_t)&extra);
    guard(image);
    if (!result && (id >= 6 || !valid_fingers[id])) return -97;
    return result;
}
int audit_engine_quality(const unsigned char *pixels)
{
    unsigned char image[7840 + 128];
    unsigned quality = 0;
    if (!ta_map) return -99;
    memset(image, 0xc3, sizeof(image)); memcpy(image + 64, pixels, 7840);
    int result = (int)invoke(0xe6b64, (uintptr_t)(image + 64), (uintptr_t)&quality, 0, 0);
    guard(image);
    return !result && (quality & 255) >= 30 && ((quality >> 8) & 255) >= 90 ? 0 : -1;
}
