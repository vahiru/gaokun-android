#!/usr/bin/env python3
"""Compile the actual fixed init header against a scripted SCM/timing fixture.

No hardware. Protocol fixtures are literal Windows-derived packets. Exercise
complete expected transitions and inject errors at every SCM call, including
empty MCU ACKs, without equating failed reads with sleep.
"""
import os
from pathlib import Path
import subprocess
import tempfile

body = Path(__file__).with_name('fp-init-sequence.h').read_text()
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint32_t __le32;
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define __maybe_unused __attribute__((unused))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
static void fixture_log(const char *fmt, ...) {(void)fmt;}
#define pr_info fixture_log
#define pr_err fixture_log
#define print_hex_dump(...) ((void)0)
#define KERN_INFO 0
#define DUMP_PREFIX_OFFSET 0
static u32 req[1024], rsp[1024];
static void *request=req, *response=rsp;
struct event {char type; unsigned size; u8 packet[8]; unsigned capacity; u8 payload[2]; int fault;};
static struct event events[1024];
static unsigned count, cursor, injections;
static int fail_at=-1, fault_kind, fault_fired;
enum {TRANSPORT, BAD_ACK, BAD_LENGTH, BAD_STATUS, BAD_CRC};
static void msleep(unsigned ms) {
 assert(cursor<count && events[cursor].type=='D' && events[cursor].size==ms);
 ++cursor;
}
static int qcom_scm_qseecom_app_send(u32 id, void *rq, size_t rn, void *rp, size_t pn) {
 assert(id==7 && rq==req && rp==rsp && cursor<count);
 struct event *e=&events[cursor]; unsigned here=cursor++;
 bool inject=(int)here==fail_at || e->fault;
 int kind=e->fault?e->fault-1:fault_kind;
 assert(e->type=='W'||e->type=='R');
 if(e->type=='W') {
  assert(req[0]==0x8021 && req[1]==e->size && req[2]==0);
  assert(rn==12+e->size && pn==12);
  assert(!memcmp((u8*)req+12,e->packet,e->size));
 } else {
  assert(req[0]==0x8020 && req[1]==0 && req[2]==e->capacity+5);
  assert(rn==12 && pn==e->capacity+17);
 }
 if(inject) {fault_fired=1;++injections;}
 if(inject && kind==TRANSPORT)return -EIO;
 req[0]=0x5aa5;req[2]=0;
 if(inject&&kind==BAD_ACK)req[2]=1;
 if(e->type=='R') {
  u8 *rx=(u8*)rsp+12;unsigned length=e->capacity+1;
  /* Header byte0 deliberately differs from observed02: Windows did not
   * validate it, so the helper must not invent that additional contract. */
  rx[0]=0x77;rx[1]=0;rx[2]=length;rx[3]=4;
  memcpy(rx+4,e->payload,e->capacity);
  u8 sum=0;for(unsigned i=1;i<=length+2;++i)sum^=rx[i];
  rx[length+3]=sum;
  if(inject&&kind==BAD_LENGTH)rx[2]=0;
  if(inject&&kind==BAD_STATUS)rx[3]=0;
  if(inject&&kind==BAD_CRC)rx[length+3]^=1;
 }
 return 0;
}
'''
suffix = r'''
static const u8 STATE[]={2,0,3,0x88,0x20,2,0xa9};
static const u8 WRITE01[]={2,0,4,0x87,1,1,1,0x82};
static const u8 WRITE30[]={2,0,4,0x87,0x30,1,0xbb,9};
static const u8 WRITE1F[]={2,0,4,0x87,0x1f,1,1,0x9c};
static const u8 WRITE1E[]={2,0,4,0x87,0x1e,1,1,0x9d};
static const u8 INDIRECT[]={2,0,3,0x89,0x1d,1,0x96};
static const u8 WAKE[]={2,0,3,0x8e,1,0x70,0xfc};
static void delay(unsigned ms) {events[count++]=(struct event){.type='D',.size=ms};}
static void exchange(const u8 *packet,unsigned n,unsigned cap,u16 reply) {
 assert(count+3<ARRAY_SIZE(events));
 struct event e={.type='W',.size=n};memcpy(e.packet,packet,n);events[count++]=e;delay(10);
 events[count++]=(struct event){.type='R',.capacity=cap,.payload={reply&255,reply>>8}};
 delay(10);if(cap)delay(1);
}
static void state(u16 reply){exchange(STATE,sizeof(STATE),2,reply);}
static void ack(const u8 *p,unsigned n){exchange(p,n,0,0);}
static void wake_round(u16 result) {
 ack(WAKE,sizeof(WAKE));delay(5);ack(WAKE,sizeof(WAKE));delay(10);state(result);
}
static void start(bool after_writes_ready) {
 state(0x5aa5);ack(WRITE01,sizeof(WRITE01));ack(WRITE30,sizeof(WRITE30));
 state(after_writes_ready?0x5aa5:0);
}
static void sleep_prefix(void){ack(WRITE1F,sizeof(WRITE1F));ack(WRITE1E,sizeof(WRITE1E));}
static void sleep_ready(unsigned rounds,bool timeout) {
 for(unsigned i=0;i<rounds;++i){delay(3);state(!timeout&&i==rounds-1?0x5aa5:0);}
}
static void finish_sleep(bool still_ready) {
 exchange(INDIRECT,sizeof(INDIRECT),1,0x42);
 state(still_ready?0x5aa5:0x1234);
}
static void reset(void){count=cursor=0;fail_at=-1;fault_kind=0;fault_fired=0;memset(events,0,sizeof(events));}
static void reset_execution(void){cursor=0;fault_fired=0;memset(req,0,sizeof(req));memset(rsp,0,sizeof(rsp));}
static void failed_indirect(int kind, bool write_side) {
 if(write_side) {
  unsigned i=count;
  struct event e={.type='W',.size=sizeof(INDIRECT),.fault=kind+1};
  memcpy(e.packet,INDIRECT,sizeof(INDIRECT));events[count++]=e;
  if(kind!=TRANSPORT)delay(10);
  assert(events[i].fault);
 } else {
  unsigned i=count;
  exchange(INDIRECT,sizeof(INDIRECT),1,0x42);
  events[i+2].fault=kind+1; /* W, D10, R, D10, D1 */
  if(kind==TRANSPORT)count-=2; /* immediate uncertain stop */
 }
}
static void retry_prefix(void) {reset();start(true);sleep_prefix();sleep_ready(1,false);}
static void awake_tail(void) {state(0x1234);state(0);wake_round(0x5aa5);}
static void normal(void){reset();start(true);sleep_prefix();sleep_ready(1,false);finish_sleep(false);state(0);wake_round(0x5aa5);}
static void run_ok(void) {bool uncertain=false;reset_execution();assert(init_sleep_wake(7,&uncertain)==0);assert(!uncertain&&cursor==count);}
static void run_error(int want) {bool uncertain=false;reset_execution();assert(init_sleep_wake(7,&uncertain)==want);assert(!uncertain&&cursor==count);}
int main(void) {
 normal();run_ok();
 /* Complete sequence: inject at each SCM call. No later SCM call is allowed
  * after an injected fault. Read's specified 1ms is still consumed before
  * protocol validation; each successful SCM also waits10. Transport errors skip all subsequent delays. */
 unsigned total=count;
 for(unsigned at=0;at<total;++at) {
  if(events[at].type=='D')continue;
  for(int kind=TRANSPORT;kind<=BAD_CRC;++kind) {
   if(events[at].type=='W'&&kind>BAD_ACK)continue;
   /* Indirect89 protocol faults are retryable and exercised separately. */
   bool indirect=events[at].type=='W' ? events[at].packet[3]==0x89 :
                  (at>=2 && events[at-2].packet[3]==0x89);
   if(indirect && kind!=TRANSPORT)continue;
   reset_execution();fail_at=at;fault_kind=kind;bool uncertain=false;
   int ret=init_sleep_wake(7,&uncertain);
   assert(ret==(kind==TRANSPORT?-EIO:kind==BAD_CRC?-EBADMSG:-EPROTO));
   assert(uncertain==(kind==TRANSPORT)&&fault_fired);
   unsigned extra=kind==TRANSPORT?0:1+(events[at].type=='R'&&events[at].capacity?1:0);
   assert(cursor==at+1+extra);
  }
 }
 normal();fail_at=-1;bool already_uncertain=true;reset_execution();
 assert(init_sleep_wake(7,&already_uncertain)==-EIO && already_uncertain && !cursor);
 reset();state(0);run_error(-EPROTO); /* no writes when initial state isn't ready */
 reset();start(true);sleep_prefix();sleep_ready(50,true);run_error(-ETIMEDOUT);
 reset();start(true);sleep_prefix();sleep_ready(50,false);finish_sleep(false);
 state(0);for(unsigned i=0;i<10;++i)wake_round(i==9?0x5aa5:0);run_ok();
 reset();start(true);sleep_prefix();sleep_ready(1,false);finish_sleep(true);run_error(-EPROTO);
 reset();start(true);sleep_prefix();sleep_ready(1,false);finish_sleep(false);
 state(0);for(unsigned i=0;i<10;++i)wake_round(0);run_error(-ETIMEDOUT);
 /* Already ready at either conditional power_mode call skips wake writes. */
 reset();start(true);sleep_prefix();sleep_ready(1,false);finish_sleep(false);state(0x5aa5);run_ok();
 reset();start(false);wake_round(0x5aa5);sleep_prefix();sleep_ready(1,false);
 finish_sleep(false);state(0);wake_round(0x5aa5);run_ok();
 reset();start(false);for(unsigned i=0;i<10;++i)wake_round(0);run_error(-ETIMEDOUT);
 /* Only the indirect89 outer transaction retries. Cover success on the
  * second/third attempt, and preserve the final error on exhaustion. */
 for(int kind=BAD_ACK;kind<=BAD_CRC;++kind) {
  for(unsigned side=0;side<2;++side) {
   if(side&&kind!=BAD_ACK)continue;
   for(unsigned attempt=2;attempt<=3;++attempt) {
    retry_prefix();
    for(unsigned i=1;i<attempt;++i)failed_indirect(kind,side);
    exchange(INDIRECT,sizeof(INDIRECT),1,0x42);awake_tail();run_ok();
   }
   retry_prefix();for(unsigned i=0;i<3;++i)failed_indirect(kind,side);
   state(0x9391); /* Read-only diagnostic cannot convert exhausted89 to success. */
   run_error(kind==BAD_CRC?-EBADMSG:-EPROTO);
  }
 }
 /* Mixed invalid frames return the last error, rather than the first one. */
 retry_prefix();failed_indirect(BAD_CRC,false);failed_indirect(BAD_CRC,false);
 failed_indirect(BAD_LENGTH,false);state(0x5aa5);run_error(-EPROTO);
 retry_prefix();failed_indirect(BAD_LENGTH,false);failed_indirect(BAD_LENGTH,false);
 failed_indirect(BAD_CRC,false);state(0);run_error(-EBADMSG);
 /* The one post-exhaustion diagnostic is observational: its own malformed
  * frame does not replace the original89 error, and SCM failure marks the
  * caller uncertain while still returning that original protocol error. */
 for(int kind=TRANSPORT;kind<=BAD_CRC;++kind) {
  for(unsigned side=0;side<2;++side) {
   if(side&&kind>BAD_ACK)continue;
   retry_prefix();for(unsigned i=0;i<3;++i)failed_indirect(BAD_CRC,false);
   unsigned at=count;
   if(side) {
    struct event e={.type='W',.size=sizeof(STATE),.fault=kind+1};
    memcpy(e.packet,STATE,sizeof(STATE));events[count++]=e;
    if(kind!=TRANSPORT)delay(10);
   } else {
    state(0x5aa5);events[at+2].fault=kind+1;
    if(kind==TRANSPORT)count-=2;
   }
   bool uncertain=false;reset_execution();
   assert(init_sleep_wake(7,&uncertain)==-EBADMSG);
   assert(uncertain==(kind==TRANSPORT)&&cursor==count);
  }
 }
 /* SCM failure on either send must stop, including after a retryable frame. */
 for(unsigned side=0;side<2;++side) {
  for(unsigned prior=0;prior<3;++prior) {
   retry_prefix();for(unsigned i=0;i<prior;++i)failed_indirect(BAD_LENGTH,false);
   failed_indirect(TRANSPORT,side);bool uncertain=false;reset_execution();
   assert(init_sleep_wake(7,&uncertain)==-EIO&&uncertain&&cursor==count);
  }
 }
 reset();bool u=false;u8 out[3];
 assert(fpinit_exchange(7,STATE,0,out,2,&u)==-EINVAL);
 assert(fpinit_exchange(7,STATE,sizeof(STATE),out,3,&u)==-EINVAL);
 assert(fpinit_exchange(7,STATE,sizeof(STATE),NULL,2,&u)==-EINVAL);
 assert(fpinit_write(7,4,&u)==-EINVAL && !cursor && !u);
 printf("PASS: actual init header exact frames/ACKs/timing, %u injected SCM/protocol faults, sleep/wake boundaries, conditional writes, bounded indirect89 retries and failure-preserving diagnostic20\n",injections);
}
'''
with tempfile.TemporaryDirectory(prefix='fp-init-sequence-') as tmp:
    path = Path(tmp)
    (path / 'fixture.c').write_text(prefix + body + suffix)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', str(path / 'fixture.c'),
                    '-o', str(path / 'fixture')], check=True)
    subprocess.run([str(path / 'fixture')], check=True)
