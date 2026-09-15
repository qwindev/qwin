// windows.h first: dwmapi.h, servprov.h and shobjidl_core.h assume its types.
#include <windows.h>
#include <dwmapi.h>
#include <servprov.h>
#include <shobjidl_core.h>

#include "windowhider.h"

#include <QDebug>

namespace hider {

namespace {

// ---- ImmersiveShell / IApplicationView(Collection) -------------------------
// Undocumented shell interfaces, no public header. The GUIDs and vtable
// layout below are stable from Windows 10 1809 through Windows 11 24H2.

// {C2F03A33-21F5-47FA-B4BB-156362A2F239}
const CLSID kClsidImmersiveShell = {
    0xC2F03A33, 0x21F5, 0x47FA, { 0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39 } };
// {1841C6D7-4F9D-42C0-AF41-8747538F10E5} (service id == IID)
const IID kIidApplicationViewCollection = {
    0x1841C6D7, 0x4F9D, 0x42C0, { 0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5 } };

// Only the vtable prefix up to the method we call, as audioapi.cpp does for
// IPolicyConfig. IInspectable's out-parameters are void*/int* rather than
// HSTRING*/TrustLevel*: only their vtable slot matters here, and the real
// types would pull in WinRT headers for nothing.
struct IApplicationView : public IUnknown
{
    // IInspectable
    virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG *, IID **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int *) = 0;
    // IApplicationView
    virtual HRESULT STDMETHODCALLTYPE SetFocus() = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchTo() = 0;
    virtual HRESULT STDMETHODCALLTYPE TryInvokeBack(IUnknown *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetThumbnailWindow(HWND *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMonitor(IUnknown **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVisibility(int *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCloak(int cloakType, int flags) = 0;
};

struct IApplicationViewCollection : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetViews(IUnknown **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IUnknown **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR, IUnknown **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND, IApplicationView **) = 0;
};

// ---- IVirtualDesktopManager -------------------------------------------------
// Documented, but the GUIDs are local constants rather than the SDK's extern
// symbols - one fewer implicit link-time dependency, as in audioapi.cpp.

// {AA509086-5CA9-4C25-8F95-589D3C07B48A}
const CLSID kClsidVirtualDesktopManager = {
    0xAA509086, 0x5CA9, 0x4C25, { 0x8F, 0x95, 0x58, 0x9D, 0x3C, 0x07, 0xB4, 0x8A } };
// {A5CD92FF-29BE-454C-8D04-D82879FB3F1B}
const IID kIidVirtualDesktopManager = {
    0xA5CD92FF, 0x29BE, 0x454C, { 0x8D, 0x04, 0xD8, 0x28, 0x79, 0xFB, 0x3F, 0x1B } };

// One hider behind the one Tiler, so cached process-wide. Acquired lazily so
// a machine that never enables the tiler never pays for CoCreateInstance.
IApplicationViewCollection *g_viewCollection = nullptr;
IVirtualDesktopManager *g_desktopManager = nullptr;

// explorer.exe restarted under a cached pointer: the RPC channel is dead, not
// just this call. Anything else is this window's own problem (a bad HWND).
bool isReacquireError(HRESULT hr)
{
    return hr == RPC_E_DISCONNECTED
        || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);
}

bool acquireViewCollection()
{
    IServiceProvider *sp = nullptr;
    HRESULT hr = CoCreateInstance(kClsidImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER,
                                   IID_PPV_ARGS(&sp));
    if (FAILED(hr) || !sp) {
        qWarning().noquote() << "hider: CoCreateInstance(ImmersiveShell) failed";
        return false;
    }
    IApplicationViewCollection *collection = nullptr;
    hr = sp->QueryService(kIidApplicationViewCollection, kIidApplicationViewCollection,
                          reinterpret_cast<void **>(&collection));
    sp->Release();
    if (FAILED(hr) || !collection) {
        qWarning().noquote() << "hider: QueryService(IApplicationViewCollection) failed";
        return false;
    }
    g_viewCollection = collection;
    return true;
}

bool ensureViewCollection()
{
    return g_viewCollection || acquireViewCollection();
}

// One attempt against the collection pointer as it stands; callers guarantee
// it is non-null. S_OK with no view must not read as success: the caller
// would record a hide that never happened and re-try it every rescan.
// Failing sends it to the minimize fallback instead.
HRESULT cloakOnce(HWND hwnd, int cloakType, int flags)
{
    IApplicationView *view = nullptr;
    HRESULT hr = g_viewCollection->GetViewForHwnd(hwnd, &view);
    if (FAILED(hr))
        return hr;
    if (!view)
        return E_FAIL;
    hr = view->SetCloak(cloakType, flags);
    view->Release();
    return hr;
}

// Every cloak/uncloak funnels through here: one re-acquire and one retry
// after an explorer restart, then give up until the next call.
HRESULT setCloakFor(HWND hwnd, int cloakType, int flags)
{
    if (!ensureViewCollection())
        return E_FAIL;
    HRESULT hr = cloakOnce(hwnd, cloakType, flags);
    if (isReacquireError(hr)) {
        g_viewCollection->Release();
        g_viewCollection = nullptr;
        if (acquireViewCollection())
            hr = cloakOnce(hwnd, cloakType, flags);
    }
    return hr;
}

bool ensureDesktopManager()
{
    if (g_desktopManager)
        return true;
    // InprocServer32, unlike ImmersiveShell: CLSCTX_LOCAL_SERVER fails this
    // one with REGDB_E_CLASSNOTREG (measured).
    HRESULT hr = CoCreateInstance(kClsidVirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                                   kIidVirtualDesktopManager,
                                   reinterpret_cast<void **>(&g_desktopManager));
    if (FAILED(hr) || !g_desktopManager) {
        qWarning().noquote() << "hider: CoCreateInstance(VirtualDesktopManager) failed";
        g_desktopManager = nullptr;
        return false;
    }
    return true;
}

bool isCloakedImpl(HWND hwnd)
{
    int cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked != 0;
}

bool onCurrentNativeDesktopImpl(HWND hwnd)
{
    if (!ensureDesktopManager())
        return true; // degrade, never fail
    BOOL onCurrent = TRUE;
    if (FAILED(g_desktopManager->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent)))
        return true; // same rule: an unresolvable query must never evict a window
    return onCurrent != FALSE;
}

} // namespace

bool cloakAvailable()
{
    return ensureViewCollection();
}

bool cloakAcquired()
{
    return g_viewCollection != nullptr;
}

bool hide(void *hwnd, Method m)
{
    switch (m) {
    case Method::Cloak: {
        const HRESULT hr = setCloakFor(static_cast<HWND>(hwnd), 1, 2);
        if (FAILED(hr))
            qWarning().noquote() << "hider: SetCloak(hide) failed for one window";
        return SUCCEEDED(hr);
    }
    case Method::Minimize:
        // Async: a hung app's queue never has to drain this for us to return.
        return ShowWindowAsync(static_cast<HWND>(hwnd), SW_MINIMIZE) != 0;
    case Method::None:
        return true;
    }
    return false;
}

bool show(void *hwnd, Method m)
{
    switch (m) {
    case Method::Cloak: {
        const HRESULT hr = setCloakFor(static_cast<HWND>(hwnd), 1, 0);
        if (FAILED(hr))
            qWarning().noquote() << "hider: SetCloak(show) failed for one window";
        return SUCCEEDED(hr);
    }
    case Method::Minimize:
        // NOACTIVATE: restoring a workspace must not decide who gets focus -
        // the caller does that explicitly afterwards.
        return ShowWindowAsync(static_cast<HWND>(hwnd), SW_SHOWNOACTIVATE) != 0;
    case Method::None:
        return true;
    }
    return false;
}

bool isCloaked(void *hwnd)
{
    return isCloakedImpl(static_cast<HWND>(hwnd));
}

bool onCurrentNativeDesktop(void *hwnd)
{
    return onCurrentNativeDesktopImpl(static_cast<HWND>(hwnd));
}

} // namespace hider
