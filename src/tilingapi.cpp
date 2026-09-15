#include "tilingapi.h"

#include "windowfocus.h"

#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QSet>

#include <windows.h>
#include <dwmapi.h>

#include <string>

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
constexpr int kMaxHideFailures = 3;
constexpr int kProcessCacheCap = 256;
constexpr int kSaveDebounceMs = 500;
constexpr int kPendingTimeoutMs = 5000;

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

QRect toRect(const RECT &r)
{
    return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

HWND toHwnd(quintptr id) { return reinterpret_cast<HWND>(id); }
quintptr toId(void *hwnd) { return reinterpret_cast<quintptr>(hwnd); }

QString hiderMethodToString(hider::Method m)
{
    switch (m) {
    case hider::Method::Cloak:    return QStringLiteral("cloak");
    case hider::Method::Minimize: return QStringLiteral("minimize");
    case hider::Method::None:     return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

hider::Method hiderMethodFromString(const QString &s)
{
    if (s == QLatin1String("cloak"))
        return hider::Method::Cloak;
    if (s == QLatin1String("minimize"))
        return hider::Method::Minimize;
    return hider::Method::None;
}

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

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param)
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
QHash<QString, QRect> workAreas()
{
    QHash<QString, QRect> out;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&out));
    return out;
}

// QScreen::name() is the GDI device name on Windows, so it matches the key
// above. Going through Qt keeps this unit off shcore/GetDpiForMonitor.
qreal scaleForDevice(const QString &device)
{
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen->name() == device)
            return screen->devicePixelRatio();
    }
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

    // Records the show before doing it: the file must never claim a window is
    // hidden when it is already back on screen.
    saveStateNow();

    m_applying = true;
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        showWindow(it.key());
        if (it->pinned && !it->wasTopmost) {
            SetWindowPos(toHwnd(it.key()), HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    m_applying = false;

    // Everything is back on screen, so there is nothing left to recover. Only
    // a clean tray quit reaches here; a forced kill is what recoverState() is
    // for.
    tilingstate::remove();

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
        if (!m_pendingRecovered.isEmpty()) {
            // The recovered data is already correct; claiming only cancels
            // the safety net that would otherwise undo it.
            m_pendingTimer.stop();
            qInfo() << "Tiler: claiming" << m_pendingRecovered.size()
                    << "window(s) recovered from a previous session";
            m_pendingRecovered.clear();
        }
        m_sweepTimer.start();
        rescan();
    } else {
        m_sweepTimer.stop();
        m_scanTimer.stop();
        m_settleTimer.stop();
        m_displayTimer.stop();

        saveStateNow(); // see the destructor for why this goes first

        m_applying = true;
        for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
            showWindow(it.key());
            if (it->pinned && !it->wasTopmost) {
                SetWindowPos(toHwnd(it.key()), HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        m_applying = false;

        // Back on screen, so nothing left to recover.
        tilingstate::remove();

        const QList<quintptr> ids = m_windows.keys();
        for (quintptr id : ids)
            releaseWindow(id, true);
        qDeleteAll(m_trees);
        m_trees.clear();
        m_windows.clear();
        m_active.clear();
        m_lastFocusedIn.clear();
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

// -------------------------------------------------------------- workspaces

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

QString TilingApi::focusedDevice() const
{
    if (!m_focusedDevice.isEmpty() && workAreas().contains(m_focusedDevice))
        return m_focusedDevice;
    POINT pt = {};
    if (GetCursorPos(&pt)) {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi))
            return QString::fromWCharArray(mi.szDevice);
    }
    if (QScreen *primary = QGuiApplication::primaryScreen())
        return primary->name();
    return QString();
}

void TilingApi::setWorkspaceCount(int count)
{
    count = qBound(1, count, 20);
    if (m_workspaceCount == count)
        return;

    if (count < m_workspaceCount) {
        for (auto dit = m_active.begin(); dit != m_active.end(); ++dit) {
            if (dit.value() >= count)
                dit.value() = count - 1;
        }
        QVector<quintptr> toClamp;
        for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
            if (it->workspace >= count)
                toClamp.append(it.key());
        }
        m_workspaceCount = count;
        for (quintptr id : toClamp)
            migrateWindow(id, m_windows.value(id).device, count - 1);
        pruneEmptyTrees();
        // The clamp moved both the windows and the active indices, which no
        // switchWorkspace() call describes - and a window left hidden on the
        // now-active workspace would have no way back, since a switch to the
        // index already active no-ops.
        if (!m_windows.isEmpty()) {
            saveStateNow(); // before the hides below, never after
            reconcileHidden();
        }
        if (m_enabled)
            retile();
    } else {
        m_workspaceCount = count;
    }
    notifyWorkspacesIfChanged();
}

int TilingApi::currentWorkspace() const
{
    return m_active.value(focusedDevice(), 0);
}

QVariantList TilingApi::workspaces() const
{
    const QString device = focusedDevice();
    const int active = m_active.value(device, 0);

    QVector<int> counts(m_workspaceCount, 0);
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        if (it->device == device && it->workspace >= 0 && it->workspace < m_workspaceCount)
            ++counts[it->workspace];
    }

    QVariantList out;
    out.reserve(m_workspaceCount);
    for (int i = 0; i < m_workspaceCount; ++i) {
        QVariantMap entry;
        entry.insert(QStringLiteral("index"), i);
        entry.insert(QStringLiteral("active"), i == active);
        entry.insert(QStringLiteral("windows"), counts.at(i));
        out.append(entry);
    }
    return out;
}

void TilingApi::focusWorkspaceMember(const QString &device, int workspace)
{
    const QString key = device + QLatin1Char('|') + QString::number(workspace);
    const quintptr target = m_lastFocusedIn.value(key);
    if (target) {
        const auto it = m_windows.constFind(target);
        if (it != m_windows.constEnd() && it->hidden == hider::Method::None) {
            SetForegroundWindow(toHwnd(target));
            return;
        }
    }
    windowfocus::focusTopmostWindow([this, device, workspace](void *hwnd) {
        const auto it = m_windows.constFind(toId(hwnd));
        if (it == m_windows.constEnd())
            return false;
        return it->device == device && (it->pinned || it->workspace == workspace);
    });
}

void TilingApi::switchWorkspace(const QString &device, int index)
{
    if (device.isEmpty())
        return;
    index = qBound(0, index, m_workspaceCount - 1);
    const int oldIndex = m_active.value(device, 0);
    if (oldIndex == index)
        return;

    m_active[device] = index;
    // Pinned windows on this device always show on the active workspace.
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (it->pinned && it->device == device)
            it->workspace = index;
    }

    saveStateNow(); // state must reflect the switch before any window is touched

    // Show before hide - no empty-desktop flash - then retile so the newly
    // shown members land in their tile rects, then hide the old workspace.
    m_applying = true;
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (it->device == device && it->workspace == index)
            showWindow(it.key());
    }
    m_applying = false;

    retile();

    m_applying = true;
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (it->device == device && it->workspace == oldIndex && !it->pinned)
            hideWindow(it.key());
    }
    m_applying = false;

    // Focus explicitly: the foreground window is one we just hid.
    focusWorkspaceMember(device, index);

    notifyWorkspacesIfChanged();
    emit layoutChanged();
}

void TilingApi::switchToWorkspace(int index)
{
    if (!m_enabled)
        return;
    switchWorkspace(focusedDevice(), index);
}

void TilingApi::moveWindow(quintptr id, int index, bool follow)
{
    const auto it = m_windows.find(id);
    if (it == m_windows.end())
        return;
    index = qBound(0, index, m_workspaceCount - 1);
    if (it->workspace == index)
        return;

    const QString device = it->device;
    migrateWindow(id, device, index);

    if (follow) {
        switchWorkspace(device, index); // shows it, with the rest of that workspace
    } else if (!it->pinned && index != m_active.value(device, 0)) {
        saveStateNow();
        m_applying = true;
        hideWindow(id);
        m_applying = false;
        // Focus something on the workspace the user is still looking at.
        focusWorkspaceMember(device, m_active.value(device, 0));
    }

    pruneEmptyTrees();
    retile();
}

void TilingApi::moveToWorkspace(int index, bool follow)
{
    if (!m_enabled)
        return;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return;
    moveWindow(toId(fg), index, follow);
}

void TilingApi::moveToEmptyWorkspace()
{
    if (!m_enabled)
        return;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return;
    const quintptr id = toId(fg);
    const auto it = m_windows.constFind(id);
    if (it == m_windows.constEnd())
        return; // not a window we manage

    const QString device = it->device;
    // The window's own workspace counts as occupied, so "first empty" never
    // resolves to the one it is already on.
    QSet<int> used;
    for (auto w = m_windows.constBegin(); w != m_windows.constEnd(); ++w) {
        if (w->device == device)
            used.insert(w->workspace);
    }

    for (int i = 0; i < m_workspaceCount; ++i) {
        if (used.contains(i))
            continue;
        moveWindow(id, i, true); // follow it
        return;
    }
    qWarning().noquote() << QStringLiteral("Tiler: no empty workspace on %1 -"
                                            " all %2 are in use")
                                 .arg(device).arg(m_workspaceCount);
}

void TilingApi::togglePinned()
{
    if (!m_enabled)
        return;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return;
    const auto it = m_windows.find(toId(fg));
    if (it == m_windows.end())
        return;

    if (it->pinned) {
        it->pinned = false;
        if (!it->wasTopmost) {
            SetWindowPos(fg, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    } else {
        if (isTiledEntry(it.value())) {
            if (layout::Tree *tree = m_trees.value(keyOf(it.value())))
                tree->remove(toId(fg));
        }
        it->pinned = true;
        it->floating = true;
        it->overflow = false;
        it->assigned = QRect(); // as in toggleFloating: no tile, no rect to re-assert
        it->rejections = 0;
        it->workspace = m_active.value(it->device, 0);

        if (m_pinnedTopmost) {
            it->wasTopmost = GetWindowLongPtrW(fg, GWL_EXSTYLE) & WS_EX_TOPMOST;
            SetWindowPos(fg, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else {
            it->wasTopmost = false;
        }
    }

    pruneEmptyTrees();
    retile();
    notifyWorkspacesIfChanged();
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
    const QString device = deviceForWindow(hwnd);
    if (!device.isEmpty())
        m_focusedDevice = device;

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
    if (tiled) {
        if (layout::Tree *tree = m_trees.value(oldKey))
            tree->remove(id);
    }
    it->device = newDevice;
    it->workspace = newWorkspace;
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
        // rather than tile, as do the configured floatProcesses.
        const int workspace = m_active.value(device, 0);
        const bool floating = !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME)
                            || floatProcessMatches(processNameFor(hwnd));

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

        // For the state file (see Managed::placementNormal); every restore
        // path but recovery uses `original` above.
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(hwnd, &wp)) {
            entry.placementShowCmd = int(wp.showCmd);
            entry.placementNormal = toRect(wp.rcNormalPosition);
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
                if (hwnd == GetForegroundWindow())
                    toReveal.append(id);
                else
                    toRehide.append(id);
            } else if (!liveAreas.contains(it->device)) {
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
        for (quintptr id : toRehide) {
            if (auto it = m_windows.find(id); it != m_windows.end())
                it->hidden = hider::Method::None; // caught up; hideWindow() re-applies
        }
        saveStateNow();
        m_applying = true;
        for (quintptr id : toRehide)
            hideWindow(id);
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
        if (const auto it = m_windows.constFind(toId(fg)); it != m_windows.constEnd())
            m_lastFocusedIn[keyOf(it.value())] = toId(fg);
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
    }
}

void TilingApi::sweep()
{
    if (!m_enabled || m_dragging)
        return;

    rescan(); // adopts windows whose title arrived after their SHOW event

    // Re-assert geometry on anything that drifted: an app moving itself, a
    // Snap gesture, a maximize. This is what "keep the grid" means in
    // practice - nothing else notices those. Hidden windows are skipped: a
    // minimized one's iconic rect would read as a permanent drift.
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

void TilingApi::applyPlacements(const QVector<layout::Placement> &places)
{
    // A hidden member must not be moved: its rect lands on the retile that
    // follows the show instead.
    QVector<layout::Placement> filtered;
    filtered.reserve(places.size());
    for (const layout::Placement &p : places) {
        const auto it = m_windows.constFind(p.id);
        if (it != m_windows.constEnd() && it->hidden != hider::Method::None)
            continue;
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
    // window by window.
    HDWP batch = BeginDeferWindowPos(int(filtered.size()));
    for (const layout::Placement &p : filtered) {
        HWND hwnd = toHwnd(p.id);
        if (batch)
            batch = DeferWindowPos(batch, hwnd, nullptr, p.rect.x(), p.rect.y(),
                                    p.rect.width(), p.rect.height(), kPlaceFlags);
        // A failed DeferWindowPos discards the whole batch, so from here on
        // the rest go one at a time; the sweep re-places whatever was lost.
        if (!batch)
            SetWindowPos(hwnd, nullptr, p.rect.x(), p.rect.y(),
                          p.rect.width(), p.rect.height(), kPlaceFlags);
        if (auto it = m_windows.find(p.id); it != m_windows.end())
            it->assigned = p.rect;
    }
    if (batch)
        EndDeferWindowPos(batch);

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
// QVariantList: every emission rebuilds the bar's Repeater delegates, and
// retile() runs for every drift fix. Same idea as m_lastVisibleCount for
// layoutChanged.
void TilingApi::notifyWorkspacesIfChanged()
{
    const QString device = focusedDevice();
    QString signature = device + QLatin1Char('#')
                      + QString::number(m_active.value(device, 0)) + QLatin1Char('#')
                      + QString::number(m_workspaceCount) + QLatin1Char('#');
    QVector<int> counts(m_workspaceCount, 0);
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        if (it->device == device && it->workspace >= 0 && it->workspace < m_workspaceCount)
            ++counts[it->workspace];
    }
    for (int c : std::as_const(counts))
        signature += QString::number(c) + QLatin1Char(',');

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

    if (restoreGeometry && entry.original.isValid()) {
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

    if (state->floating || state->overflow || state->pinned)
        return; // nothing tiled to resize or swap

    const QString key = keyOf(state.value());
    layout::Tree *tree = m_trees.value(key);
    const QString actualDevice = deviceForWindow(hwnd);
    if (!tree || !tree->contains(id) || actualDevice != state->device) {
        rescan(); // dragged onto another monitor: re-home it
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

// -------------------------------------------------------------- hide/show

hider::Method TilingApi::effectiveHideMethod(const Managed &m) const
{
    if (!hider::cloakAvailable() || m.hideFailures >= kMaxHideFailures)
        return hider::Method::Minimize;
    return m_hideMethod;
}

void TilingApi::hideWindow(quintptr id)
{
    const auto it = m_windows.find(id);
    if (it == m_windows.end() || it->hidden != hider::Method::None)
        return;
    hider::Method method = effectiveHideMethod(it.value());
    bool ok = hider::hide(toHwnd(id), method);
    if (!ok && method == hider::Method::Cloak) {
        ++it->hideFailures;
        method = hider::Method::Minimize;
        ok = hider::hide(toHwnd(id), method);
    }
    if (!ok) {
        // Never record a hide that did not happen: the membership pass would
        // read it back as "something un-hid it" and re-try it, plus a state
        // write, every sweep. Left on screen instead - wrong, but stable.
        qWarning() << "Tiler:" << describe(id)
                   << "refused both hide methods - leaving it on screen";
        return;
    }
    it->hidden = method;
}

void TilingApi::showWindow(quintptr id)
{
    const auto it = m_windows.find(id);
    if (it == m_windows.end() || it->hidden == hider::Method::None)
        return;
    hider::show(toHwnd(id), it->hidden);
    it->hidden = hider::Method::None;
}

void TilingApi::reconcileHidden()
{
    QVector<quintptr> toShow, toHide;
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        const bool shouldHide = it->workspace != m_active.value(it->device, 0) && !it->pinned;
        if (shouldHide && it->hidden == hider::Method::None)
            toHide.append(it.key());
        else if (!shouldHide && it->hidden != hider::Method::None)
            toShow.append(it.key());
    }
    if (toShow.isEmpty() && toHide.isEmpty())
        return;

    // Show before hide, as switchWorkspace() does, so nothing flashes an
    // empty screen in between.
    m_applying = true;
    for (quintptr id : std::as_const(toShow))
        showWindow(id);
    for (quintptr id : std::as_const(toHide))
        hideWindow(id);
    m_applying = false;
}

// ----------------------------------------------------------------- state file

void TilingApi::saveStateNow()
{
    m_saveTimer.stop(); // this write makes a pending debounced one redundant

    tilingstate::Snapshot snapshot;
    snapshot.hideMethod = hideMethod();
    for (auto it = m_active.constBegin(); it != m_active.constEnd(); ++it)
        snapshot.active.insert(it.key(), it.value());

    for (auto it = m_trees.constBegin(); it != m_trees.constEnd(); ++it) {
        // A tree can be empty mid-operation: treeFor() creates one before the
        // insert, and moveWindow(follow) saves between a removal and
        // pruneEmptyTrees(). It carries nothing, so it is never written.
        if (it.value()->isEmpty())
            continue;
        tilingstate::WorkspaceTree ws;
        ws.monitor = it.key().section(QLatin1Char('|'), 0, 0);
        ws.index = it.key().section(QLatin1Char('|'), 1, 1).toInt();
        ws.tree = it.value()->toJson();
        snapshot.workspaces.append(ws);
    }

    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        tilingstate::Entry e;
        e.hwnd = it.key();
        // Captured at adoption (see Managed), not re-read here: this runs
        // synchronously in front of every hide/show batch.
        e.pid = it->pid;
        e.created = it->created;
        e.windowClass = it->windowClass;
        e.hidden = hiderMethodToString(it->hidden);
        e.placementShowCmd = it->placementShowCmd;
        e.placementNormal = it->placementNormal;
        e.wasTopmost = it->wasTopmost;
        e.monitor = it->device;
        e.workspace = it->workspace;
        e.floating = it->floating;
        e.pinned = it->pinned;
        snapshot.windows.append(e);
    }

    tilingstate::save(snapshot);
}

void TilingApi::scheduleSaveState()
{
    m_saveTimer.start(); // restarted, not started-if-idle: this IS the debounce
}

void TilingApi::abandonRecovery(const QList<tilingstate::Entry> &entries,
                                 const QHash<QString, int> &active)
{
    m_applying = true;
    for (const tilingstate::Entry &e : entries) {
        HWND hwnd = toHwnd(e.hwnd);
        if (!IsWindow(hwnd))
            continue; // gone since identityMatches() checked; nothing to undo
        // Only undo a hide that was ours to make - the same two-halves test
        // recoverState()'s hiddenByUs() explains.
        const auto ours = [&](hider::Method how) {
            return (!e.pinned && e.workspace != active.value(e.monitor, 0))
                || hiderMethodFromString(e.hidden) == how;
        };
        if (hider::isCloaked(hwnd) && ours(hider::Method::Cloak))
            hider::show(hwnd, hider::Method::Cloak);
        else if (IsIconic(hwnd) && ours(hider::Method::Minimize))
            hider::show(hwnd, hider::Method::Minimize);
        if (e.pinned && !e.wasTopmost) {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    m_applying = false;

    // Everything it named is back on screen; nothing left worth reading.
    tilingstate::remove();
}

void TilingApi::abandonPending()
{
    if (m_pendingRecovered.isEmpty())
        return; // already claimed by setEnabled(true), or nothing was recovered

    qInfo() << "Tiler: never enabled within" << kPendingTimeoutMs / 1000
            << "s of recovering" << m_pendingRecovered.size()
            << "window(s) - showing and forgetting them";

    m_applying = true;
    for (quintptr id : std::as_const(m_pendingRecovered)) {
        const auto it = m_windows.find(id);
        if (it == m_windows.end())
            continue;
        showWindow(id);
        if (it->pinned && !it->wasTopmost) {
            SetWindowPos(toHwnd(id), HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    m_applying = false;

    qDeleteAll(m_trees);
    m_trees.clear();
    m_windows.clear();
    m_active.clear();
    m_pendingRecovered.clear();
    tilingstate::remove(); // nothing left to recover
}

void TilingApi::recoverState()
{
    tilingstate::Snapshot snapshot;
    QString why;
    const tilingstate::LoadResult result = tilingstate::load(&snapshot, &why);
    if (result == tilingstate::LoadResult::None) {
        qInfo().noquote() << "Tiler: nothing to recover -" << why;
        return;
    }

    // "Did WE hide this?" - our cloak and the shell's look identical from
    // outside. It takes both halves of the file, since the save runs in front
    // of the batch it describes: the recorded intent (workspace vs. active)
    // catches a crash partway through a hide, `hidden` catches one partway
    // through a show. A window answering no to both is the shell's or the
    // user's, and is left alone.
    const auto hiddenByUs = [&snapshot](const tilingstate::Entry &e, hider::Method how) {
        const bool intended = !e.pinned && e.workspace != snapshot.active.value(e.monitor, 0);
        return intended || hiderMethodFromString(e.hidden) == how;
    };

    // Per-entry identity first: a stale record of a since-closed (or
    // since-recycled-HWND) window is dropped outright, never adopted.
    QList<tilingstate::Entry> valid;
    valid.reserve(snapshot.windows.size());
    for (const tilingstate::Entry &e : snapshot.windows) {
        if (tilingstate::identityMatches(e))
            valid.append(e);
    }
    if (valid.size() != snapshot.windows.size()) {
        qInfo() << "Tiler: recovery: dropped" << (snapshot.windows.size() - valid.size())
                << "of" << snapshot.windows.size() << "entries - no longer the same window";
    }
    if (result == tilingstate::LoadResult::Salvage) {
        // Not trustworthy enough to rebuild from, but still the only record
        // of what we cloaked - ignoring it is the one path that could strand
        // a window hidden. The identity check above makes it safe: after a
        // genuine reboot nothing survives it, so nothing is touched.
        qWarning().noquote() << "Tiler: not recovering -" << why << "- showing"
                             << valid.size() << "window(s) it still names";
        abandonRecovery(valid, snapshot.active); // deletes the file either way
        return;
    }
    if (valid.isEmpty()) {
        tilingstate::remove(); // nothing survived; nothing to hold pending either
        return;
    }

    QSet<quintptr> seen;
    for (const tilingstate::Entry &e : valid) {
        if (seen.contains(e.hwnd)) {
            qWarning() << "Tiler: recovery abandoned - duplicate hwnd in tiling-state.json";
            abandonRecovery(valid, snapshot.active);
            return;
        }
        seen.insert(e.hwnd);
    }

    const QHash<QString, QRect> areas = workAreas();
    QScreen *primaryScreen = QGuiApplication::primaryScreen();
    const QString primary = primaryScreen ? primaryScreen->name() : QString();
    if (primary.isEmpty()) {
        qWarning() << "Tiler: recovery abandoned - no primary monitor to fall back to";
        abandonRecovery(valid, snapshot.active);
        return;
    }

    // Built in a scratch map first, so a structural problem below is caught
    // before anything lands in m_windows.
    QHash<quintptr, Managed> windows;
    for (const tilingstate::Entry &e : valid) {
        HWND hwnd = toHwnd(e.hwnd);
        Managed m;
        m.device = areas.contains(e.monitor) ? e.monitor : primary;
        m.workspace = qBound(0, e.workspace, m_workspaceCount - 1);
        m.floating = e.floating;
        m.pinned = e.pinned;
        m.wasTopmost = e.wasTopmost;
        m.placementShowCmd = e.placementShowCmd;
        m.placementNormal = e.placementNormal;
        // The next launch identity-checks this window by these; dropping them
        // would write an entry it would refuse to recognise, and a window we
        // are still hiding with it.
        m.pid = e.pid;
        m.created = e.created;
        m.windowClass = e.windowClass;
        m.original = e.placementNormal; // the best "geometry at adoption" left
        // Read live, never taken from the file: the file predates the batch
        // it describes, so only the window knows whether it completed.
        // Whether a hidden one is OURS is the separate hiddenByUs() question,
        // and one that is not is dropped rather than adopted - adopting it is
        // how we would end up uncloaking a window we never cloaked.
        if (hider::isCloaked(hwnd)) {
            if (!hiddenByUs(e, hider::Method::Cloak))
                continue;
            m.hidden = hider::Method::Cloak;
        } else if (IsIconic(hwnd)) {
            if (!hiddenByUs(e, hider::Method::Minimize))
                continue;
            m.hidden = hider::Method::Minimize;
        } else {
            m.hidden = hider::Method::None;
        }
        windows.insert(e.hwnd, m);
    }

    QHash<QString, layout::Tree *> trees;
    QSet<QString> keys;
    bool broken = false;
    for (const tilingstate::WorkspaceTree &ws : snapshot.workspaces) {
        const QString device = areas.contains(ws.monitor) ? ws.monitor : primary;
        const QString key = device + QLatin1Char('|')
                           + QString::number(qBound(0, ws.index, m_workspaceCount - 1));
        if (keys.contains(key)) {
            // Two monitors gone between sessions and both fell back to the
            // primary, most likely. Dropping the later tree costs its
            // arrangement, not its windows: the overflow pass below hands its
            // members to the first rescan to re-insert.
            qInfo().noquote() << "Tiler: recovery: dropping a second persisted tree for"
                              << key;
            continue;
        }
        auto *tree = new layout::Tree;
        const bool ok = tree->fromJson(ws.tree, [&windows](quintptr id) {
            const auto it = windows.constFind(id);
            return it != windows.constEnd() && isTiledEntry(it.value());
        });
        if (!ok) {
            delete tree;
            broken = true;
            break;
        }
        keys.insert(key);
        if (tree->isEmpty())
            delete tree; // every id it named was rejected; nothing to keep
        else
            trees.insert(key, tree);
    }

    if (broken) {
        qWarning() << "Tiler: recovery abandoned - tiling-state.json's workspace trees"
                      " are structurally inconsistent";
        qDeleteAll(trees);
        abandonRecovery(valid, snapshot.active);
        return;
    }

    m_windows = windows;
    m_trees = trees;
    // Live monitors first: several dead ones falling back to the primary must
    // not overwrite its own recorded index, nor each other's.
    for (auto it = snapshot.active.constBegin(); it != snapshot.active.constEnd(); ++it) {
        if (areas.contains(it.key()))
            m_active.insert(it.key(), qBound(0, it.value(), m_workspaceCount - 1));
    }
    for (auto it = snapshot.active.constBegin(); it != snapshot.active.constEnd(); ++it) {
        if (!areas.contains(it.key()) && !m_active.contains(primary))
            m_active.insert(primary, qBound(0, it.value(), m_workspaceCount - 1));
    }

    // `overflow` is not persisted, so a window that was overflow at save time
    // - or whose tree was dropped above - comes back tiled with no tree to
    // place it, and nothing in rescan() checks tree membership. Marking it
    // hands it to the retry loop that exists for exactly that state.
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (!isTiledEntry(it.value()))
            continue;
        // Any tree, not just the one keyOf() names: marking one another tree
        // still holds would give the retry loop a second leaf for the same
        // id, and releaseWindow() can only ever unhook one of them.
        bool placed = false;
        for (auto t = m_trees.constBegin(); t != m_trees.constEnd() && !placed; ++t)
            placed = t.value()->contains(it.key());
        if (!placed)
            it->overflow = true;
    }

    // Only ever shows or hides an id that came from the file: m_windows holds
    // nothing else yet.
    reconcileHidden();

    m_pendingRecovered = m_windows.keys();
    qInfo() << "Tiler: recovered" << m_pendingRecovered.size()
            << "window(s) from a previous session, pending until enabled";
    m_pendingTimer.start();
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
    if (!tree)
        return;
    if (const quintptr target = tree->neighbour(id, dir)) {
        // Allowed because these commands come from a Hotkey: WM_HOTKEY grants
        // the process foreground rights for the duration.
        SetForegroundWindow(toHwnd(target));
    }
}

void TilingApi::moveDirection(const QString &direction)
{
    layout::Direction dir;
    if (!parseDirection(direction, &dir))
        return;
    quintptr id = 0;
    layout::Tree *tree = treeForFocused(&id);
    if (!tree)
        return;
    const quintptr target = tree->neighbour(id, dir);
    // Focus needs no help: swapping exchanges the leaves' occupants, so the
    // window the user was in is still the foreground one.
    if (target && tree->swap(id, target))
        retile();
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

    if (it->floating || it->overflow) {
        // Rejoin the tiled layout.
        it->floating = false;
        it->overflow = false;
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
    } else {
        // Leave the layout, restoring the size it had when adopted.
        const QString key = keyOf(it.value());
        if (layout::Tree *tree = m_trees.value(key))
            tree->remove(id);
        it->floating = true;
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
