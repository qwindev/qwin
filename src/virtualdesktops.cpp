#include "virtualdesktops.h"

#include "windowfocus.h"

#include <QCoreApplication>
#include <QDebug>

#include <windows.h>

namespace {

// Posted by the DLL on every switch. Its wParam/lParam carry the old and new
// index; refresh() rereads both authoritatively instead.
constexpr UINT kDesktopChangedMsg = WM_APP + 0x44;

LRESULT CALLBACK listenerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == kDesktopChangedMsg) {
        auto *self = reinterpret_cast<VirtualDesktops *>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self)
            self->refresh();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

VirtualDesktops::VirtualDesktops(QObject *parent)
    : QObject(parent)
{
    const QString dllPath = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/VirtualDesktopAccessor.dll");
    m_dll.setFileName(dllPath);
    if (!m_dll.load()) {
        qWarning().noquote() << "Desktops: cannot load" << dllPath
                             << "- desktop switching disabled";
        return;
    }

    m_getDesktopCount = reinterpret_cast<int (*)()>(m_dll.resolve("GetDesktopCount"));
    m_getCurrentDesktop = reinterpret_cast<int (*)()>(m_dll.resolve("GetCurrentDesktopNumber"));
    m_goToDesktop = reinterpret_cast<int (*)(int)>(m_dll.resolve("GoToDesktopNumber"));
    m_createDesktop = reinterpret_cast<int (*)()>(m_dll.resolve("CreateDesktop"));
    m_removeDesktop = reinterpret_cast<int (*)(int, int)>(m_dll.resolve("RemoveDesktop"));
    m_moveWindowToDesktop = reinterpret_cast<int (*)(void *, int)>(
        m_dll.resolve("MoveWindowToDesktopNumber"));
    m_registerHook = reinterpret_cast<int (*)(void *, unsigned int)>(
        m_dll.resolve("RegisterPostMessageHook"));
    m_unregisterHook = reinterpret_cast<int (*)(void *)>(
        m_dll.resolve("UnregisterPostMessageHook"));
    m_isWindowOnCurrentDesktop = reinterpret_cast<int (*)(void *)>(
        m_dll.resolve("IsWindowOnCurrentVirtualDesktop"));
    if (!m_getDesktopCount || !m_getCurrentDesktop || !m_goToDesktop
        || !m_createDesktop || !m_removeDesktop || !m_moveWindowToDesktop
        || !m_registerHook || !m_unregisterHook) {
        qWarning().noquote() << "Desktops:" << dllPath
                             << "is missing expected exports - desktop switching disabled";
        m_dll.unload();
        return;
    }
    m_available = true;
    if (!m_isWindowOnCurrentDesktop) {
        qWarning().noquote() << "Desktops:" << dllPath
                             << "has no IsWindowOnCurrentVirtualDesktop export -"
                             << "focus will stay on the desktop being left";
    }

    // Optional, and not folded into the mandatory-export check above: the
    // tiler degrades to one shared tree per monitor without it, rather than
    // losing desktop switching (or this whole class) altogether.
    m_getWindowDesktopIdRaw = reinterpret_cast<void *>(m_dll.resolve("GetWindowDesktopId"));
    if (!m_getWindowDesktopIdRaw) {
        qWarning().noquote() << "Desktops:" << dllPath
                             << "has no GetWindowDesktopId export -"
                             << "the tiler falls back to one shared tree per monitor";
    }

    // The DLL's listener posts switch notifications here. The window lives on
    // the main thread, so Qt's loop dispatches them and listenerProc runs
    // there too - no marshalling needed.
    WNDCLASSW wc = {};
    wc.lpfnWndProc = listenerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"qwinDesktopListener";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        m_msgWindow = hwnd;
        m_registerHook(hwnd, kDesktopChangedMsg);
    } else {
        qWarning().noquote() << "Desktops: listener window creation failed,"
                             << "falling back to polling only";
    }

    refresh();

    // The hook covers switches only; creation/removal is visible by poll.
    m_pollTimer.setInterval(1000);
    connect(&m_pollTimer, &QTimer::timeout, this, &VirtualDesktops::refresh);
    m_pollTimer.start();
}

VirtualDesktops::~VirtualDesktops()
{
    if (m_msgWindow) {
        // First, so the DLL's thread cannot post into a dying window.
        m_unregisterHook(m_msgWindow);
        DestroyWindow(static_cast<HWND>(m_msgWindow));
    }
    // Unloaded here rather than left to process exit: the DLL's own detach
    // cleanup deadlocks once ExitProcess has terminated the threads it
    // started, and the host then hangs half-exited - windows gone, exit code
    // set, unkillable, still holding the single-instance mutex, so the next
    // launch says "already running" until a reboot. Measured: every tray
    // quit hung without this call and none with it, hook fired or not.
    m_dll.unload();
}

void VirtualDesktops::refresh()
{
    if (!m_available)
        return;
    const int count = m_getDesktopCount();
    const int index = m_getCurrentDesktop();
    if (count < 1 || index < 0)
        return; // transient COM failure: keep last-good
    if (count != m_count || index != m_currentIndex) {
        m_count = count;
        m_currentIndex = index;
        qInfo().noquote() << QStringLiteral("Desktops: count=%1 current=%2")
                                 .arg(count).arg(index);
        emit changed();
    }
}

QUuid VirtualDesktops::windowDesktopId(void *hwnd) const
{
    if (!m_getWindowDesktopIdRaw)
        return QUuid();
    // GUID is 16 bytes, so the Microsoft x64 ABI returns it through a hidden
    // pointer the caller allocates; the DLL's extern "C" export follows the
    // same platform calling convention, so declaring the pointer with GUID
    // as an ordinary by-value return type is enough - the compiler emits the
    // hidden-pointer call on both sides without any manual marshalling here.
    using GetWindowDesktopIdFn = GUID(*)(HWND);
    const auto getWindowDesktopId = reinterpret_cast<GetWindowDesktopIdFn>(m_getWindowDesktopIdRaw);
    // A zeroed GUID comes back both for a window pinned to all desktops and
    // for a query the DLL could not satisfy (an elevated/protected window, a
    // COM hiccup) - the export gives no way to tell those apart, so this
    // stays QUuid() either way and callers decide what "unknown" means to
    // them (see windowDesktopId's declaration comment in the header).
    return QUuid(getWindowDesktopId(static_cast<HWND>(hwnd)));
}

// Unlike the shell's own Win+Ctrl+Arrow, the DLL's switch leaves the OLD
// desktop's window foreground, so typing keeps landing in a window that is
// no longer on screen (verified).
void VirtualDesktops::focusWindowOnCurrentDesktop()
{
    if (!m_isWindowOnCurrentDesktop)
        return;
    windowfocus::focusTopmostWindow([this](void *hwnd) {
        return m_isWindowOnCurrentDesktop(hwnd) == 1;
    });
}

void VirtualDesktops::switchTo(int index)
{
    if (!m_available) {
        qWarning().noquote() << "Desktops: switchTo() ignored, VirtualDesktopAccessor.dll not loaded";
        return;
    }
    index = qBound(0, index, m_count - 1);
    if (index == m_currentIndex)
        return;
    if (m_goToDesktop(index) == -1) {
        qWarning().noquote() << "Desktops: GoToDesktopNumber failed for" << index;
        return;
    }
    focusWindowOnCurrentDesktop();
    refresh(); // the hook confirms too; this keeps the UI snappy
}

void VirtualDesktops::createDesktop()
{
    if (!m_available) {
        qWarning().noquote() << "Desktops: createDesktop() ignored, VirtualDesktopAccessor.dll not loaded";
        return;
    }
    const int index = m_createDesktop();
    if (index < 0) {
        qWarning().noquote() << "Desktops: CreateDesktop failed";
        return;
    }
    m_goToDesktop(index); // follow it, like Win+Ctrl+D
    focusWindowOnCurrentDesktop();
    refresh();
}

void VirtualDesktops::moveForegroundWindowToNewDesktop()
{
    if (!m_available) {
        qWarning().noquote() << "Desktops: moveForegroundWindowToNewDesktop() ignored,"
                             << "VirtualDesktopAccessor.dll not loaded";
        return;
    }

    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        qWarning().noquote() << "Desktops: no foreground window to move";
        return;
    }

    // Real application windows only - never ours or the shell's.
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (pid == GetCurrentProcessId() || hwnd == GetShellWindow()
        || wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0
        || wcscmp(cls, L"Shell_TrayWnd") == 0) {
        qWarning().noquote() << "Desktops: foreground window is a plugin/shell window, not moving";
        return;
    }

    const int index = m_createDesktop();
    if (index < 0) {
        qWarning().noquote() << "Desktops: CreateDesktop failed";
        return;
    }
    if (m_moveWindowToDesktop(hwnd, index) == -1) {
        qWarning().noquote() << "Desktops: MoveWindowToDesktopNumber failed";
        m_removeDesktop(index, m_currentIndex); // no empty desktop left behind
        refresh();
        return;
    }
    m_goToDesktop(index);
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_MAXIMIZEBOX)
        ShowWindow(hwnd, SW_MAXIMIZE);
    // The triggering WM_HOTKEY grants foreground-activation rights, so this
    // succeeds despite the switch.
    SetForegroundWindow(hwnd);
    qInfo().noquote() << QStringLiteral("Desktops: moved foreground window to new desktop %1")
                             .arg(index);
    refresh();
}

void VirtualDesktops::closeCurrentDesktop()
{
    if (!m_available) {
        qWarning().noquote() << "Desktops: closeCurrentDesktop() ignored, VirtualDesktopAccessor.dll not loaded";
        return;
    }
    if (m_count <= 1) {
        qWarning().noquote() << "Desktops: not closing the last desktop";
        return;
    }
    const int cur = m_currentIndex;
    // Land on the left neighbour, like Win+Ctrl+F4.
    if (m_removeDesktop(cur, cur > 0 ? cur - 1 : 1) == -1) {
        qWarning().noquote() << "Desktops: RemoveDesktop failed for" << cur;
        return;
    }
    focusWindowOnCurrentDesktop();
    refresh();
}
