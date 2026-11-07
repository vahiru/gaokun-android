#!/usr/bin/env python3
"""Exercise actual GPIO helper control flow with mocked syscalls, no device access.

Minimal GPIO declarations model fields used by this helper; this is a lifetime /
error-path test, not a test of Linux's ioctl ABI layout (the target build checks
that against its real linux/gpio.h).
"""
import os
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('fp-reset-version.c').read_text()
body = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
prefix = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define GPIO_V2_LINE_FLAG_INPUT (1ULL << 2)
#define GPIO_V2_LINE_FLAG_OUTPUT (1ULL << 3)
#define GPIO_V2_LINE_FLAG_BIAS_PULL_UP (1ULL << 8)
#define GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN (1ULL << 9)
#define GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES 2
#define GPIO_GET_CHIPINFO_IOCTL 101
#define GPIO_V2_GET_LINEINFO_IOCTL 102
#define GPIO_V2_GET_LINE_IOCTL 103
#define GPIO_V2_LINE_GET_VALUES_IOCTL 104
#define GPIO_V2_LINE_SET_VALUES_IOCTL 105
#define GPIO_V2_LINE_SET_CONFIG_IOCTL 106
struct gpiochip_info {char name[32], label[32]; unsigned lines;};
struct gpio_v2_line_info {char name[32], consumer[32]; unsigned offset, num_attrs; uint64_t flags;};
struct attr {unsigned id; uint64_t values;};
struct config_attr {struct attr attr; uint64_t mask;};
struct gpio_v2_line_config {uint64_t flags; unsigned num_attrs; struct config_attr attrs[10];};
struct gpio_v2_line_request {unsigned offsets[64], num_lines; char consumer[32]; struct gpio_v2_line_config config; int fd;};
struct gpio_v2_line_values {uint64_t mask, bits;};
enum {
 OK, BAD_LABEL, BAD_LINES, BAD_FLAGS, USED_LINE, LINE_ATTRS, BAD_BIAS,
 WRONG_CHIP_SECTION, CHIP_OPEN_FAIL, CHIP_INFO_FAIL, LINE_INFO_FAIL,
 PROBE_OPEN_FAIL, REQUEST_FAIL, HIGH_READ_FAIL, HIGH_WRONG_LEVEL,
 SLEEP10_FAIL, SETLOW_FAIL, LOW_READ_FAIL, LOW_WRONG_LEVEL, SLEEP400_FAIL,
 PROBE_PROTOCOL_FAIL, PROBE_UNCERTAIN, STATE_READ_FAIL_AFTER,
 STATE_OPEN_FAIL_AFTER, STATE_BAD_AFTER, RESTORE_FAIL,
 RESTORE_READBACK_FAIL, RESTORE_FLAGS_FAIL, RESTORE_BIAS_FAIL,
 PENDING_BEFORE, STATE_READ_FAIL_BEFORE, STATE_OPEN_FAIL_BEFORE,
 SIGNAL_FAIL, SHORT_WRITE, INSPECT, EINTR_SLEEP, COUNT
};
static int scenario, requests, restores, reads, lineinfo_calls, probe_writes;
static int chip_closed, probe_closed, line_closed, state_closed, pauses;
static int sigmask, getlevels, sleeps10, sleeps400, sleep_interrupted;
static int request_live, level, configured_back, log_restore_failure;
static int log_restored, log_hold, log_refuse, log_probe_fail;
static jmp_buf paused;

static int mock_open(const char *path, int flags) {
 (void)flags;
 if (!strcmp(path,"/dev/gpiochip4")) {
  if(scenario==CHIP_OPEN_FAIL) {errno=EACCES;return -1;} return 10;
 }
 if (!strcmp(path,"/sys/kernel/debug/gaokun3_fpversion/probe")) {
  if(scenario==PROBE_OPEN_FAIL) {errno=ENOENT;return -1;} return 11;
 }
 assert(!strcmp(path,"/sys/module/qcom_qseecom_fpversion/parameters/secure_pending"));
 if ((scenario==STATE_OPEN_FAIL_BEFORE && !probe_writes) ||
     (scenario==STATE_OPEN_FAIL_AFTER && probe_writes)) {errno=ENOENT;return -1;}
 return 12;
}
static ssize_t mock_read(int fd, void *buf, size_t size) {
 assert(fd==12 && size==4); ++reads;
 if((scenario==STATE_READ_FAIL_BEFORE&&!probe_writes)||
    (scenario==STATE_READ_FAIL_AFTER&&probe_writes)){errno=EIO;return -1;}
 if(scenario==PENDING_BEFORE || (scenario==PROBE_UNCERTAIN&&probe_writes)) {
  memcpy(buf,"Y\n",2);return 2;
 }
 if(scenario==STATE_BAD_AFTER&&probe_writes){memcpy(buf,"N",1);return 1;}
 memcpy(buf,"N\n",2);return 2;
}
static int mock_close(int fd) {
 if(fd==10) ++chip_closed;
 else if(fd==11) ++probe_closed;
 else if(fd==12) ++state_closed;
 else {assert(fd==13 && request_live);++line_closed;request_live=0;}
 return 0;
}
static FILE *mock_fopen(const char *path, const char *mode) {
 assert(!strcmp(path,"/sys/kernel/debug/gpio") && !strcmp(mode,"r"));
 FILE *f=tmpfile();assert(f);
 int bad=scenario==BAD_BIAS || (scenario==RESTORE_BIAS_FAIL && line_closed);
 int wrong=scenario==WRONG_CHIP_SECTION;
 fputs(wrong ? "gpiochip3: parent f100000.pinctrl\n" :
              "gpiochip4: 230 GPIOs, parent: platform/f100000.pinctrl, f100000.pinctrl:\n",f);
 fputs(bad || (line_closed && !configured_back) ?
       " gpio185 : in  low  func0 2mA pull up\n" :
       " gpio185 : in  low  func0 2mA pull down\n",f);
 fputs("gpiochip5: other\n gpio185 : in  low  func0 2mA pull down\n",f);
 rewind(f);return f;
}
static int mock_ioctl(int fd, unsigned long cmd, void *arg) {
 if(cmd==GPIO_GET_CHIPINFO_IOCTL) {
  assert(fd==10);
  if(scenario==CHIP_INFO_FAIL){errno=EIO;return -1;}
  struct gpiochip_info *x=arg;strcpy(x->name,"gpiochip4");
  strcpy(x->label,scenario==BAD_LABEL?"wrong":"f100000.pinctrl");
  x->lines=scenario==BAD_LINES?229:230;return 0;
 }
 if(cmd==GPIO_V2_GET_LINEINFO_IOCTL) {
  assert(fd==10);struct gpio_v2_line_info *x=arg;assert(x->offset==185);++lineinfo_calls;
  if(scenario==LINE_INFO_FAIL || (scenario==RESTORE_READBACK_FAIL&&lineinfo_calls>1)) {errno=EIO;return -1;}
  x->flags=GPIO_V2_LINE_FLAG_INPUT;
  if(scenario==BAD_FLAGS || (scenario==RESTORE_FLAGS_FAIL&&lineinfo_calls>1))x->flags=GPIO_V2_LINE_FLAG_OUTPUT;
  if(scenario==USED_LINE)strcpy(x->consumer,"other");
  if(scenario==LINE_ATTRS)x->num_attrs=1;
  return 0;
 }
 if(cmd==GPIO_V2_GET_LINE_IOCTL) {
  assert(fd==10);struct gpio_v2_line_request *x=arg; ++requests;
  assert(reads==1 && sigmask==7); /* State and signal policy precede GPIO. */
  assert(x->num_lines==1 && x->offsets[0]==185);
  assert(x->config.flags==(GPIO_V2_LINE_FLAG_OUTPUT|GPIO_V2_LINE_FLAG_BIAS_PULL_UP));
  assert(x->config.num_attrs==1 && x->config.attrs[0].attr.id==GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES);
  assert(x->config.attrs[0].attr.values==1 && x->config.attrs[0].mask==1);
  if(scenario==REQUEST_FAIL){errno=EBUSY;return -1;}
  x->fd=13;request_live=1;level=1;return 0;
 }
 assert(fd==13 && request_live);
 if(cmd==GPIO_V2_LINE_GET_VALUES_IOCTL) {
  struct gpio_v2_line_values *x=arg;assert(x->mask==1);++getlevels;
  if((scenario==HIGH_READ_FAIL&&getlevels==1)||(scenario==LOW_READ_FAIL&&getlevels==2)){errno=EIO;return -1;}
  x->bits=level;
  if((scenario==HIGH_WRONG_LEVEL&&getlevels==1)||(scenario==LOW_WRONG_LEVEL&&getlevels==2))x->bits=!level;
  return 0;
 }
 if(cmd==GPIO_V2_LINE_SET_VALUES_IOCTL) {
  struct gpio_v2_line_values *x=arg;assert(x->mask==1 && x->bits==0 && sleeps10);
  if(scenario==SETLOW_FAIL){errno=EIO;return -1;}level=0;return 0;
 }
 assert(cmd==GPIO_V2_LINE_SET_CONFIG_IOCTL); ++restores;
 struct gpio_v2_line_config *x=arg;
 assert(x->flags==(GPIO_V2_LINE_FLAG_INPUT|GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN));
 assert(!x->num_attrs);
 if(scenario==RESTORE_FAIL){errno=EIO;return -1;}
 configured_back=1;return 0;
}
static ssize_t mock_write(int fd, const void *buf, size_t size) {
 assert(fd==11 && size==6 && !memcmp(buf,"probe\n",6));
 assert(request_live && level==0 && sleeps10 && sleeps400 && sigmask==7);
 ++probe_writes;
 if(scenario==PROBE_PROTOCOL_FAIL||scenario==PROBE_UNCERTAIN) {errno=EPROTO;return -1;}
 if(scenario==SHORT_WRITE){errno=0;return 2;}
 return 6;
}
static int mock_nanosleep(const struct timespec *t, struct timespec *remain) {
 assert(t->tv_sec==0);
 if(t->tv_nsec==10000000) ++sleeps10; else {assert(t->tv_nsec==400000000);++sleeps400;}
 if(scenario==EINTR_SLEEP&&!sleep_interrupted){sleep_interrupted=1;*remain=*t;errno=EINTR;return -1;}
 if((scenario==SLEEP10_FAIL&&t->tv_nsec==10000000)||(scenario==SLEEP400_FAIL&&t->tv_nsec==400000000)){errno=EINVAL;return -1;}
 return 0;
}
static void (*mock_signal(int sig, void (*handler)(int)))(int) {
 assert(!request_live && handler==SIG_IGN);
 if(scenario==SIGNAL_FAIL){errno=EINVAL;return SIG_ERR;}
 if(sig==SIGINT)sigmask|=1;else if(sig==SIGTERM)sigmask|=2;else {assert(sig==SIGHUP);sigmask|=4;}
 return SIG_DFL;
}
static int mock_pause(void) {
 assert(request_live && level==0 && !restores && !line_closed && sigmask==7);
 ++pauses;longjmp(paused,1);return -1;
}
static int quiet_printf(const char *fmt, ...) {
 if(strstr(fmt,"original input/low/pull-down verified"))++log_restored;
 return 0;
}
static int quiet_fprintf(FILE *f, const char *fmt, ...) {
 (void)f;
 if(strstr(fmt,"restoration readback FAILED"))++log_restore_failure;
 if(strstr(fmt,"Secure state uncertain"))++log_hold;
 if(strstr(fmt,"refusing"))++log_refuse;
 if(strstr(fmt,"probe failed"))++log_probe_fail;
 return 0;
}
static void quiet_perror(const char *s) {if(strstr(s,"RESTORE"))++log_restore_failure;}
#define open mock_open
#define read mock_read
#define write mock_write
#define close mock_close
#define ioctl mock_ioctl
#define fopen mock_fopen
#define nanosleep mock_nanosleep
#define signal mock_signal
#define pause mock_pause
#define printf quiet_printf
#define fprintf quiet_fprintf
#define perror quiet_perror
#define main helper_main
'''
suffix = r'''
#undef main
#undef printf
#undef fprintf
static void reset(void) {
 requests=restores=reads=lineinfo_calls=probe_writes=0;
 chip_closed=probe_closed=line_closed=state_closed=pauses=0;
 sigmask=getlevels=sleeps10=sleeps400=sleep_interrupted=0;
 request_live=level=configured_back=log_restore_failure=0;
 log_restored=log_hold=log_refuse=log_probe_fail=0;
}
int main(void) {
 for(scenario=0;scenario<COUNT;++scenario) {
  reset();
  char *argv[]={"fp-reset-version",scenario==INSPECT?"--inspect":"--reset-and-probe",NULL};
  if(setjmp(paused)) {
   assert(scenario==PROBE_UNCERTAIN||scenario==STATE_READ_FAIL_AFTER||
          scenario==STATE_OPEN_FAIL_AFTER||scenario==STATE_BAD_AFTER);
   assert(log_hold==1 && pauses==1 && !restores && !line_closed && !chip_closed && !probe_closed);
   continue;
  }
  int rc=helper_main(2,argv);
  assert(scenario!=PROBE_UNCERTAIN&&scenario!=STATE_READ_FAIL_AFTER&&
         scenario!=STATE_OPEN_FAIL_AFTER&&scenario!=STATE_BAD_AFTER);
  if(scenario==OK||scenario==INSPECT||scenario==EINTR_SLEEP)assert(rc==0);else assert(rc!=0);
  if(scenario==CHIP_OPEN_FAIL)assert(!chip_closed);else assert(chip_closed==1);
  if(request_live)abort();
  if(scenario==OK||scenario==EINTR_SLEEP||scenario==PROBE_PROTOCOL_FAIL||scenario==SHORT_WRITE) {
   assert(requests==1 && restores==1 && line_closed==1 && log_restored==1 && probe_writes==1);
   if(scenario==PROBE_PROTOCOL_FAIL||scenario==SHORT_WRITE)assert(log_probe_fail==1);
  }
  if(scenario>=HIGH_READ_FAIL&&scenario<=SLEEP400_FAIL) {
   assert(requests==1 && restores==1 && !probe_writes && line_closed==1 && log_restored==1);
  }
  if(scenario==RESTORE_FAIL||scenario==RESTORE_READBACK_FAIL||scenario==RESTORE_FLAGS_FAIL||scenario==RESTORE_BIAS_FAIL)
   assert(restores==1 && log_restore_failure && !log_restored);
  if(scenario==PENDING_BEFORE||scenario==STATE_READ_FAIL_BEFORE||scenario==STATE_OPEN_FAIL_BEFORE||scenario==SIGNAL_FAIL)
   assert(!requests && !probe_writes && !restores);
  if(scenario==INSPECT)assert(!requests && !reads && !sigmask);
  if(scenario==REQUEST_FAIL)assert(!restores && !line_closed && !probe_writes);
 }
 printf("PASS: actual reset helper identity/bias guards, pulse/error restoration, secure-pending FD retention, signal ordering and readback (%d scenarios)\n", COUNT);
}
'''
with tempfile.TemporaryDirectory(prefix='fp-reset-helper-') as tmp:
    path = Path(tmp)
    (path / 'fixture.c').write_text(prefix + body + suffix)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', str(path / 'fixture.c'),
                    '-o', str(path / 'fixture')], check=True)
    subprocess.run([str(path / 'fixture')], check=True)
