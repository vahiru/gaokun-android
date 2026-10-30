#!/usr/bin/env python3
"""Extract actual query_info; exercise fixed requests and faults without hardware."""
import os
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('qcom_qseecom_fpversion.c').read_text()
body = source[source.index('static int query_info('):source.index('\nstatic int run_probe(')]
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
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define le32_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define print_hex_dump(...) ((void)0)
#define pr_info(...) ((void)0)
static bool active_scan;
static uint32_t req[1024], rsp[1024];
static void *request = req, *response = rsp;
static int fault, fault_call, calls, sleeps;
static void msleep(unsigned n) { assert(n == (sleeps % 3 == 2 ? 1u : 10u)); ++sleeps; }
static int qcom_scm_qseecom_app_send(u32 id, void *r, size_t rn, void *s, size_t sn) {
  static const unsigned char index[] = {0x14,0x15,0x20,0x01,0x30};
  static const unsigned count[] = {1,1,2,1,1};
  static const unsigned capacity[] = {1,1,2,4,4};
  unsigned k = calls / 2;
  bool reading = calls % 2;
  assert(calls < 10 && id == 5 && r == req && s == rsp && sleeps == (int)(3*k + (reading ? 1 : 0)));
  ++calls;
  unsigned char *out = (unsigned char *)rsp;
  for (unsigned i = 0; i < 4096; ++i) assert(out[i] == 0xa5);
  if (!reading) {
    unsigned char expected[19] = {0x21,0x80,0,0,7,0,0,0,0,0,0,0,2,0,3,0x88,0,0,0};
    expected[16] = index[k]; expected[17] = count[k];
    expected[18] = 3 ^ 0x88 ^ index[k] ^ count[k];
    assert(rn == 19 && sn == 12 && !memcmp(r, expected, 19));
  } else {
    assert(rn == 12 && sn == capacity[k]+17);
    assert(req[0] == 0x8020 && req[1] == 0 && req[2] == capacity[k]+5);
  }
  if (calls == fault_call && fault == 1) return -EBUSY;
  if (calls == fault_call && fault == 2) return 0; /* no ACK */
  req[0] = 0x5aa5; req[2] = 0;
  if (reading) {
    if (calls == fault_call && fault == 3) return 0; /* A5 sentinel */
    unsigned char *m = out + 12;
    unsigned length = k < 3 ? count[k]+1 : 3;
    if (k >= 3 && fault >= 11 && fault <= 14) length = fault - 9;
    memset(m, 0, capacity[k]+5);
    m[0] = 3; m[2] = length; m[3] = 4;
    if (k < 2) m[4] = fault == 10 ? 255 : (fault == 15 ? (k ? 196 : 40) : (k ? 64 : 80));
    else if (k == 2) { m[4] = 0xa5; m[5] = 0x5a; }
    else { m[4] = 0x91; m[5] = 0x93; }
    if (calls == fault_call) {
      if (fault == 4) m[2] = 1;
      if (fault == 5) m[2] = capacity[k] + 2;
      if (fault == 6) m[3] = 0x15;
      if (fault == 8 && k < 2) m[4] = 0;
      if (fault == 9 && k == 2) { m[4] = 0x5a; m[5] = 0xa5; }
    }
    unsigned char crc = 0;
    for (unsigned i = 1; i <= length + 2; ++i) crc ^= m[i];
    m[length+3] = crc ^ ((calls == fault_call && fault == 7) ? 1 : 0);
  }
  return 0;
}
static void reset(int f, int c) { fault=f; fault_call=c; calls=sleeps=0; }
'''
tests = r'''
int main(void) {
  bool uncertain;
  reset(0,0); uncertain=false; assert(query_info(5,&uncertain)==0 && !uncertain && calls==10 && sleeps==15);
  reset(10,0); uncertain=false; assert(query_info(5,&uncertain)==0 && !uncertain); /* 255*255 fits */
  for (int f=11;f<=14;++f) {
    reset(f,0); uncertain=false; assert(query_info(5,&uncertain)==0 && !uncertain && calls==10);
  }
  for (int c=1;c<=10;++c) {
    reset(1,c); uncertain=false; assert(query_info(5,&uncertain)==-EBUSY && uncertain && calls==c);
    reset(2,c); uncertain=false; assert(query_info(5,&uncertain)==-EPROTO && !uncertain && calls==c);
  }
  for (int c=2;c<=10;c+=2) for (int f=3;f<=7;++f) {
    reset(f,c); uncertain=false;
    int r=query_info(5,&uncertain);
    assert(r==(f==7 ? -EBADMSG : -EPROTO) && !uncertain && calls==c);
  }
  for (int c=2;c<=4;c+=2) {
    reset(8,c); uncertain=false; assert(query_info(5,&uncertain)==-EPROTO && !uncertain);
  }
  reset(9,6); uncertain=false; assert(query_info(5,&uncertain)==-EPROTO && !uncertain);
  active_scan=true;
  reset(0,0); uncertain=false; assert(query_info(5,&uncertain)==-EPROTO && !uncertain);
  reset(10,0); uncertain=false; assert(query_info(5,&uncertain)==-EPROTO && !uncertain);
  reset(15,0); uncertain=false; assert(query_info(5,&uncertain)==0 && !uncertain);
  active_scan=false;
  puts("PASS: actual query_info five frames, lengths, timing, ACK, faults, bounded snapshot payloads, dimensions and status endian");
}
'''
with tempfile.TemporaryDirectory(prefix='fpversion-info-') as tmp:
    path = Path(tmp)
    (path / 'fixture.c').write_text(prefix + body + tests)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', str(path / 'fixture.c'),
                    '-o', str(path / 'fixture')], check=True)
    subprocess.run([str(path / 'fixture')], check=True)
