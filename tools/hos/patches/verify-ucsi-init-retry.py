#!/usr/bin/env python3
"""Compile and exercise the actual patched ucsi_init_work with stubbed I/O.

Usage: python3 verify-ucsi-init-retry.py /path/to/patched/kernel
This is a decision-path test, not a kernel build or concurrency test.
"""
from pathlib import Path
import subprocess, sys, tempfile
source_root=Path(sys.argv[1])
workspace=tempfile.TemporaryDirectory(prefix="hos-ucsi-state-test-")
root=Path(workspace.name)
s=(source_root/'drivers/usb/typec/ucsi/ucsi.c').read_text()
a=s.index('static void ucsi_init_work('); start=s.index('{',a); depth=1;i=start+1
while depth:
 depth+=(s[i]=='{')-(s[i]=='}');i+=1
fn=s[a:i]
harness=r'''
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#define EPROBE_DEFER 517
#define UCSI_RETRY_INIT_TIMEOUT 8
#define UCSI_ROLE_SWITCH_WAIT_COUNT 100
#define UCSI_ROLE_SWITCH_INTERVAL 100
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
#define dev_err_probe(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
struct work_struct {int unused;};
struct delayed_work {struct work_struct work;};
struct ucsi {struct delayed_work work; int work_count; unsigned int init_timeout_retries; unsigned long quirks; void *connector;};
static int result,queued;static unsigned int delay;
static void *system_long_wq;
static int ucsi_init(struct ucsi *u) {(void)u;return result;}
static unsigned int msecs_to_jiffies(unsigned int v) {return v;}
static bool queue_delayed_work(void *q,struct delayed_work *w,unsigned int d) {(void)q;(void)w;queued++;delay=d;return true;}
'''
tests=r'''
static void run(struct ucsi *u,int err) {result=err;queued=0;delay=0;ucsi_init_work(&u->work.work);}
int main(void) {
 struct ucsi u={.quirks=UCSI_RETRY_INIT_TIMEOUT};
 run(&u,-ETIMEDOUT);assert(queued==1&&delay==1000&&u.init_timeout_retries==1&&u.work_count==0);
 run(&u,-EPROBE_DEFER);assert(queued==1&&delay==100&&u.work_count==1&&u.init_timeout_retries==1);
 run(&u,-ETIMEDOUT);assert(queued==1&&delay==3000&&u.init_timeout_retries==2&&u.work_count==1);
 run(&u,-ETIMEDOUT);assert(queued==0&&u.init_timeout_retries==2);
 run(&u,-EPROBE_DEFER);assert(queued==1&&u.work_count==2&&u.init_timeout_retries==2);
 run(&u,0);assert(queued==0&&u.init_timeout_retries==2);
 u=(struct ucsi){.quirks=UCSI_RETRY_INIT_TIMEOUT,.connector=(void*)1};
 run(&u,-ETIMEDOUT);assert(queued==0&&u.init_timeout_retries==0);
 u=(struct ucsi){0};run(&u,-ETIMEDOUT);assert(queued==0&&u.init_timeout_retries==0);
 u=(struct ucsi){.quirks=UCSI_RETRY_INIT_TIMEOUT};run(&u,-EIO);assert(queued==0&&u.init_timeout_retries==0);
 run(&u,-ENOMEM);assert(queued==0&&u.init_timeout_retries==0);
 u.work_count=101;run(&u,-EPROBE_DEFER);assert(queued==0&&u.work_count==102&&u.init_timeout_retries==0);
 run(&u,-ETIMEDOUT);assert(queued==1&&u.init_timeout_retries==1);
 puts("PASS: 12 init-worker state transitions; independent budgets, delays, publication guard, opt-in and error filters");
}
'''
(root/'test.c').write_text(harness+fn+tests)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-o',str(root/'test'),str(root/'test.c')],check=True)
subprocess.run([str(root/'test')],check=True)
