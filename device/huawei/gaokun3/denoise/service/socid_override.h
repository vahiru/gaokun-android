// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Akisaira
//
// In-process override of /sys/devices/soc0/soc_id for QNN; see socid_override.c.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gk3_socid_stats {
  int hits;                // opens of the soc_id path answered with the fake value
  int failures;            // matching opens that could not get a fake fd (went to the real file)
  const char* first_via;   // function of the first hit (open, fopen, ...), "-" if none
  const char* first_kind;  // memfd or pipe, "-" if none
  int value;               // the reported id
};

void gk3_socid_override_enable(int on);  // on by default
int gk3_socid_override_enabled(void);
void gk3_socid_override_stats(struct gk3_socid_stats* s);
// The real id (raw syscall, bypasses the override); returns its length or -errno.
int gk3_socid_read_real(char* buf, size_t len);

// The exported names (the linker flags in build.sh list the same set).
#define GK3_SOCID_EXPORTS "open,open64,openat,openat64,__open_2,__openat_2,fopen,fopen64"

#ifdef __cplusplus
}
#endif
