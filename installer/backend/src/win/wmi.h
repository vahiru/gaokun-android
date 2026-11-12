/* Windows：WMI 的连接、查询与实例方法调用（bitlocker.c、shrink.c 共用）。
 * 查询与方法调用都是半同步的、带超时：WMI 服务或提供程序卡住时，检查与安装不能一直不返回。 */
#ifndef GK3W_WMI_H
#define GK3W_WMI_H

#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <oleauto.h>
#include <wbemidl.h>

#include <stdint.h>

typedef struct {
    HRESULT init;
    IWbemLocator *loc;
    IWbemServices *svc;
} gk3w_wmi;

/* 连上一个命名空间（包级加密）。失败返回 -1；不论成败，之后都要 gk3w_wmi_close */
int gk3w_wmi_open(gk3w_wmi *w, const wchar_t *ns);
void gk3w_wmi_close(gk3w_wmi *w);

/* 执行 WQL 查询，逐个结果交给 pick（返回非 0 = 就是它；pick 为 NULL 时取第一个），找到时 *out 由调用方释放。
 * 每取一个结果最多等 timeout_ms。返回 0 = 找到，1 = 没有，-1 = 查询失败或超时 */
int gk3w_wmi_find(gk3w_wmi *w, const wchar_t *wql, ULONG timeout_ms, int (*pick)(IWbemClassObject *o, void *ctx), void *ctx,
                  IWbemClassObject **out);

/* 调实例 obj（gk3w_wmi_find 找到的）的一个方法，入参至多一个（pname 为 NULL 时不带）。
 * 方法的 ReturnValue 不是 0 就算失败（返回它，不是负数时换成 E_FAIL，并写日志）；调用本身失败或 timeout_ms 内
 * 没有结果时返回调用的 HRESULT。成功时 *out 由调用方释放 */
HRESULT gk3w_wmi_call(gk3w_wmi *w, IWbemClassObject *obj, const wchar_t *method, const wchar_t *pname, VARIANT *pval,
                      ULONG timeout_ms, IWbemClassObject **out);

/* 读一个非负整数属性：整数类型，或 uint64 在 WMI 里的十进制字符串形式。读不到返回 -1 */
int gk3w_wmi_u64(IWbemClassObject *o, const wchar_t *name, uint64_t *out);

#endif
