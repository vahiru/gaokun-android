/* SPDX-License-Identifier: GPL-2.0 */
/* Appended to preprocess_replay_runner.c with its main renamed.
 * This runs only app init + wire dispatch to a STOP at enrol entry.
 * Hardware, file I/O, algorithms and templates are never executed. */
#undef main
static unsigned char *workspace_map;
static void *workspace_base;
static size_t workspace_bytes;
static unsigned init_calls, file_calls, phase_before;
static uint64_t zero_boundary(void){return 0;}
static uint64_t init_boundary(void){++init_calls;return 0;}
static uint64_t file_boundary(void){++file_calls;return 0;}
static void *realloc_boundary(void *old,size_t bytes){
 assert(!old&&!workspace_base);workspace_bytes=bytes;
 workspace_base=tracked_alloc(bytes);return workspace_base;
}
static void stop_enrol(uint64_t image,uint64_t output,uint64_t work,uint64_t index){
 (void)image;(void)output;
 uintptr_t saved=*(uintptr_t *)(workspace_map+0x2a4638);
 printf("{\"allocation_bytes\":%zu,\"allocation_calls\":%u,"
        "\"saved_pointer_unchanged\":%s,\"enrol_workspace_offset\":%lu,"
        "\"enrol_workspace_within_allocation\":%s,\"index\":%lu,"
        "\"phase_before_enrol_command\":%u,\"phase_at_stop\":%u,"
        "\"mocked_initialization_calls\":%u,\"mocked_missing_file_reads\":%u,"
        "\"algorithm_executed\":false}\n",
        workspace_bytes,alloc_calls,saved==(uintptr_t)workspace_base?"true":"false",
        (unsigned long)(work-(uintptr_t)workspace_base),
        work>=(uintptr_t)workspace_base&&work<(uintptr_t)workspace_base+workspace_bytes?"true":"false",
        (unsigned long)index,phase_before,*(unsigned *)(workspace_map+0x2a4640),init_calls,file_calls);
 tracked_free(workspace_base);fflush(stdout);_Exit(0);
}
int main(int argc,char **argv){
 assert(argc==6);struct rlimit limit={0,0};assert(!setrlimit(RLIMIT_CORE,&limit));
 struct sigaction sa={0};sa.sa_sigaction=fault;sa.sa_flags=SA_SIGINFO;
 sigaction(SIGSEGV,&sa,NULL);sigaction(SIGBUS,&sa,NULL);sigaction(SIGILL,&sa,NULL);
 const size_t size=0x364000;
 unsigned char *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 assert(p!=MAP_FAILED);FILE *f=fopen(argv[1],"rb");assert(f);
 assert(fread(p,1,size,f)==size);fclose(f);workspace_map=p;ta_start=(uintptr_t)p;
 hook(p,0x120,realloc_boundary);hook(p,0x130,zero_boundary);hook(p,0x140,zero_boundary);
 hook(p,0x4e24,zero_boundary);hook(p,0x4ef0,boundary_log);
 hook(p,0x9280,init_boundary);hook(p,0x8cf0,file_boundary);
 hook(p,0x9a94,stop_enrol);hook(p,0x98888,zero_boundary);
 *(uintptr_t *)(p+0x361160)=(uintptr_t)(p+0x226138);
 *(uintptr_t *)(p+0x226138)=(uintptr_t)(p+0x2b0000);
 *(uintptr_t *)(p+0x361170)=(uintptr_t)(p+0x2261a8);
 *(uintptr_t *)(p+0x2261a8)=(uintptr_t)(p+0x2b0200);
 *(uintptr_t *)(p+0x361178)=(uintptr_t)(p+0x279648);
 __builtin___clear_cache((char *)p,(char *)p+size);
 assert(!mprotect(p,0x14000,PROT_READ|PROT_EXEC));
 assert(!mprotect(p+0x98000,0x1000,PROT_READ|PROT_EXEC));
 typedef uint64_t (*entry)(uint64_t,uint64_t,uint64_t,uint64_t);
 ((entry)(p+0xaccc))(0,0,0,0);
 assert(workspace_base&&*(uintptr_t *)(p+0x2a4638)==(uintptr_t)workspace_base);
 unsigned char request[16]={0},response[128]={0};
 entry dispatch=(entry)(p+0xadfc);
 if(!strcmp(argv[5],"8031-then-8026")){
  *(unsigned *)request=0x8031;
  dispatch((uintptr_t)request,12,(uintptr_t)response,sizeof(response));
 }else assert(!strcmp(argv[5],"direct-8026"));
 phase_before=*(unsigned *)(p+0x2a4640);
 *(unsigned *)request=0x8026;
 dispatch((uintptr_t)request,13,(uintptr_t)response,sizeof(response));
 /* A missing stop is never a successful result. */
 return 99;
}
