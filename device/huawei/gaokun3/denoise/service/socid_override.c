// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Akisaira
//
// In-process override of the SoC id that QNN reads.
//
// Why: libQnnHtp.so (QNN 2.43) initializes its HTP provider only on a SoC it knows. It reads
// /sys/devices/soc0/soc_id with libc open() (observed with an LD_PRELOAD trace:
// experiments/denoise-feasibility-20261103/npu-results/coh16/trace/probe.stderr) and fails with
// "No Snapdragon SOC detected" on the SC8280XP (soc_id 449). The HTP device-config SoC option does
// not help, it is applied later. With 415 (SM8350, the same v68 HTP; the context binaries are
// generated for SoC model 30 = SM8350) the provider starts and the DSP side runs the graphs.
//
// How: this file defines open, openat, their 64-bit and fortified (__open_2, __openat_2) variants
// and fopen in the executable, which exports them (see build.sh: --export-dynamic-symbol). The
// dynamic linker resolves the undefined references of libraries loaded later (libQnnHtp.so and
// what it loads, dlopen'd by the engine) to the executable's definitions first. A read-only open
// of exactly the soc_id path gets a memfd (or a pipe) holding "415\n"; every other call goes to
// the kernel through a raw openat syscall, or to libc's fopen. Only this process is affected:
// nothing is mounted and the file itself is not touched.
//
// Compiled as C without _FORTIFY_SOURCE, so the definitions match libc's plain declarations.
#define _GNU_SOURCE
#include "socid_override.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef O_TMPFILE
#define O_TMPFILE 0
#endif

// The id QNN 2.43 accepts for this HTP generation: 415 = SM8350 (HTP v68, same as the SC8280XP's
// CDSP). Matches the SoC model (30) the context binaries were generated for.
#define GK3_SOC_ID_VALUE "415\n"

static const char* const kSocIdPaths[] = {
    "/sys/devices/soc0/soc_id",
    "/sys/bus/soc/devices/soc0/soc_id",
};

static atomic_int g_enabled = 1;
static atomic_int g_hits;
static atomic_int g_firstVia = -1;
static atomic_int g_firstKind = -1;  // 0 memfd, 1 pipe
static atomic_int g_failures;

static const char* const kViaNames[] = {"open", "open64", "openat", "openat64", "__open_2",
                                        "__openat_2", "fopen", "fopen64"};
enum { VIA_OPEN, VIA_OPEN64, VIA_OPENAT, VIA_OPENAT64, VIA_OPEN_2, VIA_OPENAT_2, VIA_FOPEN, VIA_FOPEN64 };

void gk3_socid_override_enable(int on) { atomic_store(&g_enabled, on ? 1 : 0); }

int gk3_socid_override_enabled(void) { return atomic_load(&g_enabled); }

void gk3_socid_override_stats(struct gk3_socid_stats* s) {
  int via = atomic_load(&g_firstVia), kind = atomic_load(&g_firstKind);
  s->hits = atomic_load(&g_hits);
  s->failures = atomic_load(&g_failures);
  s->first_via = via >= 0 ? kViaNames[via] : "-";
  s->first_kind = kind == 0 ? "memfd" : kind == 1 ? "pipe" : "-";
  s->value = 415;
}

static int isSocIdPath(int dirfd, const char* path) {
  if (!path || (dirfd != AT_FDCWD && path[0] != '/')) return 0;
  for (size_t i = 0; i < sizeof(kSocIdPaths) / sizeof(kSocIdPaths[0]); i++)
    if (strcmp(path, kSocIdPaths[i]) == 0) return 1;
  return 0;
}

static int writeAll(int fd, const char* s, size_t n) {
  while (n) {
    ssize_t w = write(fd, s, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    s += w, n -= (size_t)w;
  }
  return 0;
}

// A readable fd holding the fake value, positioned at 0.
static int fakeFd(int via) {
  const int saved = errno;
  int kind = 0;
  int fd = (int)syscall(__NR_memfd_create, "gk3denoise-soc_id", MFD_CLOEXEC);
  if (fd >= 0) {
    if (writeAll(fd, GK3_SOC_ID_VALUE, sizeof(GK3_SOC_ID_VALUE) - 1) || lseek(fd, 0, SEEK_SET) != 0) {
      close(fd);
      fd = -1;
    }
  }
  if (fd < 0) {  // no memfd (old kernel or SELinux): a pipe reads the same way
    int p[2];
    kind = 1;
    if (pipe2(p, O_CLOEXEC) == 0) {
      if (writeAll(p[1], GK3_SOC_ID_VALUE, sizeof(GK3_SOC_ID_VALUE) - 1) == 0) fd = p[0];
      else close(p[0]);
      close(p[1]);
    }
  }
  if (fd < 0) {
    atomic_fetch_add(&g_failures, 1);
    return -1;  // caller falls back to the real file
  }
  int expect = -1;
  if (atomic_compare_exchange_strong(&g_firstVia, &expect, via)) atomic_store(&g_firstKind, kind);
  atomic_fetch_add(&g_hits, 1);
  errno = saved;
  return fd;
}

static int realOpenat(int dirfd, const char* path, int flags, mode_t mode) {
  return (int)syscall(__NR_openat, dirfd, path, flags, mode);
}

static int doOpen(int dirfd, const char* path, int flags, mode_t mode, int via) {
  if (atomic_load_explicit(&g_enabled, memory_order_relaxed) && (flags & O_ACCMODE) == O_RDONLY &&
      isSocIdPath(dirfd, path)) {
    int fd = fakeFd(via);
    if (fd >= 0) return fd;
  }
  return realOpenat(dirfd, path, flags, mode);
}

static int needsMode(int flags) { return (flags & O_CREAT) || (O_TMPFILE && (flags & O_TMPFILE) == O_TMPFILE); }

#define GET_MODE(flags, last)          \
  mode_t mode = 0;                     \
  if (needsMode(flags)) {              \
    va_list ap;                        \
    va_start(ap, last);                \
    mode = (mode_t)va_arg(ap, int);    \
    va_end(ap);                        \
  }

__attribute__((visibility("default"))) int open(const char* path, int flags, ...) {
  GET_MODE(flags, flags);
  return doOpen(AT_FDCWD, path, flags, mode, VIA_OPEN);
}

__attribute__((visibility("default"))) int open64(const char* path, int flags, ...) {
  GET_MODE(flags, flags);
  return doOpen(AT_FDCWD, path, flags, mode, VIA_OPEN64);
}

__attribute__((visibility("default"))) int openat(int dirfd, const char* path, int flags, ...) {
  GET_MODE(flags, flags);
  return doOpen(dirfd, path, flags, mode, VIA_OPENAT);
}

__attribute__((visibility("default"))) int openat64(int dirfd, const char* path, int flags, ...) {
  GET_MODE(flags, flags);
  return doOpen(dirfd, path, flags, mode, VIA_OPENAT64);
}

// Fortified variants (called without a mode; O_CREAT is not allowed there).
__attribute__((visibility("default"))) int __open_2(const char* path, int flags) {
  return doOpen(AT_FDCWD, path, flags, 0, VIA_OPEN_2);
}

__attribute__((visibility("default"))) int __openat_2(int dirfd, const char* path, int flags) {
  return doOpen(dirfd, path, flags, 0, VIA_OPENAT_2);
}

typedef FILE* (*FopenFn)(const char*, const char*);

static FopenFn realFopen(void) {
  static _Atomic(FopenFn) fn;
  FopenFn f = atomic_load(&fn);
  if (!f) {
    f = (FopenFn)dlsym(RTLD_NEXT, "fopen");
    atomic_store(&fn, f);
  }
  return f;
}

static FILE* doFopen(const char* path, const char* mode, int via) {
  if (atomic_load_explicit(&g_enabled, memory_order_relaxed) && mode && mode[0] == 'r' && !strchr(mode, '+') &&
      isSocIdPath(AT_FDCWD, path)) {
    int fd = fakeFd(via);
    if (fd >= 0) {
      FILE* f = fdopen(fd, "r");
      if (f) return f;
      close(fd);
    }
  }
  FopenFn real = realFopen();
  if (!real) {
    errno = ENOSYS;
    return NULL;
  }
  return real(path, mode);
}

__attribute__((visibility("default"))) FILE* fopen(const char* path, const char* mode) {
  return doFopen(path, mode, VIA_FOPEN);
}

__attribute__((visibility("default"))) FILE* fopen64(const char* path, const char* mode) {
  return doFopen(path, mode, VIA_FOPEN64);
}

int gk3_socid_read_real(char* buf, size_t len) {
  if (!len) return -1;
  buf[0] = 0;
  int fd = realOpenat(AT_FDCWD, kSocIdPaths[0], O_RDONLY | O_CLOEXEC, 0);
  if (fd < 0) return -errno;
  ssize_t n = read(fd, buf, len - 1);
  int e = errno;
  close(fd);
  if (n < 0) return -e;
  buf[n] = 0;
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
  return (int)n;
}
