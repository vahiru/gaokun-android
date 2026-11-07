/* SPDX-License-Identifier: GPL-2.0 */
/* Native host replay boundaries only; image algorithms remain original bytes. */
#ifndef __APPLE__
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <signal.h>
#include <ucontext.h>
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#endif

#define INPUT_BYTES 15680
#define OUTPUT_BYTES 7840
uintptr_t platform_x18;
extern long call_preprocess(void *,void *,unsigned,unsigned,void *);
extern void bridge_alloc(void),bridge_free(void),bridge_memset(void),bridge_memcpy(void);
extern void bridge_log(void),bridge_logpath(void);
static unsigned alloc_calls,free_calls,fail_at,live_allocs,logs;
static uintptr_t ta_start,win_start;
static size_t ta_bytes=0x1e4000;
#define MAX_ALLOCS 256
static void fault(int sig,siginfo_t *info,void *context){
#ifdef __APPLE__
 uintptr_t pc=((ucontext_t *)context)->uc_mcontext->__ss.__pc;
#else
 uintptr_t pc=((ucontext_t *)context)->uc_mcontext.pc;
#endif
 const char *region="host-boundary";uintptr_t offset=0;
 if(pc>=ta_start&&pc<ta_start+ta_bytes){region="ta";offset=pc-ta_start;}
 if(pc>=win_start&&pc<win_start+0x28000){region="windows";offset=pc-win_start;}
 printf("{\"signal\":%d,\"region\":\"%s\",\"pc_offset\":%lu,\"null_address\":%s,\"allocation_calls\":%u}\n",
        sig,region,(unsigned long)offset,(uintptr_t)info->si_addr<4096?"true":"false",alloc_calls);
 fflush(stdout);_Exit(128+sig);
}
struct allocation {unsigned char *base,*ptr;size_t size;};
static struct allocation allocations[MAX_ALLOCS];

void *tracked_alloc(size_t n) {
 ++alloc_calls;
 if(alloc_calls==fail_at)return NULL;
 if(!n||n>128*1024*1024||live_allocs>=MAX_ALLOCS)exit(90);
 unsigned char *base=malloc(n+128);if(!base)exit(91);
 memset(base,0xc3,64);memset(base+64,0xa5,n);memset(base+64+n,0x3c,64);
 for(unsigned i=0;i<MAX_ALLOCS;++i)if(!allocations[i].ptr){
  allocations[i]=(struct allocation){base,base+64,n};++live_allocs;return base+64;
 }
 exit(92);
}
void tracked_free(void *ptr) {
 if(!ptr)return;
 for(unsigned i=0;i<MAX_ALLOCS;++i)if(allocations[i].ptr==ptr){
  struct allocation *a=&allocations[i];
  for(unsigned j=0;j<64;++j)if(a->base[j]!=0xc3||a->ptr[a->size+j]!=0x3c)exit(93);
  free(a->base);memset(a,0,sizeof(*a));--live_allocs;++free_calls;return;
 }
 exit(94);
}
void *boundary_memset(void *p,int value,size_t n){return memset(p,value,n);}
void *boundary_memcpy(void *d,const void *s,size_t n){
 if(d!=s&&(uintptr_t)d<(uintptr_t)s+n&&(uintptr_t)s<(uintptr_t)d+n)exit(95);
 return memcpy(d,s,n);
}
void boundary_log(void){++logs;}
void *boundary_logpath(void){return NULL;}

static void hook(unsigned char *p,unsigned at,void *fn){
 uint32_t code[]={0x58000050,0xd61f0200};memcpy(p+at,code,8);memcpy(p+at+8,&fn,8);
}
static void branch_hook(unsigned char *p,unsigned at,unsigned island,void *fn){
 uint32_t jump=0x14000000|((uint32_t)(((int32_t)island-(int32_t)at)/4)&0x3ffffff);
 memcpy(p+at,&jump,4);hook(p,island,fn);
}
static unsigned char *load_map(const char *name,size_t size,int ta){
 FILE *f=fopen(name,"rb");assert(f);
 unsigned char *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 assert(p!=MAP_FAILED&&fread(p,1,size,f)==size);fclose(f);
 if(ta){
  /* d63c and d648 are only12 bytes apart; do not overlap absolute hooks. */
  branch_hook(p,0xd63c,0x10000,bridge_alloc);branch_hook(p,0xd648,0x10020,bridge_free);
  branch_hook(p,0xd350,0x10040,bridge_memset);branch_hook(p,0xe400,0x10060,bridge_memcpy);
  branch_hook(p,0x4ef0,0x10080,bridge_log);
 }else{
  void *a=bridge_alloc,*b=bridge_free;memcpy(p+0x18218,&a,8);memcpy(p+0x18228,&b,8);
  hook(p,0x1920,bridge_memset);hook(p,0x11c0,bridge_memcpy);
  hook(p,0xb888,bridge_log);hook(p,0x14678,bridge_logpath);
 }
#ifdef __APPLE__
 sys_icache_invalidate(p,size);
#else
 __builtin___clear_cache((char *)p,(char *)p+size);
#endif
 assert(!mprotect(p,size,PROT_READ));
 if(ta){
  assert(!mprotect(p,0x8000,PROT_READ|PROT_EXEC));
  assert(!mprotect(p+0xc000,0x8000,PROT_READ|PROT_EXEC));
 }else{
  assert(!mprotect(p,0xc000,PROT_READ|PROT_EXEC));
  assert(!mprotect(p+0x14000,0x4000,PROT_READ|PROT_EXEC));
 }
 return p;
}
struct result {int return_code;unsigned allocations,frees,leaks,log_calls;int input_changed,output_changed;};
static struct result run(unsigned char *map,int ta,const unsigned char *input,
                         unsigned char *output,unsigned failure){
 unsigned char *work=malloc(INPUT_BYTES+128),*dest=malloc(OUTPUT_BYTES+128);assert(work&&dest);
 memset(work,0xc3,INPUT_BYTES+128);memcpy(work+64,input,INPUT_BYTES);
 memset(dest,0x3c,OUTPUT_BYTES+128);memset(dest+64,0xa5,OUTPUT_BYTES);
 alloc_calls=free_calls=logs=0;fail_at=failure;assert(live_allocs==0);
 long rc=call_preprocess(work+64,dest+64,196,40,map+(ta?0x1c68:0x6f00));
 for(unsigned i=0;i<64;++i)assert(work[i]==0xc3&&work[64+INPUT_BYTES+i]==0xc3&&
                                 dest[i]==0x3c&&dest[64+OUTPUT_BYTES+i]==0x3c);
 struct result r={(int)rc,alloc_calls,free_calls,live_allocs,logs,
                   memcmp(work+64,input,INPUT_BYTES)!=0,0};
 for(unsigned i=0;i<OUTPUT_BYTES;++i)r.output_changed|=dest[64+i]!=0xa5;
 memcpy(output,dest+64,OUTPUT_BYTES);
 /* Report leaks, then release only harness-owned allocations; never disguise
    an algorithm allocation failure as successful output. */
 for(unsigned i=0;i<MAX_ALLOCS;++i)if(allocations[i].ptr)tracked_free(allocations[i].ptr);
 free(work);free(dest);return r;
}
int main(int argc,char **argv){
 assert(argc==5);struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
 struct sigaction sa={0};sa.sa_sigaction=fault;sa.sa_flags=SA_SIGINFO;
 sigaction(SIGSEGV,&sa,NULL);sigaction(SIGBUS,&sa,NULL);sigaction(SIGILL,&sa,NULL);
 unsigned char input[INPUT_BYTES],out_ta[OUTPUT_BYTES],out_win[OUTPUT_BYTES];
 assert(fread(input,1,INPUT_BYTES,stdin)==INPUT_BYTES&&fgetc(stdin)==EOF);
 unsigned failure=(unsigned)strtoul(argv[4],NULL,10);
 unsigned char *ta=load_map(argv[1],0x1e4000,1),*win=load_map(argv[2],0x28000,0);
 ta_start=(uintptr_t)ta;win_start=(uintptr_t)win;
 if(!strcmp(argv[3],"both")){
  struct result a=run(ta,1,input,out_ta,0),b=run(win,0,input,out_win,0);
  unsigned mismatches=0;for(unsigned i=0;i<OUTPUT_BYTES;++i)mismatches+=out_ta[i]!=out_win[i];
  printf("{\"ta\":{\"return_interpreted\":false,\"allocations\":%u,\"leaks\":%u,\"input_changed\":%d,\"output_changed\":%d},"
         "\"windows\":{\"return_code\":%d,\"allocations\":%u,\"leaks\":%u,\"input_changed\":%d,\"output_changed\":%d},"
         "\"output_equal\":%s,\"output_mismatch_bytes\":%u}\n",
         a.allocations,a.leaks,a.input_changed,a.output_changed,b.return_code,b.allocations,b.leaks,
         b.input_changed,b.output_changed,mismatches?"false":"true",mismatches);
 }else{
  int is_ta=!strcmp(argv[3],"ta");assert(is_ta||!strcmp(argv[3],"windows"));
  struct result r=run(is_ta?ta:win,is_ta,input,out_ta,failure);
  printf("{\"algorithm\":\"%s\",\"fail_at\":%u,\"return_interpreted\":%s,\"return_code\":%d,"
         "\"allocations\":%u,\"leaks\":%u,\"input_changed\":%d,\"output_changed\":%d}\n",
         argv[3],failure,is_ta?"false":"true",is_ta?0:r.return_code,r.allocations,r.leaks,r.input_changed,r.output_changed);
 }
 munmap(ta,0x1e4000);munmap(win,0x28000);return 0;
}
