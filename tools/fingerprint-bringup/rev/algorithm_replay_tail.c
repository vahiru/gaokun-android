/* SPDX-License-Identifier: GPL-2.0 */
/* Appended to preprocess_replay_runner.c with its main renamed.
 * Loads the whole fingerpr.mbn image (all PT_LOAD segments), applies the 373
 * RELATIVE relocations, binds the 40 dynamic imports to host shims, then runs
 * the original init/config/enrol functions.  No hardware, SFS, SPI or device
 * access; SFS/SPI return errors, logs are counted, allocations come from a
 * host arena that is also published through app_heap_* / app_stack_*. */
#undef main
#include <stdarg.h>

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
static unsigned template_count,template_nonzero;
static uint64_t template_hash;
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
static int trace_nmax=-1,trace_nmin=-1,trace_mach=-1,trace_rmach=-1;
static int trace_featsz=-1,trace_tplbin=-1,trace_far=-1;
static void trace_reset(void){
 trace_nmax=trace_nmin=trace_mach=trace_rmach=-1;
 trace_featsz=trace_tplbin=trace_far=-1;
}
static void trace_log(unsigned level,const char *tag,const char *fmt,...){
 (void)tag;char buf[512];va_list ap;va_start(ap,fmt);
 vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);
 if(strstr(buf,"FtInitSubTemplate"))return;
 if(strstr(buf,"nMaxExtremum"))
  (void)sscanf(strstr(buf,"nMaxExtremum"),"nMaxExtremum = %d, nMinExtremum = %d",
               &trace_nmax,&trace_nmin);
 if(strstr(buf,"machCnts"))
  (void)sscanf(strstr(buf,"machCnts"),"machCnts = %d, reverseMachCnts = %d",
               &trace_mach,&trace_rmach);
 if(strstr(buf,"featBufSize"))
  (void)sscanf(strstr(buf,"featBufSize"),"featBufSize = %d, templateBinDiscr = %d",
               &trace_featsz,&trace_tplbin);
 if(strstr(buf,"verify far level"))
  (void)sscanf(strstr(buf,"verify far level"),"verify far level = %d",&trace_far);
 /* High-volume per-extremum trace is decoded into the JSON counters above. */
 if(strstr(buf,"FtInterpExtremum")||strstr(buf,"FtSeqSort"))return;
 fprintf(stderr,"ta-log[%u] %s\n",level,buf);
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
        "\"sfs_calls\":%u,\"spi_calls\":%u,\"algorithm_executed\":%s,"
        "\"template_count\":%u,\"template_nonzero\":%u,\"template_hash\":\"%016llx\",\"hits\":[",
        stage,(unsigned long long)init_rc,(unsigned long long)config_rc,
        (unsigned long long)enroll_rc,image_bytes,heap_used,block_count,
        log_calls_host,sfs_calls,spi_calls,algorithm_ran?"true":"false",
        template_count,template_nonzero,(unsigned long long)template_hash);
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
/* Deterministic dot field.  A SIFT-like scale-space (FtScaleSpaceExtrema,
 * sigma=1.6, 3 intervals) only finds extrema where the image has isolated
 * blobs; a plain gradient or white noise yields 0-2 candidates, which cannot
 * support descriptor matching.  Each dot below is a smooth quadratic bump and
 * therefore a scale-space extremum. */
static int dot_w,dot_h,dot_n;
static int dot_x[256],dot_y[256],dot_r[256],dot_s[256];
static void ensure_dots(unsigned width,unsigned height,unsigned n){
 if((int)width==dot_w&&(int)height==dot_h&&(int)n==dot_n)return;
 if(n>256)n=256;if(!n)n=60;
 unsigned s=123456789u;
 for(unsigned i=0;i<n;++i){
  s=s*1664525u+1013904223u;dot_x[i]=(int)(s%width);
  s=s*1664525u+1013904223u;dot_y[i]=(int)(s%height);
  s=s*1664525u+1013904223u;dot_r[i]=2+(int)(s%3);
  s=s*1664525u+1013904223u;dot_s[i]=(s&1)?1:-1;
 }
 dot_w=(int)width;dot_h=(int)height;dot_n=(int)n;
}
static unsigned dot_field(unsigned width,unsigned height,unsigned n,unsigned x,unsigned y){
 ensure_dots(width,height,n);
 int v=128;
 for(int i=0;i<dot_n;++i){
  int dx=(int)x-dot_x[i],dy=(int)y-dot_y[i],r=dot_r[i];
  int d2=dx*dx+dy*dy,rr=r*r;
  if(d2<rr){int w=rr-d2;v+=dot_s[i]*90*(w*w)/(rr*rr);}
 }
 if(v<0)v=0;if(v>255)v=255;
 return (unsigned)v;
}
static unsigned pattern_value(unsigned kind,unsigned param,unsigned x,unsigned y,
                              unsigned width,unsigned height,unsigned i){
 switch(kind){
 case 0:return 0;
 case 1:return 128;
 case 2:return 255;
 case 3:return (unsigned char)(32+((i*7+y*29)%192));
 case 4:return (unsigned char)((i*1103515245u+12345u)>>16);
 case 5:return ((x/param+y/param)&1)?200:40;
 case 6:return ((y%16)<param)?40:200;
 case 7:return (unsigned char)(100+(((i*2654435761u)>>24)%56));
 case 8:return (x<width/2)?40:200;
 case 9:return ((x%16)<param)?40:200;
 case 12:return dot_field(width,height,param,x,y);
 case 13:{
  unsigned ridge=((x/4)&1)?1:0;
  unsigned v=(unsigned)(110+ridge*70);
  v=(v+dot_field(width,height,param?param:40,x,y))/2;
  return v;
 }
 default:{
  unsigned area=width*height*param/100,side=1;
  while((side+1)*(side+1)<=area)++side;
  unsigned x0=(width-side)/2,y0=(height-side)/2;
  int inside=x>=x0&&x<x0+side&&y>=y0&&y<y0+side;
  if(kind==10)return inside?((((x+y)%8)<4)?60:200):40;
  return inside?(unsigned char)(100+(((i*2654435761u)>>24)%56)):20;
 }}
}
int main(int argc,char **argv){
 assert(argc>=6);struct rlimit limit={0,0};assert(!setrlimit(RLIMIT_CORE,&limit));
 static char case_buf[64];
 snprintf(case_buf,sizeof case_buf,"%s",argv[5]);
 unsigned pat_kind=3,pat_param=0,pat_perturb=0,pat_flag=0;
 {char *c=strchr(case_buf,':');
  if(c){*c=0;(void)sscanf(c+1,"%u:%u:%u:%u",&pat_kind,&pat_param,&pat_perturb,&pat_flag);}}
 const char *case_name=case_buf;
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
 hook(ta_map,0x4ef0,trace_log);
 __builtin___clear_cache((char *)ta_map,(char *)ta_map+MAP_SIZE);
 mprotect_or_die(ta_map,0x225000,PROT_READ|PROT_EXEC);
 mprotect_or_die(ta_map+0x225000,MAP_SIZE-0x225000,PROT_READ|PROT_WRITE);
 typedef uint64_t (*entry)(uint64_t,uint64_t,uint64_t,uint64_t);
 uint64_t init_rc=((entry)(ta_map+0xaccc))(0,0,0,0);
 {uintptr_t s1=*(uintptr_t *)(ta_map+0x3614C8);
  uintptr_t s2=*(uintptr_t *)(ta_map+0x3614D0);
  uintptr_t s3=*(uintptr_t *)(ta_map+0x3614D8);
  if(s1)*(unsigned char *)s1=1;
  if(s2)*(unsigned int *)s2=0;
  if(s3)*(uintptr_t *)s3=0;}
 uint64_t config_rc=0,enroll_rc=0;unsigned image_bytes=0;
 if(strcmp(case_name,"init")){
  unsigned char *cfg=build_sensor_config();
  config_rc=cfg?1:0;
  int dims196x40=cfg&&strstr(case_name,"196x40")!=NULL;
  int dims40x196=cfg&&!dims196x40&&strstr(case_name,"40x196")!=NULL;
  int dims96=cfg&&!dims196x40&&!dims40x196&&strstr(case_name,"96")!=NULL;
  if(cfg&&dims196x40){*(uint32_t *)cfg=196;*(uint32_t *)(cfg+4)=40;}
  else if(cfg&&dims40x196){*(uint32_t *)cfg=40;*(uint32_t *)(cfg+4)=196;}
  else if(cfg&&dims96){*(uint32_t *)cfg=96;*(uint32_t *)(cfg+4)=96;}
  if(cfg){
   if(strstr(case_name,"forced")){
    cfg[12]=0;cfg[13]=0;*(uint16_t *)(cfg+38)=0;*(uint16_t *)(cfg+46)=0;
   }
   *(uintptr_t *)(ta_map+0x346368)=(uintptr_t)cfg;
   if(strstr(case_name,"dimsfix")){
    uintptr_t dims=*(uintptr_t *)(ta_map+0x361530);
    if(dims){*(uint32_t *)dims=*(uint32_t *)cfg;
             *(uint32_t *)(dims+4)=*(uint32_t *)(cfg+4);}
   }
   uint64_t image_instance=((entry)(ta_map+0x9746c))(0,0,0,0);
   if(image_instance)config_rc=image_instance;
   if(!strncmp(case_name,"quality",7)&&image_instance==0){
    unsigned width=*(uint32_t *)cfg,height=*(uint32_t *)(cfg+4),bytes=width*height;
    uintptr_t inst=*(uintptr_t *)(ta_map+0x346430);
    unsigned char *dst=inst?((unsigned char **)(void *)inst)[1]:NULL;
    if(!dst)dst=arena_alloc(bytes);
    if(dst){
     enum {MAXP=48};
     char names[MAXP][24];unsigned kind[MAXP],param[MAXP],count=0;
     static const char *const base_names[]={"const0","const128","const255","gradient",
       "random","checker8","ridges8","noise_mid","split"};
     for(unsigned p=0;p<9;++p){
      snprintf(names[count],24,"%s",base_names[p]);
      kind[count]=p==5?5:(p==6?6:p);
      param[count]=(p==5||p==6)?8:0;++count;
     }
     for(unsigned c=2;c<=32;c*=2){snprintf(names[count],24,"checker%u",c);
      kind[count]=5;param[count]=c;++count;}
     for(unsigned d=2;d<=14;d+=2){snprintf(names[count],24,"hstripe%u",d);
      kind[count]=6;param[count]=d;++count;}
     for(unsigned d=2;d<=14;d+=2){snprintf(names[count],24,"vstripe%u",d);
      kind[count]=9;param[count]=d;++count;}
     for(unsigned pc=50;pc<=98;pc+=12){snprintf(names[count],24,"box%u",pc);
      kind[count]=10;param[count]=pc;++count;}
     for(unsigned pc=80;pc<=98;pc+=6){snprintf(names[count],24,"noisebox%u",pc);
      kind[count]=11;param[count]=pc;++count;}
     for(unsigned p=12,dots=60;p<=13;++p,dots=40){snprintf(names[count],24,"blobs%u",dots);
      kind[count]=p;param[count]=dots;++count;}
     unsigned quals[MAXP],areas[MAXP],best=count;
     uintptr_t dims=*(uintptr_t *)(ta_map+0x361530);
     uint64_t dp=dims?(unsigned long long)dims:0ULL;
     unsigned dw=dims?*(uint32_t *)dims:0,dh=dims?*(uint32_t *)(dims+4):0;
     printf("{\"stage\":\"quality\",\"width\":%u,\"height\":%u,"
            "\"dims_ptr\":\"%llu\",\"dims\":[%u,%u],\"patterns\":[",
            width,height,(unsigned long long)dp,dw,dh);
     for(unsigned p=0;p<count;++p){
      for(unsigned i=0;i<bytes;++i)
       dst[i]=(unsigned char)pattern_value(kind[p],param[p],i%width,i/width,
                                           width,height,i);
      unsigned out=0xFFFFFFFFu;
      uint64_t q=((entry)(ta_map+0xE6B64))((uint64_t)(uintptr_t)dst,
                                            (uint64_t)(uintptr_t)&out,0,0);
      quals[p]=out&0xff;areas[p]=(out>>8)&0xff;
      printf("%s{\"name\":\"%s\",\"quality\":%u,\"area\":%u,\"b2\":%u,"
             "\"b3\":%u,\"rc\":%llu}",p?",":"",names[p],quals[p],areas[p],
             (out>>16)&0xff,(out>>24)&0xff,(unsigned long long)q);
      if(best==count&&quals[p]>=30&&areas[p]>=90)best=p;
     }
     printf("],\"selected\":");
     if(best<count){
      for(unsigned i=0;i<bytes;++i)
       dst[i]=(unsigned char)pattern_value(kind[best],param[best],i%width,i/width,
                                           width,height,i);
      algorithm_ran=1;
      enroll_rc=((entry)(ta_map+0x9b57c))(0,0,(uint64_t)(uintptr_t)dst,0);
      printf("{\"name\":\"%s\",\"quality\":%u,\"area\":%u,\"enroll_rc\":%lld}",
             names[best],quals[best],areas[best],(long long)enroll_rc);
     }else printf("null");
     printf("}\n");fflush(stdout);return 0;
    }
   }
   if(!strncmp(case_name,"loop",4)&&image_instance==0){
    unsigned width=*(uint32_t *)cfg,height=*(uint32_t *)(cfg+4),bytes=width*height;
    unsigned char *image=arena_alloc(bytes);
    if(image){
     for(unsigned i=0;i<bytes;++i)
      image[i]=(unsigned char)pattern_value(pat_kind,pat_param,i%width,i/width,
                                            width,height,i);
     uint64_t erc=((entry)(ta_map+0x9b57c))(0,0,(uint64_t)(uintptr_t)image,0);
     uintptr_t dbs=*(uintptr_t *)(ta_map+0x361540);
     uintptr_t db=dbs?*(uintptr_t *)dbs:0;
     unsigned count=db?*(uint16_t *)(db+50116):0;
     unsigned flag=db?*(uint16_t *)(db+50118):0;
     unsigned fid=db?*(uint32_t *)(db+50120):0;
     unsigned acc=db?*(uint32_t *)(db+50128):0;
     uint64_t valid_rc=0xFFFFFFFFFFFFFFFFULL;
     if(pat_flag&&db){
      valid_rc=((entry)(ta_map+0xE83B4))(0,0,0,0);
      flag=*(uint16_t *)(db+50118);
     }
     unsigned char *probe=image;
     if(pat_perturb&&erc==0){
      probe=arena_alloc(bytes);
      if(probe){
       unsigned span=2*pat_perturb+1;
       for(unsigned i=0;i<bytes;++i){
        int v=(int)image[i]+(int)(((i*2654435761u)>>16)%span)-
              (int)pat_perturb;
        if(v<0)v=0;if(v>255)v=255;probe[i]=(unsigned char)v;
       }
      }else probe=image;
     }
     unsigned char id=0xEE,v5=0xEE;
     int enmax=trace_nmax,enmin=trace_nmin,efeat=trace_featsz,etpl=trace_tplbin;
     trace_reset();
     uint64_t irc=((entry)(ta_map+0x9E1F0))((uint64_t)(uintptr_t)probe,1,
                                             (uint64_t)(uintptr_t)&id,
                                             (uint64_t)(uintptr_t)&v5);
     int inmax=trace_nmax,inmin=trace_nmin,imach=trace_mach,irmach=trace_rmach;
     int ifeat=trace_featsz,itpl=trace_tplbin,ifar=trace_far;
     uint64_t hash=1469598103934665603ULL;unsigned nonzero=0;
     if(db){const unsigned char *rec=(const unsigned char *)db;
      for(unsigned i=0;i<520;++i){hash^=rec[i];hash*=1099511628211ULL;
                                  if(rec[i])++nonzero;}}
     printf("{\"stage\":\"%s\",\"pattern\":[%u,%u,%u],"
            "\"image_bytes\":%u,\"enroll_rc\":%lld,"
            "\"template_count\":%u,\"template_nonzero\":%u,"
            "\"template_hash\":\"%016llx\",\"identify_rc\":%lld,"
            "\"id\":%u,\"v5\":%u,\"sfs_calls\":%u,\"spi_calls\":%u,"
            "\"db_header\":{\"count\":%u,\"flag\":%u,\"fid\":%u,\"acc\":%u,"
            "\"valid_rc\":%lld},"
            "\"enroll\":{\"nmax\":%d,\"nmin\":%d,\"feat\":%d,\"tplbin\":%d},"
            "\"identify\":{\"nmax\":%d,\"nmin\":%d,\"feat\":%d,\"tplbin\":%d,"
            "\"mach\":%d,\"rmach\":%d,\"far\":%d}}\n",
            case_name,pat_kind,pat_param,pat_perturb,bytes,(long long)erc,
            count,nonzero,(unsigned long long)hash,(long long)irc,
            id,v5,sfs_calls,spi_calls,
            count,flag,fid,acc,(long long)valid_rc,
            enmax,enmin,efeat,etpl,inmax,inmin,ifeat,itpl,imach,irmach,ifar);
     fflush(stdout);return 0;
    }
   }
   if(!strncmp(case_name,"enroll",6)
      &&image_instance==0){
    unsigned width=*(uint32_t *)cfg,height=*(uint32_t *)(cfg+4);
    image_bytes=width*height;
    unsigned char *image=arena_alloc(image_bytes);
    if(image){
     for(unsigned i=0;i<image_bytes;++i)
      image[i]=(unsigned char)pattern_value(pat_kind,pat_param,i%width,i/width,
                                            width,height,i);
     algorithm_ran=1;
     uint64_t rc=((entry)(ta_map+0x9b57c))(0,0,(uint64_t)(uintptr_t)image,0);
     enroll_rc=rc;
     if(rc==0){
      uintptr_t dbs=*(uintptr_t *)(ta_map+0x361540);
      uintptr_t db=dbs?*(uintptr_t *)dbs:0;
      if(db){
       template_count=*(uint16_t *)(db+50116);
       const unsigned char *record=(const unsigned char *)db;
       uint64_t h=1469598103934665603ULL;
       for(unsigned i=0;i<520;++i){h^=record[i];h*=1099511628211ULL;
                                   if(record[i])++template_nonzero;}
       template_hash=h;
      }
     }
     report(case_name,init_rc,config_rc,enroll_rc,image_bytes);
     return 0;
    }
   }
  }
 }
 report(case_name,init_rc,config_rc,enroll_rc,image_bytes);
 return 0;
}
