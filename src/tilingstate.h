#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QRect>
#include <QString>

// Persistence for the tiler's workspace state - and the only recovery there
// is: nothing runs at crash time, so a forced kill leaves windows cloaked or
// minimized, and this file, written before every hide/show batch, is what
// TilingApi::recoverState() reconciles them from on the next launch. This
// unit knows only the format and how to tell a live window from a stale
// record of one.
//
// `%APPDATA%\Qwin\tiling-state.json`, deliberately not under the watched
// plugins directory: a file rewritten on every layout change would hot-reload
// every plugin. Same note as appsapi.cpp's apps-usage.json.
namespace tilingstate {

// The persisted subset of TilingApi::Managed; see tilingapi.h for the fields.
// `hidden` is the method as of BEFORE the batch the file describes, so it
// cannot say whether a window is hidden now - the window itself is asked -
// only that one recorded hidden was hidden by US. See TilingApi's
// hiddenByUs().
struct Entry {
    quintptr hwnd = 0;
    quint32 pid = 0;
    qint64 created = 0;    // process creation FILETIME, 0 if denied; stored as a
                           // string - 57 bits does not survive a double.
    QString windowClass;
    QString hidden;        // "none" | "cloak" | "minimize"
    QRect placementNormal; // rcNormalPosition at adoption, screen px
    bool wasTopmost = false;
    QString monitor;       // szDevice
    int workspace = 0;
    bool floating = false;
    bool pinned = false;
};

// One persisted workspace tree; `tree` is layout::Tree::toJson()'s output
// verbatim - this unit never looks inside it.
struct WorkspaceTree {
    QString monitor;
    int index = 0;
    QJsonObject tree;
};

struct Snapshot {
    QString hideMethod;            // "cloak" | "minimize" - Tiler.hideMethod at save time
    QHash<QString, int> active;    // monitor -> active workspace index
    QList<WorkspaceTree> workspaces;
    QList<Entry> windows;
};

// QSaveFile: temp file plus atomic rename, so the next launch can never read
// a half-written file.
bool save(const Snapshot &snapshot);

// `Salvage` means the file parsed but is not trustworthy enough to rebuild
// from - an unknown version, or a boot time over 60 s off. `out` still
// carries every entry it could read: those name windows we may have cloaked,
// and ignoring them would strand those hidden with nothing left to show them
// again.
enum class LoadResult {
    Ok,      // trustworthy: recover from it
    Salvage, // parsed, not trustworthy: un-hide what it still names, deleted once
             // nothing named in it is left pending
    None,    // no file, or nothing readable in it
};

// `*why`, when given, says which outcome and why.
LoadResult load(Snapshot *out, QString *why = nullptr);

// Deletes the state file, once there is nothing left to recover: a clean
// shutdown, or recovery resolved either way.
void remove();

// IsWindow + pid + process creation time + window class: Windows recycles
// HWNDs, so the handle alone proves nothing. Every recovered entry passes
// this before it is trusted as "the window we cloaked".
bool identityMatches(const Entry &entry);

} // namespace tilingstate
