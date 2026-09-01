#include "windowfocus.h"

#include <windows.h>
#include <dwmapi.h>

namespace windowfocus {

bool isFocusableAppWindow(void *handle)
{
    const HWND hwnd = static_cast<HWND>(handle);
    if (!hwnd || !IsWindow(hwnd))
        return false;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
        return false;
    if (GetWindow(hwnd, GW_OWNER))
        return false; // owned popups follow their owner
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return false; // our panels, and anything else outside Alt+Tab
    if (GetWindowTextLengthW(hwnd) == 0)
        return false;
    // Cloaked windows - background UWP hosts, windows parked on another
    // desktop - are composited away yet stay IsWindowVisible and sit high in
    // the Z-order. Windows hands them the keyboard and the keystrokes go
    // nowhere (verified: without this, focus stuck on TextInputHost).
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked != 0)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() || hwnd == GetShellWindow())
        return false;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"Progman") != 0 && wcscmp(cls, L"WorkerW") != 0
           && wcscmp(cls, L"Shell_TrayWnd") != 0;
}

void focusTopmostWindow(const std::function<bool(void *)> &accept)
{
    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        if (!isFocusableAppWindow(hwnd) || (accept && !accept(hwnd)))
            continue;
        SetForegroundWindow(hwnd);
        return;
    }
    if (HWND shell = GetShellWindow())
        SetForegroundWindow(shell);
}

} // namespace windowfocus
