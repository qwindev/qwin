#pragma once

// Helpers shared by the .cpp files TilingApi is split across (listed in
// tilingapi.h); nothing else includes it. A helper only one of them uses
// stays in that file's anonymous namespace.

#include <QHash>
#include <QRect>
#include <QString>

#include <windows.h>

namespace tiling {

inline constexpr int kPendingTimeoutMs = 5000;

inline QRect toRect(const RECT &r)
{
    return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

inline HWND toHwnd(quintptr id) { return reinterpret_cast<HWND>(id); }
inline quintptr toId(void *hwnd) { return reinterpret_cast<quintptr>(hwnd); }

inline BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param)
{
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi)) {
        auto *out = reinterpret_cast<QHash<QString, QRect> *>(param);
        out->insert(QString::fromWCharArray(mi.szDevice), toRect(mi.rcWork));
    }
    return TRUE;
}

// Device name -> work area, physical pixels.
inline QHash<QString, QRect> workAreas()
{
    QHash<QString, QRect> out;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&out));
    return out;
}

// The desktop itself: Progman, or the WorkerW that hosts the icons instead
// while a wallpaper slideshow runs. Both span the whole virtual desktop, so
// MonitorFromWindow on one says nothing about which monitor the user means.
inline bool isDesktopWindow(HWND hwnd)
{
    if (!hwnd)
        return false;
    if (hwnd == GetShellWindow())
        return true;
    wchar_t cls[32] = {};
    const int n = GetClassNameW(hwnd, cls, 32);
    const QString name = QString::fromWCharArray(cls, n);
    return name == QLatin1String("Progman") || name == QLatin1String("WorkerW");
}

// The monitor the mouse is over right now, or empty if the cursor or its
// monitor could not be read. Shared by focusedDevice()'s fallback and the
// desktop-window case in onForegroundChanged().
inline QString deviceUnderCursor()
{
    POINT pt = {};
    if (!GetCursorPos(&pt))
        return QString();
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi))
        return QString();
    return QString::fromWCharArray(mi.szDevice);
}

} // namespace tiling
