/* Actual multi-file, multi-thread quota enforcement; no DSP or device access. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hexagonrpcd/hexagonfs.h"
#include "hexagonrpcd/registry_names.h"
int fixture_fsync(int fd){return fsync(fd);}
ssize_t fixture_write(int fd,const void *p,size_t n){return write(fd,p,n);}
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static int ready,start;
struct writer {struct hexagonfs_fd **fds;int fd;ssize_t result;char *data;};
static void *write_one(void *arg){struct writer *w=arg;
 pthread_mutex_lock(&lock);++ready;pthread_cond_broadcast(&cond);while(!start)pthread_cond_wait(&cond,&lock);pthread_mutex_unlock(&lock);
 w->result=hexagonfs_write(w->fds,w->fd,256*1024,w->data);return NULL;}
int main(void){
 char t[]="/tmp/hos-quota-XXXXXX";assert(mkdtemp(t));char *base=realpath(t,NULL);assert(base);
 char sub[1024],path[1200],virtual[512];snprintf(sub,sizeof(sub),"%s/registry",base);assert(!mkdir(sub,0700));
 struct hexagonfs_fd *fds[HEXAGONFS_MAX_FD]={0};struct hexagonfs_dirent root={.name="fixture",.ops=&hexagonfs_registry_ops,.u.phys=base};
 int r=hexagonfs_open_root(fds,&root);assert(r>=0);assert(REGISTRY_NAME_COUNT==142);
 int writers[2];
 for(size_t i=0;i<REGISTRY_NAME_COUNT;++i){snprintf(virtual,sizeof(virtual),"registry/%s",registry_names[i]);
  int f=hexagonfs_create(fds,r,r,virtual,O_RDWR|O_CREAT);assert(f>=0);
  if(i<7)assert(!hexagonfs_ftruncate(fds,f,256*1024));
  if(i==7||i==8)writers[i-7]=f;else assert(!hexagonfs_close(fds,f));}
 int temp=hexagonfs_create(fds,r,r,"temp.json",O_RDWR|O_CREAT);assert(temp>=0);assert(!hexagonfs_close(fds,temp));
 /* Exactly 143 allowed files exist; no additional filename is permitted. */
 assert(hexagonfs_create(fds,r,r,"registry/unreviewed",O_WRONLY|O_CREAT)<0);
 char *data=malloc(256*1024);assert(data);memset(data,0x5a,256*1024);
 for(int iteration=0;iteration<16;++iteration){
  for(int i=0;i<2;++i){assert(!hexagonfs_ftruncate(fds,writers[i],0));assert(!hexagonfs_lseek(fds,writers[i],0,SEEK_SET));}
  struct writer w[2]={{fds,writers[0],0,data},{fds,writers[1],0,data}};pthread_t th[2];ready=0;start=0;
  for(int i=0;i<2;++i)assert(!pthread_create(&th[i],NULL,write_one,&w[i]));
  pthread_mutex_lock(&lock);while(ready<2)pthread_cond_wait(&cond,&lock);start=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&lock);
  for(int i=0;i<2;++i)assert(!pthread_join(th[i],NULL));
  int success=0;for(int i=0;i<2;++i){if(w[i].result==256*1024)++success;else assert(w[i].result==-EDQUOT||w[i].result==-EWOULDBLOCK);}
  assert(success==1);
  size_t total=0;for(size_t i=0;i<REGISTRY_NAME_COUNT;++i){snprintf(path,sizeof(path),"%s/%s",sub,registry_names[i]);struct stat st;assert(!stat(path,&st));total+=(size_t)st.st_size;}
  assert(total==2*1024*1024);
 }
 assert(hexagonfs_registry_validate_new_root(base)<0);
 for(int i=0;i<2;++i)assert(!hexagonfs_close(fds,writers[i]));assert(!hexagonfs_close(fds,r));free(data);
 for(size_t i=0;i<REGISTRY_NAME_COUNT;++i){snprintf(path,sizeof(path),"%s/%s",sub,registry_names[i]);assert(!unlink(path));}
 snprintf(path,sizeof(path),"%s/temp.json",base);unlink(path);rmdir(sub);rmdir(base);free(base);
 puts("PASS exact 142 group/sentinel + temp names, 143-file bound, 2MiB quota across racing distinct-file writers (16 runs)");
}
