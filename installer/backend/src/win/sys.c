/* Windows：安装前检查要看的系统状态、systemd-boot 的 EFI 变量。
 *
 *   gk3w preflight     一行 PREFLIGHT：机型、Secure Boot、BitLocker（系统卷）、快速启动、休眠文件、电源（只读）
 *
 * 判断规则（哪些拒绝、哪些只提醒）在 gk3w_sys_gate，apply / uninstall 写真实磁盘之前调用；VHD 测试不调。 */
#define COBJMACROS
#include <windows.h>
#include <wbemidl.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "../proto.h"
#include "disk.h"
#include "win.h"

/* 允许写真实磁盘的机型（SMBIOS 的 SystemProductName）：华为 MateBook E Go（gaokun3） */
static const char *const MODELS[] = {"GK-W7X"};

/* systemd-boot 的厂商 GUID（LoaderEntryDefault 等变量所在的命名空间） */
#define LOADER_GUID L"{4a67b082-0a4c-41cf-b6c7-440b29bb8c4f}"

static int reg_dword(const wchar_t *key, const wchar_t *val, DWORD *out)
{
    DWORD sz = sizeof *out;
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, val, RRF_RT_REG_DWORD, NULL, out, &sz) == ERROR_SUCCESS ? 0 : -1;
}

static void reg_str(const wchar_t *key, const wchar_t *val, char *out, size_t cap)
{
    wchar_t w[128];
    DWORD sz = sizeof w;
    out[0] = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, val, RRF_RT_REG_SZ, NULL, w, &sz) == ERROR_SUCCESS)
        gk3w_w2u(w, out, cap);
}

/* 系统卷的 BitLocker 保护状态（WMI Win32_EncryptableVolume.ProtectionStatus）：0 关、1 开、2 未知（卷锁着时就是这个值）；查不到 -1 */
static int bitlocker_protection(wchar_t drive)
{
    IWbemLocator *loc = NULL;
    IWbemServices *svc = NULL;
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *obj = NULL;
    BSTR ns = NULL, lang = NULL, query = NULL;
    wchar_t q[128];
    ULONG n = 0;
    VARIANT v;
    int status = -1;
    HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    /* 进程里已经有人设过安全参数时这里会失败，不影响后面 */
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE,
                         NULL);
    if (FAILED(CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&loc)))
        goto out;
    ns = SysAllocString(L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption");
    if (FAILED(IWbemLocator_ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, &svc)))
        goto out;
    CoSetProxyBlanket((IUnknown *)svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    _snwprintf(q, 128, L"SELECT ProtectionStatus FROM Win32_EncryptableVolume WHERE DriveLetter='%lc:'", drive);
    lang = SysAllocString(L"WQL");
    query = SysAllocString(q);
    if (FAILED(IWbemServices_ExecQuery(svc, lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en)))
        goto out;
    if (IEnumWbemClassObject_Next(en, (LONG)WBEM_INFINITE, 1, &obj, &n) != WBEM_S_NO_ERROR || n != 1)
        goto out;
    VariantInit(&v);
    if (SUCCEEDED(IWbemClassObject_Get(obj, L"ProtectionStatus", 0, &v, NULL, NULL))) {
        if (v.vt == VT_I4 || v.vt == VT_UI4)
            status = (int)v.lVal;
        VariantClear(&v);
    }
out:
    if (obj)
        IWbemClassObject_Release(obj);
    if (en)
        IEnumWbemClassObject_Release(en);
    if (svc)
        IWbemServices_Release(svc);
    if (loc)
        IWbemLocator_Release(loc);
    SysFreeString(ns);
    SysFreeString(lang);
    SysFreeString(query);
    if (SUCCEEDED(init))
        CoUninitialize();
    return status;
}

/* 从物理盘直接读系统卷的引导扇区（在 BitLocker 过滤驱动之下）：加密卷的 OEM ID 是 "-FVE-FS-"。
 * 1 = 加密卷；0 = 不是；-1 = 读不到。不依赖 WMI 服务（LiveCD 版用 blkid 按盘上签名判断，同一个思路）。 */
static int fve_signature(wchar_t drive)
{
    wchar_t dev[8], path[40];
    HANDLE h;
    VOLUME_DISK_EXTENTS e;
    DWORD got = 0;
    uint8_t *buf;
    int r = -1;
    _snwprintf(dev, 8, L"\\\\.\\%lc:", drive);
    h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &e, sizeof e, &got, NULL) || e.NumberOfDiskExtents != 1) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    _snwprintf(path, 40, L"\\\\.\\PhysicalDrive%lu", (unsigned long)e.Extents[0].DiskNumber);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    buf = _aligned_malloc(4096, 4096);
    if (buf && !gk3w_pread(h, buf, 4096, (uint64_t)e.Extents[0].StartingOffset.QuadPart))
        r = !memcmp(buf + 3, "-FVE-FS-", 8);
    _aligned_free(buf);
    CloseHandle(h);
    return r;
}

void gk3w_sys_read(gk3w_sys *s)
{
    wchar_t win[MAX_PATH], hib[16];
    DWORD d;
    SYSTEM_POWER_STATUS ps;
    memset(s, 0, sizeof *s);
    reg_str(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemManufacturer", s->manufacturer, sizeof s->manufacturer);
    reg_str(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName", s->model, sizeof s->model);
    s->secure_boot = reg_dword(L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", L"UEFISecureBootEnabled", &d) ? -1 : (int)d;
    s->hibernate = reg_dword(L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled", &d) ? -1 : !!d;
    /* 只看 HiberbootEnabled，不管休眠开没开：休眠关着时快速启动暂时不起作用，但以后一打开休眠（powercfg /h on）
     * 它就悄悄回来了，那时 Android 写 ESP 正好撞上 Windows 休眠 */
    s->fast_startup = !reg_dword(L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", L"HiberbootEnabled", &d) && d == 1;
    s->system_drive = GetSystemWindowsDirectoryW(win, MAX_PATH) && win[1] == L':' ? win[0] : L'C';
    _snwprintf(hib, 16, L"%lc:\\hiberfil.sys", s->system_drive);
    {
        /* hiberfil.sys 被系统独占打开，GetFileAttributesW 会因共享冲突失败；目录项总读得到 */
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW(hib, &fd);
        s->hiberfil = fh != INVALID_HANDLE_VALUE;
        if (fh != INVALID_HANDLE_VALUE)
            FindClose(fh);
    }
    s->bitlocker = bitlocker_protection(s->system_drive);
    if (s->bitlocker < 0) {
        /* WMI 读不到：按盘上签名判断；签名也读不到就保持 -1，由 gk3w_sys_gate 当作开着处理 */
        int f = fve_signature(s->system_drive);
        if (f >= 0)
            s->bitlocker = f;
    }
    s->ac = -1;
    s->battery = -1;
    if (GetSystemPowerStatus(&ps)) {
        s->ac = ps.ACLineStatus == 255 ? -1 : ps.ACLineStatus;
        s->battery = ps.BatteryLifePercent == 255 ? -1 : ps.BatteryLifePercent;
    }
}

int gk3w_model_ok(const gk3w_sys *s)
{
    for (size_t i = 0; i < sizeof MODELS / sizeof MODELS[0]; i++)
        if (!strcmp(s->model, MODELS[i]))
            return 1;
    return 0;
}

/* 说明见 disk.h。bitlocker_ok 同 LiveCD 的 --bitlocker-key yes。 */
int gk3w_sys_gate(const gk3w_sys *s, int bitlocker_ok, gk3w_gate_mode mode)
{
    if (!gk3w_model_ok(s))
        return gk3w_err_kv(GK3W_TOUCHED_NO, "wrong-model", "model", s->model, "this computer is not a Huawei MateBook E Go (gaokun3); refusing to write a real disk", 0), -1;
    if (mode == GK3W_GATE_APPLY && s->secure_boot == 1)
        return gk3w_err(GK3W_TOUCHED_NO, "secure-boot-on", "Secure Boot is on: Android would not boot. Turn Secure Boot off in the firmware setup first (disk not touched)", 0), -1;
    if (mode == GK3W_GATE_APPLY && s->fast_startup == 1)
        return gk3w_err(GK3W_TOUCHED_NO, "fast-startup-on",
                        s->hibernate == 0
                            ? "Windows Fast Startup is set (HiberbootEnabled=1); hibernation is off now, but Fast Startup returns as soon as it is turned back on, and then Windows may corrupt the EFI system partition after Android writes to it. Turn it off first: reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power\" /v HiberbootEnabled /t REG_DWORD /d 0 /f (disk not touched)"
                            : "Windows Fast Startup is on: shutting down actually hibernates, and Windows may corrupt the EFI system partition after Android writes to it. Turn Fast Startup off in Power Options first (disk not touched)",
                        0), -1;
    /* 只有确认关着才放行（读不到、状态未知都当作开着）：宁可多要一次确认，也不能让没拿恢复密钥的人改 BOOTAA64.EFI */
    if (s->bitlocker != 0 && !bitlocker_ok)
        return gk3w_err(GK3W_TOUCHED_NO, "bitlocker-key-unconfirmed",
                        "BitLocker is on (or its state is unknown) and EFI/BOOT/BOOTAA64.EFI will be rewritten: Windows may ask for the 48-digit recovery key next time. Confirm you have the recovery key (--bitlocker-key yes) first (disk not touched)", 0), -1;
    if (s->ac == 0 && s->battery >= 0 && s->battery < 15)
        return gk3w_err(GK3W_TOUCHED_NO, "low-battery", "battery below 15% and not on AC power: losing power midway leaves a half-installed disk. Plug in the charger first (disk not touched)", 0), -1;
    if (mode == GK3W_GATE_APPLY && s->hiberfil)
        gk3w_log("note: a hibernation file exists (Fast Startup is off). Do not start Android while Windows is hibernated");
    return 0;
}

int gk3w_cmd_preflight(int argc, wchar_t **argv)
{
    gk3w_sys s;
    gk3w_line l = {0};
    (void)argc;
    (void)argv;
    gk3w_sys_read(&s);
    gk3w_line_begin(&l, "PREFLIGHT");
    gk3w_line_kv(&l, "manufacturer", s.manufacturer);
    gk3w_line_kv(&l, "model", s.model);
    gk3w_line_kv(&l, "model_ok", gk3w_model_ok(&s) ? "yes" : "no");
    gk3w_line_kv(&l, "secure_boot", s.secure_boot < 0 ? "unknown" : s.secure_boot ? "on" : "off");
    gk3w_line_kv(&l, "bitlocker", s.bitlocker < 0 ? "unknown" : s.bitlocker == 1 ? "on" : s.bitlocker == 2 ? "unknown" : "off");
    gk3w_line_kv(&l, "fast_startup", s.fast_startup ? "on" : "off");
    gk3w_line_kv(&l, "hibernate", s.hibernate < 0 ? "unknown" : s.hibernate ? "on" : "off");
    gk3w_line_kv(&l, "hiberfil", s.hiberfil ? "yes" : "no");
    gk3w_line_kv(&l, "ac", s.ac < 0 ? "unknown" : s.ac ? "yes" : "no");
    if (s.battery >= 0)
        gk3w_line_u64(&l, "battery", (uint64_t)s.battery);
    (void)gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
    return 0;
}

/* ── EFI 变量 ── */

static int enable_privilege(void)
{
    HANDLE tok;
    TOKEN_PRIVILEGES tp;
    int ok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return -1;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ok = LookupPrivilegeValueW(NULL, SE_SYSTEM_ENVIRONMENT_NAME, &tp.Privileges[0].Luid) &&
         AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok ? 0 : -1;
}

int gk3w_loader_var_delete(const wchar_t *name, int (*ours)(const char *value))
{
    BYTE old[512];
    DWORD n;
    char nm[64], val[256] = "";
    gk3w_w2u(name, nm, sizeof nm);
    if (enable_privilege()) {
        gk3w_log("cannot get SeSystemEnvironmentPrivilege, %s not checked", nm);
        return -1;
    }
    n = GetFirmwareEnvironmentVariableW(name, LOADER_GUID, old, sizeof old - 2);
    if (!n) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
            return 0;
        gk3w_log("cannot read EFI variable %s (%lu)", nm, (unsigned long)GetLastError());
        return -1;
    }
    old[n] = old[n + 1] = 0;   /* systemd-boot 存的是 UTF-16 */
    gk3w_w2u((const wchar_t *)old, val, sizeof val);
    if (ours && !ours(val)) {
        gk3w_log("EFI variable %s=%s is not ours, kept", nm, val);
        return 0;
    }
    if (!SetFirmwareEnvironmentVariableW(name, LOADER_GUID, NULL, 0)) {
        gk3w_line l = {0};
        gk3w_line_begin(&l, "NOTE");
        gk3w_line_kv(&l, "code", "loadervar-stuck");
        gk3w_line_kv(&l, "name", nm);
        gk3w_line_kv(&l, "value", val);
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
        gk3w_log("cannot delete EFI variable %s=%s (%lu)", nm, val, (unsigned long)GetLastError());
        return -1;
    }
    gk3w_log("deleted EFI variable %s (was %s)", nm, val);
    return 0;
}
