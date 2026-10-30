#!/usr/bin/env python3
"""Actual module-init/run_probe extraction with mock I/O; never waits on hardware."""
import os
from pathlib import Path
import subprocess
import tempfile

source=Path(__file__).with_name('qcom_qseecom_fpversion.c').read_text()
assert 'module_param(contact_wait_ms, uint, 0444);' in source
assert 'module_param(contact_poll_count, uint, 0444);' in source


def function(begin,end):
    start=source.index(begin)
    return source[start:source.index(end,start+len(begin))]


body=(function('static int validate_contact_window(void)','\nstatic int __init fpversion_init')+
      function('static int __init fpversion_init(void)','\nstatic void __exit fpversion_exit')+
      function('static int run_probe(void)','\nstatic ssize_t probe_write'))
prefix=r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint32_t __le32;
#define __init
#define FP_REQ_SIZE 12
#define FP_RSP_SIZE 16
#define THIS_MODULE 0
#define KERN_INFO 0
#define DUMP_PREFIX_OFFSET 0
#define le32_to_cpu(x) (x)
#define print_hex_dump(...) ((void)0)
#define IS_ERR(p) (false)
#define IS_ERR_OR_NULL(p) (!(p))
#define PTR_ERR(p) (-EINVAL)
static bool active_scan,status_only,secure_pending,fp_scan_valid;
static unsigned contact_wait_ms,contact_poll_count,last_poll_limit;
static unsigned sleeps[4],sleep_count,ready_count,finished_count,debugfs_count;
static int version_error,info_error,load_error,scan_error,shutdown_error;
static bool version_uncertain,info_uncertain,scan_uncertain;
static int scans,status_calls,queries,infos,shutdowns,refs,releases;
static unsigned char events[128];static unsigned event_count;
static uint32_t req[1024],rsp[1024];static void *request=req,*response=rsp;
struct dentry{int dummy;};static struct dentry node,*probe_dir;
static int probe_fops,capture_fops;
static void event(char c){assert(event_count<sizeof(events));events[event_count++]=c;}
static void log_message(const char *fmt,...){
 if(strstr(fmt,"ready-for-contact")){
  assert(queries==1 && infos==1 && !version_error && !info_error);
  assert(secure_pending && refs==1 && scans==0);++ready_count;event('R');
 }else if(strstr(fmt,"contact-window-complete")){++finished_count;event('C');}
}
#define pr_info(...) log_message(__VA_ARGS__)
#define pr_warn(...) ((void)0)
#define pr_err(...) ((void)0)
static struct dentry *debugfs_create_dir(const char *name,void *parent){
 (void)name;(void)parent;++debugfs_count;return &node;
}
static struct dentry *debugfs_create_file(const char *name,unsigned mode,struct dentry *dir,
                                         void *data,void *ops){
 (void)name;(void)mode;(void)dir;(void)data;(void)ops;++debugfs_count;return &node;
}
static void debugfs_remove_recursive(struct dentry *dir){(void)dir;}
static void msleep(unsigned n){
 assert(sleep_count<4 && secure_pending && refs==1);sleeps[sleep_count++]=n;event('D');
}
static int prepare_buffers(void **image,size_t *length){*image=req;*length=100;return 0;}
static void release_buffers(void){++releases;}
static int try_module_get(int m){(void)m;++refs;return 1;}
static void module_put(int m){(void)m;--refs;}
static int fpq_app_load(void *image,size_t mdt,size_t length,u32 *id){
 assert(image==req && mdt==0 && length==100);*id=5;return load_error;
}
static int qcom_scm_qseecom_app_get_id(const char *name,u32 *id){
 assert(!strcmp(name,"fingerprint"));*id=5;return 0;
}
static int qcom_scm_qseecom_app_send(u32 id,void *a,size_t an,void *b,size_t bn){
 assert(id==5 && a==req && an==12 && b==rsp && bn==16);
 assert(req[0]==0x8031);req[0]=0x5aa5;req[2]=0;return 0;
}
static int query_version(u32 id,bool *uncertain){assert(id==5);++queries;event('V');*uncertain=version_uncertain;return version_error;}
static int query_info(u32 id,bool *uncertain){assert(id==5);++infos;event('I');*uncertain=info_uncertain;return info_error;}
static int fp_active_scan(u32 id,bool *uncertain){
 assert(id==5 && secure_pending && refs==1);++scans;event('S');*uncertain=scan_uncertain;return scan_error;
}
static int fp_active_scan_bounded(u32 id,bool *uncertain,unsigned limit){
 assert(limit>=10&&limit<=200);last_poll_limit=limit;return fp_active_scan(id,uncertain);
}
static int fp_frame_status(u32 id,bool *uncertain){assert(id==5);(void)uncertain;++status_calls;event('T');return 0;}
static int fpq_app_shutdown(u32 id){assert(id==5);++shutdowns;event('X');return shutdown_error;}
static void reset(void){
 active_scan=status_only=secure_pending=fp_scan_valid=false;contact_wait_ms=contact_poll_count=last_poll_limit=0;
 sleep_count=ready_count=finished_count=debugfs_count=event_count=0;
 version_error=info_error=load_error=scan_error=shutdown_error=0;
 version_uncertain=info_uncertain=scan_uncertain=false;
 scans=status_calls=queries=infos=shutdowns=refs=releases=0;
 memset(req,0,sizeof(req));memset(rsp,0,sizeof(rsp));memset(events,0,sizeof(events));
}
'''
tests=r'''
int main(void){
 const unsigned invalid[]={15001,UINT_MAX};
 for(unsigned i=0;i<2;++i){reset();active_scan=true;contact_wait_ms=invalid[i];
  assert(fpversion_init()==-EINVAL && debugfs_count==0 && !secure_pending);}
 reset();contact_wait_ms=1;assert(fpversion_init()==-EINVAL && debugfs_count==0);
 reset();active_scan=status_only=true;contact_wait_ms=1;
 assert(fpversion_init()==-EINVAL && debugfs_count==0);
 for(unsigned mode=0;mode<4;++mode){
  reset();active_scan=mode&1;status_only=mode&2;
  assert(fpversion_init()==0 && debugfs_count==3 && run_probe()==0);
  assert(sleep_count==1 && sleeps[0]==10 && ready_count==0 && finished_count==0);
  assert(scans==(active_scan&&!status_only) && status_calls==status_only);
  assert(shutdowns==1 && refs==0 && releases==1 && !secure_pending);
 }
 const unsigned valid[]={1,10,5000,15000};
 for(unsigned i=0;i<4;++i){
  reset();active_scan=true;contact_wait_ms=valid[i];
  assert(fpversion_init()==0 && run_probe()==0);
  assert(sleep_count==2 && sleeps[0]==10 && sleeps[1]==valid[i]);
  assert(!strcmp((char*)events,"DVIRDCSX"));
  assert(scans==1 && ready_count==1 && finished_count==1 && !secure_pending && refs==0);
 }
 for(unsigned stage=0;stage<4;++stage){
  reset();active_scan=true;contact_wait_ms=15000;
  if(stage==0)version_error=-EPROTO;
  if(stage==1)info_error=-EPROTO;
  if(stage==2){version_error=-EBUSY;version_uncertain=true;}
  if(stage==3){info_error=-EBUSY;info_uncertain=true;}
  assert(fpversion_init()==0 && run_probe()<0);
  assert(scans==0 && sleep_count==1 && sleeps[0]==10 && ready_count==0 && finished_count==0);
  if(stage<2)assert(shutdowns==1 && refs==0 && !secure_pending);
  else assert(shutdowns==0 && refs==1 && secure_pending);
 }
 reset();active_scan=true;contact_wait_ms=5000;load_error=-EIO;
 assert(fpversion_init()==0 && run_probe()==-EIO);
 assert(scans==0 && sleep_count==0 && ready_count==0 && refs==1 && secure_pending);
 reset();active_scan=true;contact_wait_ms=5000;scan_error=-ENODATA;
 assert(fpversion_init()==0 && run_probe()==-ENODATA);
 assert(scans==1 && sleeps[1]==5000 && shutdowns==1 && !secure_pending && refs==0);
 reset();active_scan=true;contact_wait_ms=5000;scan_error=-EBUSY;scan_uncertain=true;
 assert(fpversion_init()==0 && run_probe()==-EBUSY);
 assert(scans==1 && shutdowns==0 && refs==1 && secure_pending && !fp_scan_valid);

 const unsigned badpolls[]={1,9,201,UINT_MAX};
 for(unsigned i=0;i<4;++i){reset();active_scan=true;contact_poll_count=badpolls[i];
  assert(fpversion_init()==-EINVAL && debugfs_count==0);}
 reset();contact_poll_count=10;assert(fpversion_init()==-EINVAL&&!debugfs_count);
 reset();active_scan=status_only=true;contact_poll_count=10;assert(fpversion_init()==-EINVAL&&!debugfs_count);
 reset();active_scan=true;contact_poll_count=10;contact_wait_ms=1;
 assert(fpversion_init()==-EINVAL&&!debugfs_count);
 const unsigned goodpolls[]={10,100,200};
 for(unsigned i=0;i<3;++i){
  reset();active_scan=true;contact_poll_count=goodpolls[i];
  assert(fpversion_init()==0 && run_probe()==0);
  assert(!strcmp((char*)events,"DVIRSX") && sleep_count==1 && sleeps[0]==10);
  assert(last_poll_limit==goodpolls[i]&&ready_count==1&&!finished_count&&scans==1&&!secure_pending);
 }
 for(unsigned stage=0;stage<4;++stage){
  reset();active_scan=true;contact_poll_count=200;
  if(stage==0)version_error=-EPROTO;
  if(stage==1)info_error=-EPROTO;
  if(stage==2){version_error=-EBUSY;version_uncertain=true;}
  if(stage==3){info_error=-EBUSY;info_uncertain=true;}
  assert(fpversion_init()==0 && run_probe()<0);
  assert(!scans&&!last_poll_limit&&!ready_count&&sleep_count==1);
  assert(secure_pending==(stage>=2));
 }
 puts("PASS: actual init rejects invalid contact windows before debugfs; actual run_probe preserves default and resource rules, waits once only after successful prerequisites, never retries scan");
}
'''
with tempfile.TemporaryDirectory(prefix='fp-contact-window-') as tmp:
    p=Path(tmp);(p/'fixture.c').write_text(prefix+body+tests)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(p/'fixture.c'),'-o',str(p/'fixture')],check=True)
    subprocess.run([str(p/'fixture')],check=True)
