#include "tilingapi.h"

#include "windowfocus.h"

#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>

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
constexpr int kProcessCacheCap = 256;

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
        // DESTROY/HIDE, MINIMIZESTART/END, and CLOAKED/UNCLOAKED - the last
        // being a window parked on another virtual desktop, or a UWP host
        // going dormant. All just mean "look again", and none of them is
        // urgent: nothing is sitting in the wrong place while we wait.
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
    if (!(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME))
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid != GetCurrentProcessId() && hwnd != GetShellWindow();
}

// Alive but composited away: a window parked on another virtual desktop, or
// one the switch animation has not uncloaked yet. isFocusableAppWindow
// rejects these, so a rescan sees them as "gone". With a real per-window
// desktop GUID available, windowStillOnTreeDesktop() answers the "did it
// really leave" question directly instead - this is now only the fallback
// for the two cases that cannot: no GetWindowDesktopId at all (no DLL, or an
// older one), and a single window's transient GUID query failure, where
// guessing "evicted" would drop a window's tree seat over a COM hiccup.
// Minimized is excluded: minimizing removes a window from the layout whether
// or not its desktop is the active one, and a minimized window's desktop
// GUID does not change to say so.
bool isCloakedAlive(HWND hwnd)
{
    if (!IsWindow(hwnd) || IsIconic(hwnd))
        return false;
    int cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked != 0;
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
        const int before = m_managed.size();
        rescan();
        // Stops the moment the window lands, so the common case costs one or
        // two ticks rather than the whole budget.
        if (m_managed.size() == before && ++m_settleTries < kSettleTries)
            m_settleTimer.start();
    });

    m_sweepTimer.setInterval(kSweepMs);
    connect(&m_sweepTimer, &QTimer::timeout, this, &TilingApi::sweep);

    m_displayTimer.setSingleShot(true);
    m_displayTimer.setInterval(kDisplayDebounceMs);
    connect(&m_displayTimer, &QTimer::timeout, this, [this] {
        rescan(); // a monitor may have gone, or a window may now be on a
                   // different one - both change tree membership
        retile(); // unconditional: a resolution/DPI/work-area change alone
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
}

TilingApi::~TilingApi()
{
    for (void *hook : { m_hookObject, m_hookCloak, m_hookMinimize,
                        m_hookMoveSize, m_hookForeground }) {
        if (hook)
            UnhookWinEvent(static_cast<HWINEVENTHOOK>(hook));
    }
    // Windows are deliberately left where they are: a mass re-shuffle as the
    // host exits is more startling than a tidy desktop is useful.
    qDeleteAll(m_trees);
    g_instance = nullptr;
}

void TilingApi::setDesktopGuidProvider(std::function<QUuid(void *)> provider)
{
    m_desktopGuid = std::move(provider);
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
        m_sweepTimer.start();
        rescan();
    } else {
        m_sweepTimer.stop();
        m_scanTimer.stop();
        m_settleTimer.stop();
        m_displayTimer.stop();
        const QList<quintptr> ids = m_managed.keys();
        for (quintptr id : ids)
            releaseWindow(id, true);
        qDeleteAll(m_trees);
        m_trees.clear();
        m_managed.clear();
        m_overflow.clear();
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

QString TilingApi::describe(quintptr id)
{
    if (!id)
        return QStringLiteral("(none)");
    QString title;
    const int length = GetWindowTextLengthW(toHwnd(id));
    if (length > 0) {
        std::wstring buffer(length + 1, L'\0');
        const int copied = GetWindowTextW(toHwnd(id), buffer.data(), length + 1);
        title = QString::fromWCharArray(buffer.data(), copied).left(40);
    }
    return QStringLiteral("%1 (%2)").arg(processNameFor(toHwnd(id)), title);
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

void TilingApi::setFloatProcesses(const QStringList &names)
{
    if (m_floatProcesses == names)
        return;
    m_floatProcesses = names;
    emit floatProcessesChanged();
    rescan(); // drops anything the new rules exclude
}

int TilingApi::managedCount() const
{
    // m_managed also holds cloaked survivors parked on another desktop (see
    // isCloakedAlive) so their tree seats outlive a switch; the bar's
    // indicator means tiles on screen right now, so those do not count.
    int count = 0;
    for (auto it = m_managed.constBegin(); it != m_managed.constEnd(); ++it) {
        if (!isCloakedAlive(toHwnd(it.key())))
            ++count;
    }
    return count;
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
    if (m_managed.contains(id) || m_floating.contains(id))
        return;

    // One cheap test on this window before committing to a full rescan: SHOW
    // fires for every menu, tooltip and dialog on the system, and all of
    // those fail here for the price of a couple of style reads.
    if (isTileable(hwnd)) {
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
    const quintptr id = toId(hwnd);
    if (m_managed.contains(id))
        m_lastFocused = id;
}

void TilingApi::onMoveSizeStart(void *hwnd)
{
    m_dragging = toId(hwnd);
}

QString TilingApi::processNameFor(void *hwnd)
{
    const quintptr id = toId(hwnd);
    if (const auto it = m_processCache.constFind(id); it != m_processCache.constEnd())
        return it.value();

    QString name;
    DWORD pid = 0;
    GetWindowThreadProcessId(static_cast<HWND>(hwnd), &pid);
    // Denied for elevated/protected processes even with the limited right,
    // so an empty name is expected rather than a failure.
    if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buffer[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, buffer, &size))
            name = QFileInfo(QString::fromWCharArray(buffer, int(size))).fileName();
        CloseHandle(proc);
    }

    if (m_processCache.size() >= kProcessCacheCap)
        m_processCache.clear(); // simplest bound, as in foregroundwindow.cpp
    m_processCache.insert(id, name);
    return name;
}

bool TilingApi::isTileable(void *hwnd)
{
    // The shared rule already rejects our own panels, the shell, owned
    // popups, tool windows and anything cloaked - which is also how windows
    // parked on another virtual desktop stay out of this desktop's tree.
    if (!windowfocus::isFocusableAppWindow(hwnd))
        return false;
    if (m_floating.contains(toId(hwnd)))
        return false;
    // No sizing border means the app fixed its own size: a settings dialog,
    // an installer, a splash. Those stay separate free-floating windows.
    if (!(GetWindowLongPtrW(static_cast<HWND>(hwnd), GWL_STYLE) & WS_THICKFRAME))
        return false;
    if (!m_floatProcesses.isEmpty()) {
        const QString process = processNameFor(hwnd);
        for (const QString &name : m_floatProcesses) {
            if (!process.isEmpty() && name.compare(process, Qt::CaseInsensitive) == 0)
                return false;
        }
    }
    return true;
}

QString TilingApi::treeKey(void *hwnd) const
{
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(hwnd), MONITOR_DEFAULTTONEAREST), &mi))
        return QString();

    // The suffix is the window's OWN desktop, not "whichever is current":
    // keying off the current index misattributes every tree once a desktop
    // in the middle is removed and the later ones shift down. Without
    // GetWindowDesktopId (no DLL, or an older one), there is no way to tell
    // desktops apart per window, so fall back to one shared tree per
    // monitor - eviction then leans entirely on isCloakedAlive, same as
    // before this existed.
    if (!m_desktopGuid)
        return QString::fromWCharArray(mi.szDevice) + QLatin1String("|0");

    const QUuid desktop = m_desktopGuid(hwnd);
    if (desktop.isNull())
        // Pinned to all desktops, or the query failed - either way we do not
        // know where this window belongs, so leave it exactly where a
        // non-tileable window would land: out of `wanted` entirely, and
        // never adopted.
        return QString();

    return QString::fromWCharArray(mi.szDevice) + QLatin1Char('|')
         + desktop.toString(QUuid::WithoutBraces);
}

// A window in the tree keyed `key` that no longer showed up under that key
// in this rescan's `wanted` map either really left (closed, minimized,
// floated, moved to another monitor or desktop) or is merely cloaked while
// still parked here (a Task View drag animation, a UWP host gone dormant) -
// this is the call that tells those apart.
bool TilingApi::windowStillOnTreeDesktop(quintptr id, const QString &key) const
{
    HWND hwnd = toHwnd(id);
    if (!IsWindow(hwnd) || IsIconic(hwnd))
        return false; // gone, or minimized - minimizing evicts regardless of desktop

    if (!m_desktopGuid)
        return isCloakedAlive(hwnd); // no per-window GUID: one shared tree per monitor

    const QUuid guid = m_desktopGuid(hwnd);
    if (guid.isNull())
        return isCloakedAlive(hwnd); // pinned, or the query failed - do not evict on a guess

    return key.section(QLatin1Char('|'), 1, 1) == guid.toString(QUuid::WithoutBraces);
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

void TilingApi::rescan()
{
    if (!m_enabled)
        return;

    QVector<HWND> windows;
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&windows));

    // Ordered, not a QSet: when several windows are adopted in one pass -
    // at startup, or when tiling is enabled - the order decides the shape
    // of the layout. Sorted into reading order below, not left in the
    // EnumWindows (Z-) order it arrives in: Z-order is focus history, so
    // disabling and re-enabling with a different window focused rebuilt the
    // layout the other way round, and A | B came back as B | A. Screen
    // position is what the user sees, and it is what a dwindle arrangement
    // reproduces itself from - the same windows in the same tiles.
    QHash<QString, QVector<quintptr>> wanted;
    // Passed isTileable but treeKey() came back empty this pass: a window
    // pinned to all desktops, or a transient GetWindowDesktopId failure.
    // Neither failed tileability, only "which desktop" - so the eviction
    // loop below must not read a member of this set as "it left", the way it
    // would a window that stopped being tileable outright.
    QSet<quintptr> keyUnknown;
    for (HWND hwnd : windows) {
        if (!isTileable(hwnd))
            continue;
        const QString key = treeKey(hwnd);
        if (key.isEmpty())
            keyUnknown.insert(toId(hwnd));
        else
            wanted[key].append(toId(hwnd));
    }

    for (auto it = wanted.begin(); it != wanted.end(); ++it)
        sortIntoReadingOrder(&it.value());

    bool changed = false;

    // Drop what left: closed, minimized, floated, or moved to another
    // monitor or desktop. windowStillOnTreeDesktop() is what tells that
    // apart from a window merely cloaked while still parked here - a Task
    // View drag animation, a UWP host gone dormant, or a switch away from
    // this desktop's own windows. No geometry restore here - a window that
    // only changed monitors is re-adopted below, and restoring it first
    // would just be a visible flash before the new tile lands.
    for (auto it = m_trees.constBegin(); it != m_trees.constEnd(); ++it) {
        const QVector<quintptr> keep = wanted.value(it.key());
        for (quintptr id : it.value()->ids()) {
            if (keep.contains(id) || keyUnknown.contains(id))
                continue;
            if (windowStillOnTreeDesktop(id, it.key()))
                continue;
            it.value()->remove(id);
            m_managed.remove(id);
            if (m_lastFocused == id)
                m_lastFocused = 0;
            changed = true;
        }
    }

    // Adopt what arrived, next to the last focused window so dwindle grows
    // where the user is looking.
    for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it) {
        // insert() needs these to pick each split's axis from the target's
        // real box; a monitor that vanished mid-scan has nothing to lay out.
        layout::Metrics metrics;
        if (!metricsForKey(it.key(), &metrics))
            continue;

        layout::Tree *tree = treeFor(it.key());
        for (quintptr id : it.value()) {
            if (tree->contains(id))
                continue;

            const quintptr nearId = tree->contains(m_lastFocused) ? m_lastFocused : 0;
            if (tree->insert(id, nearId, metrics) == layout::Insert::TooSmall) {
                // The tree is untouched, so leave the window exactly where its
                // application put it. Every rescan retries, which is what
                // brings it back once closing something else frees room.
                if (!m_overflow.contains(id)) {
                    qInfo() << "Tiler: leaving" << processNameFor(toHwnd(id))
                            << "floating - no room for another tile";
                    m_overflow.insert(id);
                }
                continue;
            }

            if (m_debug) {
                qInfo().noquote() << QStringLiteral("Tiler: adopt %1  splitting %2")
                                         .arg(describe(id), describe(nearId));
            }

            m_overflow.remove(id);
            WindowState state;
            RECT r;
            if (GetWindowRect(toHwnd(id), &r))
                state.original = toRect(r); // captured before the first tile
            m_managed.insert(id, state);
            changed = true;
        }
    }

    // Overflow windows that have since closed would otherwise accumulate for
    // the life of the session. Keyed on window death, not on membership in
    // `wanted`: one parked on another desktop is merely cloaked, and
    // forgetting it would re-log the adoption refusal on every switch back.
    for (auto it = m_overflow.begin(); it != m_overflow.end();) {
        if (IsWindow(toHwnd(*it)))
            ++it;
        else
            it = m_overflow.erase(it);
    }

    // A window that opened and took the foreground never produced a usable
    // FOREGROUND event: it was not managed yet when the event arrived, so
    // onForegroundChanged() ignored it. Reconcile now that it is adopted,
    // or the next window to open splits whatever was focused two windows
    // ago rather than the one actually on screen - which is what makes a
    // dwindle grow in a direction the user did not ask for.
    if (HWND fg = GetForegroundWindow()) {
        if (m_managed.contains(toId(fg)))
            m_lastFocused = toId(fg);
    }

    pruneEmptyTrees();
    if (changed) {
        retile(); // also emits layoutChanged and refreshes m_lastVisibleCount
    } else if (const int visible = managedCount(); visible != m_lastVisibleCount) {
        // A pure desktop switch: nothing adopted or evicted, but which of
        // the still-managed windows are on screen changed - which is what
        // the bar's indicator means. The cloak flips happen between rescans,
        // so the count must be compared against the last one *emitted* - two
        // reads inside one rescan always agree, since nothing here changes a
        // window's cloak state. No geometry changed either, so retile()
        // would be wasted work; the count itself is the whole story.
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
    // practice - nothing else notices those.
    QVector<layout::Placement> fixes;
    QVector<quintptr> giveUp;
    for (auto it = m_managed.begin(); it != m_managed.end(); ++it) {
        if (!it->assigned.isValid())
            continue;
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
        // on its own geometry. Float it rather than flicker at it forever.
        qInfo() << "Tiler: floating" << processNameFor(toHwnd(id))
                << "- it will not accept a tiled size";
        m_floating.insert(id);
        releaseWindow(id, false);
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
    if (places.isEmpty())
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
    for (const layout::Placement &p : places) {
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
    HDWP batch = BeginDeferWindowPos(int(places.size()));
    for (const layout::Placement &p : places) {
        HWND hwnd = toHwnd(p.id);
        if (batch)
            batch = DeferWindowPos(batch, hwnd, nullptr, p.rect.x(), p.rect.y(),
                                    p.rect.width(), p.rect.height(), kPlaceFlags);
        // A failed DeferWindowPos discards the whole batch, so from here on
        // the rest go one at a time; the sweep re-places whatever was lost.
        if (!batch)
            SetWindowPos(hwnd, nullptr, p.rect.x(), p.rect.y(),
                          p.rect.width(), p.rect.height(), kPlaceFlags);
        if (auto it = m_managed.find(p.id); it != m_managed.end())
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
    emit layoutChanged();
}

void TilingApi::releaseWindow(quintptr id, bool restoreGeometry)
{
    const auto it = m_managed.constFind(id);
    if (it == m_managed.constEnd())
        return;
    const QRect original = it->original;
    m_managed.erase(m_managed.find(id));
    if (m_lastFocused == id)
        m_lastFocused = 0;

    for (auto t = m_trees.constBegin(); t != m_trees.constEnd(); ++t) {
        if (t.value()->remove(id))
            break;
    }

    if (restoreGeometry && original.isValid()) {
        m_applying = true;
        SetWindowPos(toHwnd(id), nullptr, original.x(), original.y(),
                      original.width(), original.height(), kPlaceFlags);
        m_applying = false;
    }
}

void TilingApi::onMoveSizeEnd(void *hwnd)
{
    m_dragging = 0;
    if (!m_enabled)
        return;

    const quintptr id = toId(hwnd);
    const auto state = m_managed.constFind(id);
    if (state == m_managed.constEnd())
        return; // floating, or never ours

    const QString key = treeKey(hwnd);
    layout::Tree *tree = m_trees.value(key);
    if (!tree || !tree->contains(id)) {
        rescan(); // dragged onto another monitor: re-home it
        return;
    }

    RECT r;
    if (!GetWindowRect(static_cast<HWND>(hwnd), &r))
        return;
    const QRect actual = toRect(r);
    const QRect assigned = state->assigned;

    layout::Metrics metrics;
    if (!metricsForKey(key, &metrics))
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
            for (auto m = m_managed.constBegin(); m != m_managed.constEnd(); ++m) {
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
    const QString key = treeKey(fg);
    layout::Tree *tree = m_trees.value(key);
    if (!tree || !tree->contains(*id))
        return nullptr;

    // neighbour() reads the boxes left by the last arrange(); refresh them
    // so a command is never answered from a stale partition.
    layout::Metrics own;
    if (!metricsForKey(key, &own))
        return metrics ? nullptr : tree;
    tree->arrange(own);
    if (metrics)
        *metrics = own;
    if (scale)
        *scale = scaleForDevice(key.section(QLatin1Char('|'), 0, 0));
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
    if (m_floating.remove(id)) {
        rescan(); // adopts it back into the layout
    } else if (m_overflow.contains(id)) {
        // Already floating for want of room. Make that the user's decision so
        // it stops being reclaimed the moment something else closes.
        m_overflow.remove(id);
        m_floating.insert(id);
    } else {
        m_floating.insert(id);
        releaseWindow(id, true);
        pruneEmptyTrees();
        retile();
    }
    emit layoutChanged();
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
