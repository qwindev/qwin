// TilingApi, continued: per-monitor workspaces and the hide/show a
// switch is made of. The class contract lives in tilingapi.h.

#include "tilingapi.h"

#include "windowfocus.h"

#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QSet>

#include "screendevice.h"
#include "tilingapi_p.h"

using namespace tiling;

namespace {

constexpr int kMaxHideFailures = 3;

} // namespace

// -------------------------------------------------------------- workspaces

QString TilingApi::focusedDevice() const
{
    if (!m_focusedDevice.isEmpty() && workAreas().contains(m_focusedDevice))
        return m_focusedDevice;
    if (const QString cursor = deviceUnderCursor(); !cursor.isEmpty())
        return cursor;
    if (QScreen *primary = QGuiApplication::primaryScreen())
        return screendevice::nameOf(primary);
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

QVariantList TilingApi::workspacesFor(const QString &device) const
{
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

QVariantList TilingApi::workspaces() const
{
    return workspacesFor(focusedDevice());
}

int TilingApi::tilesOnDevice(const QString &device) const
{
    int count = 0;
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        if (it->device == device && isTiledEntry(it.value()) && it->hidden == hider::Method::None)
            ++count;
    }
    return count;
}

QVariantMap TilingApi::monitors() const
{
    // Keyed off workAreas() rather than m_active/m_windows, so a monitor with
    // no active-index entry yet and no windows on it still appears - a bar
    // on it would otherwise have nothing to bind to.
    const QString focused = focusedDevice();
    const QHash<QString, QRect> areas = workAreas();
    QVariantMap out;
    for (auto it = areas.constBegin(); it != areas.constEnd(); ++it) {
        const QString &device = it.key();
        QVariantMap entry;
        entry.insert(QStringLiteral("active"), m_active.value(device, 0));
        entry.insert(QStringLiteral("focused"), device == focused);
        entry.insert(QStringLiteral("tiles"), tilesOnDevice(device));
        entry.insert(QStringLiteral("workspaces"), workspacesFor(device));
        out.insert(device, entry);
    }
    return out;
}

void TilingApi::focusWorkspaceMember(const QString &device, int workspace)
{
    // Set here rather than read back from the FOREGROUND event: with no
    // member to focus the shell gets the keyboard, and that event cannot say
    // which monitor was meant - see m_shellFocusDevice.
    const HWND before = GetForegroundWindow();
    m_focusedDevice = device;
    m_shellFocusDevice = device;

    const QString key = device + QLatin1Char('|') + QString::number(workspace);
    const quintptr target = m_lastFocusedIn.value(key);
    const auto last = target ? m_windows.constFind(target) : m_windows.constEnd();
    if (last != m_windows.constEnd() && last->hidden == hider::Method::None) {
        SetForegroundWindow(toHwnd(target));
    } else {
        windowfocus::focusTopmostWindow([this, device, workspace](void *hwnd) {
            const auto it = m_windows.constFind(toId(hwnd));
            if (it == m_windows.constEnd())
                return false;
            return it->device == device && (it->pinned || it->workspace == workspace);
        });
    }

    // Only a foreground change TO the desktop has an event coming that needs
    // the hint. Left set otherwise - the shell already had the keyboard, or
    // a member took it - it would misdirect the next click on the desktop
    // of another monitor.
    const HWND after = GetForegroundWindow();
    if (after == before || !isDesktopWindow(after))
        m_shellFocusDevice.clear();
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

    // Focus explicitly: the foreground window is one we just hid. Also what
    // keeps this monitor the focused one when `index` is empty.
    focusWorkspaceMember(device, index);

    notifyWorkspacesIfChanged();
    emit layoutChanged();
}

void TilingApi::switchToWorkspace(int index)
{
    if (!m_enabled)
        return;
    // Brings the foreground window's recorded monitor - and m_focusedDevice
    // with it - up to date before the chord acts: a monitor-only move (a
    // drag, Win+Shift+Arrow) fires no event that would otherwise do it, and
    // it would sit stale until the next sweep.
    rescan();
    switchWorkspace(focusedDevice(), index);
}

void TilingApi::switchToWorkspaceOn(const QString &device, int index)
{
    if (!m_enabled)
        return;
    rescan(); // see switchToWorkspace(): brings monitor-only moves up to date
    if (!workAreas().contains(device))
        return;
    switchWorkspace(device, index);
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
    rescan();
    HWND fg = GetForegroundWindow();
    if (!fg)
        return;
    moveWindow(toId(fg), index, follow);
}

void TilingApi::moveToEmptyWorkspace()
{
    if (!m_enabled)
        return;
    rescan();
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
        m_stickyFloat.insert(toId(fg)); // stays floating; survive a minimize too
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
    it->showFailed = false; // a fresh hide starts a fresh warn-once cycle
}

bool TilingApi::showWindow(quintptr id)
{
    const auto it = m_windows.find(id);
    if (it == m_windows.end() || it->hidden == hider::Method::None)
        return true; // nothing to do
    if (!hider::show(toHwnd(id), it->hidden)) {
        if (!it->showFailed) {
            it->showFailed = true;
            qWarning() << "Tiler:" << describe(id) << "failed to show - leaving it hidden";
        }
        return false;
    }
    if (it->showFailed) {
        it->showFailed = false;
        qInfo() << "Tiler:" << describe(id) << "shown on retry";
    }
    it->hidden = hider::Method::None;
    return true;
}

void TilingApi::reconcileHidden()
{
    QVector<quintptr> toShow, toHide;
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        const bool shouldHide = !belongsOnScreen(it.value());
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
