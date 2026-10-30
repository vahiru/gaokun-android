/* Exercise the original interp2 decoder/dispatcher with a fake transport only. */
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "hexagonrpcd/apps_std.c"
#include "hexagonrpcd/listener.c"
int fixture_fsync(int fd){return fsync(fd);}
ssize_t fixture_write(int fd,const void *p,size_t n){return write(fd,p,n);}
static char *encoded;static uint32_t encoded_len,request_sc;static int fetches,fetch_error,oversized_reply,short_reply,loop_mode,loop_next;
int fastrpc2(const struct fastrpc_function_def_interp2 *def,int fd,uint32_t handle,...){
 (void)fd;assert(handle==ADSP_LISTENER_HANDLE);va_list a;va_start(a,handle);
 if(def->msg_id==4){
  (void)va_arg(a,uint32_t);uint32_t previous=va_arg(a,uint32_t);uint32_t previous_size=va_arg(a,uint32_t);(void)va_arg(a,void*);
  if(loop_mode && loop_next++){assert(previous==20);assert(previous_size==0);va_end(a);return 9;}
  uint32_t *ctx=va_arg(a,uint32_t*),*h=va_arg(a,uint32_t*),*sc=va_arg(a,uint32_t*),*len=va_arg(a,uint32_t*);
  uint32_t capacity=va_arg(a,uint32_t);void *buf=va_arg(a,void*);assert(capacity==256);
  *ctx=71;*h=0;*sc=request_sc;*len=encoded_len;memcpy(buf,encoded,encoded_len<256?encoded_len:256);
 }else if(def->msg_id==5){
  ++fetches;assert(va_arg(a,uint32_t)==71);uint32_t offset=va_arg(a,uint32_t);assert(offset==256);
  uint32_t *required=va_arg(a,uint32_t*);uint32_t capacity=va_arg(a,uint32_t);char *buf=va_arg(a,char*);
  assert(capacity==encoded_len-256);*required=encoded_len+(oversized_reply?1:0)-(short_reply?1:0);
  if(fetch_error){va_end(a);return fetch_error;}
  memcpy(buf,encoded+offset,capacity-(short_reply?1:0));
 }else if(def->msg_id!=3) assert(0);
 va_end(a);return 0;
}
int main(void){
 char t[]="/tmp/hos-interp2-XXXXXX";assert(mkdtemp(t));char *base=realpath(t,NULL);assert(base);
 char sub[1024],file[1200];snprintf(sub,sizeof(sub),"%s/registry",base);assert(!mkdir(sub,0700));snprintf(file,sizeof(file),"%s/registry",sub);
 struct apps_std_ctx ctx={0};ctx.registry_output=true;struct hexagonfs_dirent root={.name="fixture",.ops=&hexagonfs_registry_ops,.u.phys=base};
 ctx.rootfd=hexagonfs_open_root(ctx.fds,&root);assert(ctx.rootfd>=0);ctx.adsp_library_dirfd=ctx.rootfd;
 int opened=apps_std_open_mode(&ctx,ctx.rootfd,"registry/registry","w+");assert(opened>=0);
 char data[4096];memset(data,'L',sizeof(data));uint32_t words[2]={(uint32_t)opened,sizeof(data)};
 struct fastrpc_io_buffer wire[2]={{.p=words,.s=sizeof(words)},{.p=data,.s=sizeof(data)}};
 encoded_len=(uint32_t)outbufs_calculate_size(2,wire);encoded=malloc(encoded_len);assert(encoded);outbufs_encode(2,wire,encoded);
 request_sc=REMOTE_SCALARS_MAKE(5,2,1);
 uint32_t rctx=0,handle=0,sc=0;struct fastrpc_io_buffer *decoded=NULL,*returned=NULL;
 assert(return_for_next_invoke(0,0,&rctx,&handle,&sc,NULL,&decoded,false)!=0);assert(fetches==0);
 sc=0;assert(!return_for_next_invoke(0,0,&rctx,&handle,&sc,NULL,&decoded,true));assert(fetches==1);
 assert(decoded[1].s==sizeof(data));assert(!memcmp(decoded[1].p,data,sizeof(data)));
 struct fastrpc_interface iface={.name="apps_std",.data=&ctx,.n_procs=32,.procs=registry_procs};struct fastrpc_interface *ifaces[1]={&iface};uint32_t status=99;
 assert(!invoke_requested_procedure(1,ifaces,0,sc,&status,decoded,&returned));assert(status==0);assert(((uint32_t*)returned[0].p)[0]==sizeof(data));
 iobuf_free(1,returned);returned=NULL;iobuf_free(2,decoded);decoded=NULL;
 assert(!hexagonfs_lseek(ctx.fds,opened,0,SEEK_SET));char actual[4096];assert(hexagonfs_read(ctx.fds,opened,sizeof(actual),actual)==sizeof(actual));assert(!memcmp(actual,data,sizeof(data)));
 /* Errors and oversized fetch declarations never decode or write a request. */
 fetch_error=77;sc=0;assert(return_for_next_invoke(0,0,&rctx,&handle,&sc,NULL,&decoded,true)!=0);assert(!decoded);
 fetch_error=0;oversized_reply=1;sc=0;assert(return_for_next_invoke(0,0,&rctx,&handle,&sc,NULL,&decoded,true)!=0);assert(!decoded);oversized_reply=0;
 uint32_t original_len=encoded_len;encoded_len=REGISTRY_MAX_INBUFS+1;int n=fetches;sc=0;
 assert(return_for_next_invoke(0,0,&rctx,&handle,&sc,NULL,&decoded,true)!=0);assert(fetches==n);encoded_len=original_len;
 /* Invalid embedded length is rejected before the legacy decoder allocates. */
 unsigned char saved[4];memcpy(saved,encoded,4);memset(encoded,0xff,4);sc=0;
 assert(return_for_next_invoke(0,0,&rctx,&handle,&sc,NULL,&decoded,true)!=0);assert(!decoded);memcpy(encoded,saved,4);
 short_reply=1;sc=0;assert(return_for_next_invoke(0,0,&rctx,&handle,&sc,NULL,&decoded,true)!=0);assert(!decoded);short_reply=0;
 /* Preserve legitimate stat31 while refusing unsupported full extended IDs. */
 char path[]="registry/registry";uint32_t args[3]={31,sizeof(path),0};
 struct fastrpc_io_buffer ext[2]={{.p=args,.s=2*sizeof(uint32_t)},{.p=path,.s=sizeof(path)}};
 sc=REMOTE_SCALARS_MAKE(31,2,1);status=99;
 assert(!invoke_requested_procedure(1,ifaces,0,sc,&status,ext,&returned));assert(status==0);iobuf_free(1,returned);returned=NULL;
 for(uint32_t method=32;method<=33;++method){args[0]=method;status=0;
    assert(invoke_requested_procedure(1,ifaces,0,sc,&status,ext,&returned)!=0);assert(status==20);assert(!returned);}
 /* A malformed producer that masks 32 into inline0 still cannot dispatch a
  * ftrunc-shaped primary buffer as fopen: validate counts/sizes first. */
 args[0]=32;args[1]=(uint32_t)opened;args[2]=0;ext[0].s=sizeof(args);status=0;
 assert(invoke_requested_procedure(1,ifaces,0,REMOTE_SCALARS_MAKE(32,1,0),&status,ext,&returned)!=0);assert(status==AEE_EBADPARM);
 assert(!hexagonfs_lseek(ctx.fds,opened,0,SEEK_SET));assert(hexagonfs_read(ctx.fds,opened,sizeof(actual),actual)==sizeof(actual));assert(!memcmp(actual,data,sizeof(data)));
 /* Actual default dispatcher still answers unsupported for new method 5. */
 iface.procs=apps_std_procs;status=0;assert(invoke_requested_procedure(1,ifaces,0,request_sc,&status,wire,&returned)!=0);assert(status==20);
 /* Opt-in listener actually sends rejection before the simulated transport
  * ends; direct status inspection alone would not prove a DSP reply. */
 iface.procs=registry_procs;free(encoded);uint32_t unsupported[3]={32,(uint32_t)opened,0};
 struct fastrpc_io_buffer unsupported_wire[1]={{.p=unsupported,.s=sizeof(unsupported)}};
 encoded_len=(uint32_t)outbufs_calculate_size(1,unsupported_wire);encoded=malloc(encoded_len);assert(encoded);outbufs_encode(1,unsupported_wire,encoded);
 request_sc=REMOTE_SCALARS_MAKE(31,1,0);loop_mode=1;loop_next=0;
 assert(run_fastrpc_listener(0,1,ifaces,true)==9);assert(loop_next==2);loop_mode=0;
 assert(!hexagonfs_lseek(ctx.fds,opened,0,SEEK_SET));assert(hexagonfs_read(ctx.fds,opened,sizeof(actual),actual)==sizeof(actual));assert(!memcmp(actual,data,sizeof(data)));
 /* Qualcomm apps_std_skel method33 uses three input buffers and no output:
  * [33, old-length, new-length], NUL-terminated old path, new path. This tests
  * only rejection; these fixture names do not authorize any real rename pair. */
 int staged=hexagonfs_create(ctx.fds,ctx.rootfd,ctx.rootfd,"temp.json",O_RDWR|O_CREAT);assert(staged>=0);
 assert(hexagonfs_write(ctx.fds,staged,5,"STAGE")==5);assert(!hexagonfs_close(ctx.fds,staged));
 char oldpath[]="/temp.json",newpath[]="/registry/registry";
 uint32_t rename_words[3]={33,sizeof(oldpath),sizeof(newpath)};
 struct fastrpc_io_buffer rename_wire[3]={{.p=rename_words,.s=sizeof(rename_words)},
   {.p=oldpath,.s=sizeof(oldpath)},{.p=newpath,.s=sizeof(newpath)}};
 free(encoded);encoded_len=(uint32_t)outbufs_calculate_size(3,rename_wire);encoded=malloc(encoded_len);assert(encoded);outbufs_encode(3,rename_wire,encoded);
 request_sc=REMOTE_SCALARS_MAKE(31,3,0);sc=0;decoded=NULL;
 assert(!return_for_next_invoke(0,0,&rctx,&handle,&sc,NULL,&decoded,true));status=0;
 assert(invoke_requested_procedure(1,ifaces,0,sc,&status,decoded,&returned)!=0);assert(status==20);assert(!returned);
 iobuf_free(3,decoded);decoded=NULL;
 staged=hexagonfs_openat(ctx.fds,ctx.rootfd,ctx.rootfd,"temp.json");assert(staged>=0);char stage[6]={0};
 assert(hexagonfs_read(ctx.fds,staged,5,stage)==5);assert(!strcmp(stage,"STAGE"));assert(!hexagonfs_close(ctx.fds,staged));
 assert(!hexagonfs_lseek(ctx.fds,opened,0,SEEK_SET));assert(hexagonfs_read(ctx.fds,opened,sizeof(actual),actual)==sizeof(actual));assert(!memcmp(actual,data,sizeof(data)));
 char temporary[1200];snprintf(temporary,sizeof(temporary),"%s/temp.json",base);assert(!unlink(temporary));
 puts("PASS official method33 complete framing is rejected with 20; source STAGE and destination 4096 bytes both unchanged");
 assert(!hexagonfs_close(ctx.fds,opened));assert(!hexagonfs_close(ctx.fds,ctx.rootfd));unlink(file);rmdir(sub);rmdir(base);free(base);free(encoded);
 puts("PASS old interp2 real framing/dispatch: opt-in 4096B exact write; default method5=20 and no large fetch; fetch errors/short reply/size cap reject, extended32/33 and malformed masked32 never mutate");
}
