#!/usr/bin/env python3
"""Test actual fixed status-only header plus real exchange helper, offline."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).parent
body = (root / 'fp-init-sequence.h').read_text() + '\n' + (root / 'fp-frame-status.h').read_text()
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
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
#define KERN_INFO 0
#define DUMP_PREFIX_OFFSET 0
static u32 req[1024], rsp[1024];
static void *request=req, *response=rsp;
static u8 samples[10];
static unsigned logs, calls, delays[40], ndelay, delay_at, injections;
static int fail_call=-1, fault_kind, fault_fired;
enum {TRANSPORT, BAD_ACK, SHORT_LENGTH, LONG_LENGTH, BAD_STATUS, BAD_CRC};
static void fixture_log(const char *fmt, ...) {
 if(strstr(fmt,"fpstatus: sample=")) {
  va_list ap;va_start(ap,fmt);
  unsigned sample=va_arg(ap,unsigned), value=va_arg(ap,unsigned), frame=va_arg(ap,unsigned);
  assert(sample==logs+1 && value==samples[logs] && frame==(samples[logs]&1));
  ++logs;va_end(ap);
 }
}
#define pr_info fixture_log
#define pr_err fixture_log
static void fixture_dump(size_t n){assert(n==6);}
#define print_hex_dump(level,prefix,type,row,group,data,n,ascii) fixture_dump(n)
static void msleep(unsigned ms) {
 assert(delay_at<ndelay && delays[delay_at]==ms);++delay_at;
}
static int qcom_scm_qseecom_app_send(u32 id, void *rq, size_t rn, void *rp, size_t pn) {
 static const u8 expected[]={2,0,3,0x88,0x1d,1,0x97};
 assert(id==7 && rq==req && rp==rsp && calls<20 && !fault_fired);
 unsigned here=calls++, sample=here/2;bool read_side=here&1;
 bool inject=(int)here==fail_call;
 if(!read_side) {
  assert(req[0]==0x8021 && req[1]==7 && req[2]==0 && rn==19 && pn==12);
  assert(!memcmp((u8*)req+12,expected,7)); /* No87/89/SRAM/raw commands. */
 } else assert(req[0]==0x8020 && req[1]==0 && req[2]==6 && rn==12 && pn==18);
 if(inject){fault_fired=1;++injections;}
 if(inject&&fault_kind==TRANSPORT)return -EIO;
 req[0]=0x5aa5;req[2]=0;
 if(inject&&fault_kind==BAD_ACK)req[2]=1;
 if(read_side) {
  u8 *rx=(u8*)rsp+12;
  rx[0]=0x77;rx[1]=0;rx[2]=2;rx[3]=4;rx[4]=samples[sample];rx[5]=0x02^0x04^rx[4];
  if(inject&&fault_kind==SHORT_LENGTH)rx[2]=1;
  if(inject&&fault_kind==LONG_LENGTH){rx[1]=0xff;rx[2]=0xff;}
  if(inject&&fault_kind==BAD_STATUS)rx[3]=0;
  if(inject&&fault_kind==BAD_CRC)rx[5]^=1;
 }
 return 0;
}
'''
suffix = r'''
static void reset(void) {
 static const u8 nonframe[]={0,0x10,0x20,0x30,0x80,0xa0,0xfe,0x12,0x42,0x70};
 memcpy(samples,nonframe,10);logs=calls=ndelay=delay_at=0;
 fail_call=-1;fault_kind=fault_fired=0;memset(req,0,sizeof(req));memset(rsp,0,sizeof(rsp));
}
static void schedule(unsigned sample_count) {
 ndelay=0;
 for(unsigned i=0;i<sample_count;++i) {
  delays[ndelay++]=10;delays[ndelay++]=10;delays[ndelay++]=1;
  if(i+1<sample_count)delays[ndelay++]=100;
 }
}
int main(void) {
 (void)init_sleep_wake;
 reset();schedule(10);bool uncertain=false;
 assert(fp_frame_status(7,&uncertain)==0 && !uncertain && calls==20 && logs==10 && delay_at==39);
 /* Highbit4 alone must not be classified as a completed frame. A lowbit1
  * at any sample terminates without the next100ms or another command. */
 for(unsigned at=0;at<10;++at) {
  reset();samples[at]=(at&1)?0x11:0x81;schedule(at+1);uncertain=false;
  assert(fp_frame_status(7,&uncertain)==0 && !uncertain);
  assert(calls==2*(at+1)&&logs==at+1&&delay_at==ndelay);
 }
 for(unsigned at=0;at<20;++at) {
  for(int kind=TRANSPORT;kind<=BAD_CRC;++kind) {
   if(!(at&1)&&kind>BAD_ACK)continue;
   reset();schedule(10);fail_call=at;fault_kind=kind;uncertain=false;
   int ret=fp_frame_status(7,&uncertain);
   assert(ret==(kind==TRANSPORT?-EIO:kind==BAD_CRC?-EBADMSG:-EPROTO));
   assert(uncertain==(kind==TRANSPORT) && calls==at+1 && logs==at/2);
   unsigned expected=4*(at/2); /* prior complete samples plus gaps */
   if(at&1)expected++; /* this sample's successful write10ms */
   if(kind!=TRANSPORT)expected+=(at&1)?2:1;
   assert(delay_at==expected);
  }
 }
 reset();uncertain=true;
 assert(fp_frame_status(7,&uncertain)==-EIO && uncertain && !calls && !logs && !delay_at);
 printf("PASS: actual status-only header fixed88/1d frames, %u injected faults, 10-sample timing and lowbit early-stop without SRAM/87/89\n",injections);
}
'''
with tempfile.TemporaryDirectory(prefix='fp-frame-status-') as tmp:
    path = Path(tmp)
    (path / 'fixture.c').write_text(prefix + body + suffix)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', str(path / 'fixture.c'),
                    '-o', str(path / 'fixture')], check=True)
    subprocess.run([str(path / 'fixture')], check=True)
