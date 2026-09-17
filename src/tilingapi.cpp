#include "tilingapi.h"

#include "windowfocus.h"

#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>

#include <windows.h>
#include <dwmapi.h>

#include <string>

#include "screendevice.h"
#include "tilingapi_p.h"

using namespace tiling;

namespace {

constexpr int kScanDebounceMs = 150;
constexpr int kSweepMs = 2000;
// Coalesces a burst of QScreen signals from one monitor reconfiguration into
// a single rescan+retile.
constexpr int kDisplayDebounceMs = 300;
// A window that showed but is not tileable yet gets re-checked this often,
// this many times, before it is left to the sweep. Tuned to be shorter than
// a frame or two of the window sitting in the wrong place.
constexpr int kSettleMs = 40;
constexpr int kSettleTries = 12;
// Rounding, shadow trim and apps that nudge themselves by a pixel all live
// under this; anything larger is a real move.
constexpr int kDriftSlack = 4;
constexpr int kMaxRejections = 3;
constexpr int kProcessCacheCap = 256;
constexpr int kSaveDebounceMs = 500;

// SWP_NOSENDCHANGING is what lets a tile be narrower than the application's
// own minimum width. Per the WM_WINDOWPOSCHANGING docs, DefWindowProc answers
// that message by sending WM_GETMINMAXINFO and clamping the request to
// ptMinTrackSize - Chrome's is around 500 logical px, so without this a
// narrow column leaves Chrome overhanging its neighbour. Suppressing the
// message removes the clamp; the window still gets WM_WINDOWPOSCHANGED and
// WM_SIZE afterwards, so it lays itself out correctly at the size it was
// given, it just does not get to veto it. GlazeWM does exactly the same.
// SWP_NOCOPYBITS discards the stale client bits that otherwise smear across
// a window as it shrinks.
constexpr UINT kPlaceFlags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER
                           | SWP_NOSENDCHANGING | SWP_NOCOPYBITS;

// Win 8.1+, but not in every SDK's winuser.h.
#ifndef EVENT_OBJECT_CLOAKED
constexpr DWORD EVENT_OBJECT_CLOAKED = 0x8017;
constexpr DWORD EVENT_OBJECT_UNCLOAKED = 0x8018;
#endif

TilingApi *g_instance = nullptr; // exactly one ever exists; see main.cpp

// Also in tilingstate.cpp, for identityMatches(): ten lines is cheaper than a
// shared unit.
qint64 processCreationTime(DWORD pid)
{
    qint64 created = 0;
    if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        FILETIME creationTime, exitTime, kernelTime, userTime;
        if (GetProcessTimes(proc, &creationTime, &exitTime, &kernelTime, &userTime)) {
            ULARGE_INTEGER v;
            v.LowPart = creationTime.dwLowDateTime;
            v.HighPart = creationTime.dwHighDateTime;
            created = qint64(v.QuadPart);
        }
        CloseHandle(proc);
    }
    return created;
}

// WINEVENT_OUTOFCONTEXT hooks arrive as ordinary messages pumped by the
// registering thread's own loop, unlike a COM/WinRT callback. The hooks are
// installed from the constructor, on Qt's main thread, so this runs there
// too and needs no marshalling before touching state.
void CALLBACK winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                            LONG idChild, DWORD, DWORD)
{
    if (!g_instance)
        return;
    // Show/hide fire for every menu, tooltip and control on the system. The
    // object filter is what keeps this off the hot path.
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
        return;

    switch (event) {
    case EVENT_SYSTEM_FOREGROUND:
        g_instance->onForegroundChanged(hwnd);
        break;
    case EVENT_SYSTEM_MOVESIZESTART:
        g_instance->onMoveSizeStart(hwnd);
        break;
    case EVENT_SYSTEM_MOVESIZEEND:
        g_instance->onMoveSizeEnd(hwnd);
        break;
    case EVENT_OBJECT_SHOW:
        // The one event worth reacting to without waiting for the debounce:
        // until it is handled, the new window sits wherever its application
        // put it, in full view.
        g_instance->onWindowShown(hwnd);
        break;
    default:
        // DESTROY/HIDE, MINIMIZESTART/END, CLOAKED/UNCLOAKED. Our own hides
        // fire these too, but m_applying brackets them; what is left is the
        // shell, an explorer restart or the user. All just mean "look again",
        // and none of it is urgent.
        g_instance->onWindowSetChanged();
        break;
    }
}

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM param)
{
    reinterpret_cast<QVector<HWND> *>(param)->append(hwnd);
    return TRUE;
}

// QScreen::name() is a friendly name, not the GDI device workAreas() keys by
// (see screendevice.h) - screendevice::find() is what maps device -> QScreen.
// Going through Qt for the DPR keeps this unit off shcore/GetDpiForMonitor.
qreal scaleForDevice(const QString &device)
{
    if (QScreen *screen = screendevice::find(device))
        return screen->devicePixelRatio();
    QScreen *primary = QGuiApplication::primaryScreen();
    return primary ? primary->devicePixelRatio() : 1.0;
}

// isFocusableAppWindow minus its title requirement. Most applications show
// their window a frame or two before setting a title, so at SHOW time the
// shared predicate rejects a window that is about to qualify. This answers
// the weaker question "worth watching for a moment", which is what lets the
// settle poll catch it in tens of milliseconds instead of leaving it to the
// two-second sweep - the difference between a flicker and a visible jump.
bool looksLikeCandidate(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        return false;
    if (GetWindow(hwnd, GW_OWNER))
        return false;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid != GetCurrentProcessId() && hwnd != GetShellWindow();
}

// Top-to-bottom, then left-to-right, with "same row" judged loosely: two
// windows whose tops differ by less than half the shorter one's height are
// side by side, and the left one goes first. A strict compare on the top
// edge would put a window a few pixels higher ahead of one well to its
// left, which is not how anyone reads a screen. The tolerance makes this no
// strict weak ordering, so it is an insertion sort by hand rather than
// std::sort, which requires one. A handful of windows per monitor, so cost
// is not a concern. Z-order (the input order) breaks the remaining ties.
bool readsBefore(HWND a, HWND b)
{
    RECT ra, rb;
    if (!GetWindowRect(a, &ra) || !GetWindowRect(b, &rb))
        return false;
    const LONG tolerance = qMin(ra.bottom - ra.top, rb.bottom - rb.top) / 2;
    if (qAbs(ra.top - rb.top) > tolerance)
        return ra.top < rb.top;
    return ra.left < rb.left;
}

void sortIntoReadingOrder(QVector<quintptr> *ids)
{
    for (int i = 1; i < ids->size(); ++i) {
        const quintptr id = ids->at(i);
        int j = i;
        while (j > 0 && readsBefore(toHwnd(id), toHwnd(ids->at(j - 1)))) {
            (*ids)[j] = ids->at(j - 1);
            --j;
        }
        (*ids)[j] = id;
    }
}

bool parseDirection(const QString &text, layout::Direction *out)
{
    const QString d = text.trimmed().toLower();
    if (d == QLatin1String("left"))  { *out = layout::Direction::Left;  return true; }
    if (d == QLatin1String("right")) { *out = layout::Direction::Right; return true; }
    if (d == QLatin1String("up"))    { *out = layout::Direction::Up;    return true; }
    if (d == QLatin1String("down"))  { *out = layout::Direction::Down;  return true; }
    qWarning() << "Tiler: unknown direction" << text << "- use left/right/up/down";
    return false;
}

// "wider" / "narrower" / "taller" / "shorter" rather than a direction: the
// divider that moves is whichever one is nearest, so which way the window
// grows is not the caller's to say - only how much of which dimension.
bool parseResize(const QString &text, layout::SplitKind *axis, int *sign)
{
    const QString r = text.trimmed().toLower();
    if (r == QLatin1String("wider"))    { *axis = layout::SplitKind::Columns; *sign =  1; return true; }
    if (r == QLatin1String("narrower")) { *axis = layout::SplitKind::Columns; *sign = -1; return true; }
    if (r == QLatin1String("taller"))   { *axis = layout::SplitKind::Rows;    *sign =  1; return true; }
    if (r == QLatin1String("shorter"))  { *axis = layout::SplitKind::Rows;    *sign = -1; return true; }
    qWarning() << "Tiler: unknown resize" << text
               << "- use wider/narrower/taller/shorter";
    return false;
}

// True iff `hwnd` occupies its whole monitor on its own initiative - a
// browser or video player going HTML5/F11 fullscreen strips
// WS_CAPTION/WS_THICKFRAME from the SAME hwnd and SetWindowPos-es it to the
// monitor's full rect. IsZoomed is excluded up front: an ordinary maximize
// is still treated as drift and restored, as today. Compared against
// rcMonitor, not rcWork - a borderless window maximized to the work area
// (no bar, auto-hidden taskbar) covers only the work area and is not
// fullscreen. Containment on raw RECT edges, not QRect::right()/bottom()
// (off by one): no slack, since browsers hit the monitor rect exactly, and
// a rect larger than the monitor also counts.
bool coversMonitor(HWND hwnd, QRect *monitor = nullptr)
{
    if (IsZoomed(hwnd))
        return false;
    RECT wr;
    if (!GetWindowRect(hwnd, &wr))
        return false;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    if (monitor)
        *monitor = toRect(mi.rcMonitor);
    const RECT &rm = mi.rcMonitor;
    return wr.left <= rm.left && wr.top <= rm.top && wr.right >= rm.right && wr.bottom >= rm.bottom;
}

} // namespace

TilingApi::TilingApi(QObject *parent)
    : QObject(parent)
{
    g_instance = this;

    m_scanTimer.setSingleShot(true);
    m_scanTimer.setInterval(kScanDebounceMs);
    connect(&m_scanTimer, &QTimer::timeout, this, &TilingApi::rescan);

    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(kSettleMs);
    connect(&m_settleTimer, &QTimer::timeout, this, [this] {
        const int before = m_windows.size();
        rescan();
        // Stops the moment the window lands, so the common case costs one or
        // two ticks rather than the whole budget.
        if (m_windows.size() == before && ++m_settleTries < kSettleTries)
            m_settleTimer.start();
    });

    m_sweepTimer.setInterval(kSweepMs);
    connect(&m_sweepTimer, &QTimer::timeout, this, &TilingApi::sweep);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDebounceMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &TilingApi::saveStateNow);

    m_pendingTimer.setSingleShot(true);
    m_pendingTimer.setInterval(kPendingTimeoutMs);
    connect(&m_pendingTimer, &QTimer::timeout, this, &TilingApi::abandonPending);

    m_displayTimer.setSingleShot(true);
    m_displayTimer.setInterval(kDisplayDebounceMs);
    connect(&m_displayTimer, &QTimer::timeout, this, [this] {
        rescan();  // a monitor may have gone, or a window may now be on a
                   // different one - both change tree membership
        retile();  // unconditional: a resolution/DPI/work-area change alone
                   // leaves membership untouched, but rescan() only retiles
                   // when membership or the visible count changed
    });

    // The 2 s sweep's drift-fixer re-applies `assigned` rects computed from
    // the last arrange() - after a resolution, DPI or work-area change those
    // rects describe the OLD area, so the sweep actively pushes windows back
    // onto stale geometry until some unrelated window event forces a retile.
    // Watching QScreen directly is what recomputes metrics promptly instead.
    for (QScreen *screen : QGuiApplication::screens())
        watchScreen(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
        watchScreen(screen);
        scheduleDisplayRecheck();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *) {
        scheduleDisplayRecheck();
    });

    // Adjacent event ids, so one hook covers each pair or run.
    m_hookObject = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE,
                                    nullptr, winEventProc, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    m_hookCloak = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED,
                                   nullptr, winEventProc, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    m_hookMinimize = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
                                      nullptr, winEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    m_hookMoveSize = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND,
                                      nullptr, winEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    m_hookForeground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                        nullptr, winEventProc, 0, 0,
                                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!m_hookObject || !m_hookMoveSize) {
        qWarning() << "Tiler: SetWinEventHook failed - tiling disabled";
        m_hookObject = nullptr; // setEnabled() refuses without discovery
    }

    // Last: it hides and shows windows, which needs the hooks above. Runs
    // whether or not the config ends up enabling the tiler at all.
    recoverState();
}

TilingApi::~TilingApi()
{
    for (void *hook : { m_hookObject, m_hookCloak, m_hookMinimize,
                        m_hookMoveSize, m_hookForeground }) {
        if (hook)
            UnhookWinEvent(static_cast<HWINEVENTHOOK>(hook));
    }

    // Nothing of ours to show or save - and if an earlier failed teardown
    // left the state file naming windows still hidden, a clean exit must not
    // touch it; recoverState() picks it up on the next launch.
    if (!m_windows.isEmpty()) {
        // Records the show before doing it: the file must never claim a
        // window is hidden when it is already back on screen.
        saveStateNow();

        m_applying = true;
        QVector<quintptr> unshown;
        for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
            if (!showWindow(it.key()))
                unshown.append(it.key());
            if (it->pinned && !it->wasTopmost) {
                SetWindowPos(toHwnd(it.key()), HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        m_applying = false;

        // Only a clean tray quit reaches here; a forced kill is what
        // recoverState() is for. retainUnshown() deletes the state file when
        // everything came back, else keeps just what did not for the next
        // launch to retry.
        retainUnshown(unshown);
    }

    // Geometry is deliberately left as it is: a mass re-shuffle as the host
    // exits is more startling than a tidy desktop is useful.
    qDeleteAll(m_trees);
    g_instance = nullptr;
}

void TilingApi::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    if (enabled && !m_hookObject) {
        qWarning() << "Tiler: cannot enable, the window hooks are not installed";
        return;
    }

    m_enabled = enabled;
    qInfo() << "Tiler: enabled ->" << enabled;
    if (m_enabled) {
        const bool claimedRecovered = !m_pendingRecovered.isEmpty();
        if (claimedRecovered) {
            // The recovered data is already correct; claiming only cancels
            // the safety net that would otherwise undo it.
            m_pendingTimer.stop();
            qInfo() << "Tiler: claiming" << m_pendingRecovered.size()
                    << "pending window(s)";
            for (quintptr id : std::as_const(m_pendingRecovered)) {
                const auto it = m_windows.constFind(id);
                if (it != m_windows.constEnd() && it->floating && !it->pinned)
                    m_stickyFloat.insert(id); // survive its first minimize too
            }
            m_pendingRecovered.clear();
        }
        m_sweepTimer.start();
        rescan();
        // Recovered members are already in m_windows, so rescan()'s discovery
        // pass adopts nothing new and `changed` stays false - retile() would
        // never run, and their `assigned` rect (invalid, never having been
        // placed this session) makes the sweep's drift-fixer skip them too.
        // Without this they would sit wherever they were before the crash
        // until something unrelated happens to trigger a retile.
        if (claimedRecovered)
            retile();
    } else {
        m_sweepTimer.stop();
        m_scanTimer.stop();
        m_settleTimer.stop();
        m_displayTimer.stop();

        saveStateNow(); // see the destructor for why this goes first

        m_applying = true;
        QVector<quintptr> unshown;
        for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
            if (!showWindow(it.key()))
                unshown.append(it.key());
            if (it->pinned && !it->wasTopmost) {
                SetWindowPos(toHwnd(it.key()), HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        m_applying = false;

        const QList<quintptr> ids = m_windows.keys();
        for (quintptr id : ids) {
            if (!unshown.contains(id))
                releaseWindow(id, true);
        }
        retainUnshown(unshown);
        // Whatever would not show stays pending, as a recovered window does:
        // the next enable claims it, m_pendingTimer retries it meanwhile.
        m_pendingRecovered = unshown;
        if (!unshown.isEmpty())
            m_pendingTimer.start();

        // Its dead-handle prune only runs in rescan(), so while disabled a
        // recycled HWND could inherit a stale float; forgotten like the rest
        // of what disabling forgets.
        m_stickyFloat.clear();
        m_lastVisibleCount = 0; // nothing tiled; keep the diff below honest
    }
    emit enabledChanged();
    emit layoutChanged();
}

void TilingApi::setGap(int gap)
{
    gap = qMax(0, gap);
    if (m_gap == gap)
        return;
    m_gap = gap;
    emit gapsChanged();
    retile();
}

void TilingApi::setOuterGap(int outerGap)
{
    outerGap = qMax(0, outerGap);
    if (m_outerGap == outerGap)
        return;
    m_outerGap = outerGap;
    emit gapsChanged();
    retile();
}

void TilingApi::setDebug(bool debug)
{
    if (m_debug == debug)
        return;
    m_debug = debug;
    emit debugChanged();
    if (m_debug)
        retile(); // so the log starts with the layout as it stands
}

QString TilingApi::titleFor(void *hwnd)
{
    const int length = GetWindowTextLengthW(static_cast<HWND>(hwnd));
    if (length <= 0)
        return QString();
    std::wstring buffer(length + 1, L'\0');
    const int copied = GetWindowTextW(static_cast<HWND>(hwnd), buffer.data(), length + 1);
    return QString::fromWCharArray(buffer.data(), copied);
}

QString TilingApi::classNameFor(void *hwnd)
{
    wchar_t buf[256] = {};
    const int n = GetClassNameW(static_cast<HWND>(hwnd), buf, 256);
    return QString::fromWCharArray(buf, n);
}

QString TilingApi::describe(quintptr id)
{
    if (!id)
        return QStringLiteral("(none)");
    return QStringLiteral("%1 (%2)").arg(processNameFor(toHwnd(id)), titleFor(toHwnd(id)).left(40));
}

void TilingApi::setMinWidth(int minWidth)
{
    minWidth = qMax(0, minWidth);
    if (m_minWidth == minWidth)
        return;
    m_minWidth = minWidth;
    emit minSizeChanged();
    rescan(); // a smaller minimum may let overflow windows back into the tree
}

void TilingApi::setMinHeight(int minHeight)
{
    minHeight = qMax(0, minHeight);
    if (m_minHeight == minHeight)
        return;
    m_minHeight = minHeight;
    emit minSizeChanged();
    rescan();
}

void TilingApi::setResizeStep(int step)
{
    // One pixel is a legal, useless step; zero would make the command a
    // silent no-op, which is worth not shipping.
    step = qMax(1, step);
    if (m_resizeStep == step)
        return;
    m_resizeStep = step;
    emit resizeStepChanged();
}

bool TilingApi::floatProcessMatches(const QString &process) const
{
    if (process.isEmpty())
        return false;
    for (const QString &name : m_floatProcesses) {
        if (name.compare(process, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

void TilingApi::setFloatProcesses(const QStringList &names)
{
    if (m_floatProcesses == names)
        return;
    m_floatProcesses = names;
    emit floatProcessesChanged();
    if (!m_enabled)
        return;

    // Pulled out immediately; nothing ever un-floats by itself - floating is
    // sticky.
    bool changed = false;
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (!isTiledEntry(it.value()))
            continue;
        if (!floatProcessMatches(processNameFor(toHwnd(it.key()))))
            continue;
        if (layout::Tree *tree = m_trees.value(keyOf(it.value())))
            tree->remove(it.key());
        it->floating = true;
        changed = true;
    }
    if (changed)
        pruneEmptyTrees();
    retile();
}

int TilingApi::managedCount() const
{
    int count = 0;
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        if (isTiledEntry(it.value()) && it->hidden == hider::Method::None)
            ++count;
    }
    return count;
}

QString TilingApi::hideMethod() const
{
    return m_hideMethod == hider::Method::Minimize ? QStringLiteral("minimize")
                                                     : QStringLiteral("cloak");
}

void TilingApi::setHideMethod(const QString &method)
{
    const QString m = method.trimmed().toLower();
    hider::Method parsed;
    if (m == QLatin1String("cloak")) {
        parsed = hider::Method::Cloak;
    } else if (m == QLatin1String("minimize")) {
        parsed = hider::Method::Minimize;
    } else {
        qWarning() << "Tiler: unknown hideMethod" << method << "- use cloak/minimize";
        return;
    }
    if (m_hideMethod == parsed)
        return;
    m_hideMethod = parsed;
    emit hideMethodChanged();
    // Already-hidden windows keep their recorded method (Managed::hidden).
}

void TilingApi::setPinnedTopmost(bool pinned)
{
    if (m_pinnedTopmost == pinned)
        return;
    m_pinnedTopmost = pinned;
    emit pinnedTopmostChanged();
    // Not retroactive: only pin actions from here on touch HWND_TOPMOST.
}

// ---------------------------------------------------------------- discovery

void TilingApi::scheduleScan()
{
    // Started only when idle, never restarted: a busy desktop can emit
    // show/hide faster than the debounce, and a restarting timer would
    // never fire.
    if (m_enabled && !m_scanTimer.isActive())
        m_scanTimer.start();
}

void TilingApi::watchScreen(QScreen *screen)
{
    if (!screen)
        return;
    connect(screen, &QScreen::geometryChanged, this, [this] { scheduleDisplayRecheck(); });
    connect(screen, &QScreen::availableGeometryChanged, this, [this] { scheduleDisplayRecheck(); });
    // A scale change alone: the physical work area is the same, but every
    // gap and minimum is scaled from logical px, so the metrics still moved.
    connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this](qreal) { scheduleDisplayRecheck(); });
}

void TilingApi::scheduleDisplayRecheck()
{
    // Restarted rather than started-if-idle: a monitor reconfiguration fires
    // geometryChanged/availableGeometryChanged in bursts, and this is what
    // coalesces a burst into one rescan+retile instead of several.
    m_displayTimer.start();
}

void TilingApi::onWindowSetChanged()
{
    if (m_applying)
        return;
    scheduleScan();
}

void TilingApi::onWindowShown(void *hwnd)
{
    if (!m_enabled || m_applying)
        return;
    const quintptr id = toId(hwnd);
    if (m_windows.contains(id))
        return;

    // One cheap test on this window before committing to a full rescan: SHOW
    // fires for every menu, tooltip and dialog on the system, and all of
    // those fail here for the price of a couple of style/COM reads.
    if (windowfocus::isFocusableAppWindow(hwnd) && hider::onCurrentNativeDesktop(hwnd)) {
        rescan(); // straight through - the debounce is the flicker
        return;
    }

    // Not tileable yet, but on its way there. Poll briefly instead of leaving
    // it to the sweep two seconds out.
    if (looksLikeCandidate(static_cast<HWND>(hwnd))) {
        m_settleTries = 0;
        m_settleTimer.start();
    }
}

void TilingApi::onForegroundChanged(void *hwnd)
{
    // Which monitor gets the next chord follows the foreground window, in
    // three cases - see m_shellFocusDevice for why the desktop-like one
    // exists at all.
    QString device;
    if (isDesktopWindow(static_cast<HWND>(hwnd))) {
        // The hint focusWorkspaceMember() left, if this is the event it was
        // left for; otherwise a click on the empty desktop, and the cursor
        // names the monitor that was clicked. One-shot either way.
        if (!m_shellFocusDevice.isEmpty() && workAreas().contains(m_shellFocusDevice))
            device = m_shellFocusDevice;
        else
            device = deviceUnderCursor();
        m_shellFocusDevice.clear();
    } else if (m_windows.contains(toId(hwnd)) || windowfocus::isFocusableAppWindow(hwnd)) {
        // A real application window taking focus is unambiguous: trust it,
        // and forget whatever workspace switch last set m_shellFocusDevice.
        device = deviceForWindow(hwnd);
        m_shellFocusDevice.clear();
    }
    // Anything else - taskbar, flyouts, tool windows - carries no useful
    // "which monitor" signal of its own, so m_focusedDevice is left as it
    // was rather than being pulled onto whatever monitor that popup opened
    // on.

    if (!device.isEmpty() && device != m_focusedDevice) {
        m_focusedDevice = device;
        notifyWorkspacesIfChanged();
    }

    const auto it = m_windows.find(toId(hwnd));
    if (it == m_windows.end())
        return;

    m_lastFocusedIn[keyOf(it.value())] = toId(hwnd);

    if (it->hidden == hider::Method::None)
        return;

    // Something handed this window the keyboard while our records still say
    // it is hidden - the taskbar, Alt+Tab, a jump list. Take the user to it
    // rather than fight it back down; rescan()'s membership pass is the
    // catch-up for what never reaches the foreground event at all.
    if (it->workspace == m_active.value(it->device, 0)) {
        // Already on its monitor's active workspace: switchWorkspace() would
        // no-op and leave the flag set, and a window stuck at hidden != None
        // is skipped by applyPlacements() and the sweep alike - on screen,
        // never placed again. Do what the switch would have done for it.
        m_applying = true;
        showWindow(toId(hwnd));
        m_applying = false;
        scheduleSaveState();
        emit layoutChanged();
        return;
    }
    switchWorkspace(it->device, it->workspace);
}

void TilingApi::onMoveSizeStart(void *hwnd)
{
    m_dragging = toId(hwnd);
}

QString TilingApi::fullExePathFor(void *hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(static_cast<HWND>(hwnd), &pid);
    QString path;
    // Denied for elevated/protected processes even with the limited right,
    // so an empty path is expected rather than a failure.
    if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buffer[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, buffer, &size))
            path = QString::fromWCharArray(buffer, int(size));
        CloseHandle(proc);
    }
    return path;
}

QString TilingApi::processNameFor(void *hwnd)
{
    const quintptr id = toId(hwnd);
    if (const auto it = m_processCache.constFind(id); it != m_processCache.constEnd())
        return it.value();

    const QString path = fullExePathFor(hwnd);
    const QString name = path.isEmpty() ? QString() : QFileInfo(path).fileName();

    if (m_processCache.size() >= kProcessCacheCap)
        m_processCache.clear(); // simplest bound, as in foregroundwindow.cpp
    m_processCache.insert(id, name);
    return name;
}

QString TilingApi::keyOf(const Managed &m)
{
    return m.device + QLatin1Char('|') + QString::number(m.workspace);
}

QString TilingApi::deviceForWindow(void *hwnd) const
{
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(hwnd), MONITOR_DEFAULTTONEAREST), &mi))
        return QString();
    return QString::fromWCharArray(mi.szDevice);
}

layout::Tree *TilingApi::treeFor(const QString &key)
{
    layout::Tree *&tree = m_trees[key];
    if (!tree)
        tree = new layout::Tree;
    return tree;
}

void TilingApi::pruneEmptyTrees()
{
    for (auto it = m_trees.begin(); it != m_trees.end();) {
        if (it.value()->isEmpty()) {
            delete it.value();
            it = m_trees.erase(it);
        } else {
            ++it;
        }
    }
}

bool TilingApi::metricsForKey(const QString &key, layout::Metrics *out) const
{
    const QString device = key.section(QLatin1Char('|'), 0, 0);
    const QHash<QString, QRect> areas = workAreas();
    const auto it = areas.constFind(device);
    if (it == areas.constEnd())
        return false; // monitor unplugged since the tree was made

    // Everything the QML side sets is logical; the tree works in the same
    // physical pixels SetWindowPos does, so scale on the way through.
    const qreal scale = scaleForDevice(device);
    out->area = it.value();
    out->gap = qRound(m_gap * scale);
    out->outerGap = qRound(m_outerGap * scale);
    out->minWidth = qRound(m_minWidth * scale);
    out->minHeight = qRound(m_minHeight * scale);
    return true;
}

void TilingApi::migrateWindow(quintptr id, const QString &newDevice, int newWorkspace)
{
    const auto it = m_windows.find(id);
    if (it == m_windows.end())
        return;
    const QString oldKey = keyOf(it.value());
    const bool tiled = isTiledEntry(it.value());
    const bool wasFocused = it->device != newDevice && toHwnd(id) == GetForegroundWindow();
    if (tiled) {
        if (layout::Tree *tree = m_trees.value(oldKey))
            tree->remove(id);
    }
    it->device = newDevice;
    it->workspace = newWorkspace;
    if (wasFocused) {
        // Moving the foreground window to another monitor (a drag,
        // Win+Shift+Arrow) fires no FOREGROUND event.
        m_focusedDevice = newDevice;
    }
    if (tiled) {
        const QString newKey = keyOf(it.value());
        layout::Tree *tree = treeFor(newKey);
        layout::Metrics metrics;
        if (metricsForKey(newDevice, &metrics)) {
            const quintptr nearId = tree->contains(m_lastFocusedIn.value(newKey))
                                 ? m_lastFocusedIn.value(newKey) : 0;
            if (tree->insert(id, nearId, metrics) == layout::Insert::TooSmall)
                it->overflow = true;
        } else {
            it->overflow = true; // no metrics this pass; retried like any overflow entry
        }
    }
}

void TilingApi::rescan()
{
    if (!m_enabled)
        return;

    // HWNDs are recycled: without this, a fresh window that happens to reuse
    // a stale handle would be adopted floating for no reason of its own.
    for (auto it = m_stickyFloat.begin(); it != m_stickyFloat.end(); ) {
        if (!IsWindow(toHwnd(*it)))
            it = m_stickyFloat.erase(it);
        else
            ++it;
    }

    // Acquisition is lazy and can drop when explorer.exe restarts; this is
    // the only place that runs often enough to notice. The non-acquiring
    // accessor deliberately: acquiring here would mean a CoCreateInstance,
    // and a warning where it keeps failing, every tick.
    if (const bool cloak = hider::cloakAcquired(); cloak != m_cloakAvailable) {
        m_cloakAvailable = cloak;
        emit cloakAvailableChanged();
    }

    QVector<HWND> windows;
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&windows));

    bool changed = false;

    // ---- 1. discover and adopt windows not seen before ----
    struct NewInfo { QString device; int workspace; bool floating; };
    QHash<HWND, NewInfo> newInfo;
    QHash<QString, QVector<HWND>> tiledGroups; // tree key -> hwnds, sorted below
    QVector<HWND> floatingNew;

    for (HWND hwnd : windows) {
        const quintptr id = toId(hwnd);
        if (m_windows.contains(id))
            continue;
        if (!windowfocus::isFocusableAppWindow(hwnd))
            continue;
        if (!hider::onCurrentNativeDesktop(hwnd))
            continue; // parked on another native desktop - not ours to manage

        const QString device = deviceForWindow(hwnd);
        if (device.isEmpty())
            continue; // monitor vanished mid-scan; retried next pass

        // Onto whatever workspace its monitor is showing right now. Fixed-size
        // windows (no sizing border: dialogs, installers, splashes) float
        // rather than tile, as do the configured floatProcesses and anything
        // in m_stickyFloat.
        const int workspace = m_active.value(device, 0);
        const bool floating = !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME)
                            || floatProcessMatches(processNameFor(hwnd))
                            || m_stickyFloat.contains(id);

        newInfo.insert(hwnd, { device, workspace, floating });
        if (floating)
            floatingNew.append(hwnd);
        else
            tiledGroups[device + QLatin1Char('|') + QString::number(workspace)].append(hwnd);
    }

    // Reading order, not the Z-order EnumWindows gives: Z-order is focus
    // history, and screen position is what a dwindle has to reproduce itself
    // from when several windows land in one pass.
    for (auto it = tiledGroups.begin(); it != tiledGroups.end(); ++it) {
        QVector<quintptr> ids;
        ids.reserve(it.value().size());
        for (HWND hwnd : it.value())
            ids.append(toId(hwnd));
        sortIntoReadingOrder(&ids);
        it.value().clear();
        for (quintptr id : ids)
            it.value().append(toHwnd(id));
    }

    auto adoptOne = [&](HWND hwnd, bool floating) {
        const quintptr id = toId(hwnd);
        const NewInfo &info = newInfo.value(hwnd);

        Managed entry;
        entry.device = info.device;
        entry.workspace = info.workspace;
        entry.floating = floating;

        RECT r;
        if (GetWindowRect(hwnd, &r))
            entry.original = toRect(r); // geometry at adoption, restored on release

        // For the state file; every restore path but recovery uses
        // `original`. rcNormalPosition is in workspace coordinates (screen
        // minus the work area's inset), converted here while that inset is
        // current: at recovery our own AppBar may not be registered yet.
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(hwnd, &wp)) {
            QRect normal = toRect(wp.rcNormalPosition);
            MONITORINFO mi = {};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONEAREST), &mi))
                normal.translate(mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
            entry.placementNormal = normal;
        }

        // Also for the state file, also read once: deriving these in
        // saveStateNow() put two OpenProcess calls per window on the
        // workspace-switch path.
        DWORD adoptPid = 0;
        GetWindowThreadProcessId(hwnd, &adoptPid);
        entry.pid = quint32(adoptPid);
        entry.created = processCreationTime(adoptPid);
        entry.windowClass = classNameFor(hwnd);

        if (!floating) {
            const QString key = info.device + QLatin1Char('|') + QString::number(info.workspace);
            layout::Metrics metrics;
            if (metricsForKey(info.device, &metrics)) {
                layout::Tree *tree = treeFor(key);
                const quintptr nearId = tree->contains(m_lastFocusedIn.value(key))
                                     ? m_lastFocusedIn.value(key) : 0;
                if (tree->insert(id, nearId, metrics) == layout::Insert::TooSmall) {
                    entry.overflow = true;
                    qInfo() << "Tiler: leaving" << processNameFor(hwnd)
                            << "floating - no room for another tile";
                }
            } else {
                entry.overflow = true;
            }
        }

        if (m_debug)
            qInfo().noquote() << QStringLiteral("Tiler: adopt %1").arg(describe(id));

        m_windows.insert(id, entry);
        changed = true;
    };

    for (HWND hwnd : floatingNew)
        adoptOne(hwnd, true);
    for (auto it = tiledGroups.constBegin(); it != tiledGroups.constEnd(); ++it) {
        for (HWND hwnd : it.value())
            adoptOne(hwnd, false);
    }

    // ---- 2. membership pass over what we already track ----
    QVector<quintptr> toRelease;
    // Recorded hidden but actually visible: go to it if it has the focus,
    // put it back down if it does not.
    QVector<quintptr> toReveal;
    QVector<quintptr> toRehide;
    // Recorded hidden but should be showing: a show() that failed (explorer
    // mid-restart) or a migrate below onto the survivor's active workspace.
    QVector<quintptr> toShow;
    // Which monitors exist, rather than where a window sits: a minimized one
    // is at an off-screen iconic rect, so MonitorFromWindow would name the
    // nearest survivor and migrate it for no reason.
    const QHash<QString, QRect> liveAreas = workAreas();

    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        const quintptr id = it.key();
        HWND hwnd = toHwnd(id);
        if (!IsWindow(hwnd)) {
            toRelease.append(id);
            continue;
        }

        if (it->hidden == hider::Method::None) {
            if (IsIconic(hwnd)) {
                toRelease.append(id); // user minimized it - re-adopted fresh on restore
                continue;
            }
            if (hider::isCloaked(hwnd)) {
                toRelease.append(id); // the OS took it: another desktop, dormant UWP host
                continue;
            }
            const QString device = deviceForWindow(hwnd);
            if (device.isEmpty()) {
                toRelease.append(id); // monitor gone; a later rescan re-homes it
                continue;
            }
            if (device != it->device) {
                migrateWindow(id, device, m_active.value(device, 0));
                changed = true;
                continue;
            }
            if (!windowfocus::isFocusableAppWindow(hwnd)) {
                toRelease.append(id); // title gone, became a tool window, etc.
                continue;
            }
        } else {
            const bool stillHidden = it->hidden == hider::Method::Cloak
                                    ? hider::isCloaked(hwnd) : IsIconic(hwnd) != 0;
            if (!stillHidden) {
                if (hwnd == GetForegroundWindow()) {
                    toReveal.append(id);
                } else if (belongsOnScreen(it.value())) {
                    // Back on screen by some other hand, and belongs there:
                    // record it; the retile places it.
                    it->hidden = hider::Method::None;
                    changed = true;
                } else {
                    toRehide.append(id);
                }
            } else {
                if (!liveAreas.contains(it->device)) {
                    // Its monitor is gone (a dock unplugged while it sat on an
                    // inactive workspace). The visible branch above never runs
                    // for a hidden window, so without this it stays cloaked on a
                    // monitor no workspace can reach. The index is kept, so it
                    // stays hidden exactly where it was put.
                    const QString device = deviceForWindow(hwnd);
                    if (!device.isEmpty() && device != it->device) {
                        migrateWindow(id, device, it->workspace);
                        changed = true;
                    }
                }
                // After the migrate above: it may have landed on the active
                // workspace.
                if (belongsOnScreen(it.value()))
                    toShow.append(id);
            }
        }
    }

    if (!toRelease.isEmpty()) {
        changed = true;
        for (quintptr id : toRelease)
            releaseWindow(id, false);
    }

    for (quintptr id : toReveal) {
        auto it = m_windows.find(id);
        if (it == m_windows.end())
            continue;
        // Reality already caught up, so record that before switching: it also
        // covers the window whose workspace is already the active one, where
        // switchWorkspace() no-ops and would leave it flagged hidden for good.
        it->hidden = hider::Method::None;
        switchWorkspace(it->device, it->workspace);
    }

    if (!toRehide.isEmpty()) {
        // Re-checked at processing time: toReveal above may have switched
        // one of these ids onto the workspace it just became active on.
        QVector<quintptr> stillToHide;
        for (quintptr id : toRehide) {
            auto it = m_windows.find(id);
            if (it == m_windows.end())
                continue;
            it->hidden = hider::Method::None; // caught up; hideWindow() re-applies
            if (!belongsOnScreen(it.value()))
                stillToHide.append(id);
        }
        if (!stillToHide.isEmpty()) {
            saveStateNow();
            m_applying = true;
            for (quintptr id : stillToHide)
                hideWindow(id);
            m_applying = false;
        }
    }

    if (!toShow.isEmpty()) {
        // No saveStateNow() first: the file already names these hidden by us
        // and recovery reads live cloak state, so a write per pass while
        // explorer is down buys nothing.
        const bool cloakUp = hider::cloakAvailable(); // one acquire attempt for the batch
        m_applying = true;
        for (quintptr id : toShow) {
            auto it = m_windows.find(id);
            if (it == m_windows.end() || !belongsOnScreen(it.value()))
                continue; // toReveal above may have moved the active workspace
            if ((it->hidden == hider::Method::Cloak && !cloakUp) || !IsWindowVisible(toHwnd(id)))
                continue; // explorer not back yet, or the app hid it itself: a later pass retries
            if (showWindow(id))
                changed = true;
        }
        m_applying = false;
    }

    // Overflow windows retry every pass: closing a window, or a metrics
    // change, may have freed the room they were refused.
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (!it->overflow)
            continue;
        layout::Metrics metrics;
        if (!metricsForKey(it->device, &metrics))
            continue;
        const QString key = keyOf(it.value());
        layout::Tree *tree = treeFor(key);
        const quintptr nearId = tree->contains(m_lastFocusedIn.value(key)) ? m_lastFocusedIn.value(key) : 0;
        if (tree->insert(it.key(), nearId, metrics) == layout::Insert::Placed) {
            it->overflow = false;
            changed = true;
        }
    }

    // A window that opened and took the foreground never produced a usable
    // FOREGROUND event: it was not managed yet when the event arrived, so
    // onForegroundChanged() ignored it. Reconcile now that it is adopted, or
    // the next window to open splits whatever was focused two windows ago
    // rather than the one actually on screen.
    if (HWND fg = GetForegroundWindow()) {
        if (const auto it = m_windows.constFind(toId(fg)); it != m_windows.constEnd()) {
            m_lastFocusedIn[keyOf(it.value())] = toId(fg);
            // Same catch-up for m_focusedDevice: a window that took focus
            // before it had a title was still unmanaged when FOREGROUND
            // fired, so onForegroundChanged()'s rules never matched it.
            if (it->device != m_focusedDevice) {
                m_focusedDevice = it->device;
                m_shellFocusDevice.clear();
                notifyWorkspacesIfChanged();
            }
        }
    }

    pruneEmptyTrees();
    if (changed) {
        retile(); // also emits layoutChanged, refreshes the count, saves state
    } else if (const int visible = managedCount(); visible != m_lastVisibleCount) {
        // A pure workspace switch, or a rehide above: nothing adopted or
        // evicted, but the on-screen count changed - which is what the bar's
        // indicator means. Diffed against the last *emitted* count, since two
        // reads inside one rescan always agree.
        m_lastVisibleCount = visible;
        emit layoutChanged();
        notifyWorkspacesIfChanged(); // `tiles` lives in the monitors map too
    }
}

void TilingApi::sweep()
{
    if (!m_enabled || m_dragging)
        return;

    rescan(); // adopts windows whose title arrived after their SHOW event

    // Re-assert geometry on anything that drifted: an app moving itself, a
    // Snap gesture, a maximize - unless it drifted by going fullscreen on its
    // own (HTML5 video, F11), which keeps its tree seat untouched until it
    // shrinks back. Nothing else notices a self-move, so this is what "keep
    // the grid" means in practice for everything short of that one
    // exception. Hidden windows are skipped: a minimized one's iconic rect
    // would read as a permanent drift.
    QVector<layout::Placement> fixes;
    QVector<quintptr> giveUp;
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (it->hidden != hider::Method::None)
            continue;
        // A floated or pinned window keeps `assigned` from its tiled days;
        // without this the drift-fixer would read it as "it moved" and shove
        // the window back onto its old tile every sweep.
        if (!isTiledEntry(it.value()))
            continue;
        if (refreshFullscreen(it.key(), it.value()))
            continue; // fullscreen: left alone, no rejection counted
        if (!it->assigned.isValid())
            continue; // adopted but never placed yet
        RECT r;
        if (!GetWindowRect(toHwnd(it.key()), &r))
            continue;
        const QRect actual = toRect(r);
        const QRect &want = it->assigned;
        if (qAbs(actual.x() - want.x()) <= kDriftSlack
            && qAbs(actual.y() - want.y()) <= kDriftSlack
            && qAbs(actual.width() - want.width()) <= kDriftSlack
            && qAbs(actual.height() - want.height()) <= kDriftSlack) {
            it->rejections = 0;
            continue;
        }
        if (++it->rejections > kMaxRejections) {
            giveUp.append(it.key());
            continue;
        }
        fixes.append({ it.key(), want });
    }

    for (quintptr id : giveUp) {
        // Three sweeps ignored: a minimum track size, or an app that insists
        // on its own geometry. Float it rather than flicker at it forever -
        // pulled out of its tree in place, still fully tracked.
        qInfo() << "Tiler: floating" << processNameFor(toHwnd(id))
                << "- it will not accept a tiled size";
        auto it = m_windows.find(id);
        if (it == m_windows.end())
            continue;
        if (layout::Tree *tree = m_trees.value(keyOf(it.value())))
            tree->remove(id);
        it->floating = true;
        it->overflow = false;
        it->rejections = 0;
        m_stickyFloat.insert(id);
    }

    if (m_debug) {
        // The retile log shows what the layout wants; this shows a window
        // being pushed back after it moved itself, which is the other way a
        // rect gets applied.
        for (const layout::Placement &p : fixes) {
            qInfo().noquote() << QStringLiteral("Tiler: drift-fix %1,%2 %3x%4  %5")
                                     .arg(p.rect.x(), 5).arg(p.rect.y(), 5)
                                     .arg(p.rect.width(), 5).arg(p.rect.height(), 5)
                                     .arg(describe(p.id));
        }
    }
    if (!fixes.isEmpty())
        applyPlacements(fixes);
    if (!giveUp.isEmpty()) {
        pruneEmptyTrees();
        retile();
    }
}

// ------------------------------------------------------------------- layout

bool TilingApi::refreshFullscreen(quintptr id, Managed &m)
{
    QRect monitorRect;
    const bool covers = coversMonitor(toHwnd(id), &monitorRect);
    if (!covers) {
        if (m.fullscreen) {
            m.fullscreen = false;
            qInfo().noquote() << QStringLiteral("Tiler: %1 left fullscreen").arg(describe(id));
        }
        return m.fullscreen;
    }
    if (!m.fullscreen) {
        // Only set the flag if the window got there on its own. With zero
        // gaps, no bar and an auto-hidden taskbar, a lone tile's own rect
        // already equals the monitor rect; a bare "covers the monitor" test
        // would exempt that window forever, and it would never shrink back
        // when a second window opens. `assigned` containing monitorRect
        // means the tiler itself put it there.
        const bool ownDoing = !m.assigned.isValid()
            || m.assigned.x() > monitorRect.x() || m.assigned.y() > monitorRect.y()
            || m.assigned.x() + m.assigned.width() < monitorRect.x() + monitorRect.width()
            || m.assigned.y() + m.assigned.height() < monitorRect.y() + monitorRect.height();
        if (ownDoing) {
            m.fullscreen = true;
            qInfo().noquote()
                << QStringLiteral("Tiler: %1 went fullscreen - leaving it alone").arg(describe(id));
        }
    }
    return m.fullscreen;
}

void TilingApi::applyPlacements(const QVector<layout::Placement> &places)
{
    // A hidden member must not be moved: its rect lands on the retile that
    // follows the show instead. A fullscreen tiled member is skipped too,
    // but `assigned` is still updated for it: `assigned` means "where it
    // belongs when it comes back" - if the layout changed while this window
    // was fullscreen, the app restores its OLD bounds on exit, and the
    // drift-fixer needs the NEW rect on record to move it to the current
    // layout within one sweep. Deliberate exception to "assigned only for
    // placements that succeeded", below.
    QVector<layout::Placement> filtered;
    filtered.reserve(places.size());
    for (const layout::Placement &p : places) {
        auto it = m_windows.find(p.id);
        if (it != m_windows.end()) {
            if (it->hidden != hider::Method::None)
                continue;
            if (isTiledEntry(it.value()) && refreshFullscreen(p.id, it.value())) {
                it->assigned = p.rect;
                continue;
            }
        }
        filtered.append(p);
    }
    if (filtered.isEmpty())
        return;
    m_applying = true;

    // A maximized window ignores SetWindowPos, and ShowWindow is not valid
    // inside a DeferWindowPos batch - so the restores go first, alone.
    //
    // Not SW_RESTORE: that activates the window, so enabling the tiler with
    // a maximized window behind the focused one would hand it the focus.
    // Even SW_SHOWNOACTIVATE raises it to the top of the Z-order (measured),
    // which a tiler has no business changing either - so put it back under
    // whatever was above it.
    for (const layout::Placement &p : filtered) {
        HWND hwnd = toHwnd(p.id);
        if (!IsZoomed(hwnd))
            continue;
        // NULL is HWND_TOP: nothing above it, or only topmost windows -
        // inserting after one of those would make this one topmost too,
        // and HWND_TOP lands at the head of the non-topmost band anyway.
        HWND above = GetWindow(hwnd, GW_HWNDPREV);
        if (above && (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST))
            above = nullptr;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, above, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    // One batch, so a re-tile lands in a single frame instead of cascading
    // window by window. A failed DeferWindowPos discards the whole batch,
    // and EndDeferWindowPos can itself refuse it - either way the fallback
    // is to place EVERY window of the batch individually, not just the ones
    // not yet deferred: crediting `assigned` to a window whose DeferWindowPos
    // call merely queued without the batch ever committing would have
    // sweep() see it as drifted and re-batch it with the same poison window
    // next pass.
    HDWP batch = BeginDeferWindowPos(int(filtered.size()));
    bool batchOk = batch != nullptr;
    for (const layout::Placement &p : filtered) {
        if (!batchOk)
            break;
        batch = DeferWindowPos(batch, toHwnd(p.id), nullptr, p.rect.x(), p.rect.y(),
                                p.rect.width(), p.rect.height(), kPlaceFlags);
        batchOk = batch != nullptr;
    }
    if (batchOk)
        batchOk = EndDeferWindowPos(batch) != FALSE;

    if (batchOk) {
        for (const layout::Placement &p : filtered) {
            if (auto it = m_windows.find(p.id); it != m_windows.end())
                it->assigned = p.rect;
        }
    } else {
        // Batch died - one window refused to move (elevated process, or an
        // HWND that died mid-pass) and took its batch-mates down with it.
        // Placed one at a time instead, so only the windows that actually
        // move get `assigned`, and the one that does not gets floated
        // rather than dragging its neighbours through the same failure
        // every sweep.
        bool anyFloated = false;
        for (const layout::Placement &p : filtered) {
            HWND hwnd = toHwnd(p.id);
            if (!IsWindow(hwnd))
                continue; // gone; rescan() releases it
            const bool ok = SetWindowPos(hwnd, nullptr, p.rect.x(), p.rect.y(),
                                          p.rect.width(), p.rect.height(), kPlaceFlags);
            auto it = m_windows.find(p.id);
            if (ok) {
                if (it != m_windows.end())
                    it->assigned = p.rect;
                continue;
            }
            if (it == m_windows.end())
                continue;
            if (layout::Tree *tree = m_trees.value(keyOf(it.value())))
                tree->remove(p.id);
            it->floating = true;
            it->overflow = false;
            it->rejections = 0;
            m_stickyFloat.insert(p.id);
            qInfo() << "Tiler: floating" << processNameFor(hwnd)
                    << "- it cannot be moved (elevated?)";
            anyFloated = true;
        }
        if (anyFloated) {
            pruneEmptyTrees();
            // Not a direct retile() call: this runs inside applyPlacements(),
            // which retile() itself calls, so recursing here would re-enter
            // it mid-pass. Queued instead, so the hole this leaves closes on
            // the next spin of the event loop.
            QMetaObject::invokeMethod(this, &TilingApi::retile, Qt::QueuedConnection);
        }
    }

    m_applying = false;
}

void TilingApi::retile()
{
    if (!m_enabled)
        return;
    QVector<layout::Placement> all;
    for (auto it = m_trees.constBegin(); it != m_trees.constEnd(); ++it) {
        layout::Metrics metrics;
        if (!metricsForKey(it.key(), &metrics))
            continue;
        const QVector<layout::Placement> places = it.value()->arrange(metrics);
        all += places;

        if (m_debug) {
            qInfo().noquote()
                << QStringLiteral("Tiler: retile %1  area %2x%3  gap %4/%5  min %6x%7  tiles %8")
                       .arg(it.key())
                       .arg(metrics.area.width()).arg(metrics.area.height())
                       .arg(metrics.gap).arg(metrics.outerGap)
                       .arg(metrics.minWidth).arg(metrics.minHeight)
                       .arg(places.size());
            for (const layout::Placement &p : places) {
                // The trailing marker should never appear: computeBoxes is
                // supposed to make an undersized tile impossible. If it does
                // show up, the floor is being bypassed somewhere.
                const bool under = (metrics.minWidth > 0 && p.rect.width() < metrics.minWidth)
                                || (metrics.minHeight > 0 && p.rect.height() < metrics.minHeight);
                qInfo().noquote()
                    << QStringLiteral("Tiler:   %1,%2 %3x%4  %5%6")
                           .arg(p.rect.x(), 5).arg(p.rect.y(), 5)
                           .arg(p.rect.width(), 5).arg(p.rect.height(), 5)
                           .arg(describe(p.id), under ? QStringLiteral("  <== UNDER MINIMUM")
                                                      : QString());
            }
        }
    }
    applyPlacements(all);
    m_lastVisibleCount = managedCount(); // rescan() diffs against this
    scheduleSaveState(); // tree shape/membership may have changed
    emit layoutChanged();
    notifyWorkspacesIfChanged(); // a move, adoption or close changes who is where
}

// Gated rather than emitted straight from retile(), because the property is a
// QVariantMap: every emission rebuilds a bar's Repeater delegates, and
// retile() runs for every drift fix. Same idea as m_lastVisibleCount for
// layoutChanged. The signature covers every monitor, not just the focused
// one - a per-monitor bar has to be told about a change on any of them, and
// deriving it from `monitors` (a compact JSON dump is the cheapest stable
// text form of a QVariantMap) is what makes that automatic instead of having
// to enumerate what changed by hand.
void TilingApi::notifyWorkspacesIfChanged()
{
    const QString signature = QString::number(m_workspaceCount) + QLatin1Char('#')
        + QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(monitors()))
                                 .toJson(QJsonDocument::Compact));

    if (signature == m_workspacesSignature)
        return;
    m_workspacesSignature = signature;
    emit workspacesChanged();
}

void TilingApi::releaseWindow(quintptr id, bool restoreGeometry)
{
    const auto found = m_windows.constFind(id);
    if (found == m_windows.constEnd())
        return;
    const Managed entry = found.value();
    m_windows.erase(m_windows.find(id));

    const QString key = keyOf(entry);
    if (m_lastFocusedIn.value(key) == id)
        m_lastFocusedIn.remove(key);

    if (isTiledEntry(entry)) {
        if (layout::Tree *tree = m_trees.value(key))
            tree->remove(id);
    }

    if (entry.pinned && !entry.wasTopmost) {
        // Best effort: the window may already be gone.
        SetWindowPos(toHwnd(id), HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Disabling the tiler mid-video must not shrink a fullscreen window back
    // to the geometry it had at adoption.
    if (restoreGeometry && entry.original.isValid() && !coversMonitor(toHwnd(id))) {
        m_applying = true;
        SetWindowPos(toHwnd(id), nullptr, entry.original.x(), entry.original.y(),
                      entry.original.width(), entry.original.height(), kPlaceFlags);
        m_applying = false;
    }
}

void TilingApi::onMoveSizeEnd(void *hwnd)
{
    m_dragging = 0;
    if (!m_enabled)
        return;

    const quintptr id = toId(hwnd);
    const auto state = m_windows.constFind(id);
    if (state == m_windows.constEnd())
        return; // not ours

    if (deviceForWindow(hwnd) != state->device) {
        rescan(); // dragged onto another monitor: re-home it, floating or not
        return;
    }

    if (state->floating || state->overflow || state->pinned)
        return; // nothing tiled to resize or swap

    const QString key = keyOf(state.value());
    layout::Tree *tree = m_trees.value(key);
    if (!tree || !tree->contains(id)) {
        rescan(); // out of sync with its tree somehow; re-home it
        return;
    }

    RECT r;
    if (!GetWindowRect(static_cast<HWND>(hwnd), &r))
        return;
    const QRect actual = toRect(r);
    const QRect assigned = state->assigned;

    layout::Metrics metrics;
    if (!metricsForKey(state->device, &metrics))
        return;

    const bool resized = !assigned.isValid()
                       || qAbs(actual.width() - assigned.width()) > kDriftSlack
                       || qAbs(actual.height() - assigned.height()) > kDriftSlack;

    if (resized) {
        tree->applyResize(id, actual, metrics);
    } else {
        // Same size, new place: a title-bar drag. Trade places with whatever
        // sits under the pointer - the only reading of a drag that keeps the
        // partition intact. With no target, the retile below snaps it back.
        POINT cursor;
        if (GetCursorPos(&cursor)) {
            for (auto m = m_windows.constBegin(); m != m_windows.constEnd(); ++m) {
                if (m.key() == id || !tree->contains(m.key()))
                    continue;
                if (m->assigned.contains(cursor.x, cursor.y)) {
                    tree->swap(id, m.key());
                    break;
                }
            }
        }
    }

    retile();
}

// --------------------------------------------------------------- QML commands

layout::Tree *TilingApi::treeForFocused(quintptr *id, layout::Metrics *metrics, qreal *scale)
{
    if (!m_enabled)
        return nullptr;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return nullptr;
    *id = toId(fg);

    const auto it = m_windows.constFind(*id);
    if (it == m_windows.constEnd() || !isTiledEntry(it.value()))
        return nullptr;

    const QString key = keyOf(it.value());
    layout::Tree *tree = m_trees.value(key);
    if (!tree || !tree->contains(*id))
        return nullptr;

    // neighbour() reads the boxes left by the last arrange(); refresh them
    // so a command is never answered from a stale partition.
    layout::Metrics own;
    if (!metricsForKey(it->device, &own))
        return metrics ? nullptr : tree;
    tree->arrange(own);
    if (metrics)
        *metrics = own;
    if (scale)
        *scale = scaleForDevice(it->device);
    return tree;
}

void TilingApi::focusDirection(const QString &direction)
{
    layout::Direction dir;
    if (!parseDirection(direction, &dir))
        return;
    quintptr id = 0;
    layout::Tree *tree = treeForFocused(&id);
    const quintptr target = tree ? tree->neighbour(id, dir) : 0;
    if (target) {
        // Allowed because these commands come from a Hotkey: WM_HOTKEY grants
        // the process foreground rights for the duration.
        SetForegroundWindow(toHwnd(target));
        return;
    }
    // The foreground window is not a tiled member of ours, or it already
    // sits at the edge of its monitor's tree that way - either way there is
    // nothing left to do on this monitor, so carry on to the next one, the
    // way Hyprland's movefocus does at a monitor edge.
    focusMonitor(direction);
}

void TilingApi::moveDirection(const QString &direction)
{
    layout::Direction dir;
    if (!parseDirection(direction, &dir))
        return;
    quintptr id = 0;
    layout::Tree *tree = treeForFocused(&id);
    const quintptr target = tree ? tree->neighbour(id, dir) : 0;
    if (target) {
        // Focus needs no help: swapping exchanges the leaves' occupants, so
        // the window the user was in is still the foreground one.
        if (tree->swap(id, target))
            retile();
        return;
    }
    moveToMonitor(direction);
}

// The monitor beside focusedDevice() in `direction`. Same onSide/inLane/
// distance shape as Tree::neighbour(), one level up: monitors, not leaves.
QString TilingApi::adjacentDevice(const QString &direction) const
{
    layout::Direction dir;
    if (!parseDirection(direction, &dir))
        return QString();

    const QHash<QString, QRect> areas = workAreas();
    const auto fromIt = areas.constFind(focusedDevice());
    if (fromIt == areas.constEnd())
        return QString();
    const QRect me = fromIt.value();
    const QPoint c = me.center();

    // "That way" means wholly past this monitor's edge, not merely a centre
    // further along: two side-by-side monitors of different heights have
    // centres at different y, and "down" from the shorter one must not jump
    // sideways. Perpendicular overlap is then a preference, not a filter, so
    // monitors stacked diagonally still reach each other.
    QString bestOverlap, bestAny;
    int bestOverlapDistance = 0, bestAnyDistance = 0;
    for (auto it = areas.constBegin(); it != areas.constEnd(); ++it) {
        if (it.key() == fromIt.key())
            continue;
        const QRect &r = it.value();
        const QPoint o = r.center();

        bool onSide = false, inLane = false;
        int distance = 0;
        switch (dir) {
        case layout::Direction::Left:
            onSide = r.right() < me.left();
            inLane = r.top() < me.bottom() && me.top() < r.bottom();
            distance = c.x() - o.x();
            break;
        case layout::Direction::Right:
            onSide = r.left() > me.right();
            inLane = r.top() < me.bottom() && me.top() < r.bottom();
            distance = o.x() - c.x();
            break;
        case layout::Direction::Up:
            onSide = r.bottom() < me.top();
            inLane = r.left() < me.right() && me.left() < r.right();
            distance = c.y() - o.y();
            break;
        case layout::Direction::Down:
            onSide = r.top() > me.bottom();
            inLane = r.left() < me.right() && me.left() < r.right();
            distance = o.y() - c.y();
            break;
        }
        if (!onSide)
            continue;
        if (bestAny.isEmpty() || distance < bestAnyDistance) {
            bestAny = it.key();
            bestAnyDistance = distance;
        }
        if (inLane && (bestOverlap.isEmpty() || distance < bestOverlapDistance)) {
            bestOverlap = it.key();
            bestOverlapDistance = distance;
        }
    }
    return !bestOverlap.isEmpty() ? bestOverlap : bestAny;
}

void TilingApi::focusMonitor(const QString &direction)
{
    if (!m_enabled)
        return;
    const QString target = adjacentDevice(direction);
    if (target.isEmpty())
        return;

    // Allowed for the same reason as in focusDirection(): WM_HOTKEY. Lands on
    // the last-focused or topmost member, or the shell when the monitor is
    // empty - and makes `target` the focused device either way.
    focusWorkspaceMember(target, m_active.value(target, 0));
    notifyWorkspacesIfChanged();
}

void TilingApi::moveToMonitor(const QString &direction)
{
    if (!m_enabled)
        return;
    // First, as in moveToWorkspace() - and before picking the target: it is
    // what brings m_focusedDevice, which "adjacent" is measured from, up to
    // date with the foreground window.
    rescan();
    const QString target = adjacentDevice(direction);
    if (target.isEmpty())
        return;

    HWND fg = GetForegroundWindow();
    if (!fg)
        return;
    const quintptr id = toId(fg);
    const auto before = m_windows.constFind(id);
    if (before == m_windows.constEnd())
        return; // not ours to move

    const QString source = before->device;
    RECT rectBefore = {};
    const bool hadRect = GetWindowRect(fg, &rectBefore);

    // Updates m_focusedDevice for the foreground window itself.
    migrateWindow(id, target, m_active.value(target, 0));

    const auto after = m_windows.constFind(id);
    if (after != m_windows.constEnd() && !isTiledEntry(after.value()) && hadRect) {
        // Not tiled at the new spot - floating, pinned, or it came back
        // overflow - so migrateWindow()'s bookkeeping-only move left its
        // geometry sitting over the OLD monitor; without repositioning it
        // here the next sweep reads that as "on the wrong monitor" and
        // migrates it straight back.
        const QHash<QString, QRect> areas = workAreas();
        const auto sourceArea = areas.constFind(source);
        const auto targetArea = areas.constFind(target);
        if (sourceArea != areas.constEnd() && targetArea != areas.constEnd()
            && sourceArea->width() > 0 && sourceArea->height() > 0) {
            const QRect from = toRect(rectBefore);
            const QRect &oldArea = sourceArea.value();
            const QRect &newArea = targetArea.value();
            // Same relative offset inside the new work area, same size,
            // clamped to fit a monitor that may be smaller.
            const qreal fx = qreal(from.x() - oldArea.x()) / oldArea.width();
            const qreal fy = qreal(from.y() - oldArea.y()) / oldArea.height();
            const int w = qMin(from.width(), newArea.width());
            const int h = qMin(from.height(), newArea.height());
            int x = newArea.x() + qRound(fx * newArea.width());
            int y = newArea.y() + qRound(fy * newArea.height());
            x = qBound(newArea.x(), x, newArea.x() + newArea.width() - w);
            y = qBound(newArea.y(), y, newArea.y() + newArea.height() - h);

            m_applying = true;
            SetWindowPos(fg, nullptr, x, y, w, h, kPlaceFlags);
            m_applying = false;
        }
    }

    pruneEmptyTrees();
    retile();
    saveStateNow();
    notifyWorkspacesIfChanged();
}

void TilingApi::resize(const QString &how)
{
    layout::SplitKind axis;
    int sign = 1;
    if (!parseResize(how, &axis, &sign))
        return;
    quintptr id = 0;
    layout::Metrics metrics;
    qreal scale = 1.0;
    layout::Tree *tree = treeForFocused(&id, &metrics, &scale);
    if (!tree)
        return;
    if (tree->resize(id, axis, sign * qMax(1, qRound(m_resizeStep * scale)), metrics))
        retile();
}

void TilingApi::toggleFloating()
{
    HWND fg = GetForegroundWindow();
    if (!fg || !m_enabled)
        return;
    const quintptr id = toId(fg);
    auto it = m_windows.find(id);
    if (it == m_windows.end())
        return;
    if (it->pinned)
        return; // togglePinned() is the command for a pinned window

    if (it->floating) {
        // Rejoin the tiled layout.
        it->floating = false;
        m_stickyFloat.remove(id);
        const QString key = keyOf(it.value());
        layout::Metrics metrics;
        if (metricsForKey(it->device, &metrics)) {
            layout::Tree *tree = treeFor(key);
            const quintptr nearId = tree->contains(m_lastFocusedIn.value(key))
                                 ? m_lastFocusedIn.value(key) : 0;
            if (tree->insert(id, nearId, metrics) == layout::Insert::TooSmall)
                it->overflow = true; // still no room; stays floating in practice
        } else {
            it->overflow = true;
        }
    } else if (it->overflow) {
        // Floating only for lack of room so far - make that the user's
        // decision instead, so it stops being reclaimed the moment something
        // else closes. Already floating where it sits, so no geometry to
        // restore.
        it->overflow = false;
        it->floating = true;
        it->assigned = QRect();
        it->rejections = 0;
        m_stickyFloat.insert(id);
    } else {
        // Leave the layout, restoring the size it had when adopted.
        const QString key = keyOf(it.value());
        if (layout::Tree *tree = m_trees.value(key))
            tree->remove(id);
        it->floating = true;
        m_stickyFloat.insert(id);
        // Forget the tile rect with the tile: it is what the sweep would
        // otherwise keep dragging this window back to.
        it->assigned = QRect();
        it->rejections = 0;
        if (it->original.isValid()) {
            m_applying = true;
            SetWindowPos(fg, nullptr, it->original.x(), it->original.y(),
                         it->original.width(), it->original.height(), kPlaceFlags);
            m_applying = false;
        }
    }
    pruneEmptyTrees();
    retile();
}

void TilingApi::toggleSplit()
{
    quintptr id = 0;
    layout::Tree *tree = treeForFocused(&id);
    if (tree && tree->toggleSplit(id))
        retile();
}

void TilingApi::equalize()
{
    quintptr id = 0;
    if (layout::Tree *tree = treeForFocused(&id)) {
        tree->equalize();
        retile();
    }
}
