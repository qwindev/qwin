#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

// The `Apps` QML singleton: the installed-app list a launcher searches.
//
// The list comes from `shell:AppsFolder`, the shell namespace behind Start's
// "All apps". It is deliberately the only source scanned, because it is
// already the union of the two lists a launcher would otherwise merge and
// dedupe by hand: the Start Menu .lnk trees (per-user and all-users) and the
// packaged/Store apps, which have no shortcut anywhere on disk. Desktop
// shortcuts are the one thing it does not carry, so those are scanned
// separately when includeDesktop is on.
//
// Entries are identified by their shell parsing name, which round-trips
// through SHParseDisplayName - so one code path launches a Store app, a
// Win32 program and a .lnk alike, and none of them needs an exe path.
//
// Everything here runs on the GUI thread. A scan is a namespace enumeration
// with no icon work, and costs ~300 ms warm / ~600 ms cold - the shell's own
// cost, not something this can optimise - so it is kept off the summon path
// by a long staleness window. Icons cost ~12 ms each and are resolved by
// iconFor() on demand, then cached; after a scan a timer walks the list and
// warms the rest, so typing never waits on one.
class AppsApi : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    // False when the AppsFolder enumeration failed outright; the plugin then
    // says so instead of showing an empty list that looks like "no matches".
    Q_PROPERTY(bool available READ available NOTIFY changed)
public:
    explicit AppsApi(QObject *parent = nullptr);
    ~AppsApi() override;

    int count() const { return int(m_entries.size()); }
    bool available() const { return m_available; }

    // Config pushed in from QML, like the tiler's. Takes effect on the next
    // scan, and forces one if the value actually changed.
    Q_INVOKABLE void setIncludeDesktop(bool on);

    // Rescans if the last scan is older than a few seconds, so summoning the
    // launcher repeatedly does not re-enumerate every time. force ignores
    // that window.
    Q_INVOKABLE void refresh(bool force = false);

    // Ranked matches, best first. Each entry is
    //   { id, name, subtitle, kind }
    // where kind is "store", "app" or "shortcut". An empty query returns the
    // most-used entries, which is what the launcher shows before a keystroke.
    Q_INVOKABLE QVariantList search(const QString &query, int limit = 12) const;

    // Launches by id and records the use. Returns false (and logs) if the
    // shell refused - a stale id after an uninstall is the usual reason.
    Q_INVOKABLE bool launch(const QString &id);

    // Fallback for a query that matched nothing: hand it to the shell as if
    // typed into Run, so "cmd", "wt", or a path still goes somewhere.
    Q_INVOKABLE bool runCommand(const QString &command);

    // "data:image/png;base64,..." for an entry, or "" if it has no icon.
    // Cached; the first call for an id costs a shell image lookup.
    Q_INVOKABLE QString iconFor(const QString &id);

signals:
    void changed();

private:
    struct Entry {
        QString id;         // "shell:AppsFolder\<appid>", or a path for desktop items
        QString name;       // display name
        QString subtitle;   // shortened app id, shown but not matched on
        QString kind;       // "store" | "app" | "shortcut"
        QString lowerName;
        QString lowerAppId; // the full app id, matched on; empty when synthesised
        QString initials;   // "vsc" for "Visual Studio Code"
    };
    struct Usage {
        int count = 0;
        qint64 lastUsedMs = 0;
    };

    void scan();
    void scanAppsFolder(QHash<QString, int> &seenNames);
    void scanDesktop(QHash<QString, int> &seenNames);
    void warmNextIcon();
    int frecencyBonus(const QString &id) const;
    void loadUsage();
    void saveUsage() const;

    QList<Entry> m_entries;
    QHash<QString, Usage> m_usage;   // id -> how often and how recently launched
    QHash<QString, QString> m_iconCache; // id -> data URL, capped
    QElapsedTimer m_sinceScan;
    QTimer m_iconWarmTimer;          // fills m_iconCache in the background
    int m_warmIndex = 0;             // how far through m_entries it has got
    QString m_usagePath;
    bool m_available = false;
    bool m_includeDesktop = false;
    bool m_comOwnedByUs = false;
};
