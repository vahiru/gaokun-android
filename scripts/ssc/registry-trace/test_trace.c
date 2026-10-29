#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include "hexagonrpcd/apps_std.c"
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned logs;
static char last[1400];
int __android_log_print(int priority, const char *tag, const char *format, ...) {
    assert(priority==4 && !strcmp(tag,"hexagonrpcd-denied"));
    pthread_mutex_lock(&lock); ++logs;
    va_list ap; va_start(ap,format);vsnprintf(last,sizeof(last),format,ap);va_end(ap);
    assert(!strchr(last,'\n') && !strchr(last,'\r'));
    pthread_mutex_unlock(&lock);return 0;
}
static uint32_t invoke(struct apps_std_ctx *ctx,const char *path,const char *mode) {
    char env[]="ADSP_LIBRARY_PATH";uint32_t result=0xdeadbeef;
    struct fastrpc_io_buffer in[5]={{0},{.p=env,.s=sizeof(env)},{0},{.p=(void*)path,.s=strlen(path)+1},{.p=(void*)mode,.s=strlen(mode)+1}};
    struct fastrpc_io_buffer out[1]={{.p=&result,.s=4}};
    uint32_t status=apps_std_fopen_with_env(ctx,in,out);
    if(status)assert(result==0xdeadbeef);
    return status;
}
static void *hammer(void *ctx){for(int i=0;i<1000;++i)assert(invoke(ctx,"/missing","w")==20);return NULL;}
int main(void) {
    struct apps_std_ctx ctx={0};ctx.rootfd=-1;ctx.adsp_library_dirfd=-1;
    assert(invoke(&ctx,"/missing","w")==20);assert(invoke(&ctx,"/missing","a")==20);
    assert(invoke(&ctx,"/missing","r")==AEE_EFAILED);
    assert(invoke(&ctx,"/test\r/\"\\\x01","w+\n")==20);
    assert(strstr(last,"/test\\x0d/\\x22\\x5c\\x01"));assert(strstr(last,"w+\\x0a"));
    char path[600];memset(path,'Q',sizeof(path));path[sizeof(path)-1]=0;
    assert(invoke(&ctx,path,"a")==20);
    char *start=strstr(last,"path=\"")+6;assert(strlen(start)==257);assert(start[256]=='"');
    char env[]="ADSP_LIBRARY_PATH",file[]="/x",mode[]="w";uint32_t result;
    struct fastrpc_io_buffer in[5]={{0},{.p=env,.s=sizeof(env)},{0},{.p=file,.s=sizeof(file)},{.p=mode,.s=sizeof(mode)}};
    struct fastrpc_io_buffer out[1]={{.p=&result,.s=4}};
    for(int k=1;k<=4;++k){if(k==2)continue;struct fastrpc_io_buffer saved=in[k];
        in[k].s=0;assert(apps_std_fopen_with_env(&ctx,in,out)==AEE_EBADPARM);in[k]=saved;
        in[k].p=NULL;assert(apps_std_fopen_with_env(&ctx,in,out)==AEE_EBADPARM);in[k]=saved;
        in[k].s--;assert(apps_std_fopen_with_env(&ctx,in,out)==AEE_EBADPARM);in[k]=saved;}
    out[0].s=3;assert(apps_std_fopen_with_env(&ctx,in,out)==AEE_EBADPARM);
    out[0].s=4;out[0].p=NULL;assert(apps_std_fopen_with_env(&ctx,in,out)==AEE_EBADPARM);
    assert(apps_std_fopen_with_env(&ctx,NULL,out)==AEE_EBADPARM);
    assert(apps_std_fopen_with_env(&ctx,in,NULL)==AEE_EBADPARM);
    char controls[300];memset(controls,'\r',sizeof(controls));controls[299]=0;
    char escaped[257];escape_denied_path(escaped,controls,256);assert(strlen(escaped)==256);
    for(int i=0;i<64;++i)assert(!memcmp(escaped+i*4,"\\x0d",4));
    char temp[]="/tmp/hos-trace-XXXXXX";assert(mkdtemp(temp));
    char file_name[512];snprintf(file_name,sizeof(file_name),"%s/sentinel",temp);
    FILE *f=fopen(file_name,"wb");assert(f);assert(fwrite("unchanged",1,9,f)==9);assert(!fclose(f));
    struct hexagonfs_dirent root={.name="fixture",.ops=&hexagonfs_mapped_ops,.u.phys=temp};
    ctx.rootfd=hexagonfs_open_root(ctx.fds,&root);assert(ctx.rootfd>=0);ctx.adsp_library_dirfd=ctx.rootfd;
    assert(invoke(&ctx,"/sentinel","w+")==20);assert(invoke(&ctx,"/new-file","a")==20);
    assert(invoke(&ctx,"/missing","r")==AEE_EFAILED);
    assert(invoke(&ctx,"/sentinel\r","r")==AEE_SUCCESS); // Production CR treefix composed.
    char observed[16]={0};f=fopen(file_name,"rb");assert(f);assert(fread(observed,1,sizeof(observed),f)==9);fclose(f);assert(!strcmp(observed,"unchanged"));
    char new_file[512];snprintf(new_file,sizeof(new_file),"%s/new-file",temp);assert(access(new_file,F_OK)==-1);
    for(int i=HEXAGONFS_MAX_FD-1;i>=0;--i)if(ctx.fds[i])hexagonfs_close(ctx.fds,i);
    assert(!unlink(file_name));assert(!rmdir(temp));
    pthread_t threads[8];for(int i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,hammer,&ctx));
    for(int i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
    assert(logs==64 && atomic_load(&denied_open_logs)==64);
    puts("PASS original w/a=20; read failure unchanged; RPC bounds; escaped CR; 256-byte cap; 64 process logs under contention");
}
