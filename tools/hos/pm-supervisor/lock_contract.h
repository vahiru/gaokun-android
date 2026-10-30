#ifndef HOS_PM_LOCK_CONTRACT_H
#define HOS_PM_LOCK_CONTRACT_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
static const char *const pm_original_locks[] = {
    "gaokun3_nosuspend", "gaokun3_usb_device_only", "gaokun3_usbrole"
};
#define PM_OWN_LOCK "hos_r22_pm_test_native"
struct pm_ops {
    void *ctx;
    int (*lock)(void *, const char *);
    int (*unlock)(void *, const char *);
    int (*read_locks)(void *, char *, size_t);
    int (*arm)(void *);
};
static bool pm_contains(const char *list, const char *name) {
    size_t n = strlen(name);
    while (*list) {
        while (isspace((unsigned char)*list)) ++list;
        const char *end = list;
        while (*end && !isspace((unsigned char)*end)) ++end;
        if ((size_t)(end-list)==n && !memcmp(list,name,n)) return true;
        list=end;
    }
    return false;
}
/* Parse the sole v2 membership line without discarding mixed v1 evidence. */
static bool pm_v2_path(const char *text, char *out, size_t cap) {
    unsigned found=0;
    while (*text) {
        const char *end=strchr(text,'\n');
        if(!end)end=text+strlen(text);
        size_t n=(size_t)(end-text);
        if(n>=2 && text[0]=='0' && text[1]==':') {
            if(n<4 || text[2]!=':' || text[3]!='/' || ++found!=1 || n-3>=cap) return false;
            memcpy(out,text+3,n-3);out[n-3]=0;
        }
        text=*end?end+1:end;
    }
    return found==1;
}
/* Failure never unlocks our protection. Still try all originals if one fails. */
static int pm_restore(struct pm_ops *o) {
    char locks[8192];
    int failed = o->lock(o->ctx,PM_OWN_LOCK)!=0;
    for(size_t i=0;i<3;++i) if(o->lock(o->ctx,pm_original_locks[i])) failed=1;
    if(o->read_locks(o->ctx,locks,sizeof(locks))) failed=1;
    else {
        if(!pm_contains(locks,PM_OWN_LOCK)) failed=1;
        for(size_t i=0;i<3;++i) if(!pm_contains(locks,pm_original_locks[i])) failed=1;
    }
    if(failed) return -1;
    if(o->unlock(o->ctx,PM_OWN_LOCK)) return -1;
    return 0;
}
/* Called only after the explicit gate + complete revalidation. */
static int pm_begin_window(struct pm_ops *o) {
    char locks[8192];
    if(o->read_locks(o->ctx,locks,sizeof(locks))) return -1;
    if(!pm_contains(locks,PM_OWN_LOCK)) return -1;
    for(size_t i=0;i<3;++i) if(!pm_contains(locks,pm_original_locks[i])) return -1;
    if(o->arm(o->ctx)) return -1;
    for(size_t i=0;i<3;++i) if(o->unlock(o->ctx,pm_original_locks[i])) return -1;
    if(o->unlock(o->ctx,PM_OWN_LOCK)) return -1;
    if(o->read_locks(o->ctx,locks,sizeof(locks))) return -1;
    if(pm_contains(locks,PM_OWN_LOCK)) return -1;
    for(size_t i=0;i<3;++i) if(pm_contains(locks,pm_original_locks[i])) return -1;
    return 0;
}
#endif
