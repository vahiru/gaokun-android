#!/usr/bin/env python3
"""Exercise actual query_version C with a mock transport, not a real device."""
import os
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('qcom_qseecom_fpversion.c').read_text()
body = source[source.index('static int query_version('):source.index('\nstatic int query_info(')]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint32_t __le32;
#define KERN_INFO 0
#define DUMP_PREFIX_OFFSET 0
#define le32_to_cpu(x) (x)
#define print_hex_dump(...) ((void)0)
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
static uint32_t req[1024], rsp[1024];
static void *request = req, *response = rsp;
static int scenario, calls, sleeps;
static void msleep(unsigned ms) {
  const unsigned expected[] = {10,20,10};
  assert(sleeps < 3 && ms == expected[sleeps]); ++sleeps;
}
static int qcom_scm_qseecom_app_send(u32 id, void *r, size_t rn, void *s, size_t sn) {
  static const unsigned char write_frame[17] = {
    0x21,0x80,0,0,5,0,0,0,0,0,0,0,2,0,1,0x30,0x31
  };
  static const unsigned char read_frame[12] = {
    0x20,0x80,0,0,0,0,0,0,69,0,0,0
  };
  assert(id == 5 && r == req && s == rsp);
  ++calls;
  unsigned char *out = (unsigned char *)rsp;
  for (unsigned i = 0; i < 4096; ++i) assert(out[i] == 0xa5);
  if (calls == 1) {
    assert(sleeps == 0);
    assert(rn == 17 && sn == 12 && !memcmp(r, write_frame, 17));
    if (scenario == 1) return -EBUSY;
    if (scenario == 2) return 0; /* No ACK: don't start the read. */
  } else {
    assert(calls == 2 && rn == 12 && sn == 81 && !memcmp(r, read_frame, 12));
    assert(sleeps == 2);
    if (scenario == 3) return -EIO;
    if (scenario == 4) return 0; /* No ACK. */
    if (scenario != 5) { /* Scenario 5 leaves the MCU response unwritten. */
      unsigned char *m = out + 12;
      unsigned length = scenario == 6 ? 1 : scenario == 7 ? 66 : scenario == 8 ? 65 : 6;
      m[0] = 3; m[1] = length >> 8; m[2] = length; m[3] = scenario == 9 ? 0x15 : 4;
      if (length <= 65) {
        memset(m + 4, 'V', length - 1);
        unsigned char x = 0;
        for (unsigned i = 1; i <= length + 2; ++i) x ^= m[i];
        m[length + 3] = x ^ (scenario == 10 ? 1 : 0);
      }
    }
  }
  req[0] = 0x5aa5; req[2] = 0;
  return 0;
}
'''
tests = r'''
int main(void) {
  for (scenario = 0; scenario <= 10; ++scenario) {
    calls = sleeps = 0;
    bool uncertain = false;
    int ret = query_version(5, &uncertain);
    if (scenario == 0 || scenario == 8) assert(ret == 0 && !uncertain);
    else if (scenario == 1) assert(ret == -EBUSY && uncertain);
    else if (scenario == 3) assert(ret == -EIO && uncertain);
    else if (scenario == 10) assert(ret == -EBADMSG && !uncertain);
    else assert(ret != 0 && !uncertain);
    assert(calls == (scenario == 1 || scenario == 2 ? 1 : 2));
    assert(sleeps == (scenario == 1 ? 0 : scenario == 2 ? 1 : scenario == 3 ? 2 : 3));
  }
  puts("PASS: actual query_version frame sizes, delay, ACK, error retention flag, bounds and checksum");
}
'''
with tempfile.TemporaryDirectory(prefix='fpversion-query-') as tmp:
    path = Path(tmp)
    (path / 'fixture.c').write_text(prefix + body + tests)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', str(path / 'fixture.c'),
                    '-o', str(path / 'fixture')], check=True)
    subprocess.run([str(path / 'fixture')], check=True)
