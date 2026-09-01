#pragma once

#include <QLibrary>
#include <QObject>
#include <QTimer>
#include <QUuid>

// The `Desktops` QML singleton. Windows has no public virtual-desktop API,
// so everything goes through VirtualDesktopAccessor.dll
// (github.com/Ciantic/VirtualDesktopAccessor, MIT), which wraps the
// undocumented COM interfaces and tracks their per-build GUID changes.
// Loaded at runtime from the exe directory; without it the singleton is
// inert (available == false, count 1, warning no-ops). Switches arrive
// instantly through the DLL's PostMessage hook into a message-only window;
// a slow poll keeps `count` fresh, as creation/removal has no hook.
class VirtualDesktops : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY changed)
public:
    explicit VirtualDesktops(QObject *parent = nullptr);
    ~VirtualDesktops() override;

    bool available() const { return m_available; }
    int count() const { return m_count; }
    int currentIndex() const { return m_currentIndex; }

    Q_INVOKABLE void switchTo(int index);
    Q_INVOKABLE void createDesktop();
    Q_INVOKABLE void closeCurrentDesktop();
    // Sends the focused window to a fresh desktop, follows it and maximizes
    // it. Meant for a Hotkey: WM_HOTKEY does not shift focus, so the
    // foreground window is still the one the user was in.
    Q_INVOKABLE void moveForegroundWindowToNewDesktop();

    // The window's own virtual desktop - not "whichever is current" - so the
    // tiler (a separate unit) can key its layouts by the window's real
    // desktop instead of a shifting index. Null when unresolvable: the
    // window is pinned to all desktops, or the query failed transiently -
    // GetWindowDesktopId (below) cannot tell those apart, so callers must
    // treat a null result conservatively rather than as "no desktop".
    QUuid windowDesktopId(void *hwnd) const;
    // False without VirtualDesktopAccessor.dll, or with an older build that
    // lacks the GetWindowDesktopId export; windowDesktopId() then always
    // returns null and callers should not bother asking.
    bool supportsWindowDesktopId() const { return m_getWindowDesktopIdRaw != nullptr; }

signals:
    void changed();

public slots:
    void refresh(); // also called from the message-only window's WndProc

private:
    // Hands the keyboard to a window on the desktop we just landed on.
    void focusWindowOnCurrentDesktop();

    QLibrary m_dll;
    bool m_available = false;
    int m_count = 1;
    int m_currentIndex = 0;
    QTimer m_pollTimer;
    void *m_msgWindow = nullptr; // HWND receiving the DLL's notifications

    // VirtualDesktopAccessor exports; all return -1 on failure.
    int (*m_getDesktopCount)() = nullptr;
    int (*m_getCurrentDesktop)() = nullptr;
    int (*m_goToDesktop)(int) = nullptr;
    int (*m_createDesktop)() = nullptr;
    int (*m_removeDesktop)(int, int) = nullptr;
    int (*m_moveWindowToDesktop)(void *, int) = nullptr; // (HWND, index)
    int (*m_registerHook)(void *, unsigned int) = nullptr;
    int (*m_unregisterHook)(void *) = nullptr;
    // Optional: an older DLL without it keeps the old (wrong) focus behaviour
    // rather than losing switching altogether.
    int (*m_isWindowOnCurrentDesktop)(void *) = nullptr; // (HWND)
    // Optional, resolved but left untyped here: GetWindowDesktopId returns a
    // GUID by value (16 bytes, so the x64 ABI passes it back through a
    // hidden pointer), and pulling that type in would need windows.h in this
    // header. Cast to the real signature only in the .cpp, where windows.h
    // is already included.
    void *m_getWindowDesktopIdRaw = nullptr; // (HWND) -> GUID
};
