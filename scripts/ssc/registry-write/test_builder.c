/* Actual VFS builder alias wiring: only explicit registry output is writable. */
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hexagonrpcd/rpcd_builder.h"
static struct hexagonfs_dirent *graphs[2]; /* Builder owns a persistent graph. */
int fixture_fsync(int fd){return fsync(fd);}
ssize_t fixture_write(int fd,const void *p,size_t n){return write(fd,p,n);}
int main(void){
 char t[]="/tmp/hos-builder-XXXXXX";assert(mkdtemp(t));char *base=realpath(t,NULL);assert(base);
 char vendor[1024],output[1024],p[1200],json[1200],original[1200];
 snprintf(vendor,sizeof(vendor),"%s/vendor",base);assert(!mkdir(vendor,0700));
 snprintf(p,sizeof(p),"%s/sensors",vendor);assert(!mkdir(p,0700));
 snprintf(p,sizeof(p),"%s/sensors/config",vendor);assert(!mkdir(p,0700));
 snprintf(json,sizeof(json),"%s/sensors/config/fac_cal.json",vendor);int fd=open(json,O_CREAT|O_EXCL|O_WRONLY,0600);assert(fd>=0);assert(write(fd,"UNCHANGED",9)==9);close(fd);
 snprintf(p,sizeof(p),"%s/sensors/registry",vendor);assert(!mkdir(p,0700));
 snprintf(original,sizeof(original),"%s/sensors/registry/registry",vendor);fd=open(original,O_CREAT|O_EXCL|O_WRONLY,0600);assert(fd>=0);close(fd);
 snprintf(output,sizeof(output),"%s/output",base);assert(!mkdir(output,0700));snprintf(p,sizeof(p),"%s/registry",output);assert(!mkdir(p,0700));
 const char *aliases[]={"/persist/sensors/registry/registry/registry","/mnt/vendor/persist/sensors/registry/registry/registry"};
 for(int enabled=0;enabled<2;++enabled){
  struct hexagonfs_fd *fds[HEXAGONFS_MAX_FD]={0};
  graphs[enabled]=construct_root_dir(vendor,"sdsp",enabled?strdup(output):NULL);assert(graphs[enabled]);
  int r=hexagonfs_open_root(fds,graphs[enabled]);assert(r>=0);
  for(unsigned j=0;j<2;++j){int f=hexagonfs_create(fds,r,r,aliases[j],O_RDWR|O_CREAT|O_TRUNC);
   if(!enabled)assert(f<0);else{assert(f>=0);assert(hexagonfs_write(fds,f,4,"DATA")==4);assert(!hexagonfs_sync(fds,f));
    struct hexagonfs_fd *independent[HEXAGONFS_MAX_FD]={0};
    int r2=hexagonfs_open_root(independent,graphs[enabled]);assert(r2>=0);
    for(int append=0;append<2;++append){int flags=O_RDWR|O_CREAT|(append?O_APPEND:O_TRUNC);
      assert(hexagonfs_create(fds,r,r,aliases[1-j],flags)==-EWOULDBLOCK);
      assert(hexagonfs_create(independent,r2,r2,aliases[1-j],flags)==-EWOULDBLOCK);}
    char intact[5]={0};assert(!hexagonfs_lseek(fds,f,0,SEEK_SET));
    assert(hexagonfs_read(fds,f,4,intact)==4);assert(!strcmp(intact,"DATA"));
    assert(!hexagonfs_close(independent,r2));assert(!hexagonfs_close(fds,f));}}
  int f=hexagonfs_openat(fds,r,r,"/vendor/etc/sensors/config/fac_cal.json");assert(f>=0);char data[10]={0};assert(hexagonfs_read(fds,f,9,data)==9);assert(!strcmp(data,"UNCHANGED"));assert(!hexagonfs_close(fds,f));
  assert(hexagonfs_create(fds,r,r,"/vendor/etc/sensors/config/fac_cal.json",O_RDWR|O_TRUNC)<0);
  assert(hexagonfs_create(fds,r,r,"/persist/sensors/registry/registry/new-group",O_WRONLY|O_CREAT)<0);
  assert(hexagonfs_close(fds,r)==0);
 }
 struct stat st;assert(!stat(original,&st));assert(st.st_size==0);
 snprintf(p,sizeof(p),"%s/registry/registry",output);unlink(p);snprintf(p,sizeof(p),"%s/registry",output);rmdir(p);rmdir(output);
 unlink(json);unlink(original);snprintf(p,sizeof(p),"%s/sensors/config",vendor);rmdir(p);snprintf(p,sizeof(p),"%s/sensors/registry",vendor);rmdir(p);snprintf(p,sizeof(p),"%s/sensors",vendor);rmdir(p);rmdir(vendor);rmdir(base);free(base);
 puts("PASS actual VFS builder: opt-in only, both aliases/independent roots reject second append/truncate writer before mutation, untouched config/calibration/old registry");
}
