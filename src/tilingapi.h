#pragma once

#include <QHash>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "layouttree.h"
#include "tilingstate.h"
#include "windowhider.h"

class QScreen;

// The `Tiler` QML singleton: a dwindle tiling window manager with its own
// fixed-count workspaces per monitor, on top of - not instead of - Windows'
// native virtual desktops. Switching a workspace is not a desktop switch: it
// cloaks (or minimizes, as a fallback - see windowhider.h) every member of
// the one being left and uncloaks the one being shown. `layouttree.*` holds
// the geometry and knows nothing of Windows or workspaces; this unit holds
// everything native. HWNDs travel as quintptr, as a tree's leaf keys do; one
// tree per "device|workspace", holding its TILED members only.
//
// Only a window on the current native desktop is ever adopted, checked once
// at discovery, so the two systems never collide: we never touch a window the
// OS cloaked, and never uncloak one we did not cloak.
//
// Discovery is event-driven off WinEvent hooks plus a slow safety sweep. The
// sweep is not belt-and-braces: `isFocusableAppWindow` requires a title, and
// a great many apps show their window before setting one, so the SHOW event
// arrives while the window still looks untileable. The sweep is what adopts
// those a moment later, and it also re-asserts geometry on windows that
// drifted (an app moving itself, a Snap gesture, a maximize).
//
// Two invariants keep it from fighting the user or itself:
//   - `m_applying` brackets every SetWindowPos AND every cloak/minimize
//     batch: our own hides fire the same CLOAKED/MINIMIZE events the hook
//     discovers windows by, so without it a workspace switch would look like
//     a wave of windows leaving.
//   - a window that ignores its assigned rect three sweeps running is
//     floated and left alone. Fixed-size dialogs (no WS_THICKFRAME) are
//     adopted floating and never enter the layout at all; this catches the
//     ones with a sizing border that still refuse, which would otherwise
//     flicker every sweep.
//
// Member definitions are split by concern: tilingapi.cpp (lifecycle, most
// properties, discovery, layout, the window commands),
// tilingapi_workspaces.cpp (workspace properties and commands, pinning,
// hide/show) and tilingapi_recovery.cpp (the state file, crash recovery).
// tilingapi_p.h holds the helpers more than one of them needs.
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
    // 1..20, default 9. Fixed per monitor - workspaces are never created or
    // closed, only switched to.
    Q_PROPERTY(int workspaceCount READ workspaceCount WRITE setWorkspaceCount
                   NOTIFY workspacesChanged)
    // "cloak" | "minimize". Only how NEW hides pick a method - a window
    // already hidden keeps the method recorded for it (Managed::hidden).
    Q_PROPERTY(QString hideMethod READ hideMethod WRITE setHideMethod NOTIFY hideMethodChanged)
    // When false the effective method is minimize whatever `hideMethod` says.
    // Not CONSTANT: acquisition is lazy and can flip either way when
    // explorer.exe restarts; rescan() is what notices and notifies.
    Q_PROPERTY(bool cloakAvailable READ cloakAvailable NOTIFY cloakAvailableChanged)
    Q_PROPERTY(bool pinnedTopmost READ pinnedTopmost WRITE setPinnedTopmost
                   NOTIFY pinnedTopmostChanged)
    // Focused monitor, 0-based.
    Q_PROPERTY(int currentWorkspace READ currentWorkspace NOTIFY workspacesChanged)
    // Focused monitor: [{index, active, windows}], `windows` the member
    // count on that (device, workspace) - tiled, floating and pinned alike,
    // hidden or not.
    Q_PROPERTY(QVariantList workspaces READ workspaces NOTIFY workspacesChanged)
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

    // Tiled windows on screen right now, not the total under management: a
    // member of a hidden workspace keeps its tree seat, but the bar's
    // indicator means "how many tiles am I looking at".
    int managedCount() const;

    bool debug() const { return m_debug; }
    void setDebug(bool debug);

    int workspaceCount() const { return m_workspaceCount; }
    void setWorkspaceCount(int count);

    QString hideMethod() const;
    void setHideMethod(const QString &method);

    bool cloakAvailable() const { return hider::cloakAvailable(); }

    bool pinnedTopmost() const { return m_pinnedTopmost; }
    void setPinnedTopmost(bool pinned);
    int currentWorkspace() const;
    QVariantList workspaces() const;

    // "left" | "right" | "up" | "down", case-insensitive.
    Q_INVOKABLE void focusDirection(const QString &direction);
    Q_INVOKABLE void moveDirection(const QString &direction);
    // "wider" | "narrower" | "taller" | "shorter", case-insensitive: moves
    // the divider nearest the focused window by `resizeStep`.
    Q_INVOKABLE void resize(const QString &how);
    // Takes the focused window out of the layout, restoring the size it had
    // when adopted, or puts it back in. On a window floating only for lack
    // of room, makes that the user's choice instead (no longer reclaimed the
    // moment room frees up). A float survives a minimize. No-op on a pinned
    // window - see togglePinned().
    Q_INVOKABLE void toggleFloating();
    // Flips the split that placed the focused window: the one-key fix for a
    // dwindle that divided the wrong way.
    Q_INVOKABLE void toggleSplit();
    // Forgets every resize on the focused window's monitor.
    Q_INVOKABLE void equalize();
    Q_INVOKABLE void retile();
    // Switches the focused monitor (see focusedDevice()) to `index`.
    Q_INVOKABLE void switchToWorkspace(int index);
    // Moves the foreground window to `index` on its own monitor; `follow`
    // also switches that monitor to it.
    Q_INVOKABLE void moveToWorkspace(int index, bool follow = false);
    // Moves the foreground window to the lowest-numbered empty workspace on
    // its own monitor and follows it there. Its own workspace counts as
    // occupied, so a window alone on 1 lands on 2. A warning no-op when every
    // workspace on that monitor is in use.
    Q_INVOKABLE void moveToEmptyWorkspace();
    // Pins/unpins the foreground window: pinned means floating, never
    // hidden, and always showing on its monitor's active workspace.
    Q_INVOKABLE void togglePinned();

public slots:
    void rescan();

signals:
    void enabledChanged();
    void gapsChanged();
    void floatProcessesChanged();
    void minSizeChanged();
    void resizeStepChanged();
    void debugChanged();
    void layoutChanged();
    void workspacesChanged();
    void hideMethodChanged();
    void cloakAvailableChanged();
    void pinnedTopmostChanged();

public:
    // Entry points for tilingapi.cpp's WinEvent callback, routed through a
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
    // One entry per adopted window - every real app window on the current
    // native desktop is a member of some workspace, tiled or floating.
    struct Managed {
        QString device;          // monitor szDevice
        int workspace = 0;       // 0-based, per monitor
        bool floating = false;   // user toggle, no sizing border, refused its rect
        bool overflow = false;   // floated only for lack of room; retried every rescan
        bool pinned = false;     // floating, never hidden, follows the active workspace
        hider::Method hidden = hider::Method::None; // how WE hid it; None = on screen
        int hideFailures = 0;    // cloak refusals; 3 -> this window uses Minimize
        bool showFailed = false; // warned once; see showWindow()
        bool wasTopmost = false; // pinned + pinnedTopmost: the flag before we set it
        QRect assigned;          // tiled only: last rect we asked for, physical px
        QRect original;          // geometry at adoption, restored on release
        int rejections = 0;
        // rcNormalPosition, not GetWindowRect: a window already maximized at
        // adoption would otherwise remember the maximized rect. Screen px
        // (see adoptOne). Persisted, and all a recovered entry's `original`
        // has left to go on.
        QRect placementNormal;
        // The state file's identity check, read once at adoption:
        // saveStateNow() runs in front of every hide/show batch, where two
        // OpenProcess calls per window would be real latency on a workspace
        // switch. None of the three changes while the window lives.
        quint32 pid = 0;
        qint64 created = 0;    // process creation FILETIME; 0 if denied
        QString windowClass;
    };

    static bool isTiledEntry(const Managed &m)
    {
        return !m.floating && !m.overflow && !m.pinned;
    }
    // Whether `m` should currently be on screen: pinned windows always are,
    // everything else only on its monitor's active workspace.
    bool belongsOnScreen(const Managed &m) const
    {
        return m.pinned || m.workspace == m_active.value(m.device, 0);
    }
    static QString keyOf(const Managed &m);

    layout::Tree *treeFor(const QString &key);
    void pruneEmptyTrees();

    // Work area in physical pixels - rcWork, so the AppBar registration in
    // panelwindow.cpp has already carved our own panels out of it - plus the
    // gaps scaled to that monitor. `key` may be a bare device name or a full
    // "device|workspace" key; only the device part is read. False if the
    // monitor is gone.
    bool metricsForKey(const QString &key, layout::Metrics *out) const;

    QString deviceForWindow(void *hwnd) const;
    // The monitor `switchToWorkspace()` and the workspace properties act on:
    // the foreground window's monitor when `m_focusedDevice` still names a
    // real one, else the monitor under the cursor, else the primary screen.
    QString focusedDevice() const;

    QString classNameFor(void *hwnd);
    QString titleFor(void *hwnd);
    QString processNameFor(void *hwnd);   // cached; file name only
    QString fullExePathFor(void *hwnd);   // uncached; full path, for the state file
    QString describe(quintptr id); // "chrome.exe (New Tab)", for the log
    bool floatProcessMatches(const QString &process) const;

    void applyPlacements(const QVector<layout::Placement> &places);
    void releaseWindow(quintptr id, bool restoreGeometry);
    // Moves an already-managed window to (newDevice, newWorkspace): tree
    // remove + insert for a tiled member, plain bookkeeping otherwise.
    // TooSmall at the new spot sets `overflow`.
    void migrateWindow(quintptr id, const QString &newDevice, int newWorkspace);

    void scheduleScan();
    void sweep(); // safety timer: adopt latecomers, re-assert drifted rects
    // Debounced screenAdded/Removed + per-screen geometry/availableGeometry
    // watcher: a resolution, DPI or work-area change must recompute the
    // trees' metrics promptly, or the sweep's drift-fixer spends its time
    // re-applying rects derived from the stale area instead. See the ctor.
    void watchScreen(QScreen *screen);
    void scheduleDisplayRecheck();
    // Focused window plus the tree holding it (nullptr if the focused window
    // is not a tiled member of ours), with that tree's boxes refreshed -
    // every navigation command needs exactly this. `metrics` and `scale`,
    // when asked for, describe the monitor that tree is on; a null return
    // then also means they were unavailable, since a command needing them
    // cannot run without.
    layout::Tree *treeForFocused(quintptr *id, layout::Metrics *metrics = nullptr,
                                 qreal *scale = nullptr);

    // m_hideMethod, forced to Minimize when cloaking is unavailable or this
    // window has already refused it three times.
    hider::Method effectiveHideMethod(const Managed &m) const;
    // Emits workspacesChanged() only when the focused monitor's workspace
    // list would actually read differently; see tilingapi.cpp for why it is
    // gated.
    void notifyWorkspacesIfChanged();
    // Hides `id` and records how. A cloak failure bumps hideFailures and
    // falls back to Minimize for this call. No-op if already hidden.
    void hideWindow(quintptr id);
    // Shows `id` by the method recorded in Managed::hidden. False if that
    // failed; `hidden` is kept so rescan() retries it instead of releasing a
    // still-cloaked window as the OS's.
    bool showWindow(quintptr id);
    // Every managed window hidden iff its workspace is not its monitor's
    // active one and it is not pinned. For the two places that move windows
    // and the active index in one go - recovery, and a workspaceCount shrink
    // - where no single switchWorkspace() describes the result. Callers write
    // the state file first; recoverState() is the exception, the file it
    // reconciles to is already on disk.
    void reconcileHidden();

    // Shows every member of (device, workspace) not already visible, or the
    // last-focused one if it is still a member and visible; falls back to
    // the topmost focusable member (or a pinned window on that device).
    void focusWorkspaceMember(const QString &device, int workspace);
    void switchWorkspace(const QString &device, int index);
    void moveWindow(quintptr id, int index, bool follow);

    // State-file persistence (tilingstate.*). saveStateNow() runs
    // synchronously before every hide/show batch, so a crash between the
    // write and the cloak still leaves an accurate file. scheduleSaveState()
    // is the 500 ms debounce for what hides nothing: adoption, release,
    // retile, resize, swap.
    void saveStateNow();
    void scheduleSaveState();

    // Runs once, from the constructor: rebuilds m_windows/m_trees/m_active
    // from tiling-state.json and reconciles what is hidden to match. The
    // recovered set waits in m_pendingRecovered until the first
    // setEnabled(true) claims it, or m_pendingTimer's 5 s deadline gives up.
    void recoverState();
    // Shows every one of `entries` that WE hid and restores topmost - the
    // answer to a file that cannot be trusted, rather than guessing which
    // half of it to keep. Whatever fails to show goes back into m_windows and
    // m_pendingRecovered for another round; the state file is deleted only
    // once nothing is left pending. Works from raw HWNDs and a handed-in
    // `active`: nothing is in m_windows yet when recoverState() calls it.
    void abandonRecovery(const QList<tilingstate::Entry> &entries,
                         const QHash<QString, int> &active);
    // m_pendingTimer's timeout: something was recovered, but 5 s later the
    // tiler has still never been enabled ("enabled": false, most likely).
    // Shows the pending set; whatever fails to show stays pending and the
    // timer restarts, so a disabled tiler can never strand a cloaked window.
    void abandonPending();
    // For a teardown that could not show everything: keeps only `unshown` in
    // m_windows, on workspace 0 with the trees and active map cleared (so
    // each belongs on screen and rescan()'s retries reseat it), and writes a
    // state file naming only them - or removes the file if `unshown` is
    // empty.
    void retainUnshown(const QVector<quintptr> &unshown);

    bool m_enabled = false;
    int m_gap = 16;
    int m_outerGap = 16;
    int m_minWidth = 360;
    int m_minHeight = 220;
    int m_resizeStep = 40;
    QStringList m_floatProcesses;
    int m_workspaceCount = 9;
    hider::Method m_hideMethod = hider::Method::Cloak;
    // Last value reported through cloakAvailableChanged(), so rescan() can
    // tell a flip from a repeat. The getter itself answers live.
    bool m_cloakAvailable = false;
    bool m_pinnedTopmost = false;

    QHash<quintptr, Managed> m_windows;
    // Floated on purpose (user toggle, refused its rect), by HWND: a
    // minimize releases the Managed entry and the restore re-adopts it
    // fresh. Pruned of dead handles in rescan().
    QSet<quintptr> m_stickyFloat;
    QHash<QString, layout::Tree *> m_trees;   // key "device|workspace"; TILED members only
    QHash<QString, int> m_active;             // device -> active workspace index
    QHash<QString, quintptr> m_lastFocusedIn; // tree key -> last focused member
    QString m_focusedDevice;
    // Last workspace list reported to QML, as a signature; see
    // notifyWorkspacesIfChanged().
    QString m_workspacesSignature;

    // Ids waiting to be shown: what recoverState() put in m_windows, or what
    // a teardown (setEnabled(false), abandonPending, abandonRecovery) failed
    // to show and retainUnshown() kept. Claimed by the next setEnabled(true),
    // or retried every m_pendingTimer tick while nothing enables the tiler.
    // Empty the rest of the time.
    QVector<quintptr> m_pendingRecovered;
    QTimer m_pendingTimer;

    QHash<quintptr, QString> m_processCache;
    quintptr m_dragging = 0; // between MOVESIZESTART and MOVESIZEEND
    // managedCount() as of the last layoutChanged emission. A pure workspace
    // switch changes which managed windows are hidden without touching tree
    // membership, so rescan() has to diff the visible count against what the
    // bar last saw - not against a value read earlier in the same pass,
    // which could never differ. -1 so the first rescan always emits.
    int m_lastVisibleCount = -1;
    bool m_applying = false; // our own SetWindowPos / cloak / minimize in flight
    bool m_debug = false;

    QTimer m_scanTimer; // coalesces a burst of window events
    QTimer m_settleTimer; // fast re-check for a window that is not ready yet
    int m_settleTries = 0;
    QTimer m_sweepTimer;
    // Debounced (not throttled - restarted on every event) so a monitor
    // reconfiguration's burst of screen signals coalesces into one
    // rescan+retile instead of several.
    QTimer m_displayTimer;
    QTimer m_saveTimer; // 500 ms single-shot debounce for scheduleSaveState()

    void *m_hookObject = nullptr;   // DESTROY..HIDE
    void *m_hookCloak = nullptr;    // CLOAKED..UNCLOAKED
    void *m_hookMinimize = nullptr; // MINIMIZESTART..MINIMIZEEND
    void *m_hookMoveSize = nullptr; // MOVESIZESTART..MOVESIZEEND
    void *m_hookForeground = nullptr;
};
