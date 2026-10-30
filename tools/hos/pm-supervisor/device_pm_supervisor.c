#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "lock_contract.h"

#define BOOT "64b3ea5c-0428-4d05-ba5a-bda6586d4dd3"
#define OUT "/data/local/tmp/hos-r22-device-pm-native"
#define CG "/sys/fs/cgroup/hos-r22-pm-test"
#define ROLE "/sys/class/usb_role/a600000.usb-role-switch/role"
#define L9 "/sys/firmware/devicetree/base/soc@0/rsc@18200000/regulators-2/ldo9"
#define GATE_WAIT_SECONDS 300
static int logfd=-1, dirfd=-1, timer=-1;
static uint64_t armed_ns;
static uint64_t now_ns(void){struct timespec t;if(clock_gettime(CLOCK_BOOTTIME,&t))return 0;return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
static void note(const char *fmt,...){if(logfd<0)return;dprintf(logfd,"boottime_ns=%llu ",(unsigned long long)now_ns());va_list a;va_start(a,fmt);vdprintf(logfd,fmt,a);va_end(a);dprintf(logfd,"\n");fsync(logfd);}
static int read_fd(int fd,char *buf,size_t cap){size_t at=0;while(at+1<cap){ssize_t n=read(fd,buf+at,cap-at-1);if(n<0&&errno==EINTR)continue;if(n<0)return -1;if(!n){buf[at]=0;return 0;}at+=(size_t)n;}char c;ssize_t n=read(fd,&c,1);buf[at]=0;return n==0?0:-1;}
static int read_file(const char *path,char *buf,size_t cap){int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;int rc=read_fd(fd,buf,cap);close(fd);return rc;}
static int write_text(const char *path,const char *text){int fd=open(path,O_WRONLY|O_CLOEXEC);if(fd<0)return -1;size_t n=strlen(text);ssize_t w;do{w=write(fd,text,n);}while(w<0&&errno==EINTR);int rc=w==(ssize_t)n?0:-1;if(close(fd))rc=-1;return rc;}
static bool file_equals(const char *path,const char *want){char b[8192];if(read_file(path,b,sizeof(b)))return false;size_t n=strlen(b);while(n&&isspace((unsigned char)b[n-1]))b[--n]=0;return !strcmp(b,want);}
/* Fixed read-only commands, bounded output and a five-second deadline. */
static int command(const char *exe,const char *arg,char *buf,size_t cap){
 int fds[2];if(pipe2(fds,O_CLOEXEC|O_NONBLOCK))return -1;pid_t p=fork();
 if(p<0){close(fds[0]);close(fds[1]);return -1;}
 if(!p){close(fds[0]);fcntl(fds[1],F_SETFL,0);dup2(fds[1],1);int dn=open("/dev/null",O_WRONLY);if(dn>=0)dup2(dn,2);execl(exe,exe,arg,(char*)NULL);_exit(127);}
 close(fds[1]);size_t used=0;bool eof=false,ok=true;uint64_t end=now_ns()+5000000000ULL;
 while(!eof){if(now_ns()>=end){ok=false;break;}struct pollfd q={fds[0],POLLIN|POLLHUP,0};int rc=poll(&q,1,100);if(rc<0&&errno==EINTR)continue;if(rc<0){ok=false;break;}if(!rc)continue;
  if(used+1>=cap){ok=false;break;}ssize_t n=read(fds[0],buf+used,cap-used-1);if(n<0&&(errno==EAGAIN||errno==EINTR))continue;if(n<0){ok=false;break;}if(!n)eof=true;else used+=(size_t)n;
 }
 close(fds[0]);buf[used]=0;
 int status=0;
 for(;;){
  pid_t waited=waitpid(p,&status,WNOHANG);
  if(waited==p)break;
  if(waited<0){if(errno==EINTR)continue;return -1;}
  if(!ok||now_ns()>=end){ok=false;kill(p,SIGKILL);while(waitpid(p,&status,0)<0){if(errno!=EINTR)return -1;}break;}
  struct timespec pause={0,10000000};nanosleep(&pause,NULL);
 }
 return ok&&WIFEXITED(status)&&WEXITSTATUS(status)==0?0:-1;
}
static bool prop_equals(const char *key,const char *want){char b[256];if(command("/system/bin/getprop",key,b,sizeof(b)))return false;size_t n=strlen(b);while(n&&isspace((unsigned char)b[n-1]))b[--n]=0;return !strcmp(b,want);}
static int preconditions(void){
 char b[131072];struct stat st;
 if(geteuid()!=0||!file_equals("/proc/sys/kernel/random/boot_id",BOOT))return -1;
 if(read_file("/proc/cmdline",b,sizeof(b))||!pm_contains(b,"androidboot.force_super_partition=super_hos"))return -1;
 if(!prop_equals("persist.vendor.gaokun3.allow_suspend","0")||!prop_equals("persist.vendor.gaokun3.usbrole.device_only","true"))return -1;
 if(!file_equals("/proc/modules","")||!file_equals(ROLE,"device"))return -1;
 if(stat(L9 "/regulator-always-on",&st)||!S_ISREG(st.st_mode)||!file_equals(L9 "/regulator-name","vreg_l9d"))return -1;
 if(read_file("/sys/power/pm_test",b,sizeof(b))||!pm_contains(b,"[none]"))return -1;
 if(read_file("/sys/power/mem_sleep",b,sizeof(b))||!pm_contains(b,"[s2idle]"))return -1;
 if(command("/system/bin/dumpsys","power",b,sizeof(b))||!strstr(b,"mWakefulness=Asleep"))return -1;
 if(read_file("/sys/power/wake_lock",b,sizeof(b)))return -1;
 for(int i=0;i<3;++i)if(!pm_contains(b,pm_original_locks[i]))return -1;
 return 0;
}
static int check_isolation(void){
 char ours[8192],adb[8192],our_path[4096],adb_path[4096];
 if(!file_equals("/proc/845/comm","adbd")||read_file("/proc/845/cgroup",adb,sizeof(adb)))return -1;
 if(read_file("/proc/self/cgroup",ours,sizeof(ours))||!pm_v2_path(ours,our_path,sizeof(our_path))||!pm_v2_path(adb,adb_path,sizeof(adb_path))||strcmp(our_path,"/hos-r22-pm-test")||!strcmp(our_path,adb_path))return -1;
 return getsid(0)==getpid()&&getpgrp()==getpid()?0:-1;
}
static int isolate(void){
 struct statfs fs;char pid[32],ours[8192],adb[8192],our_path[4096],adb_path[4096];
 if(setsid()<0||statfs("/sys/fs/cgroup",&fs)||fs.f_type!=0x63677270)return -1;
 if(!file_equals("/proc/845/comm","adbd")||read_file("/proc/845/cgroup",adb,sizeof(adb)))return -1;
 if(mkdir(CG,0700))return -1; // No reuse of an old or foreign cgroup.
 snprintf(pid,sizeof(pid),"%ld",(long)getpid());if(write_text(CG "/cgroup.procs",pid))return -1;
 if(read_file("/proc/self/cgroup",ours,sizeof(ours))||!pm_v2_path(ours,our_path,sizeof(our_path))||!pm_v2_path(adb,adb_path,sizeof(adb_path))||strcmp(our_path,"/hos-r22-pm-test")||!strcmp(our_path,adb_path))return -1;
 if(getsid(0)!=getpid()||getpgrp()!=getpid())return -1;
 note("ISOLATED pid=%ld sid=%ld pgid=%ld self_cgroup=%s adbd845_cgroup=%s",(long)getpid(),(long)getsid(0),(long)getpgrp(),ours,adb);return 0;
}
static int lock_cb(void *unused,const char *name){(void)unused;int rc=write_text("/sys/power/wake_lock",name);if(rc)note("LOCK_FAILED name=%s errno=%d",name,errno);return rc;}
static int unlock_cb(void *unused,const char *name){(void)unused;int rc=write_text("/sys/power/wake_unlock",name);if(rc)note("UNLOCK_FAILED name=%s errno=%d",name,errno);return rc;}
static int locks_cb(void *unused,char *b,size_t n){(void)unused;return read_file("/sys/power/wake_lock",b,n);}
static int arm_cb(void *unused){(void)unused;uint64_t started=now_ns();if(!started)return -1;struct itimerspec spec={.it_value={.tv_sec=(time_t)(started/1000000000ULL+20),.tv_nsec=(long)(started%1000000000ULL)}};
 if(timerfd_settime(timer,TFD_TIMER_ABSTIME,&spec,NULL))return -1;
 armed_ns=started;
 struct itimerspec left;if(timerfd_gettime(timer,&left)||(!left.it_value.tv_sec&&!left.it_value.tv_nsec))return -1;
 note("ARMED CLOCK_BOOTTIME_ALARM duration_seconds=20");return 0;
}
static void snapshot(const char *label){
 char b[32768];const char *paths[]={"/sys/kernel/debug/suspend_stats","/sys/kernel/debug/wakeup_sources","/sys/power/wake_lock",ROLE};
 for(size_t i=0;i<4;++i){if(read_file(paths[i],b,sizeof(b)))note("SNAPSHOT_FAILED phase=%s path=%s errno=%d",label,paths[i],errno);else note("SNAPSHOT phase=%s path=%s\n%s",label,paths[i],b);}
}
/* Return 1 only for an exact, root-owned regular gate carrying this run's token. */
static int gate(const char *token){
 int fd=openat(dirfd,"GO",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return errno==ENOENT?0:-1;
 struct stat st;char b[128];int ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&st.st_nlink==1&&(st.st_mode&0777)==0600&&st.st_size==(off_t)strlen(token)&&!read_fd(fd,b,sizeof(b))&&!strcmp(b,token);close(fd);return ok?1:-1;
}
static int run_child(int ready_pipe){
 sigset_t mask;sigemptyset(&mask);int signals[]={SIGHUP,SIGINT,SIGTERM,SIGQUIT};for(size_t i=0;i<4;++i)sigaddset(&mask,signals[i]);if(sigprocmask(SIG_BLOCK,&mask,NULL))return 1;
 int sigfd=signalfd(-1,&mask,SFD_CLOEXEC|SFD_NONBLOCK);if(sigfd<0)return 1;
 int rc=1;bool own=false;const char *reason="setup_failed";struct pm_ops ops={NULL,lock_cb,unlock_cb,locks_cb,arm_cb};
 if(preconditions()||isolate())goto finish;
 timer=timerfd_create(CLOCK_BOOTTIME_ALARM,TFD_CLOEXEC|TFD_NONBLOCK);if(timer<0){note("TIMERFD_UNAVAILABLE errno=%d no_fallback=true",errno);goto finish;}
 char locks[8192];if(locks_cb(NULL,locks,sizeof(locks))||pm_contains(locks,PM_OWN_LOCK))goto finish;
 if(lock_cb(NULL,PM_OWN_LOCK))goto finish;own=true;
 if(locks_cb(NULL,locks,sizeof(locks))||!pm_contains(locks,PM_OWN_LOCK))goto finish;
 unsigned char random[16];if(getrandom(random,sizeof(random),0)!=(ssize_t)sizeof(random))goto finish;
 char token[33];for(int i=0;i<16;++i)snprintf(token+2*i,3,"%02x",random[i]);
 int ready=openat(dirfd,"READY",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(ready<0)goto finish;
 dprintf(ready,"boot=%s\npid=%ld\nsid=%ld\ncgroup=/hos-r22-pm-test\narmed=false\ngate_token=%s\ngate_max_seconds=%d\n",BOOT,(long)getpid(),(long)getsid(0),token,GATE_WAIT_SECONDS);fsync(ready);close(ready);fsync(dirfd);
 note("READY no_original_lock_released gate_wait_seconds=%d",GATE_WAIT_SECONDS);
 if(write(ready_pipe,"R",1)!=1)goto finish;close(ready_pipe);ready_pipe=-1;
 uint64_t deadline=now_ns()+(uint64_t)GATE_WAIT_SECONDS*1000000000ULL;
 for(;;){
  struct pollfd p={sigfd,POLLIN,0};int n=poll(&p,1,250);if(n<0&&errno==EINTR)continue;if(n<0){reason="gate_poll_failed";goto finish;}
  if(n){struct signalfd_siginfo si;if(read(sigfd,&si,sizeof(si))==(ssize_t)sizeof(si))note("SIGNAL before_arm=%u",si.ssi_signo);reason="signal_before_arm";goto finish;}
  if(now_ns()>=deadline){reason="gate_timeout";goto finish;}int g=gate(token);if(g<0){reason="invalid_gate";goto finish;}if(g==1)break;
 }
 if(preconditions()||check_isolation()){reason="gate_revalidation_failed";goto finish;}
 snapshot("before");
 struct pollfd pending={sigfd,POLLIN,0};
 if(poll(&pending,1,0)!=0){reason="signal_or_poll_error_before_arm";goto finish;}
 if(pm_begin_window(&ops)){reason="arm_or_release_failed";goto finish;}
 note("WINDOW_OPEN all_four_named_locks_released");
 for(;;){
  struct pollfd p[2]={{sigfd,POLLIN,0},{timer,POLLIN,0}};int n=poll(p,2,30000);if(n<0&&errno==EINTR)continue;
  if(n<=0){reason=n==0?"timer_poll_timeout":"window_poll_failed";goto finish;}
  if(p[0].revents){struct signalfd_siginfo si;if(read(sigfd,&si,sizeof(si))==(ssize_t)sizeof(si))note("SIGNAL during_window=%u",si.ssi_signo);reason="signal_aborted";goto finish;}
  if(p[1].revents&POLLIN){uint64_t ticks;if(read(timer,&ticks,sizeof(ticks))!=(ssize_t)sizeof(ticks)||!ticks){reason="timer_read_failed";goto finish;}reason="alarm_elapsed";rc=0;break;}
  reason="unexpected_poll_event";goto finish;
 }
finish:
 note("RESTORING reason=%s elapsed_since_arm_ns=%llu",reason,(unsigned long long)(armed_ns?now_ns()-armed_ns:0));
 if(own){
  if(pm_restore(&ops)){note("RESTORE_FAILED own_unlock_not_attempted_or_failed=true manual_review_required=true");rc=2;}
  else note("RESTORED_VERIFIED reason=%s result=%s not_a_sleep_pass=true",reason,rc==0?"window_elapsed":"aborted");
 }else note("SETUP_ABORTED no_original_lock_release_attempted=true restoration_not_claimed=true");
 bool invariant_ok=file_equals("/proc/sys/kernel/random/boot_id",BOOT)&&file_equals(ROLE,"device")&&prop_equals("persist.vendor.gaokun3.allow_suspend","0")&&prop_equals("persist.vendor.gaokun3.usbrole.device_only","true");
 note("FINAL_INVARIANTS boot_role_properties_unchanged=%s",invariant_ok?"true":"false");
 if(!invariant_ok){if(own)lock_cb(NULL,PM_OWN_LOCK);rc=3;}
 if(timer>=0)close(timer);snapshot("after");
 note("FINISHED exit_code=%d reason=%s elapsed_since_arm_ns=%llu sleep_success_not_inferred=true",rc,reason,(unsigned long long)(armed_ns?now_ns()-armed_ns:0));
 close(sigfd);if(ready_pipe>=0)close(ready_pipe);return rc;
}
int main(int argc,char **argv){
 if(argc!=2||strcmp(argv[1],"--prepare")){fprintf(stderr,"Usage: %s --prepare (READY only; explicit GO token required)\n",argv[0]);return 2;}
 if(geteuid()!=0){fprintf(stderr,"root required\n");return 2;}umask(077);
 if(mkdir(OUT,0700)){perror("fresh output directory required");return 2;}
 dirfd=open(OUT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dirfd<0)return 2;
 logfd=openat(dirfd,"events.log",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(logfd<0)return 2;
 int pipefd[2];if(pipe2(pipefd,O_CLOEXEC))return 2;pid_t pid=fork();if(pid<0)return 2;
 if(pid){close(pipefd[1]);char c;ssize_t n;do{n=read(pipefd[0],&c,1);}while(n<0&&errno==EINTR);close(pipefd[0]);if(n==1&&c=='R'){printf("READY supervisor_pid=%ld output=%s (no arm/no original unlock)\n",(long)pid,OUT);return 0;}fprintf(stderr,"supervisor refused setup; inspect events.log\n");return 1;}
 close(pipefd[0]);int dn=open("/dev/null",O_RDWR);if(dn>=0){dup2(dn,0);dup2(dn,1);dup2(dn,2);if(dn>2)close(dn);}int rc=run_child(pipefd[1]);close(logfd);close(dirfd);_exit(rc);
}
