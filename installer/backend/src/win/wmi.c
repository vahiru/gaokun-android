#include "wmi.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "disk.h"
#include "win.h"

int gk3w_wmi_open(gk3w_wmi *w, const wchar_t *ns)
{
    BSTR b = NULL;
    int rc = -1;

    memset(w, 0, sizeof *w);
    w->init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* 进程里已经设过安全参数时这里会失败，不影响后面（代理上另设 CoSetProxyBlanket） */
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE,
                         NULL);
    if (FAILED(CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&w->loc)))
        goto out;
    b = SysAllocString(ns);
    /* WBEM_FLAG_CONNECT_USE_MAX_WAIT：WMI 服务卡住时连接最多等 2 分钟（Windows 的上限），不无限等 */
    if (FAILED(IWbemLocator_ConnectServer(w->loc, b, NULL, NULL, NULL, WBEM_FLAG_CONNECT_USE_MAX_WAIT, NULL, NULL, &w->svc)))
        goto out;
    /* 包级加密：BitLocker 的命名空间要求加密，取回的恢复密钥也要经过 RPC */
    CoSetProxyBlanket((IUnknown *)w->svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                      RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    rc = 0;
out:
    SysFreeString(b);
    return rc;
}

void gk3w_wmi_close(gk3w_wmi *w)
{
    if (w->svc)
        IWbemServices_Release(w->svc);
    if (w->loc)
        IWbemLocator_Release(w->loc);
    if (SUCCEEDED(w->init))
        CoUninitialize();
    memset(w, 0, sizeof *w);
    w->init = E_FAIL;
}

int gk3w_wmi_find(gk3w_wmi *w, const wchar_t *wql, ULONG timeout_ms, int (*pick)(IWbemClassObject *o, void *ctx), void *ctx,
                  IWbemClassObject **out)
{
    IEnumWbemClassObject *en = NULL;
    BSTR lang = SysAllocString(L"WQL"), query = SysAllocString(wql);
    int rc = -1;

    *out = NULL;
    if (!w->svc || FAILED(IWbemServices_ExecQuery(w->svc, lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en)))
        goto done;
    for (;;) {
        IWbemClassObject *o = NULL;
        ULONG n = 0;
        HRESULT hr = IEnumWbemClassObject_Next(en, (LONG)timeout_ms, 1, &o, &n);
        if (hr == WBEM_S_FALSE && n == 0) {
            rc = 1;   /* 没有（更多）结果 */
            break;
        }
        if (hr != WBEM_S_NO_ERROR || n != 1)
            break;   /* 失败或超时（WBEM_S_TIMEDOUT） */
        if (!pick || pick(o, ctx)) {
            *out = o;
            rc = 0;
            break;
        }
        IWbemClassObject_Release(o);
    }
done:
    if (en)
        IEnumWbemClassObject_Release(en);
    SysFreeString(lang);
    SysFreeString(query);
    return rc;
}

/* 取对象的一个字符串属性（__CLASS、__PATH）；调用方 SysFreeString */
static BSTR get_bstr(IWbemClassObject *o, const wchar_t *name)
{
    VARIANT v;
    BSTR r = NULL;
    VariantInit(&v);
    if (SUCCEEDED(IWbemClassObject_Get(o, name, 0, &v, NULL, NULL)) && v.vt == VT_BSTR && v.bstrVal) {
        r = v.bstrVal;   /* 接管 */
        v.vt = VT_EMPTY;
    }
    VariantClear(&v);
    return r;
}

HRESULT gk3w_wmi_call(gk3w_wmi *w, IWbemClassObject *obj, const wchar_t *method, const wchar_t *pname, VARIANT *pval,
                      ULONG timeout_ms, IWbemClassObject **out)
{
    IWbemClassObject *cls = NULL, *def = NULL, *in = NULL;
    IWbemCallResult *cr = NULL;
    BSTR cname = get_bstr(obj, L"__CLASS"), path = get_bstr(obj, L"__PATH"), m = SysAllocString(method);
    VARIANT rv;
    HRESULT hr = cname && path ? S_OK : E_UNEXPECTED;

    *out = NULL;
    if (SUCCEEDED(hr) && pname) {
        /* 入参的定义从类上取（半同步，最多等 30 秒） */
        hr = IWbemServices_GetObject(w->svc, cname, WBEM_FLAG_RETURN_IMMEDIATELY, NULL, NULL, &cr);
        if (SUCCEEDED(hr))
            hr = IWbemCallResult_GetResultObject(cr, 30000, &cls);
        if (hr == WBEM_S_TIMEDOUT)
            hr = WBEM_E_TIMED_OUT;
        if (cr) {
            IWbemCallResult_Release(cr);
            cr = NULL;
        }
        if (SUCCEEDED(hr))
            hr = IWbemClassObject_GetMethod(cls, method, 0, &def, NULL);
        if (SUCCEEDED(hr))
            hr = IWbemClassObject_SpawnInstance(def, 0, &in);
        if (SUCCEEDED(hr))
            hr = IWbemClassObject_Put(in, pname, 0, pval, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IWbemServices_ExecMethod(w->svc, path, m, WBEM_FLAG_RETURN_IMMEDIATELY, NULL, in, NULL, &cr);
        if (SUCCEEDED(hr)) {
            hr = IWbemCallResult_GetResultObject(cr, (LONG)timeout_ms, out);
            if (hr == WBEM_S_TIMEDOUT)
                hr = WBEM_E_TIMED_OUT;
        }
    }
    if (SUCCEEDED(hr)) {
        VariantInit(&rv);
        if (*out && SUCCEEDED(IWbemClassObject_Get(*out, L"ReturnValue", 0, &rv, NULL, NULL)) && (rv.vt == VT_I4 || rv.vt == VT_UI4)) {
            if (rv.lVal) {
                char mn[64];
                gk3w_w2u(method, mn, sizeof mn);
                gk3w_log("WMI: %s returned %ld (0x%08lx)", mn, (long)rv.lVal, (unsigned long)rv.lVal);
            }
            hr = rv.lVal == 0 ? S_OK : FAILED((HRESULT)rv.lVal) ? (HRESULT)rv.lVal : E_FAIL;
        } else {
            hr = E_UNEXPECTED;
        }
        VariantClear(&rv);
    }
    if (FAILED(hr) && *out) {
        IWbemClassObject_Release(*out);
        *out = NULL;
    }
    if (cr)
        IWbemCallResult_Release(cr);
    if (in)
        IWbemClassObject_Release(in);
    if (def)
        IWbemClassObject_Release(def);
    if (cls)
        IWbemClassObject_Release(cls);
    SysFreeString(cname);
    SysFreeString(path);
    SysFreeString(m);
    return hr;
}

int gk3w_wmi_u64(IWbemClassObject *o, const wchar_t *name, uint64_t *out)
{
    VARIANT v;
    int rc = -1;
    VariantInit(&v);
    if (SUCCEEDED(IWbemClassObject_Get(o, name, 0, &v, NULL, NULL))) {
        switch (v.vt) {
        case VT_UI1:
            *out = v.bVal;
            rc = 0;
            break;
        case VT_I2:
        case VT_UI2:
            *out = (uint16_t)v.uiVal;
            rc = 0;
            break;
        case VT_I4:
        case VT_UI4:
            *out = (uint32_t)v.ulVal;
            rc = 0;
            break;
        case VT_BSTR:   /* uint64 / sint64 在 WMI 的 VARIANT 里是十进制字符串 */
            if (v.bstrVal && v.bstrVal[0] >= L'0' && v.bstrVal[0] <= L'9') {
                wchar_t *end = NULL;
                *out = _wcstoui64(v.bstrVal, &end, 10);
                rc = end && !*end ? 0 : -1;
            }
            break;
        default:
            break;
        }
    }
    VariantClear(&v);
    return rc;
}
