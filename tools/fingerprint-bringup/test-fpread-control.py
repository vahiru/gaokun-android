#!/usr/bin/env python3
"""Compile the actual run_probe body with mocked SCM calls; no hardware access."""
import os
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('qcom_qseecom_fpread.c').read_text()
start = source.index('static int run_probe(void)')
end = source.index('\nstatic ssize_t probe_write', start)
body = source[start:end]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint32_t __le32;
#define FP_REQ_SIZE 12
#define FP_RSP_SIZE 16
#define THIS_MODULE 0
#define KERN_INFO 0
#define DUMP_PREFIX_OFFSET 0
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
#define pr_err(...) ((void)0)
#define le32_to_cpu(x) (x)
#define print_hex_dump(...) ((void)0)
static uint32_t req[1024], rsp[1024];
static void *request = req, *response = rsp;
static int prepare_ret, load_ret, lookup_ret, send_ret, shutdown_ret;
static int loads, sends, shutdowns, references, releases, bad_ack;
static u32 lookup_id;
static int prepare_buffers(void **image, size_t *size) {
  *image = req; *size = 100; return prepare_ret;
}
static void release_buffers(void) { ++releases; }
static int try_module_get(int ignored) { (void)ignored; ++references; return 1; }
static void module_put(int ignored) { (void)ignored; --references; }
static int fpq_app_load(void *image, size_t mdt, size_t size, u32 *id) {
  assert(image == req && !mdt && size == 100); ++loads; *id = 5; return load_ret;
}
static int qcom_scm_qseecom_app_get_id(const char *name, u32 *id) {
  assert(!strcmp(name, "fingerprint")); *id = lookup_id; return lookup_ret;
}
static int qcom_scm_qseecom_app_send(u32 id, void *request_, size_t req_size,
                                    void *response_, size_t rsp_size) {
  assert(id == 5 && request_ == req && response_ == rsp);
  assert(req_size == 12 && rsp_size == 16);
  assert(req[0] == 0x8031 && req[1] == 0 && req[2] == 4);
  ++sends;
  if (!send_ret && !bad_ack) { req[0] = 0x5aa5; req[2] = 0; }
  return send_ret;
}
static int fpq_app_shutdown(u32 id) { assert(id == 5); ++shutdowns; return shutdown_ret; }
static void reset(void) {
  prepare_ret = load_ret = lookup_ret = send_ret = shutdown_ret = 0;
  loads = sends = shutdowns = references = releases = bad_ack = 0;
  lookup_id = 5;
  memset(req, 0, sizeof(req)); memset(rsp, 0, sizeof(rsp));
}
'''
tests = r'''
int main(void) {
  reset(); assert(run_probe() == 0);
  assert(loads == 1 && sends == 1 && shutdowns == 1 && references == 0 && releases == 1);
  reset(); lookup_ret = -ENOENT; lookup_id = 123;
  assert(run_probe() == 0); /* LOAD id, never the name lookup's stale output. */
  assert(sends == 1 && shutdowns == 1 && references == 0 && releases == 1);
  const int errors[] = {-EIO, -EBUSY, -EPROTO, -ELOOP, -EINVAL};
  for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
    reset(); lookup_ret = errors[i]; assert(run_probe() == errors[i]);
    assert(sends == 0 && shutdowns == 0 && references == 1 && releases == 0);
  }
  reset(); lookup_id = 6; assert(run_probe() == -EPROTO);
  assert(sends == 0 && shutdowns == 0 && references == 1 && releases == 0);
  reset(); load_ret = -ENOENT; assert(run_probe() == -ENOENT);
  assert(sends == 0 && shutdowns == 0 && references == 1 && releases == 0);
  reset(); send_ret = -EBUSY; assert(run_probe() == -EBUSY);
  assert(sends == 1 && shutdowns == 0 && references == 1 && releases == 0);
  reset(); shutdown_ret = -EIO; assert(run_probe() == -EIO);
  assert(sends == 1 && shutdowns == 1 && references == 1 && releases == 0);
  reset(); bad_ack = 1; assert(run_probe() == -EPROTO);
  assert(sends == 1 && shutdowns == 1 && references == 0 && releases == 1);
  reset(); prepare_ret = -ENOMEM; assert(run_probe() == -ENOMEM);
  assert(loads == 0 && sends == 0 && references == 0 && releases == 1);
  puts("PASS: actual run_probe framing, ENOENT exception, error retention and ACK paths");
}
'''
with tempfile.TemporaryDirectory(prefix='fpread-control-') as tmp:
    path = Path(tmp)
    (path / 'fixture.c').write_text(prefix + body + tests)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined',
                    str(path / 'fixture.c'), '-o', str(path / 'fixture')], check=True)
    subprocess.run([str(path / 'fixture')], check=True)
