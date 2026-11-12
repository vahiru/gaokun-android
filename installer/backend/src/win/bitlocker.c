/* Windows：系统卷的 BitLocker（WMI 的 Win32_EncryptableVolume）—— 保护状态、恢复密钥的校验、暂停保护。
 *
 *   gk3w bitlocker-key [--suspend N] [--volume X:]
 *
 * 从标准输入读一行恢复密钥，与卷上每个数字密码保护器（恢复密钥）比对，一致就输出 BITLOCKER volume=X: key=ok。
 * --suspend N（1–15）：校验通过之后暂停这个卷的 BitLocker 保护，Windows 再启动 N 次后自动恢复，恢复时按那时的
 * 启动链重新绑定 TPM。关闭 Secure Boot、改动回落路径的引导程序之前这样做，之后启动 Windows 就不会要求输入
 * 恢复密钥。默认是系统卷；--volume 用于对着一个加密的数据卷测试。
 *
 * 为什么要用户输入密钥、而不是只确认"拿得到"：暂停之后重新绑定仍可能失败（例如固件额外测量了别的内容），
 * 那时 Windows 启动时要求输入恢复密钥，安装器帮不上忙。比对的是 Windows 自己保存的恢复密钥，输错的、
 * 别的设备的都过不去。
 *
 * 密钥只从标准输入读（参数会原样记进安装日志），用完即清除，不写日志、不输出。 */
#include "wmi.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "../blkey.h"
#include "../proto.h"
#include "disk.h"
#include "win.h"

/* 一个卷在 WMI 里的实例 */
typedef struct {
    gk3w_wmi w;
    IWbemClassObject *vol;      /* Win32_EncryptableVolume 的实例 */
    int protection;             /* ProtectionStatus：0 关、1 开、2 未知；读不到 -1 */
} blw;

static void blw_close(blw *b)
{
    if (b->vol)
        IWbemClassObject_Release(b->vol);
    b->vol = NULL;
    gk3w_wmi_close(&b->w);
}

/* 连上 BitLocker 的 WMI 命名空间、找到盘符为 drive 的卷。失败返回 -1（之后仍要 blw_close） */
static int blw_open(blw *b, wchar_t drive)
{
    wchar_t q[128];
    uint64_t ps;

    b->vol = NULL;
    b->protection = -1;
    if (gk3w_wmi_open(&b->w, L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption"))
        return -1;
    _snwprintf(q, 128, L"SELECT * FROM Win32_EncryptableVolume WHERE DriveLetter='%lc:'", drive);
    /* 查询结果最多等 30 秒：WMI 服务卡住时不能让检查一直不返回 */
    if (gk3w_wmi_find(&b->w, q, 30000, NULL, NULL, &b->vol))
        return -1;
    if (!gk3w_wmi_u64(b->vol, L"ProtectionStatus", &ps) && ps <= 2)
        b->protection = (int)ps;
    return 0;
}

/* 调这个卷的一个方法（见 gk3w_wmi_call），最多等 60 秒 */
static HRESULT blw_call(blw *b, const wchar_t *method, const wchar_t *pname, VARIANT *pval, IWbemClassObject **out)
{
    return gk3w_wmi_call(&b->w, b->vol, method, pname, pval, 60000, out);
}

/* 卷上数字密码保护器（恢复密钥）的 ID。返回个数（*ids 由调用方 SafeArrayDestroy；0 个时为 NULL），失败返回 -1 */
static int blw_password_ids(blw *w, SAFEARRAY **ids, LONG *lo)
{
    IWbemClassObject *out = NULL;
    VARIANT t, v;
    HRESULT hr;
    int n = -1;

    *ids = NULL;
    *lo = 0;
    VariantInit(&t);
    t.vt = VT_I4;
    t.lVal = 3;   /* KeyProtectorType 3：数字密码 */
    hr = blw_call(w, L"GetKeyProtectors", L"KeyProtectorType", &t, &out);
    if (FAILED(hr)) {
        gk3w_log("BitLocker: GetKeyProtectors failed (0x%08lx)", (unsigned long)hr);
        return -1;
    }
    VariantInit(&v);
    if (SUCCEEDED(IWbemClassObject_Get(out, L"VolumeKeyProtectorID", 0, &v, NULL, NULL))) {
        if (v.vt == (VT_ARRAY | VT_BSTR) && v.parray) {
            LONG hi = -1;
            if (SUCCEEDED(SafeArrayGetLBound(v.parray, 1, lo)) && SUCCEEDED(SafeArrayGetUBound(v.parray, 1, &hi))) {
                n = hi >= *lo ? (int)(hi - *lo + 1) : 0;
                if (n) {
                    *ids = v.parray;   /* 接管 */
                    v.vt = VT_EMPTY;
                    v.parray = NULL;
                }
            }
        } else if (v.vt == VT_NULL || v.vt == VT_EMPTY) {
            n = 0;   /* 没有这类保护器 */
        }
    }
    VariantClear(&v);
    IWbemClassObject_Release(out);
    return n;
}

int gk3w_bitlocker_status(wchar_t drive, char *ids, size_t cap, int *ids_known, int *conversion)
{
    blw w;
    int status;

    if (cap)
        ids[0] = 0;
    *ids_known = 0;
    *conversion = -1;
    if (blw_open(&w, drive)) {
        status = w.protection;
        blw_close(&w);
        return status;
    }
    status = w.protection;
    {
        /* 加密或解密进行到哪一步（ConversionStatus：0 完全解密、1 完全加密、2 正在加密、3 正在解密、4 加密已暂停、
         * 5 解密已暂停）：保护状态 0 也可能是加密还没完成，那时 TPM 保护器照样在用 */
        IWbemClassObject *out = NULL;
        HRESULT hr = blw_call(&w, L"GetConversionStatus", NULL, NULL, &out);
        if (SUCCEEDED(hr)) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(IWbemClassObject_Get(out, L"ConversionStatus", 0, &v, NULL, NULL)) && (v.vt == VT_I4 || v.vt == VT_UI4))
                *conversion = (int)v.lVal;
            VariantClear(&v);
            IWbemClassObject_Release(out);
        } else {
            gk3w_log("BitLocker: GetConversionStatus failed (0x%08lx)", (unsigned long)hr);
        }
    }
    if (status == 1 || status == 2) {
        SAFEARRAY *arr;
        LONG lo;
        int n = blw_password_ids(&w, &arr, &lo);
        if (n >= 0) {
            size_t len = 0;
            *ids_known = 1;
            for (LONG i = lo; i < lo + n; i++) {
                BSTR id = NULL;
                char u[64];
                /* 保护器 ID 形如 {1A2B3C4D-…}：前 8 位就是恢复界面与 Microsoft 账户页面上的"密钥 ID" */
                if (FAILED(SafeArrayGetElement(arr, &i, &id)) || !id)
                    continue;
                gk3w_w2u(id, u, sizeof u);
                SysFreeString(id);
                if (u[0] != '{' || strlen(u) < 9 || len + 10 > cap)
                    continue;
                if (len)
                    ids[len++] = ',';
                memcpy(ids + len, u + 1, 8);
                len += 8;
                ids[len] = 0;
            }
        }
        if (arr)
            SafeArrayDestroy(arr);
    }
    blw_close(&w);
    return status;
}

gk3w_bl_result gk3w_bitlocker_verify(wchar_t drive, const char *key, int *bad_group)
{
    char want[GK3W_BLKEY_TEXT];
    blw w;
    SAFEARRAY *arr = NULL;
    LONG lo = 0;
    int n, unreadable = 0, g;
    gk3w_bl_result res;

    *bad_group = 0;
    g = gk3w_blkey_normalize(key, want);
    if (g) {
        *bad_group = g > 0 ? g : 0;
        return GK3W_BL_FORMAT;
    }
    if (blw_open(&w, drive)) {
        gk3w_log("BitLocker: cannot query volume %c: through WMI", (char)drive);
        blw_close(&w);
        gk3w_wipe(want, sizeof want);
        return GK3W_BL_QUERY;
    }
    n = blw_password_ids(&w, &arr, &lo);
    if (n < 0) {
        res = GK3W_BL_QUERY;
    } else if (n == 0) {
        res = GK3W_BL_NO_PASSWORD;
    } else {
        res = GK3W_BL_WRONG;
        for (LONG i = lo; i < lo + n && res != GK3W_BL_OK; i++) {
            IWbemClassObject *out = NULL;
            VARIANT in, pw;
            HRESULT hr;
            BSTR id = NULL;
            if (FAILED(SafeArrayGetElement(arr, &i, &id)) || !id) {
                unreadable = 1;
                continue;
            }
            VariantInit(&in);
            in.vt = VT_BSTR;
            in.bstrVal = id;
            hr = blw_call(&w, L"GetKeyProtectorNumericalPassword", L"VolumeKeyProtectorID", &in, &out);
            VariantClear(&in);
            if (FAILED(hr)) {
                gk3w_log("BitLocker: GetKeyProtectorNumericalPassword failed (0x%08lx)", (unsigned long)hr);
                unreadable = 1;
                continue;
            }
            VariantInit(&pw);
            if (SUCCEEDED(IWbemClassObject_Get(out, L"NumericalPassword", 0, &pw, NULL, NULL)) && pw.vt == VT_BSTR && pw.bstrVal) {
                char have[GK3W_BLKEY_TEXT * 2];
                gk3w_w2u(pw.bstrVal, have, sizeof have);
                if (gk3w_blkey_equal(want, have))
                    res = GK3W_BL_OK;
                gk3w_wipe(have, sizeof have);
                SecureZeroMemory(pw.bstrVal, SysStringByteLen(pw.bstrVal));
            } else {
                unreadable = 1;
            }
            VariantClear(&pw);
            IWbemClassObject_Release(out);
        }
        /* 有的读不出来、读出来的又都不一致：不能断定用户输错了 */
        if (res == GK3W_BL_WRONG && unreadable)
            res = GK3W_BL_QUERY;
    }
    if (arr)
        SafeArrayDestroy(arr);
    blw_close(&w);
    gk3w_wipe(want, sizeof want);
    return res;
}

int gk3w_bitlocker_suspend(wchar_t drive, unsigned reboots)
{
    blw w;
    IWbemClassObject *out = NULL;
    VARIANT c;
    HRESULT hr;
    int rc = -1;

    if (blw_open(&w, drive)) {
        gk3w_log("BitLocker: cannot query volume %c: through WMI, protection not suspended", (char)drive);
        blw_close(&w);
        return -1;
    }
    VariantInit(&c);
    c.vt = VT_I4;
    c.lVal = (LONG)reboots;
    /* WMI 里这个参数叫 DisableCount（Suspend-BitLocker 的 -RebootCount、manage-bde 的 -RebootCount 都映射到它） */
    hr = blw_call(&w, L"DisableKeyProtectors", L"DisableCount", &c, &out);
    if (SUCCEEDED(hr)) {
        IWbemClassObject_Release(out);
        gk3w_log("BitLocker: protection of %c: suspended; Windows resumes it after %u restart(s)", (char)drive, reboots);
        rc = 0;
    } else {
        gk3w_log("BitLocker: DisableKeyProtectors on %c: failed (0x%08lx)", (char)drive, (unsigned long)hr);
    }
    blw_close(&w);
    return rc;
}

int gk3w_bitlocker_err(gk3w_bl_result r, int bad_group, gk3w_touched t)
{
    const char *tail = t == GK3W_TOUCHED_NO ? " (disk not touched)" : "";
    char human[320], grp[8];
    switch (r) {
    case GK3W_BL_FORMAT:
        if (bad_group > 0) {
            snprintf(human, sizeof human, "the recovery key is not valid: group %d of 8 fails its checksum, most likely a typing error%s", bad_group, tail);
            snprintf(grp, sizeof grp, "%d", bad_group);
            return gk3w_err_kv(t, "bitlocker-key-format", "group", grp, human, 0);
        }
        snprintf(human, sizeof human, "the recovery key is not valid: it must be 48 digits in 8 groups of 6%s", tail);
        return gk3w_err(t, "bitlocker-key-format", human, 0);
    case GK3W_BL_WRONG:
        snprintf(human, sizeof human, "the recovery key does not match any BitLocker recovery key of the volume%s", tail);
        return gk3w_err(t, "bitlocker-key-wrong", human, 0);
    case GK3W_BL_NO_PASSWORD:
        snprintf(human, sizeof human,
                 "the system volume has no BitLocker recovery key (numerical password protector), so there is nothing to check the key "
                 "against. Add one first (manage-bde -protectors -add C: -RecoveryPassword) and keep it safe%s",
                 tail);
        return gk3w_err(t, "bitlocker-no-recovery-key", human, 0);
    case GK3W_BL_QUERY:
    default:
        snprintf(human, sizeof human, "cannot read the BitLocker recovery keys of the system volume through WMI, so the recovery key cannot be checked%s", tail);
        return gk3w_err(t, "bitlocker-unverifiable", human, 0);
    }
}

int gk3w_read_key_stdin(char *buf, size_t cap)
{
    size_t n;
    if (!cap)
        return -1;
    /* 不经 C 运行库的缓冲区：那里的一份清不掉 */
    setvbuf(stdin, NULL, _IONBF, 0);
    if (!fgets(buf, cap > 0x7fffffff ? 0x7fffffff : (int)cap, stdin)) {
        buf[0] = 0;
        return -1;
    }
    n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    return n ? 0 : -1;
}

int gk3w_cmd_bitlocker_key(int argc, wchar_t **argv)
{
    const wchar_t *vol = gk3w_arg(argc, argv, L"--volume");
    uint64_t reboots = 0;
    wchar_t drive, sysdrive, win[MAX_PATH];
    char key[128], dl[3];
    int group = 0;
    gk3w_bl_result r;
    gk3w_line l = {0};

    if ((gk3w_arg(argc, argv, L"--suspend") && (gk3w_arg_u64(argc, argv, L"--suspend", &reboots) || reboots < 1 || reboots > 15)) ||
        (vol && (!iswalpha(vol[0]) || vol[1] != L':' || vol[2])))
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: bitlocker-key [--suspend 1-15] [--volume X:]  (the recovery key is read from standard input)", 0);
    sysdrive = GetSystemWindowsDirectoryW(win, MAX_PATH) && win[1] == L':' ? towupper(win[0]) : L'C';
    drive = vol ? towupper(vol[0]) : sysdrive;
    /* 暂停系统卷的保护只为在这台平板上安装：别的电脑上不动它（--volume 指别的卷用于测试） */
    if (reboots && drive == sysdrive && !gk3w_model_ok_now())
        return gk3w_err(GK3W_TOUCHED_NA, "wrong-model", "this computer is not a Huawei MateBook E Go (gaokun3); not suspending its BitLocker protection", 0);
    if (gk3w_read_key_stdin(key, sizeof key)) {
        gk3w_wipe(key, sizeof key);
        return gk3w_err(GK3W_TOUCHED_NA, "bitlocker-key-required", "no recovery key on standard input", 0);
    }
    r = gk3w_bitlocker_verify(drive, key, &group);
    gk3w_wipe(key, sizeof key);
    if (r != GK3W_BL_OK)
        return gk3w_bitlocker_err(r, group, GK3W_TOUCHED_NA);
    gk3w_log("BitLocker: the recovery key matches volume %c:", (char)drive);
    if (reboots && gk3w_bitlocker_suspend(drive, (unsigned)reboots))
        return gk3w_err(GK3W_TOUCHED_NA, "bitlocker-suspend-failed", "the recovery key is correct, but BitLocker protection could not be suspended", 0);
    dl[0] = (char)drive;
    dl[1] = ':';
    dl[2] = 0;
    gk3w_line_begin(&l, "BITLOCKER");
    gk3w_line_kv(&l, "volume", dl);
    gk3w_line_kv(&l, "key", "ok");
    if (reboots)
        gk3w_line_u64(&l, "suspended", reboots);
    (void)gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
    return 0;
}
