#pragma once

#include <QDir>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

// The `Plugins` QML singleton. Scans <pluginsDir>/*/manifest.json into a
// name -> plugin table and reads <pluginsDir>/config.json: an "enabled"
// array plus one free-form section per plugin name. Names are dashed and
// case-invariant, stored lowercase. QML reads its own section with
// config(name) and embeds other plugins with Loader { source: source(name) }.
class PluginRegistry : public QObject
{
    Q_OBJECT
public:
    struct Plugin {
        QString name;      // lowercase dashed, from manifest.json
        QString author;
        QString version;
        QString entryFile; // absolute path to the folder's plugin.qml
    };
    // An unregisterable plugin folder. The manager shows an error window
    // keyed under entryFile, so fixing the file swaps in the real plugin.
    struct Broken {
        QString entryFile;
        QString error;
    };

    explicit PluginRegistry(const QString &pluginsDir, QObject *parent = nullptr);

    // Returns true when config.json changed - the manager escalates that to
    // a full reload, since any plugin may have read any section.
    bool rescan();

    QList<Broken> broken() const { return m_broken; }
    QStringList enabledNames() const { return m_enabled; }
    QString entryFile(const QString &name) const;
    QString nameForEntry(const QString &entryFile) const;
    bool isEnabled(const QString &name) const;
    // Name appears in another plugin's config section, i.e. is embedded, so
    // a change to it must reload the embedders too.
    bool isEmbedded(const QString &name) const;

    // QML API (the `Plugins` singleton).
    Q_INVOKABLE QVariantMap config(const QString &name) const;
    Q_INVOKABLE QUrl source(const QString &name) const;
    Q_INVOKABLE bool has(const QString &name) const;

private:
    void readConfig();
    void scanManifests();
    QJsonObject configSection(const QString &normalizedName) const;

    QDir m_dir;
    QString m_configPath;
    QJsonObject m_config;              // last-good parse of config.json
    QHash<QString, Plugin> m_plugins;  // normalized name -> plugin
    QHash<QString, QString> m_entryToName;
    QList<Broken> m_broken;
    QStringList m_enabled;             // registered names resolved from "enabled"

    // Diff-logging state: rescan() runs every debounce tick, so only news is logged.
    QHash<QString, QString> m_loggedBroken;   // entryFile -> error
    QSet<QString> m_loggedUnknownEnabled;
    QSet<QString> m_loggedTopLevelQml;
    QSet<QString> m_loggedPlugins;            // names already announced
    bool m_configLoggedInvalid = false;
};
