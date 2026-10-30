/* SPDX-License-Identifier: GPL-2.0 */
/* Appended to preprocess_replay_runner.c with its main renamed.
 * Loads the whole fingerpr.mbn image (all PT_LOAD segments), applies the 373
 * RELATIVE relocations, binds the 40 dynamic imports to host shims, then runs
 * the original init/config/enrol functions.  No hardware, SFS, SPI or device
 * access; SFS/SPI return errors, logs are counted, allocations come from a
 * host arena that is also published through app_heap_* / app_stack_*. */
#undef main

#define MAP_SIZE 0x37d000UL
#define HEAP_BYTES (128UL*1024*1024)

typedef struct {uint32_t slot;uint32_t pad;uint64_t addend;} relative_reloc_t;
typedef struct {uint64_t slot;uintptr_t fn;} import_binding_t;
typedef struct {uint64_t slot;uint32_t code;uint32_t pad;} abs_binding_t;

enum {IMP_OK,IMP_MALLOC,IMP_REALLOC,IMP_FREE,IMP_LOG,IMP_SFS,IMP_SPI,IMP_FATAL};
enum {ABS_HEAP_BASE,ABS_HEAP_LIMIT,ABS_STACK_BASE,ABS_STACK_LIMIT};

static unsigned char *ta_map;
static unsigned char *heap_arena;
static size_t heap_used,heap_size=HEAP_BYTES;
static unsigned log_calls_host,sfs_calls,spi_calls;
static int algorithm_ran;
struct arena_block {void *ptr;size_t size;};
#define MAX_BLOCKS 4096
static struct arena_block blocks[MAX_BLOCKS];
static unsigned block_count;
static unsigned import_hits[4096];
static uint64_t import_hit(unsigned idx,uint64_t a,uint64_t b,uint64_t c,uint64_t d);

/*__LOADER_DATA__*/

static void *arena_alloc(size_t n){
 if(!n)n=1;
 n=(n+15)&~(size_t)15;
 if(!heap_arena||heap_used+n>heap_size)return NULL;
 void *p=heap_arena+heap_used;heap_used+=n;
 memset(p,0,n);
 if(block_count<MAX_BLOCKS){blocks[block_count].ptr=p;blocks[block_count].size=n;++block_count;}
 return p;
}
static size_t arena_size(void *p){
 for(unsigned i=0;i<block_count;++i)if(blocks[i].ptr==p)return blocks[i].size;
 return 0;
}
static void boundary_zero(uint64_t a,uint64_t b,uint64_t c,uint64_t d){
 (void)a;(void)b;(void)c;(void)d;
}
static uint64_t import_hit(unsigned idx,uint64_t a,uint64_t b,uint64_t c,uint64_t d){
 (void)c;(void)d;
 if(idx>=import_count)return 0;
 ++import_hits[idx];
 switch(import_class[idx]){
 case IMP_MALLOC:return (uint64_t)(uintptr_t)arena_alloc((size_t)a);
 case IMP_REALLOC:{
  void *old=(void *)(uintptr_t)a;size_t n=(size_t)b;
  void *p=arena_alloc(n);if(p&&old)memcpy(p,old,arena_size(old)<n?arena_size(old):n);
  return (uint64_t)(uintptr_t)p;}
 case IMP_FREE:return 0;
 case IMP_LOG:++log_calls_host;return 0;
 case IMP_SFS:++sfs_calls;return (uint64_t)-1;
 case IMP_SPI:++spi_calls;return (uint64_t)-1;
 case IMP_FATAL:{
  printf("{\"boundary\":\"qsee_err_fatal\",\"import\":\"%s\"}\n",import_names[idx]);
  fflush(stdout);_Exit(0);}
 default:return 0;
 }
}
static void hook(unsigned char *p,unsigned at,void *fn);
static void mprotect_or_die(void *addr,size_t len,int prot){
 if(mprotect(addr,len,prot)){perror("mprotect");_Exit(97);}
}
static void report(const char *stage,uint64_t init_rc,uint64_t config_rc,uint64_t enroll_rc,
                   unsigned image_bytes){
 printf("{\"stage\":\"%s\",\"app_init_rc\":%llu,\"config_build_rc\":%llu,"
        "\"enroll_rc\":%llu,\"image_bytes\":%u,"
        "\"arena_used\":%zu,\"allocations\":%u,\"log_calls\":%u,"
        "\"sfs_calls\":%u,\"spi_calls\":%u,\"algorithm_executed\":%s,\"hits\":[",
        stage,(unsigned long long)init_rc,(unsigned long long)config_rc,
        (unsigned long long)enroll_rc,image_bytes,heap_used,block_count,
        log_calls_host,sfs_calls,spi_calls,algorithm_ran?"true":"false");
 unsigned shown=0;
 for(unsigned i=0;i<import_count;++i)if(import_hits[i]){
  printf("%s{\"name\":\"%s\",\"count\":%u}",shown?",":"",import_names[i],import_hits[i]);
  ++shown;
 }
 printf("]}\n");
 fflush(stdout);
}
static unsigned char *build_sensor_config(void){
 unsigned char *cfg=arena_alloc(124);if(!cfg)return NULL;
 *(uint64_t *)(cfg+0)=0x4000000050ULL;
 cfg[8]=0;cfg[10]=12;cfg[11]=48;cfg[12]=30;cfg[13]=30;
 *(uint32_t *)(cfg+16)=1;*(uint32_t *)(cfg+20)=40;*(uint32_t *)(cfg+24)=80;
 cfg[28]=0;cfg[29]=10;*(uint16_t *)(cfg+30)=0;
 cfg[32]=1;cfg[33]=0;cfg[34]=1;cfg[35]=2;cfg[36]=5;cfg[37]=80;
 *(uint16_t *)(cfg+38)=90;
 memcpy(cfg+40,"fff?P",6);
 *(uint16_t *)(cfg+46)=30;
 *(uint32_t *)(cfg+48)=1050253722U;
 *(uint16_t *)(cfg+52)=80;*(uint16_t *)(cfg+54)=50;
 *(uint16_t *)(cfg+56)=20;*(uint16_t *)(cfg+58)=30;*(uint16_t *)(cfg+60)=50;
 *(uint32_t *)(cfg+64)=2;
 cfg[68]=75;cfg[69]=50;cfg[70]=40;
 *(uint32_t *)(cfg+71)=0;*(uint16_t *)(cfg+75)=0;cfg[77]=1;
 *(uint16_t *)(cfg+78)=0;cfg[80]=0;cfg[81]=1;*(uint16_t *)(cfg+82)=0;
 cfg[84]=0;cfg[85]=0;cfg[86]=12;cfg[87]=60;cfg[88]=60;cfg[89]=60;
 memcpy(cfg+92,"333?",5);
 cfg[97]=1;cfg[98]=14;cfg[99]=15;
 *(uint32_t *)(cfg+100)=0;*(uint16_t *)(cfg+104)=0;cfg[106]=1;
 *(uint16_t *)(cfg+108)=1;cfg[110]=0;cfg[111]=1;
 *(uint16_t *)(cfg+112)=0;*(uint16_t *)(cfg+114)=0;*(uint16_t *)(cfg+116)=0;
 cfg[118]=0;*(uint16_t *)(cfg+119)=1;*(uint16_t *)(cfg+121)=6;
 return cfg;
}
int main(int argc,char **argv){
 assert(argc>=6);struct rlimit limit={0,0};assert(!setrlimit(RLIMIT_CORE,&limit));
 const char *case_name=argv[5];
 struct sigaction sa={0};sa.sa_sigaction=fault;sa.sa_flags=SA_SIGINFO;
 sigaction(SIGSEGV,&sa,NULL);sigaction(SIGBUS,&sa,NULL);sigaction(SIGILL,&sa,NULL);
 ta_map=mmap(NULL,MAP_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 assert(ta_map!=MAP_FAILED);
 FILE *f=fopen(argv[1],"rb");assert(f);
 assert(fread(ta_map,1,MAP_SIZE,f)==MAP_SIZE);fclose(f);
 ta_start=(uintptr_t)ta_map;ta_bytes=MAP_SIZE;
 for(unsigned i=0;i<relative_reloc_count;++i)
  *(uint64_t *)(ta_map+relative_relocs[i].slot)=(uint64_t)(ta_map+relative_relocs[i].addend);
 for(unsigned i=0;i<import_count;++i)
  *(uintptr_t *)(ta_map+import_bindings[i].slot)=(uintptr_t)import_bindings[i].fn;
 heap_arena=mmap(NULL,heap_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 assert(heap_arena!=MAP_FAILED);
 uintptr_t stack_marker=(uintptr_t)&stack_marker;
 for(unsigned i=0;i<abs_count;++i){
  uintptr_t value=abs_bindings[i].code==ABS_HEAP_BASE?((uintptr_t)heap_arena):
                  abs_bindings[i].code==ABS_HEAP_LIMIT?((uintptr_t)heap_arena+heap_size):
                  abs_bindings[i].code==ABS_STACK_BASE?stack_marker:stack_marker-0x10000;
  *(uintptr_t *)(ta_map+abs_bindings[i].slot)=value;
 }
 hook(ta_map,0x4e24,boundary_zero);
 hook(ta_map,0x4ef0,boundary_log);
 __builtin___clear_cache((char *)ta_map,(char *)ta_map+MAP_SIZE);
 mprotect_or_die(ta_map,0x225000,PROT_READ|PROT_EXEC);
 mprotect_or_die(ta_map+0x225000,MAP_SIZE-0x225000,PROT_READ|PROT_WRITE);
 typedef uint64_t (*entry)(uint64_t,uint64_t,uint64_t,uint64_t);
 uint64_t init_rc=((entry)(ta_map+0xaccc))(0,0,0,0);
 uint64_t config_rc=0,enroll_rc=0;unsigned image_bytes=0;
 if(strcmp(case_name,"init")){
  unsigned char *cfg=build_sensor_config();
  config_rc=cfg?1:0;
  if(cfg){
   if(!strcmp(case_name,"enroll_forced")){
    cfg[12]=0;cfg[13]=0;*(uint16_t *)(cfg+38)=0;*(uint16_t *)(cfg+46)=0;
   }
   *(uintptr_t *)(ta_map+0x346368)=(uintptr_t)cfg;
   uint64_t image_instance=((entry)(ta_map+0x9746c))(0,0,0,0);
   if(image_instance)config_rc=image_instance;
   if(!strcmp(case_name,"quality")&&image_instance==0){
    unsigned width=*(uint32_t *)cfg,height=*(uint32_t *)(cfg+4);
    unsigned bytes=width*height;
    uintptr_t inst=*(uintptr_t *)(ta_map+0x346430);
    unsigned char *dst=inst?((unsigned char **)(void *)inst)[1]:NULL;
    if(!dst)dst=arena_alloc(bytes);
    if(dst){
     static const char *const names[]={"const0","const128","const255","gradient",
       "random","checker8","ridges8","noise_mid","split"};
     printf("{\"stage\":\"quality\",\"width\":%u,\"height\":%u,\"patterns\":[",width,height);
     for(unsigned p=0;p<9;++p){
      for(unsigned i=0;i<bytes;++i){
       unsigned x=i%width,y=i/width,v=0;
       switch(p){
       case 0:v=0;break;case 1:v=128;break;case 2:v=255;break;
       case 3:v=(unsigned char)(32+((i*7+y*29)%192));break;
       case 4:v=(unsigned char)((i*1103515245u+12345u)>>16);break;
       case 5:v=((x/8+y/8)&1)?200:40;break;
       case 6:v=((y%16)<8)?40:200;break;
       case 7:v=(unsigned char)(100+(((i*2654435761u)>>24)%56));break;
       default:v=(x<width/2)?40:200;break;
       }
       dst[i]=(unsigned char)v;
      }
      unsigned out=0xFFFFFFFFu;
      uint64_t q=((entry)(ta_map+0xE6B64))((uint64_t)(uintptr_t)dst,
                                            (uint64_t)(uintptr_t)&out,0,0);
      printf("%s{\"name\":\"%s\",\"quality\":%u,\"area\":%u,\"b2\":%u,"
             "\"b3\":%u,\"rc\":%llu}",p?",":"",names[p],out&0xff,(out>>8)&0xff,
             (out>>16)&0xff,(out>>24)&0xff,(unsigned long long)q);
     }
     printf("]}\n");fflush(stdout);return 0;
    }
   }
   if((!strcmp(case_name,"enroll")||!strcmp(case_name,"enroll_forced"))
      &&image_instance==0){
    unsigned width=*(uint32_t *)cfg,height=*(uint32_t *)(cfg+4);
    image_bytes=width*height;
    unsigned char *image=arena_alloc(image_bytes);
    if(image){
     for(unsigned i=0;i<image_bytes;++i)
      image[i]=(unsigned char)(32+((i*7+(i/width)*29)%192));
     algorithm_ran=1;
     uint64_t rc=((entry)(ta_map+0x9b57c))(0,0,(uint64_t)(uintptr_t)image,0);
     enroll_rc=rc;
     report(case_name,init_rc,config_rc,enroll_rc,image_bytes);
     return 0;
    }
   }
  }
 }
 report(case_name,init_rc,config_rc,enroll_rc,image_bytes);
 return 0;
}
