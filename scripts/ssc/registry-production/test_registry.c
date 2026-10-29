/* Offline fixture only. Link the actual candidate hexagonfs C sources. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hexagonrpcd/hexagonfs.h"
static int sync_fail_at, sync_calls;
static int write_mode, write_calls;
ssize_t fixture_write(int fd,const void *buf,size_t n) {
    ++write_calls;
    if(write_mode==1 && write_calls==1){errno=EINTR;return -1;}
    if(write_mode==2 && write_calls==2){errno=ENOSPC;return -1;}
    if(write_mode==3)return 0;
    if((write_mode==1 && write_calls==2) || (write_mode==2 && write_calls==1))
        return write(fd,buf,n>1?n/2:n);
    return write(fd,buf,n);
}
int fixture_fsync(int fd) {
    ++sync_calls;
    if (sync_fail_at==sync_calls) { errno=EIO; return -1; }
    return fsync(fd);
}
static int openroot(struct hexagonfs_fd **fds,const char *path,bool writable) {
    struct hexagonfs_dirent e={.name="fixture",.ops=writable?&hexagonfs_registry_ops:&hexagonfs_mapped_ops,.u.phys=path};
    return hexagonfs_open_root(fds,&e);
}
static int modeopen(struct hexagonfs_fd **fds,int root,const char *path,const char *mode) {
    int flags, ret=hexagonfs_parse_mode(mode,&flags);if(ret)return ret;
    return (flags&O_ACCMODE)==O_RDONLY?hexagonfs_openat(fds,root,root,path):hexagonfs_create(fds,root,root,path,flags);
}
static void putfile(const char *path,const char *value) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);
    assert(write(fd,value,strlen(value))==(ssize_t)strlen(value));assert(!close(fd));
}
static void checkfile(const char *path,const char *value) {
    char b[128]={0};int fd=open(path,O_RDONLY);assert(fd>=0);
    ssize_t n=read(fd,b,sizeof(b));assert(n==(ssize_t)strlen(value));assert(!memcmp(b,value,n));close(fd);
}
int main(void) {
    char name[]="/tmp/hos-registry-fixture-XXXXXX";assert(mkdtemp(name));
    char *base=realpath(name,NULL);assert(base);
    char root[1024],sub[1024],file[1024],outside[1024],linkpath[1024],other[1024];
    snprintf(root,sizeof(root),"%s/output",base);assert(!mkdir(root,0700));
    snprintf(sub,sizeof(sub),"%s/registry",root);assert(!mkdir(sub,0700));
    snprintf(file,sizeof(file),"%s/registry",sub);
    snprintf(outside,sizeof(outside),"%s/sentinel",base);putfile(outside,"KEEP");
    snprintf(other,sizeof(other),"%s/other",sub);
    assert(!hexagonfs_registry_validate_new_root(root));
    struct hexagonfs_fd *fds[HEXAGONFS_MAX_FD]={0};int r=openroot(fds,root,true);assert(r>=0);
    const char *valid[]={"r","rb","r+","rb+","r+b","w","wb","w+","wb+","w+b","a","ab","a+","ab+","a+b"};
    int flags;
    for(size_t i=0;i<sizeof(valid)/sizeof(*valid);++i)assert(!hexagonfs_parse_mode(valid[i],&flags));
    const char *bad[]={"","x","rw","r++","rbb","+r","wxyz","rbe","w x","r\r"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i)assert(hexagonfs_parse_mode(bad[i],&flags)<0);
    assert(modeopen(fds,r,"registry/registry","r+")<0); /* Never creates. */
    int f=modeopen(fds,r,"registry/registry","w+");assert(f>=0);
    assert(hexagonfs_write(fds,f,3,"abc")==3);assert(!hexagonfs_sync(fds,f));
    assert(!hexagonfs_lseek(fds,f,0,SEEK_SET));char b[4]={0};assert(hexagonfs_read(fds,f,3,b)==3);assert(!strcmp(b,"abc"));
    assert(!hexagonfs_ftruncate(fds,f,0));assert(!hexagonfs_lseek(fds,f,0,SEEK_SET));
    write_mode=1;write_calls=0;assert(hexagonfs_write(fds,f,3,"abc")==3);write_mode=0;
    assert(!hexagonfs_ftruncate(fds,f,0));assert(!hexagonfs_lseek(fds,f,0,SEEK_SET));
    write_mode=2;write_calls=0;assert(hexagonfs_write(fds,f,3,"abc")==-ENOSPC);write_mode=0;checkfile(file,"a");
    write_mode=3;assert(hexagonfs_write(fds,f,1,"x")==-EIO);write_mode=0;
    assert(!hexagonfs_ftruncate(fds,f,0));assert(!hexagonfs_lseek(fds,f,0,SEEK_SET));assert(hexagonfs_write(fds,f,3,"abc")==3);
    int concurrent=modeopen(fds,r,"registry/registry","w");assert(concurrent<0);checkfile(file,"abc");
    /* Both file and containing directory fsync failures must propagate. */
    sync_fail_at=sync_calls+1;assert(hexagonfs_sync(fds,f)==-EIO);
    sync_fail_at=sync_calls+2;assert(hexagonfs_sync(fds,f)==-EIO);
    sync_fail_at=sync_calls+1;assert(hexagonfs_close(fds,f)==-EIO);
    sync_fail_at=0;assert(!hexagonfs_close(fds,f));
    f=modeopen(fds,r,"registry/registry","a");assert(f>=0);assert(hexagonfs_write(fds,f,1,"d")==1);
    assert(hexagonfs_read(fds,f,1,b)==-EBADF);assert(!hexagonfs_close(fds,f));checkfile(file,"abcd");assert(hexagonfs_registry_validate_new_root(root)<0);
    int directory=hexagonfs_openat(fds,r,r,"registry");assert(directory>=0);
    int child=hexagonfs_openat(fds,r,directory,"registry");assert(child>=0);
    assert(!hexagonfs_close(fds,directory));char live[5]={0};assert(hexagonfs_read(fds,child,4,live)==4);assert(!strcmp(live,"abcd"));assert(!hexagonfs_close(fds,child));
    f=modeopen(fds,r,"registry/registry","r");assert(f>=0);assert(hexagonfs_write(fds,f,1,"x")<0);sync_fail_at=sync_calls+1;assert(hexagonfs_sync(fds,f)==-EIO);sync_fail_at=0;assert(!hexagonfs_sync(fds,f));assert(!hexagonfs_close(fds,f));
    f=modeopen(fds,r,"registry\r/registry\r","r+");assert(f>=0);
    assert(!hexagonfs_ftruncate(fds,f,2));assert(!hexagonfs_lseek(fds,f,256*1024,SEEK_SET));
    assert(hexagonfs_write(fds,f,1,"x")==-EFBIG);assert(hexagonfs_ftruncate(fds,f,256*1024+1)==-EFBIG);
    assert(!hexagonfs_close(fds,f));checkfile(file,"ab");
    const char *paths[]={"../sentinel","registry/../registry/registry","registry/..\r/registry/registry","/etc/passwd","registry/other","registry/registry.tmp","registry","registry/registry/registry"};
    for(size_t i=0;i<sizeof(paths)/sizeof(*paths);++i)assert(modeopen(fds,r,paths[i],"w")<0);
    /* No rename/unlink capability or RPC method is added. */
    assert(!hexagonfs_close(fds,r));
    /* Generic maps stay read-only even when the host fixture is writable. */
    memset(fds,0,sizeof(fds));r=openroot(fds,base,false);assert(r>=0);
    for(const char **m=(const char*[]){"r+","w","w+","a+",NULL};*m;++m)assert(modeopen(fds,r,"sentinel",*m)<0);
    f=modeopen(fds,r,"sentinel","r");assert(f>=0);assert(hexagonfs_write(fds,f,1,"x")<0);
    assert(!hexagonfs_close(fds,f));assert(!hexagonfs_close(fds,r));checkfile(outside,"KEEP");
    /* Final symlink and hardlink are rejected before any truncation. */
    assert(!unlink(file));assert(!symlink(outside,file));memset(fds,0,sizeof(fds));assert(openroot(fds,root,true)<0);
    assert(!unlink(file));assert(!link(outside,file));assert(openroot(fds,root,true)<0);checkfile(outside,"KEEP");assert(!unlink(file));
    /* Intermediate symlink and symlink in the trusted CLI root are rejected. */
    assert(!rmdir(sub));assert(!symlink(base,sub));assert(openroot(fds,root,true)<0);assert(!unlink(sub));assert(!mkdir(sub,0700));
    snprintf(linkpath,sizeof(linkpath),"%s/root-link",base);assert(!symlink(root,linkpath));assert(openroot(fds,linkpath,true)<0);unlink(linkpath);
    /* Unknown pre-existing entries reject the output tree; config cannot be reused. */
    putfile(other,"DO NOT MODIFY");assert(openroot(fds,root,true)<0);checkfile(other,"DO NOT MODIFY");unlink(other);
    /* Race-like substitution after initial open is checked again on write-open. */
    r=openroot(fds,root,true);assert(r>=0);assert(!symlink(outside,file));assert(modeopen(fds,r,"registry/registry","w")<0);
    checkfile(outside,"KEEP");assert(!hexagonfs_close(fds,r));unlink(file);
    unlink(outside);rmdir(sub);rmdir(root);rmdir(base);free(base);
    puts("PASS modes, readonly maps, bounded/short/EINTR/ENOSPC writes, fd modes, sync/close errors, traversal, links, unknown names, no rename/unlink");
}
