/* libgk3core：安装器写进 misc 的初始状态（见 gk3core.h 的 gk3_misc_build_install）。 */
#include "gk3core.h"

gk3_err gk3_misc_build_install(uint8_t head[GK3_MISC_INIT_HEAD_SIZE], uint8_t rec[GK3_REC_SIZE], unsigned slot,
                               gk3_setdef def)
{
    if (slot > 1 || (def != GK3_SETDEF_NONE && def != GK3_SETDEF_WINDOWS && def != GK3_SETDEF_ANDROID))
        return GK3_EINVAL;
    gk3_memset(head, 0, GK3_MISC_INIT_HEAD_SIZE);
    gk3_bcab_init_install(head + GK3_MISC_BCAB_OFF, slot);
    gk3_rec_init(rec);
    gk3_rec_migrate(rec, head /* 全零的 BCB */, GK3_DISPATCH_VER);
    if (def != GK3_SETDEF_NONE)
        gk3_rec_put_set_default_req(rec, def);
    gk3_rec_seal(rec);
    /* 自己造的都不过 = 库坏了 */
    if (gk3_bcab_validate(head + GK3_MISC_BCAB_OFF) || gk3_rec_validate(rec))
        return GK3_ECRC;
    return GK3_OK;
}
