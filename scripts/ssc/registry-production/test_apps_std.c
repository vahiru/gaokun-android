/* Calls the actual pinned/patched apps_std dispatch methods; no DSP involved. */
#include <assert.h>
#include <sys/stat.h>
#include "hexagonrpcd/apps_std.c"
static int fail_sync, write_mode, write_calls;
int fixture_fsync(int fd){if(fail_sync){errno=EIO;return -1;}return fsync(fd);}
ssize_t fixture_write(int fd,const void *buf,size_t n){
    ++write_calls;
    if(write_mode==1 && write_calls==1){errno=EINTR;return -1;}
    if(write_mode==2 && write_calls==2){errno=ENOSPC;return -1;}
    if(write_mode==3)return 0;
    if((write_mode==1 && write_calls==2)||(write_mode==2 && write_calls==1))return write(fd,buf,n>1?n/2:n);
    return write(fd,buf,n);
}
static int call_open(struct apps_std_ctx *ctx,const char *path,const char *mode,uint32_t *outfd){
    struct fastrpc_io_buffer in[3]={{0},{.p=(void*)path,.s=strlen(path)+1},{.p=(void*)mode,.s=strlen(mode)+1}};
    struct fastrpc_io_buffer out[1]={{.p=outfd,.s=sizeof(*outfd)}};
    return registry_procs[0].impl(ctx,in,out);
}
static void call_write(struct apps_std_ctx *ctx,uint32_t fd,int mode){
    uint32_t words[2]={fd,3}, result[2]={0xdeadbeef,0xdeadbeef};
    char bytes[]="abc";
    struct fastrpc_io_buffer in[2]={{.p=words,.s=sizeof(words)},{.p=bytes,.s=3}};
    struct fastrpc_io_buffer out[1]={{.p=result,.s=sizeof(result)}};
    write_mode=mode;write_calls=0;
    uint32_t status=registry_procs[5].impl(ctx,in,out);
    printf("apps_std_fwrite mode=%d status=%u written=%u\n",mode,status,result[0]);
    if(mode==0||mode==1){assert(status==AEE_SUCCESS);assert(result[0]==3);}
    else {assert(status==AEE_EFAILED);assert(result[0]==0);}
    write_mode=0;
    words[1]=4;assert(registry_procs[5].impl(ctx,in,out)==AEE_EBADPARM);
    in[0].s=0;assert(registry_procs[5].impl(ctx,in,out)==AEE_EBADPARM);
}
int main(void){
    char temp[]="/tmp/hos-apps-std-XXXXXX";assert(mkdtemp(temp));char *base=realpath(temp,NULL);assert(base);
    char sub[1024],file[1024];snprintf(sub,sizeof(sub),"%s/registry",base);assert(!mkdir(sub,0700));snprintf(file,sizeof(file),"%s/registry",sub);
    struct apps_std_ctx ctx={0};ctx.registry_output=true;
    struct hexagonfs_dirent root={.name="fixture",.ops=&hexagonfs_registry_ops,.u.phys=base};
    ctx.rootfd=hexagonfs_open_root(ctx.fds,&root);assert(ctx.rootfd>=0);ctx.adsp_library_dirfd=ctx.rootfd;
    uint32_t fd;
    assert(call_open(&ctx,"/registry/registry","wxyz",&fd)==AEE_EFAILED);
    assert(call_open(&ctx,"/registry/registry.tmp","w",&fd)==AEE_EFAILED);
    assert(call_open(&ctx,"/registry/registry","r+",&fd)==AEE_EFAILED);
    assert(call_open(&ctx,"/registry/registry","w+",&fd)==AEE_SUCCESS);
    for(int m=0;m<4;++m){assert(!hexagonfs_ftruncate(ctx.fds,fd,0));assert(!hexagonfs_lseek(ctx.fds,fd,0,SEEK_SET));call_write(&ctx,fd,m);
        if(m==2){char actual[4]={0};int observed=open(file,O_RDONLY);assert(observed>=0);
            assert(read(observed,actual,sizeof(actual))==1);assert(actual[0]=='a');close(observed);
            puts("PARTIAL_FAILURE actual_file_bytes=1 contents=a despite status=1/written=0 (not atomic)");}}
    uint32_t readonly;
    assert(call_open(&ctx,"/registry/registry","r",&readonly)==AEE_SUCCESS);
    struct fastrpc_io_buffer ri[1]={{.p=&readonly,.s=sizeof(readonly)}};
    uint32_t rr=0;struct fastrpc_io_buffer ro[1]={{.p=&rr,.s=sizeof(rr)}};
    fail_sync=1;assert(registry_procs[23].impl(&ctx,ri,ro)==AEE_EFAILED);
    assert(registry_procs[3].impl(&ctx,ri,ro)==AEE_EFAILED);
    fail_sync=0;assert(registry_procs[23].impl(&ctx,ri,ro)==AEE_SUCCESS);
    assert(registry_procs[3].impl(&ctx,ri,ro)==AEE_SUCCESS);
    uint32_t result=0,word=fd;
    struct fastrpc_io_buffer in[1]={{.p=&word,.s=sizeof(word)}},out[1]={{.p=&result,.s=sizeof(result)}};
    fail_sync=1;assert(registry_procs[23].impl(&ctx,in,out)==AEE_EFAILED);
    assert(registry_procs[3].impl(&ctx,in,out)==AEE_EFAILED);assert(ctx.fds[fd]);
    fail_sync=0;assert(registry_procs[23].impl(&ctx,in,out)==AEE_SUCCESS);
    /* Oversized writes and ftrunc surface failure rather than success. */
    assert(!hexagonfs_lseek(ctx.fds,fd,256*1024,SEEK_SET));
    uint32_t w[2]={fd,1},o[2]={99,99};char c='x';
    struct fastrpc_io_buffer wi[2]={{.p=w,.s=sizeof(w)},{.p=&c,.s=1}},wo[1]={{.p=o,.s=sizeof(o)}};
    assert(registry_procs[5].impl(&ctx,wi,wo)==AEE_EFAILED);assert(o[0]==0);
    in[0].p=&word;in[0].s=sizeof(word);assert(registry_procs[3].impl(&ctx,in,out)==AEE_SUCCESS);
    assert(!hexagonfs_close(ctx.fds,ctx.rootfd));
    memset(&ctx,0,sizeof(ctx));root.ops=&hexagonfs_mapped_ops;
    ctx.rootfd=hexagonfs_open_root(ctx.fds,&root);ctx.adsp_library_dirfd=ctx.rootfd;assert(ctx.rootfd>=0);
    assert(!apps_std_procs[0].impl && !apps_std_procs[5].impl && !apps_std_procs[23].impl);
    const char *modes[]={"w","wb","w+","a","a+","wxyz"};
    for(size_t i=0;i<sizeof(modes)/sizeof(*modes);++i){
      char env[]="ADSP_LIBRARY_PATH",separator[]=";",name[]="/registry/registry";
      uint32_t lens[4]={sizeof(env),sizeof(separator),sizeof(name),(uint32_t)strlen(modes[i])+1}, opened=0;
      struct fastrpc_io_buffer ei[5]={{.p=lens,.s=sizeof(lens)},{.p=env,.s=sizeof(env)},
        {.p=separator,.s=sizeof(separator)},{.p=name,.s=sizeof(name)},
        {.p=(void*)modes[i],.s=(uint32_t)strlen(modes[i])+1}};
      struct fastrpc_io_buffer eo[1]={{.p=&opened,.s=sizeof(opened)}};
      assert(apps_std_procs[19].impl(&ctx,ei,eo)==AEE_EUNSUPPORTED);
      assert(AEE_EUNSUPPORTED==20);
    }
    /* Production historically treats r+ as a read-only open: retain it. */
    char env[]="ADSP_LIBRARY_PATH",sep[]=";",name[]="/registry/registry",mode[]="r+";
    uint32_t lens[4]={sizeof(env),sizeof(sep),sizeof(name),sizeof(mode)},old_read=0;
    struct fastrpc_io_buffer ei[5]={{.p=lens,.s=sizeof(lens)},{.p=env,.s=sizeof(env)},
      {.p=sep,.s=sizeof(sep)},{.p=name,.s=sizeof(name)},{.p=mode,.s=sizeof(mode)}};
    struct fastrpc_io_buffer eo[1]={{.p=&old_read,.s=sizeof(old_read)}};
    assert(apps_std_procs[19].impl(&ctx,ei,eo)==0);
    assert(hexagonfs_write(ctx.fds,old_read,1,"x")<0);assert(!hexagonfs_close(ctx.fds,old_read));
    assert(!hexagonfs_close(ctx.fds,ctx.rootfd));unlink(file);rmdir(sub);rmdir(base);free(base);
    puts("PASS apps_std real dispatch: exact byte count, short/EINTR completion, zero/ENOSPC/quota/fsync/close failures including read-only handles, modes, default production w/a=20 and unchanged default dispatch");
}
