// TilingApi, continued: the state file and crash recovery. saveStateNow()
// runs in front of every hide, so the file may lag a show but never a
// cloak; recoverState() reads the last write back on the next launch. See
// tilingapi.h and tilingstate.h.

#include "tilingapi.h"

#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QSet>

#include "tilingapi_p.h"

using namespace tiling;

namespace {

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

} // namespace

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
    QVector<quintptr> unshown;
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
        hider::Method attempted = hider::Method::None;
        bool shown = true;
        if (hider::isCloaked(hwnd) && ours(hider::Method::Cloak)) {
            attempted = hider::Method::Cloak;
            shown = hider::show(hwnd, hider::Method::Cloak);
        } else if (IsIconic(hwnd) && ours(hider::Method::Minimize)) {
            attempted = hider::Method::Minimize;
            shown = hider::show(hwnd, hider::Method::Minimize);
        }
        if (!shown) {
            // Couldn't undo the hide - m_windows is empty this early (only
            // recoverState() calls this, from the ctor), so track it there
            // for rescan()'s toShow retry to pick up once something enables
            // the tiler.
            Managed m;
            m.device = e.monitor;
            m.hidden = attempted;
            m.pid = e.pid;
            m.created = e.created;
            m.windowClass = e.windowClass;
            m.floating = e.floating;
            m.pinned = e.pinned;
            m.wasTopmost = e.wasTopmost;
            m.placementNormal = e.placementNormal;
            m.original = e.placementNormal;
            m.showFailed = true;
            m_windows.insert(e.hwnd, m);
            unshown.append(e.hwnd);
        }
        if (e.pinned && !e.wasTopmost) {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    m_applying = false;

    retainUnshown(unshown);
    m_pendingRecovered = unshown;
    if (!unshown.isEmpty())
        m_pendingTimer.start();
}

void TilingApi::abandonPending()
{
    if (m_pendingRecovered.isEmpty())
        return; // already claimed by setEnabled(true), or nothing was recovered

    // Only a genuine first abandon is worth a log line; on a retry every
    // pending entry already carries showFailed from the previous round, and
    // repeating it every 5 s while explorer stays down would just be noise.
    bool freshAbandon = false;
    for (quintptr id : std::as_const(m_pendingRecovered)) {
        const auto it = m_windows.constFind(id);
        if (it != m_windows.constEnd() && !it->showFailed) {
            freshAbandon = true;
            break;
        }
    }
    if (freshAbandon) {
        qInfo() << "Tiler: never enabled within" << kPendingTimeoutMs / 1000
                << "s of recovering" << m_pendingRecovered.size()
                << "window(s) - showing them";
    }

    m_applying = true;
    QVector<quintptr> unshown;
    for (quintptr id : std::as_const(m_pendingRecovered)) {
        const auto it = m_windows.find(id);
        if (it == m_windows.end())
            continue; // released some other way already
        if (!IsWindow(toHwnd(id)))
            continue; // gone; counts as shown - nothing left to retry
        if (!showWindow(id))
            unshown.append(id);
        if (it->pinned && !it->wasTopmost) {
            SetWindowPos(toHwnd(id), HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    m_applying = false;

    retainUnshown(unshown);
    m_pendingRecovered = unshown;
    if (!unshown.isEmpty())
        m_pendingTimer.start(); // retry every 5 s while nothing enables the tiler
}

void TilingApi::retainUnshown(const QVector<quintptr> &unshown)
{
    qDeleteAll(m_trees);
    m_trees.clear();
    m_active.clear();
    m_lastFocusedIn.clear();

    for (auto it = m_windows.begin(); it != m_windows.end(); ) {
        if (unshown.contains(it.key()))
            ++it;
        else
            it = m_windows.erase(it);
    }
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        it->workspace = 0; // m_active is empty now, so 0 means "belongs on screen"
        if (isTiledEntry(it.value()))
            it->overflow = true; // its tree is gone; the overflow retry reseats it
    }

    if (unshown.isEmpty())
        tilingstate::remove();
    else
        saveStateNow();
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
        abandonRecovery(valid, snapshot.active); // shows what it can, retries the rest
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
