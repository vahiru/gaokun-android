/* SPDX-License-Identifier: GPL-2.0 */
/* Private diagnostic replay only: never sends a QSEE command. */
#define main unused_preprocess_replay_main
#include "rev/preprocess_replay_runner.c"
#undef main
#include <openssl/sha.h>

static unsigned char *native_map;
#ifdef GAOKUN3_AUDIT_FAULT_BRIDGE
extern void audit_bridge_fault(int, siginfo_t*, void*);
void audit_native_fault(int signal_number, siginfo_t *info, void *context)
{
    fault(signal_number, info, context);
}
#endif
int audit_native_open(void)
{
    static const unsigned char expected[32] = {
        0x31,0x4e,0x1b,0xbf,0xb2,0x0f,0x5e,0xad,0xcd,0x6b,0xc5,0xd1,0xb7,0xc4,0x69,0xce,
        0x85,0x29,0xa6,0x39,0xf0,0x01,0xa8,0x15,0xa1,0x84,0x0b,0x70,0x8f,0x34,0x4a,0x10
    };
    const char *path = "/data/local/tmp/gaokun3-fp-template-audit-20261103/native-preprocess.map";
    unsigned char digest[32];
    unsigned char *bytes;
    FILE *file;
    struct rlimit limit = {0, 0};
    struct sigaction action = {0};
    if (native_map || setrlimit(RLIMIT_CORE, &limit)) return -1;
#ifdef GAOKUN3_AUDIT_FAULT_BRIDGE
    action.sa_sigaction = audit_bridge_fault;
#else
    action.sa_sigaction = fault;
#endif
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
    file = fopen(path, "rb");
    if (!file) return -1;
    bytes = malloc(0x1e4000);
    if (!bytes) { fclose(file); return -1; }
    if (fread(bytes, 1, 0x1e4000, file) != 0x1e4000 || fgetc(file) != EOF) {
        free(bytes); fclose(file); return -1;
    }
    fclose(file);
    SHA256(bytes, 0x1e4000, digest);
    free(bytes);
    if (memcmp(digest, expected, sizeof(expected))) return -1;
    native_map = load_map(path, 0x1e4000, 1);
    ta_start = (uintptr_t)native_map;
    ta_bytes = 0x1e4000;
    return 0;
}

int audit_native_apply(const unsigned char *pixels, unsigned char *output)
{
    unsigned char input[INPUT_BYTES];
    struct result result;
    unsigned i;
    if (!native_map) return -1;
    /* Existing GK3A frames lost the raw percentile endpoints. This inverse
     * scaling is a diagnostic proxy, not reconstruction of the ADC samples. */
    for (i = 0; i < OUTPUT_BYTES; ++i) {
        unsigned value = 200 + (255 - pixels[i]) * 8;
        input[2*i] = value & 255;
        input[2*i+1] = value >> 8;
    }
    result = run(native_map, 1, input, output, 0);
    /* The TA return register is not a validated status ABI. Check the known
     * allocation/output boundaries instead; run() verifies all redzones. */
    return result.leaks || !result.allocations || !result.output_changed ? -1 : 0;
}

int audit_native_apply_raw(const unsigned char *input, unsigned char *output)
{
    struct result result;
    if (!native_map) return -1;
    result = run(native_map, 1, input, output, 0);
    return result.leaks || !result.allocations || !result.output_changed ? -1 : 0;
}

void audit_native_close(void)
{
    if (native_map) munmap(native_map, 0x1e4000);
    native_map = NULL;
}
