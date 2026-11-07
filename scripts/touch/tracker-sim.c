/* hx_track_contacts() 的逐行复刻 —— 不用硬件就能验证跟踪器的改动。
 *
 * 本文件里的版本【已经带上 patches/kernel/input/0002 的修正】：跳点检测比的是与预测位置的偏差。
 * 想复现修正前的行为，把 swap_test/run 里那两行改回
 *     long dx = det[di].x - t->x, dy = det[di].y - t->y;
 * 即可。详见 scripts/touch/README.md。
 *
 *   cc -O2 -o tracker-sim tracker-sim.c && ./tracker-sim
 */
/* 把 hx_track_contacts 的逻辑原样搬过来，喂直线匀速运动，看哪个速度下输出消失。
 * 逻辑逐行对照 refs/gaokun-buildbot/drivers/touchscreen-hx83121a/hx-algo.c:960-1120 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#define MAXT 10
struct pos { int x, y; };
struct track { bool active; int x,y,vx,vy; unsigned char age, missed, debounce; };
struct algo {
    struct track tracks[MAXT];
    long track_dist2_max; unsigned char track_lost_frames, debounce_base;
    bool track_smoothing, track_active_guard; unsigned char track_start_debounce;
    long track_jump_dist2;
    bool touch_active; unsigned char touch_start_frames;
};
static void reset_track(struct track *t){ memset(t,0,sizeof(*t)); }

static void track_contacts(struct algo *a, struct pos *det, int det_cnt)
{
    bool det_used[MAXT]={false}, tmatch[MAXT]={false};
    unsigned jump_released=0;
    struct { int ti,di; long d2; } cand[MAXT*MAXT]; int cc=0;
    for (int i=0;i<MAXT;i++){ struct track*t=&a->tracks[i]; if(!t->active) continue;
        for(int j=0;j<det_cnt;j++){ long dx=det[j].x-(t->x+t->vx), dy=det[j].y-(t->y+t->vy);
            long d2=dx*dx+dy*dy; if(d2>a->track_dist2_max) continue;
            cand[cc].ti=i;cand[cc].di=j;cand[cc].d2=d2;cc++; } }
    for(int i=0;i<cc;i++){int b=i;for(int j=i+1;j<cc;j++)if(cand[j].d2<cand[b].d2)b=j;
        if(b!=i){typeof(cand[0]) t=cand[i];cand[i]=cand[b];cand[b]=t;}}
    for(int k=0;k<cc;k++){ int ti=cand[k].ti,di=cand[k].di;
        if(tmatch[ti]||det_used[di]) continue;
        struct track*t=&a->tracks[ti]; if(!t->active) continue;
        if(a->track_jump_dist2>0 && t->age>=2){
            /* 修正：跟原始位移比没有意义 —— 匀速快滑位移就是很大。
               真正的换手指表现为【与预测位置的偏差】大，而匹配阶段
               算的 cand[].d2 正是这个偏差，直接复用。 */
            long dx=det[di].x-(t->x+t->vx), dy=det[di].y-(t->y+t->vy);
            if(dx*dx+dy*dy > a->track_jump_dist2){
                reset_track(t); jump_released|=(1u<<ti); tmatch[ti]=true; continue; } }
        t->vx=det[di].x-t->x; t->vy=det[di].y-t->y;
        if(a->track_smoothing){ t->x=(t->x*3+det[di].x)/4; t->y=(t->y*3+det[di].y)/4; }
        else { t->x=det[di].x; t->y=det[di].y; }
        t->missed=0; if(t->age<255)t->age++; if(t->debounce>0)t->debounce--;
        tmatch[ti]=true; det_used[di]=true; }
    for(int i=0;i<MAXT;i++){ struct track*t=&a->tracks[i];
        if(!t->active||tmatch[i]) continue;
        if(a->track_active_guard && !a->touch_active){ reset_track(t); continue; }
        t->missed++; if(t->missed>a->track_lost_frames) reset_track(t); }
    for(int j=0;j<det_cnt;j++){ if(det_used[j])continue; struct track*t=NULL;
        for(int i=0;i<MAXT;i++) if(!a->tracks[i].active && !(jump_released&(1u<<i))){t=&a->tracks[i];break;}
        if(!t)continue;
        t->active=true;t->age=1;t->missed=0;t->debounce=a->debounce_base;
        t->x=det[j].x;t->y=det[j].y;t->vx=0;t->vy=0; } }

static int stable(struct algo*a){int n=0;for(int i=0;i<MAXT;i++)if(a->tracks[i].active&&a->tracks[i].debounce==0)n++;return n;}

/* 一次直线滑动：120 帧，每帧前进 step 个单位 */
static void run(long jump, int step, int *reported, int *idswaps)
{
    struct algo a; memset(&a,0,sizeof(a));
    a.track_dist2_max=420*420; a.track_lost_frames=3; a.debounce_base=2;
    a.track_smoothing=false; a.track_active_guard=true; a.track_start_debounce=0;
    a.track_jump_dist2=jump;
    int rep=0, sw=0, prev_slot=-1;
    for(int f=0;f<120;f++){
        struct pos d={ 800, 200+f*step };
        track_contacts(&a,&d,1);
        int sc=stable(&a);
        if(sc>0 && !a.touch_active){ a.touch_start_frames++;
            if(a.touch_start_frames>=a.track_start_debounce) a.touch_active=true; }
        else if(sc==0){ a.touch_start_frames=0; a.touch_active=false; }
        int slot=-1; for(int i=0;i<MAXT;i++) if(a.tracks[i].active&&a.tracks[i].debounce==0){slot=i;break;}
        if(slot>=0){ rep++; if(prev_slot>=0&&slot!=prev_slot) sw++; prev_slot=slot; }
    }
    *reported=rep; *idswaps=sw;
}


/* 换手指场景：前 30 帧以 step 匀速滑行，第 31 帧检测点突然偏离 off 个单位。
   合格的跳点检测应当在这里【换槽】。 */
static int swap_test(long jump, int step, int off)
{
    struct algo a; memset(&a,0,sizeof(a));
    a.track_dist2_max=420*420; a.track_lost_frames=3; a.debounce_base=2;
    a.track_smoothing=false; a.track_active_guard=true; a.track_start_debounce=0;
    a.track_jump_dist2=jump;
    int prev=-1, sw=0;
    for(int f=0;f<60;f++){
        struct pos d={ 800, 200+f*step };
        if(f>=30) d.x = 800 + off;          /* 另一根手指，横向偏开 */
        track_contacts(&a,&d,1);
        int sc=stable(&a);
        if(sc>0&&!a.touch_active){a.touch_start_frames++;
            if(a.touch_start_frames>=a.track_start_debounce)a.touch_active=true;}
        else if(sc==0){a.touch_start_frames=0;a.touch_active=false;}
        int slot=-1;for(int i=0;i<MAXT;i++)if(a.tracks[i].active&&a.tracks[i].debounce==0){slot=i;break;}
        if(slot>=0){ if(prev>=0&&slot!=prev)sw++; prev=slot; }
    }
    return sw;
}

int main(void){
    printf("每帧位移  速度      game(jump=6400)        daily(jump=0)\n");
    printf("(单位)   (m/s)   上报帧/120  换槽次数   上报帧/120  换槽次数\n");
    int steps[]={2,4,8,20,40,60,79,80,81,100,150,200,300,400};
    for(unsigned i=0;i<sizeof(steps)/sizeof(*steps);i++){
        int s=steps[i]; int rg,sg,rd,sd;
        run(6400,s,&rg,&sg); run(0,s,&rd,&sd);
        printf("%6d  %6.2f   %8d %9d   %10d %9d\n",
               s, s*0.1043*120/1000.0, rg,sg, rd,sd);
    }
    printf("\n换手指检测（滑行中第 31 帧横向跳开 off 个单位，jump=6400）：\n");
    printf("  滑速      off=20   off=60   off=100  off=200  off=300\n");
    int sp[]={20,80,150,300};
    for(unsigned i=0;i<sizeof(sp)/sizeof(*sp);i++){
        printf("  %5.2f m/s", sp[i]*0.1043*120/1000.0);
        int offs[]={20,60,100,200,300};
        for(int k=0;k<5;k++) printf("%9s", swap_test(6400,sp[i],offs[k])?"抓到":"漏了");
        printf("\n"); }
    return 0; }
