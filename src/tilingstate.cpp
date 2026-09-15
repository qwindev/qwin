#include "tilingstate.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

#include <windows.h>

namespace tilingstate {

namespace {

constexpr int kVersion = 1;
constexpr qint64 kBootToleranceSecs = 60;

QString statePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
         + QStringLiteral("/tiling-state.json");
}

// Seconds since epoch this machine booted. Derived the same way on both ends,
// so the comparison is exact to the second.
qint64 bootTimeSecs()
{
    return QDateTime::currentSecsSinceEpoch() - qint64(GetTickCount64() / 1000);
}

// Also in tilingapi.cpp, which needs the same fact for Managed::created: ten
// lines is cheaper than a shared unit.
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

QJsonObject entryToJson(const Entry &e)
{
    return QJsonObject{
        { QStringLiteral("hwnd"), double(e.hwnd) },
        { QStringLiteral("pid"), double(e.pid) },
        // A string: a FILETIME needs 57 bits and a JSON number is a double,
        // which rounds it (measured: ...291855 came back as ...291860, and
        // every identity check failed).
        { QStringLiteral("created"), QString::number(e.created) },
        { QStringLiteral("class"), e.windowClass },
        { QStringLiteral("hidden"), e.hidden },
        { QStringLiteral("placement"), QJsonObject{
              { QStringLiteral("showCmd"), e.placementShowCmd },
              { QStringLiteral("x"), e.placementNormal.x() },
              { QStringLiteral("y"), e.placementNormal.y() },
              { QStringLiteral("w"), e.placementNormal.width() },
              { QStringLiteral("h"), e.placementNormal.height() },
          } },
        { QStringLiteral("wasTopmost"), e.wasTopmost },
        { QStringLiteral("monitor"), e.monitor },
        { QStringLiteral("workspace"), e.workspace },
        { QStringLiteral("floating"), e.floating },
        { QStringLiteral("pinned"), e.pinned },
    };
}

Entry entryFromJson(const QJsonObject &w)
{
    Entry e;
    e.hwnd = quintptr(w.value(QStringLiteral("hwnd")).toDouble());
    e.pid = quint32(w.value(QStringLiteral("pid")).toDouble());
    e.created = w.value(QStringLiteral("created")).toString().toLongLong();
    e.windowClass = w.value(QStringLiteral("class")).toString();
    e.hidden = w.value(QStringLiteral("hidden")).toString();
    const QJsonObject placement = w.value(QStringLiteral("placement")).toObject();
    e.placementShowCmd = placement.value(QStringLiteral("showCmd")).toInt(1);
    e.placementNormal = QRect(placement.value(QStringLiteral("x")).toInt(),
                               placement.value(QStringLiteral("y")).toInt(),
                               placement.value(QStringLiteral("w")).toInt(),
                               placement.value(QStringLiteral("h")).toInt());
    e.wasTopmost = w.value(QStringLiteral("wasTopmost")).toBool();
    e.monitor = w.value(QStringLiteral("monitor")).toString();
    e.workspace = w.value(QStringLiteral("workspace")).toInt();
    e.floating = w.value(QStringLiteral("floating")).toBool();
    e.pinned = w.value(QStringLiteral("pinned")).toBool();
    return e;
}

} // namespace

bool save(const Snapshot &snapshot)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), kVersion);
    root.insert(QStringLiteral("bootTime"), double(bootTimeSecs()));
    root.insert(QStringLiteral("hideMethod"), snapshot.hideMethod);

    QJsonObject active;
    for (auto it = snapshot.active.constBegin(); it != snapshot.active.constEnd(); ++it)
        active.insert(it.key(), it.value());
    root.insert(QStringLiteral("active"), active);

    QJsonArray workspaces;
    for (const WorkspaceTree &ws : snapshot.workspaces) {
        workspaces.append(QJsonObject{
            { QStringLiteral("monitor"), ws.monitor },
            { QStringLiteral("index"), ws.index },
            { QStringLiteral("tree"), ws.tree },
        });
    }
    root.insert(QStringLiteral("workspaces"), workspaces);

    QJsonArray windows;
    for (const Entry &e : snapshot.windows)
        windows.append(entryToJson(e));
    root.insert(QStringLiteral("windows"), windows);

    const QString path = statePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning().noquote() << "Tiler: cannot write" << path << "- recovery will not be available";
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!file.commit()) {
        qWarning().noquote() << "Tiler: failed to save" << path << "-" << file.errorString();
        return false;
    }
    return true;
}

LoadResult load(Snapshot *out, QString *why)
{
    auto say = [&](const QString &reason) {
        if (why)
            *why = reason;
    };

    QFile file(statePath());
    if (!file.open(QIODevice::ReadOnly)) {
        say(QStringLiteral("no state file"));
        return LoadResult::None;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        say(QStringLiteral("parse error: %1").arg(err.errorString()));
        return LoadResult::None;
    }

    const QJsonObject root = doc.object();

    // Demoted to Salvage, not abandoned: the window list below is still
    // parsed so the caller can un-hide what the file names. Nonsense from an
    // unknown version is harmless - it fails identityMatches().
    QString distrust;
    if (root.value(QStringLiteral("version")).toInt(-1) != kVersion)
        distrust = QStringLiteral("unknown version");
    const qint64 fileBoot = qint64(root.value(QStringLiteral("bootTime")).toDouble());
    if (distrust.isEmpty() && qAbs(fileBoot - bootTimeSecs()) > kBootToleranceSecs)
        distrust = QStringLiteral("boot time mismatch - a different session");

    Snapshot snapshot;
    snapshot.hideMethod = root.value(QStringLiteral("hideMethod")).toString();

    const QJsonObject active = root.value(QStringLiteral("active")).toObject();
    for (auto it = active.constBegin(); it != active.constEnd(); ++it)
        snapshot.active.insert(it.key(), it.value().toInt());

    const QJsonArray workspaces = root.value(QStringLiteral("workspaces")).toArray();
    for (const QJsonValue &v : workspaces) {
        const QJsonObject w = v.toObject();
        WorkspaceTree ws;
        ws.monitor = w.value(QStringLiteral("monitor")).toString();
        ws.index = w.value(QStringLiteral("index")).toInt();
        ws.tree = w.value(QStringLiteral("tree")).toObject();
        snapshot.workspaces.append(ws);
    }

    const QJsonArray windows = root.value(QStringLiteral("windows")).toArray();
    for (const QJsonValue &v : windows) {
        Entry e = entryFromJson(v.toObject());
        if (!e.hwnd) {
            // 0 is never a valid handle. In a file we already distrust, one
            // unreadable entry is not a verdict on the rest.
            if (distrust.isEmpty()) {
                say(QStringLiteral("malformed window entry"));
                return LoadResult::None;
            }
            continue;
        }
        snapshot.windows.append(e);
    }

    *out = std::move(snapshot);
    if (!distrust.isEmpty()) {
        say(distrust);
        return LoadResult::Salvage;
    }
    return LoadResult::Ok;
}

void remove()
{
    QFile::remove(statePath());
}

bool identityMatches(const Entry &entry)
{
    if (!entry.hwnd)
        return false;
    HWND hwnd = reinterpret_cast<HWND>(entry.hwnd);
    if (!IsWindow(hwnd))
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != entry.pid)
        return false;

    wchar_t cls[256] = {};
    const int n = GetClassNameW(hwnd, cls, 256);
    if (QString::fromWCharArray(cls, n) != entry.windowClass)
        return false;

    // A disagreeing creation time means another process now wears this PID.
    // One we could not read (elevated) proves nothing either way, so pid +
    // class has to do; load()'s boot check keeps PID reuse rare.
    if (entry.created != 0) {
        const qint64 created = processCreationTime(pid);
        if (created != 0 && created != entry.created)
            return false;
    }
    return true;
}

} // namespace tilingstate
