#include "lock_contract.h"
#include <assert.h>
#include <stdio.h>
struct fake { bool held[4]; int calls, fail_call, arms, releases, read_fail, reads, fail_read_call, suppress_lock; };
static int idx(const char *s){if(!strcmp(s,PM_OWN_LOCK))return 3;for(int i=0;i<3;++i)if(!strcmp(s,pm_original_locks[i]))return i;assert(0);return -1;}
static int lock(void *p,const char *s){struct fake*f=p;if(++f->calls==f->fail_call)return -1;if(!f->suppress_lock)f->held[idx(s)]=true;return 0;}
static int unlock(void *p,const char *s){struct fake*f=p;if(++f->calls==f->fail_call)return -1;f->held[idx(s)]=false;++f->releases;return 0;}
static int readlocks(void*p,char*b,size_t n){struct fake*f=p;if(f->read_fail||++f->reads==f->fail_read_call)return -1;b[0]=0;for(int i=0;i<4;++i)if(f->held[i]){const char*s=i==3?PM_OWN_LOCK:pm_original_locks[i];assert(strlen(b)+strlen(s)+2<n);strcat(b,s);strcat(b," ");}return 0;}
static int arm(void*p){struct fake*f=p;++f->arms;return ++f->calls==f->fail_call?-1:0;}
static struct pm_ops ops(struct fake*f){return(struct pm_ops){f,lock,unlock,readlocks,arm};}
int main(void){
 assert(pm_contains("a\n gaokun3_usbrole\t", "gaokun3_usbrole"));assert(!pm_contains("gaokun3_usbrole_extra", "gaokun3_usbrole"));
 char cg[128];
 assert(pm_v2_path("4:memory:/\n3:cpuset:/\n2:cpu:/system\n1:blkio:/\n0::/hos-r22-pm-test\n",cg,sizeof(cg))&&!strcmp(cg,"/hos-r22-pm-test"));
 assert(!pm_v2_path("0::/a\n0::/b\n",cg,sizeof(cg)));
 assert(!pm_v2_path("4:memory:/\n",cg,sizeof(cg)));
 assert(!pm_v2_path("0:cpu:/a\n",cg,sizeof(cg)));
 assert(pm_v2_path("0::/hos-r22-pm-test-other\n",cg,sizeof(cg))&&strcmp(cg,"/hos-r22-pm-test"));
 assert(!pm_v2_path("0::/path-too-long",cg,4));
 struct fake f={.held={true,true,true,true}};struct pm_ops o=ops(&f);
 assert(!pm_begin_window(&o));assert(f.arms==1&&f.releases==4);assert(!pm_restore(&o));for(int i=0;i<3;++i)assert(f.held[i]);assert(!f.held[3]);
 for(int fail=1;fail<=5;++fail){f=(struct fake){.held={true,true,true,true},.fail_call=fail};o=ops(&f);assert(pm_begin_window(&o)==-1);if(fail==1)assert(f.releases==0);f.fail_call=0;assert(!pm_restore(&o));for(int i=0;i<3;++i)assert(f.held[i]);}
 for(int fail=1;fail<=5;++fail){f=(struct fake){.held={true,true,true,true},.fail_call=fail};o=ops(&f);assert(pm_restore(&o)==-1);assert(f.held[3]);assert(f.releases==0);}
 f=(struct fake){.held={true,true,true,true},.read_fail=1};o=ops(&f);assert(pm_begin_window(&o)==-1&&f.arms==0);assert(pm_restore(&o)==-1&&f.held[3]&&f.releases==0);
 f=(struct fake){.held={true,true,false,true}};o=ops(&f);assert(pm_begin_window(&o)==-1&&f.arms==0&&f.releases==0);
 f=(struct fake){.held={true,true,true,true},.fail_read_call=2};o=ops(&f);assert(pm_begin_window(&o)==-1&&f.arms==1);f.fail_read_call=0;assert(!pm_restore(&o));
 f=(struct fake){.held={false,true,true,true},.suppress_lock=1};o=ops(&f);assert(pm_restore(&o)==-1&&f.held[3]&&f.releases==0);
 puts("PASS arm-before-release, no-gate primitive boundary, all partial-release failures restore, restore failure retains own lock, exact tokens");
}
