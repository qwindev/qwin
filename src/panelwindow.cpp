#include "panelwindow.h"

#include <QAbstractNativeEventFilter>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QHash>

#include <windows.h>
#include <shellapi.h>

namespace {

UINT appBarNotifyMessage()
{
    static const UINT msg = RegisterWindowMessageW(L"qwin_AppBarNotify");
    return msg;
}

QHash<HWND, PanelWindow *> &appBars()
{
    static QHash<HWND, PanelWindow *> bars;
    return bars;
}

// Forwards ABN_POSCHANGED (another appbar, or the resolution, moved) to the
// affected panel so it re-negotiates its slot.
class AppBarEventFilter : public QAbstractNativeEventFilter
{
public:
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override
    {
        MSG *msg = static_cast<MSG *>(message);
        if (msg->message == appBarNotifyMessage() && msg->wParam == ABN_POSCHANGED) {
            if (PanelWindow *panel = appBars().value(msg->hwnd))
                panel->updateAppBar();
        }
        return false;
    }
};

void ensureEventFilter()
{
    static bool installed = [] {
        QCoreApplication::instance()->installNativeEventFilter(new AppBarEventFilter);
        return true;
    }();
    Q_UNUSED(installed);
}

} // namespace

PanelWindow::PanelWindow(QWindow *parent)
    : QQuickWindow(parent)
{
    setFlags(Qt::FramelessWindowHint | Qt::Tool);

    connect(this, &QWindow::visibleChanged, this, [this](bool visible) {
        if (visible)
            updateAppBar();
        else
            removeAppBar();
    });
    connect(this, &QWindow::screenChanged, this, [this](QScreen *) { updateAppBar(); });
}

PanelWindow::~PanelWindow()
{
    removeAppBar();
}

// A panel is host chrome: Alt+F4 would leave the host running with nothing
// on screen but the tray icon. Teardown skips this path - the manager
// deleteLater()s, and ~PanelWindow() releases the AppBar slot.
void PanelWindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
}

void PanelWindow::setEdge(Qt::Edge edge)
{
    if (m_edge == edge)
        return;
    m_edge = edge;
    emit edgeChanged();
    updateAppBar();
}

void PanelWindow::setThickness(int thickness)
{
    if (m_thickness == thickness)
        return;
    m_thickness = thickness;
    emit thicknessChanged();
    updateAppBar();
}

void PanelWindow::updateAppBar()
{
    if (!isVisible())
        return;
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd)
        return;

    ensureEventFilter();

    // The panel must never take foreground. MA_NOACTIVATE alone stops the
    // panel activating but still lets Windows deactivate the window the user
    // was in (verified: foreground went to NULL, keystrokes nowhere to land).
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_NOACTIVATE))
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);

    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    abd.hWnd = hwnd;

    if (!m_registered) {
        abd.uCallbackMessage = appBarNotifyMessage();
        SHAppBarMessage(ABM_NEW, &abd);
        appBars().insert(hwnd, this);
        m_hwnd = hwnd;
        m_registered = true;
    }

    // The AppBar API negotiates in physical pixels; Qt geometry is logical.
    // Everything below stays physical.
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT mon = mi.rcMonitor;
    const int px = qRound(m_thickness * devicePixelRatio());

    UINT nativeEdge = ABE_TOP;
    RECT rc = mon;
    switch (m_edge) {
    case Qt::LeftEdge:   nativeEdge = ABE_LEFT;   rc.right = mon.left + px;  break;
    case Qt::RightEdge:  nativeEdge = ABE_RIGHT;  rc.left = mon.right - px;  break;
    case Qt::BottomEdge: nativeEdge = ABE_BOTTOM; rc.top = mon.bottom - px;  break;
    case Qt::TopEdge:    nativeEdge = ABE_TOP;    rc.bottom = mon.top + px;  break;
    }

    abd.uEdge = nativeEdge;
    abd.rc = rc;

    // QUERYPOS shifts the strip past appbars already docked on this edge -
    // what makes panels stack - then it is re-cut to our thickness.
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    switch (nativeEdge) {
    case ABE_LEFT:   abd.rc.right = abd.rc.left + px;   break;
    case ABE_RIGHT:  abd.rc.left = abd.rc.right - px;   break;
    case ABE_BOTTOM: abd.rc.top = abd.rc.bottom - px;   break;
    case ABE_TOP:    abd.rc.bottom = abd.rc.top + px;   break;
    }
    SHAppBarMessage(ABM_SETPOS, &abd);

    // MoveWindow, not a Qt setter: Qt translates the resulting WM messages
    // back to logical geometry itself.
    MoveWindow(hwnd, abd.rc.left, abd.rc.top,
               abd.rc.right - abd.rc.left, abd.rc.bottom - abd.rc.top, TRUE);
}

void PanelWindow::removeAppBar()
{
    if (!m_registered)
        return;
    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    abd.hWnd = static_cast<HWND>(m_hwnd);
    SHAppBarMessage(ABM_REMOVE, &abd);
    appBars().remove(abd.hWnd);
    m_hwnd = nullptr;
    m_registered = false;
}
