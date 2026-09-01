#pragma once

#include <QHash>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUuid>

#include <functional>

#include "layouttree.h"

class QScreen;

// The `Tiler` QML singleton: a dwindle tiling window manager over the
// desktop's own windows. One layout::Tree per (monitor, virtual desktop);
// `layouttree.*` holds the geometry and knows nothing of Windows, this file
// holds everything native. HWNDs travel as quintptr, which is also what the
// tree keys its leaves by.
//
// Discovery is event-driven off WinEvent hooks plus a slow safety sweep. The
// sweep is not belt-and-braces: `isFocusableAppWindow` requires a title, and
// a great many apps show their window before setting one, so the SHOW event
// arrives while the window still looks untileable. The sweep is what adopts
// those a moment later, and it also re-asserts geometry on windows that
// drifted (an app moving itself, a Snap gesture, a maximize).
//
// Two rules keep it from fighting the user or itself:
//   - `m_applying` brackets every SetWindowPos we make, so our own moves are
//     never mistaken for the user's.
//   - a window that ignores its assigned rect three sweeps running is
//     floated and left alone. Fixed-size dialogs never enter in the first
//     place (no WS_THICKFRAME); this catches the ones that have a sizing
//     border and still refuse, which would otherwise flicker every sweep.
class TilingApi : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(int gap READ gap WRITE setGap NOTIFY gapsChanged)
    Q_PROPERTY(int outerGap READ outerGap WRITE setOuterGap NOTIFY gapsChanged)
    Q_PROPERTY(int minWidth READ minWidth WRITE setMinWidth NOTIFY minSizeChanged)
    Q_PROPERTY(int minHeight READ minHeight WRITE setMinHeight NOTIFY minSizeChanged)
    Q_PROPERTY(int resizeStep READ resizeStep WRITE setResizeStep NOTIFY resizeStepChanged)
    Q_PROPERTY(QStringList floatProcesses READ floatProcesses WRITE setFloatProcesses
                   NOTIFY floatProcessesChanged)
    Q_PROPERTY(int managedCount READ managedCount NOTIFY layoutChanged)
    // Logs every adoption and every rect applied, with the metrics behind
    // them. Off by default: it is one line per window per re-tile.
    Q_PROPERTY(bool debug READ debug WRITE setDebug NOTIFY debugChanged)
public:
    explicit TilingApi(QObject *parent = nullptr);
    ~TilingApi() override;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);

    // Logical pixels, like the rest of the QML API; scaled per monitor on
    // the way out, since the Win32 side is physical throughout.
    int gap() const { return m_gap; }
    void setGap(int gap);
    int outerGap() const { return m_outerGap; }
    void setOuterGap(int outerGap);

    // The smallest tile a split may create, logical px. A window that cannot
    // be placed without breaking these is left floating and reclaimed later,
    // once closing something else makes room.
    int minWidth() const { return m_minWidth; }
    void setMinWidth(int minWidth);
    int minHeight() const { return m_minHeight; }
    void setMinHeight(int minHeight);

    // How far one resize command moves a divider, logical px.
    int resizeStep() const { return m_resizeStep; }
    void setResizeStep(int step);

    // Executable names ("spotify.exe"), case-insensitive: never tiled.
    QStringList floatProcesses() const { return m_floatProcesses; }
    void setFloatProcesses(const QStringList &names);

    // Windows on screen right now, not the total under management: one
    // parked on another desktop stays in m_managed (a cloaked survivor - see
    // isCloakedAlive in the .cpp) so the layout survives the round trip, but
    // the bar's tiling indicator means "how many tiles am I looking at".
    int managedCount() const;

    bool debug() const { return m_debug; }
    void setDebug(bool debug);

    // Virtual desktops are a separate unit; main.cpp injects the accessor so
    // this one keeps compiling on its own. The callback returns the window's
    // own desktop as a GUID, or a null QUuid when that is unknown - pinned
    // to all desktops, or a transient query failure; the two cannot be told
    // apart from this alone, so callers decide per call site what "unknown"
    // should mean. Leaving the provider unset entirely - no
    // VirtualDesktopAccessor.dll, or an older one missing GetWindowDesktopId
    // - falls back to one shared tree per monitor.
    void setDesktopGuidProvider(std::function<QUuid(void *)> provider);

    // "left" | "right" | "up" | "down", case-insensitive.
    Q_INVOKABLE void focusDirection(const QString &direction);
    Q_INVOKABLE void moveDirection(const QString &direction);
    // "wider" | "narrower" | "taller" | "shorter", case-insensitive: moves
    // the divider nearest the focused window by `resizeStep`.
    Q_INVOKABLE void resize(const QString &how);
    // Takes the focused window out of the layout, restoring the size it had
    // when adopted, or puts it back in.
    Q_INVOKABLE void toggleFloating();
    // Flips the split that placed the focused window: the one-key fix for a
    // dwindle that divided the wrong way.
    Q_INVOKABLE void toggleSplit();
    // Forgets every resize on the focused window's monitor.
    Q_INVOKABLE void equalize();
    Q_INVOKABLE void retile();

public slots:
    // Also the virtual-desktop-changed entry point; main.cpp connects it.
    void rescan();

signals:
    void enabledChanged();
    void gapsChanged();
    void floatProcessesChanged();
    void minSizeChanged();
    void resizeStepChanged();
    void debugChanged();
    void layoutChanged();

public:
    // Entry points for the .cpp's WinEvent callback, routed through a
    // file-static instance pointer (main.cpp constructs exactly one).
    // WINEVENT_OUTOFCONTEXT hooks are pumped by our own message loop, so
    // these run on the GUI thread and may touch state directly. Plain
    // methods, not slots: nothing but that callback should drive them.
    void onWindowSetChanged();
    void onWindowShown(void *hwnd);
    void onForegroundChanged(void *hwnd);
    void onMoveSizeStart(void *hwnd);
    void onMoveSizeEnd(void *hwnd);

private:
    struct WindowState {
        QRect assigned;  // last rect we asked for, physical pixels
        QRect original;  // geometry when adopted, restored on release
        int rejections = 0;
    };

    // "\\.\DISPLAY1|{guid}" - monitor device name and the window's own
    // virtual desktop (or "0" as a constant stand-in when the GUID provider
    // is unset). Empty when the window cannot be placed right now: its
    // monitor is gone, or its desktop is unknown (see setDesktopGuidProvider).
    QString treeKey(void *hwnd) const;
    layout::Tree *treeFor(const QString &key);
    void pruneEmptyTrees();
    // True if `id`, currently seated in the tree keyed `key`, still belongs
    // on that tree's desktop. The GUID-vs-cloak decision lives in the .cpp,
    // next to isCloakedAlive.
    bool windowStillOnTreeDesktop(quintptr id, const QString &key) const;

    // Work area in physical pixels - rcWork, so the AppBar registration in
    // panelwindow.cpp has already carved our own panels out of it - plus the
    // gaps scaled to that monitor. False if the monitor is gone.
    bool metricsForKey(const QString &key, layout::Metrics *out) const;

    bool isTileable(void *hwnd);
    QString processNameFor(void *hwnd);
    QString describe(quintptr id); // "chrome.exe (New Tab)", for the log

    void applyPlacements(const QVector<layout::Placement> &places);
    void releaseWindow(quintptr id, bool restoreGeometry);
    void scheduleScan();
    void sweep(); // safety timer: adopt latecomers, re-assert drifted rects
    // Debounced screenAdded/Removed + per-screen geometry/availableGeometry
    // watcher: a resolution, DPI or work-area change must recompute the
    // trees' metrics promptly, or the sweep's drift-fixer spends its time
    // re-applying rects derived from the stale area instead. See the ctor.
    void watchScreen(QScreen *screen);
    void scheduleDisplayRecheck();
    // Focused window plus the tree holding it, with that tree's boxes
    // refreshed - every navigation command needs exactly this.
    // `metrics` and `scale`, when asked for, describe the monitor that tree
    // is on; a null return then also means they were unavailable, since a
    // command needing them cannot run without.
    layout::Tree *treeForFocused(quintptr *id, layout::Metrics *metrics = nullptr,
                                 qreal *scale = nullptr);

    bool m_enabled = false;
    int m_gap = 16;
    int m_outerGap = 16;
    int m_minWidth = 360;
    int m_minHeight = 220;
    int m_resizeStep = 40;
    QStringList m_floatProcesses;

    QHash<QString, layout::Tree *> m_trees;
    QHash<quintptr, WindowState> m_managed; // tiled windows only
    // Floated on purpose - by the user, or because the app kept refusing its
    // rect. Sticky: never reclaimed, or the refusers would flicker forever.
    QSet<quintptr> m_floating;
    // Floated only because there was no room left. Retried on every rescan,
    // so closing a window pulls these back into the layout by itself.
    QSet<quintptr> m_overflow;
    QHash<quintptr, QString> m_processCache;
    // The managed window that last had focus. New windows are inserted next
    // to it, which is what makes dwindle grow where the user is looking -
    // GetForegroundWindow() at adoption time is already the new window.
    quintptr m_lastFocused = 0;
    quintptr m_dragging = 0; // between MOVESIZESTART and MOVESIZEEND
    // managedCount() as of the last layoutChanged emission. A pure desktop
    // switch changes which managed windows are cloaked without touching tree
    // membership, so rescan() has to diff the visible count against what the
    // bar last saw - not against a value read earlier in the same pass,
    // which could never differ. -1 so the first rescan always emits.
    int m_lastVisibleCount = -1;
    bool m_applying = false; // our own SetWindowPos in flight
    bool m_debug = false;

    std::function<QUuid(void *)> m_desktopGuid;

    QTimer m_scanTimer; // coalesces a burst of window events
    QTimer m_settleTimer; // fast re-check for a window that is not ready yet
    int m_settleTries = 0;
    QTimer m_sweepTimer;
    // Debounced (not throttled - restarted on every event) so a monitor
    // reconfiguration's burst of screen signals coalesces into one
    // rescan+retile instead of several.
    QTimer m_displayTimer;

    void *m_hookObject = nullptr;   // DESTROY..HIDE
    void *m_hookCloak = nullptr;    // CLOAKED..UNCLOAKED
    void *m_hookMinimize = nullptr; // MINIMIZESTART..MINIMIZEEND
    void *m_hookMoveSize = nullptr; // MOVESIZESTART..MOVESIZEEND
    void *m_hookForeground = nullptr;
};
