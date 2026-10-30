#!/usr/bin/env python3
"""Actual MCU frame-gated capture headers, scripted SCM only: never hardware.
Tests empty polling without SRAM access, lowbit gating, fixed transfer timing,
full encoded-frame bounds, and clearing stale capture data on every failure.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).parent
body = (root / 'fp-init-sequence.h').read_text() + '\n' + (root / 'fp-active-scan.h').read_text()
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
#define FP_RESPONSE_CAPACITY 16384
#define KERN_INFO 0
#define DUMP_PREFIX_OFFSET 0
static void fixture_log(const char *fmt, ...) {(void)fmt;}
#define pr_info fixture_log
#define pr_err fixture_log
static void fixture_dump(size_t n) {assert(n==6);} /* only1d, never image */
#define print_hex_dump(level,prefix,type,row,group,data,n,ascii) fixture_dump(n)
#define memzero_explicit(p,n) memset(p,0,n)
static u32 req[1024], rsp[FP_RESPONSE_CAPACITY/4];
static void *request=req, *response=rsp;
struct event {char type; unsigned size; u8 packet[9]; unsigned capacity; u8 status;};
static struct event events[2000];
static unsigned count, cursor, injections, sram_writes, sram_reads;
static int fail_at=-1, fault_kind, fault_fired;
enum {TRANSPORT, BAD_ACK, SHORT_LENGTH, LONG_LENGTH, BAD_STATUS, BAD_CRC};
static u8 image_byte(unsigned i) {return (u8)(i*37+11);}
static void msleep(unsigned ms) {
 assert(cursor<count && events[cursor].type=='D' && events[cursor].size==ms);
 ++cursor;
}
static int qcom_scm_qseecom_app_send(u32 id, void *rq, size_t rn, void *rp, size_t pn) {
 assert(id==7 && rq==req && rp==rsp && cursor<count && !fault_fired);
 struct event *e=&events[cursor];unsigned here=cursor++;
 bool inject=(int)here==fail_at;
 assert(e->type=='W'||e->type=='R');assert(pn<=FP_RESPONSE_CAPACITY);
 if(e->type=='W') {
  assert(req[0]==0x8021 && req[1]==e->size && req[2]==0 && rn==12+e->size && pn==12);
  assert(!memcmp((u8*)req+12,e->packet,e->size));
  assert((e->size==7&&e->packet[3]==0x88&&e->packet[4]==0x1d) ||
         (e->size==9&&e->packet[3]==0x8b)); /* No old87/ST/89/fake20. */
  if(e->packet[3]==0x8b)++sram_writes;
 } else {
  assert(req[0]==0x8020 && req[1]==0 && req[2]==e->capacity+5 && rn==12 && pn==e->capacity+17);
  assert(e->capacity==1||e->capacity==15680);
  if(e->capacity==15680)++sram_reads;
 }
 if(inject){fault_fired=1;++injections;}
 if(inject&&fault_kind==TRANSPORT)return -EIO;
 req[0]=0x5aa5;req[2]=0;
 if(inject&&fault_kind==BAD_ACK)req[2]=1;
 if(e->type=='R') {
  u8 *rx=(u8*)rsp+12;unsigned length=e->capacity+1;
  rx[0]=0x77;rx[1]=length>>8;rx[2]=length&255;rx[3]=4;
  if(e->capacity==15680)for(unsigned i=0;i<e->capacity;++i)rx[4+i]=image_byte(i);
  else rx[4]=e->status;
  u8 sum=0;for(unsigned i=1;i<=length+2;++i)sum^=rx[i];rx[length+3]=sum;
  if(inject&&fault_kind==SHORT_LENGTH){rx[1]=0;rx[2]=0;}
  if(inject&&fault_kind==LONG_LENGTH){rx[1]=0xff;rx[2]=0xff;}
  if(inject&&fault_kind==BAD_STATUS)rx[3]=0;
  if(inject&&fault_kind==BAD_CRC)rx[length+3]^=1;
 }
 return 0;
}
'''
suffix = r'''
static const u8 FRAME_STATUS[]={2,0,3,0x88,0x1d,1,0x97};
static const u8 SRAM[]={2,0,5,0x8b,0x34,0,0x3d,0x40,0xc7};
static void delay(unsigned ms){events[count++]=(struct event){.type='D',.size=ms};}
static void write_packet(const u8 *p,unsigned n) {
 struct event e={.type='W',.size=n};memcpy(e.packet,p,n);events[count++]=e;delay(10);
}
static void read_packet(unsigned cap,u8 status) {
 events[count++]=(struct event){.type='R',.capacity=cap,.status=status};delay(10);
 if(cap==1)delay(1);
}
static void reset(void){count=cursor=sram_writes=sram_reads=0;fail_at=-1;fault_kind=fault_fired=0;memset(events,0,sizeof(events));}
static void reset_execution(void){cursor=sram_writes=sram_reads=0;fault_fired=0;fp_scan_valid=true;memset(fp_scan_data,0xa5,sizeof(fp_scan_data));}
static void assert_empty(void){assert(!fp_scan_valid);for(unsigned i=0;i<sizeof(fp_scan_data);++i)assert(fp_scan_data[i]==0);}
static void status(u8 value){write_packet(FRAME_STATUS,sizeof(FRAME_STATUS));read_packet(1,value);}
static void image(void){write_packet(SRAM,sizeof(SRAM));delay(20);read_packet(15680,0);}
static void scenario_limit(unsigned limit,unsigned frame_at,u8 absent_value) {
 reset();
 for(unsigned i=0;i<limit;++i) {
  status(i==frame_at?0x11:absent_value);
  if(i==frame_at){image();break;}
  if(i+1!=limit)delay(100);
 }
}
static void scenario(unsigned frame_at,u8 absent_value){scenario_limit(10,frame_at,absent_value);}
static void run_ok(void) {
 bool uncertain=false;reset_execution();assert(fp_active_scan(7,&uncertain)==0);
 assert(!uncertain&&fp_scan_valid&&cursor==count&&sram_writes==1&&sram_reads==1);
 for(unsigned i=0;i<sizeof(fp_scan_data);++i)assert(fp_scan_data[i]==image_byte(i));
}
static void run_empty(void) {
 bool uncertain=false;reset_execution();assert(fp_active_scan(7,&uncertain)==-ENODATA);
 assert(!uncertain&&cursor==count&&!sram_writes&&!sram_reads);assert_empty();
}
int main(void) {
 for(unsigned path=0;path<3;++path) {
  scenario(path==0?0:path==1?9:10,0x10);
  if(path<2)run_ok();else run_empty();
  unsigned total=count;
  for(unsigned at=0;at<total;++at) {
   if(events[at].type=='D')continue;
   for(int kind=TRANSPORT;kind<=BAD_CRC;++kind) {
    if(events[at].type=='W'&&kind>BAD_ACK)continue;
    reset_execution();fail_at=at;fault_kind=kind;bool uncertain=false;
    int ret=fp_active_scan(7,&uncertain);
    assert(ret==(kind==TRANSPORT?-EIO:kind==BAD_CRC?-EBADMSG:-EPROTO));
    assert(uncertain==(kind==TRANSPORT)&&fault_fired);
    unsigned extra=kind==TRANSPORT?0:1+(events[at].type=='R'&&events[at].capacity==1?1:0);
    assert(cursor==at+1+extra);assert_empty();
    if((events[at].type=='W'&&events[at].packet[3]==0x88)||
       (events[at].type=='R'&&events[at].capacity==1))
     assert(!sram_writes&&!sram_reads); /* no capture after any status fault */
   }
  }
 }
 /* Every possible lowbit1 sample permits exactly one frame; no added outer
  * GetImage10ms is allowed between status and the fixed8b write. */
 for(unsigned at=0;at<10;++at){scenario(at,0);run_ok();}
 scenario(10,0);run_empty();
 scenario(10,0x10);run_empty(); /* bit4-only is not frame_available */
 scenario(10,0xfe);run_empty(); /* all other bits set still cannot open gate */
 scenario(0,0);reset_execution();bool uncertain=true;
 assert(fp_active_scan(7,&uncertain)==-EIO&&uncertain&&!cursor);assert_empty();

 assert(injections==184); /* Preserve every original fault-injection case. */
 for(unsigned limit=10;limit<=200;limit+=190){
  scenario_limit(limit,limit,0x10);reset_execution();uncertain=false;
  assert(fp_active_scan_bounded(7,&uncertain,limit)==-ENODATA);
  assert(cursor==count&&!uncertain&&!sram_writes&&!sram_reads);assert_empty();
  scenario_limit(limit,limit-1,0);reset_execution();uncertain=false;
  assert(fp_active_scan_bounded(7,&uncertain,limit)==0);
  assert(cursor==count&&fp_scan_valid&&sram_writes==1&&sram_reads==1);
 }
 const unsigned invalid[]={0,9,201,~0u};
 for(unsigned i=0;i<4;++i){
  reset();reset_execution();uncertain=false;
  assert(fp_active_scan_bounded(7,&uncertain,invalid[i])==-EINVAL&&!cursor);assert_empty();
 }
 const unsigned fault_positions[]={2,602,1196};
 for(unsigned i=0;i<3;++i){
  scenario_limit(200,200,0);reset_execution();uncertain=false;
  fail_at=fault_positions[i];fault_kind=SHORT_LENGTH;
  assert(fp_active_scan_bounded(7,&uncertain,200)==-EPROTO);
  assert(!uncertain&&fault_fired&&cursor==fault_positions[i]+3&&!sram_writes&&!sram_reads);
  assert_empty();
 }
 printf("PASS: actual MCU-gated capture: %u injected faults, ten empty samples forbid8b, first/last lowbit permit one exact frame, bit4 rejection, stale-data clearing and transfer timing\n",injections);
}
'''
with tempfile.TemporaryDirectory(prefix='fp-active-scan-') as tmp:
    path = Path(tmp)
    (path / 'fixture.c').write_text(prefix + body + suffix)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', str(path / 'fixture.c'),
                    '-o', str(path / 'fixture')], check=True)
    subprocess.run([str(path / 'fixture')], check=True)
    (path / 'too-small.c').write_text((prefix + body + suffix).replace(
        '#define FP_RESPONSE_CAPACITY 16384', '#define FP_RESPONSE_CAPACITY 4096'))
    rejected = subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-fsyntax-only',
                               str(path / 'too-small.c')], capture_output=True, text=True)
    if rejected.returncode == 0 or 'requires a verified response allocation' not in rejected.stderr:
        raise AssertionError('4KiB response must fail the actual header capacity guard')
    print('PASS: stale4KiB response configuration rejected at compile time')
