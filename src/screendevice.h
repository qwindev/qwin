#pragma once

// The one sanctioned QScreen <-> GDI device mapping.
//
// Measured fact (Qt 6.8.3, this machine): QScreen::name() / QML Screen.name /
// Qt.application.screens[i].name return the monitor's FRIENDLY name ("LG HDR
// WQHD", "VDD by MTT"), not the GDI device name ("\\.\DISPLAY5"). Friendly
// names are not even unique - two identical monitors report the same one.
// The tiler and the AppBar code key everything by szDevice, so a QScreen's
// name must never be used as that key; go through nameOf()/find() instead,
// which resolve the device via the public native interface
// (QNativeInterface::QWindowsScreen::handle() -> HMONITOR ->
// GetMonitorInfoW's szDevice).

#include <QGuiApplication>
#include <QScreen>
#include <QString>
#include <QtGui/qscreen_platform.h>

#include <windows.h>

namespace screendevice {

// szDevice of `screen`, or empty if it could not be resolved.
inline QString nameOf(const QScreen *screen)
{
    if (!screen)
        return QString();
    auto *win = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
    if (!win)
        return QString();
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(win->handle(), &mi))
        return QString();
    return QString::fromWCharArray(mi.szDevice);
}

// The QScreen whose szDevice is `device`, or nullptr if none matches.
inline QScreen *find(const QString &device)
{
    for (QScreen *screen : QGuiApplication::screens()) {
        if (nameOf(screen) == device)
            return screen;
    }
    return nullptr;
}

} // namespace screendevice
