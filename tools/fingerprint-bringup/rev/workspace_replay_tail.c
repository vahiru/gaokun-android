/* SPDX-License-Identifier: GPL-2.0 */
/* Appended to preprocess_replay_runner.c with its main renamed.
 * This runs only app init + wire dispatch, then either stops at the enrol
 * entry or lets the original workspace clear run and stops at the enrol
 * algorithm gate.  Hardware, file I/O, algorithms and templates are never
 * executed. */
#undef main
static unsigned char *workspace_map;
static void *workspace_base;
static size_t workspace_requested,workspace_bytes,workspace_guard_bytes;
static unsigned char *workspace_guard_lo;
static size_t workspace_min;
static unsigned config_index_bound;
static unsigned init_calls, file_calls, phase_before;
static int clear_mode,gate_hit;
static unsigned long stop_index;

/* sub_9A94, reached from dispatcher 0x8026, clears this extent at
 * qword_2A4638+0x950000.  Both numbers come from the original code. */
#define ENROL_WORKSPACE_OFFSET 0x950000UL
#define ENROL_CLEAR_BYTES 666624UL
static uint64_t zero_boundary(void){return 0;}
static uint64_t init_boundary(void){++init_calls;return 0;}
static uint64_t file_boundary(void){++file_calls;return 0;}
/* Guard-page allocation: an access outside the granted extent faults instead of
 * silently corrupting the host heap, which makes the exhausted workspace
 * observable rather than masked.  The body is pre-filled so the original clear
 * can be observed. */
static void *guarded_alloc(size_t bytes){
 const size_t page=4096;
 size_t body=(bytes+page-1)&~(size_t)(page-1);
 unsigned char *raw=mmap(NULL,body+2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 assert(raw!=MAP_FAILED);
 memset(raw+page,0xa5,body);
 assert(!mprotect(raw,page,PROT_NONE));
 assert(!mprotect(raw+page+body,page,PROT_NONE));
 workspace_guard_lo=raw;workspace_guard_bytes=page;workspace_bytes=body;
 return raw+page;
}
static void guarded_free(void){
 if(workspace_guard_lo)munmap(workspace_guard_lo,workspace_bytes+2*workspace_guard_bytes);
 workspace_guard_lo=NULL;
}
static void *realloc_boundary(void *old,size_t bytes){
 assert(!old&&!workspace_base);workspace_requested=bytes;
 if(!workspace_min){workspace_bytes=bytes;workspace_base=tracked_alloc(bytes);}
 else{size_t granted=bytes>workspace_min?bytes:workspace_min;++alloc_calls;
      workspace_base=guarded_alloc(granted);}
 return workspace_base;
}
static void report_and_exit(const char *stage){
 uintptr_t base=(uintptr_t)workspace_base;
 int inside=base&&ENROL_WORKSPACE_OFFSET<workspace_bytes;
 int end_inside=base&&ENROL_WORKSPACE_OFFSET+ENROL_CLEAR_BYTES<=workspace_bytes;
 int applied=-1,outside=-1;
 if(clear_mode&&inside&&end_inside){
  const unsigned char *work=(const unsigned char *)(base+ENROL_WORKSPACE_OFFSET);
  applied=work[0]==0&&work[ENROL_CLEAR_BYTES/2]==0&&work[ENROL_CLEAR_BYTES-1]==0;
  outside=work[-1]==0xa5&&work[ENROL_CLEAR_BYTES]==0xa5;
 }
 printf("{\"stage\":\"%s\",\"allocation_requested_bytes\":%zu,\"allocation_bytes\":%zu,"
        "\"allocation_calls\":%u,\"guard_pages\":%s,"
        "\"saved_pointer_unchanged\":%s,\"enrol_workspace_offset\":%lu,"
        "\"enrol_workspace_within_allocation\":%s,"
        "\"enrol_clear_bytes\":%lu,\"enrol_clear_end_offset\":%lu,"
        "\"enrol_clear_end_within_allocation\":%s,"
        "\"enrol_clear_applied\":%s,\"enrol_clear_outside_untouched\":%s,"
        "\"index\":%lu,\"phase_before_enrol_command\":%u,\"phase_at_stop\":%u,"
        "\"config_index_bound\":%u,"
        "\"mocked_initialization_calls\":%u,\"mocked_missing_file_reads\":%u,"
        "\"algorithm_gate\":\"%s\",\"algorithm_executed\":false}\n",
        stage,workspace_requested,workspace_bytes,alloc_calls,
        workspace_guard_lo?"true":"false",
        base&&*(uintptr_t *)(workspace_map+0x2a4638)==base?"true":"false",
        ENROL_WORKSPACE_OFFSET,inside?"true":"false",
        ENROL_CLEAR_BYTES,ENROL_WORKSPACE_OFFSET+ENROL_CLEAR_BYTES,
        end_inside?"true":"false",
        applied<0?"null":applied?"true":"false",
        outside<0?"null":outside?"true":"false",
        stop_index,phase_before,*(unsigned *)(workspace_map+0x2a4640),
        config_index_bound,init_calls,file_calls,
        gate_hit?"hit":clear_mode?"closed":"entry-stop");
 if(workspace_guard_lo)guarded_free();else tracked_free(workspace_base);
 fflush(stdout);_Exit(0);
}
static void stop_enrol(uint64_t image,uint64_t output,uint64_t work,uint64_t index){
 (void)image;(void)output;(void)work;
 stop_index=(unsigned long)index;report_and_exit("entry");
}
static void stop_algorithm(uint64_t image,uint64_t index,uint64_t descriptor){
 (void)image;(void)descriptor;
 stop_index=(unsigned long)index;gate_hit=1;report_and_exit("algorithm-gate");
}
int main(int argc,char **argv){
 assert(argc>=6&&argc<=8);struct rlimit limit={0,0};assert(!setrlimit(RLIMIT_CORE,&limit));
 const char *case_name=argv[5];
 clear_mode=strstr(case_name,"-clear")!=NULL;
 if(argc>=7)workspace_min=(size_t)strtoull(argv[6],NULL,0);
 if(argc==8)config_index_bound=(unsigned)strtoul(argv[7],NULL,0);
 assert(!clear_mode||workspace_min);
 assert(!config_index_bound||clear_mode);
 struct sigaction sa={0};sa.sa_sigaction=fault;sa.sa_flags=SA_SIGINFO;
 sigaction(SIGSEGV,&sa,NULL);sigaction(SIGBUS,&sa,NULL);sigaction(SIGILL,&sa,NULL);
 const size_t size=0x364000;
 unsigned char *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 assert(p!=MAP_FAILED);FILE *f=fopen(argv[1],"rb");assert(f);
 assert(fread(p,1,size,f)==size);fclose(f);workspace_map=p;ta_start=(uintptr_t)p;
 hook(p,0x120,realloc_boundary);hook(p,0x130,zero_boundary);hook(p,0x140,zero_boundary);
 hook(p,0x4e24,zero_boundary);hook(p,0x4ef0,boundary_log);
 hook(p,0x9280,init_boundary);hook(p,0x8cf0,file_boundary);
 hook(p,0x98888,zero_boundary);
 if(clear_mode){
  /* State reset/logging helpers are outside the mapped image and would touch
     unmapped globals; stub them, but execute the original clear (0xd350). */
  hook(p,0xb8300,zero_boundary);hook(p,0xc3204,zero_boundary);hook(p,0xc3208,zero_boundary);
  hook(p,0x9b57c,stop_algorithm);
 }else hook(p,0x9a94,stop_enrol);
 *(uintptr_t *)(p+0x361160)=(uintptr_t)(p+0x226138);
 *(uintptr_t *)(p+0x226138)=(uintptr_t)(p+0x2b0000);
 *(uintptr_t *)(p+0x361170)=(uintptr_t)(p+0x2261a8);
 *(uintptr_t *)(p+0x2261a8)=(uintptr_t)(p+0x2b0200);
 *(uintptr_t *)(p+0x361178)=(uintptr_t)(p+0x279648);
 /* Seed the real static value at unk_2261B0+8 (the index bound sub_9A94 checks)
    when requested; otherwise the zeroed map keeps the gate closed. */
 if(config_index_bound)*(unsigned *)(p+0x2261b8)=config_index_bound;
 __builtin___clear_cache((char *)p,(char *)p+size);
 assert(!mprotect(p,0x14000,PROT_READ|PROT_EXEC));
 assert(!mprotect(p+0x98000,0x1000,PROT_READ|PROT_EXEC));
 if(clear_mode){
  /* Stub and stop-hook landing pads live above 0x14000. */
  assert(!mprotect(p+0x9b000,0x1000,PROT_READ|PROT_EXEC));
  assert(!mprotect(p+0xb8000,0x1000,PROT_READ|PROT_EXEC));
  assert(!mprotect(p+0xc3000,0x1000,PROT_READ|PROT_EXEC));
 }
 typedef uint64_t (*entry)(uint64_t,uint64_t,uint64_t,uint64_t);
 ((entry)(p+0xaccc))(0,0,0,0);
 assert(workspace_base&&*(uintptr_t *)(p+0x2a4638)==(uintptr_t)workspace_base);
 unsigned char request[16]={0},response[128]={0};
 entry dispatch=(entry)(p+0xadfc);
 if(!strncmp(case_name,"8031-then-8026",14)){
  *(unsigned *)request=0x8031;
  dispatch((uintptr_t)request,12,(uintptr_t)response,sizeof(response));
 }else assert(!strncmp(case_name,"direct-8026",11));
 phase_before=*(unsigned *)(p+0x2a4640);
 *(unsigned *)request=0x8026;
 dispatch((uintptr_t)request,13,(uintptr_t)response,sizeof(response));
 /* A missing stop is never a successful result. */
 if(clear_mode){stop_index=0;report_and_exit("return");}
 return 99;
}
