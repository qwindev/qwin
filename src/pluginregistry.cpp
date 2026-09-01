#include "pluginregistry.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>

namespace {

// Canonical form for names and every config reference to one.
QString normalized(const QString &name)
{
    return name.trimmed().toLower();
}

// Dashed lowercase identifiers: "bar", "system-stats", "cpu2-graph".
bool isValidName(const QString &name)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z0-9]+(-[a-z0-9]+)*$"));
    return pattern.match(name).hasMatch();
}

// Deep search for a string equal to `needle`: does this config embed that plugin?
bool jsonContainsString(const QJsonValue &value, const QString &needle)
{
    if (value.isString())
        return normalized(value.toString()) == needle;
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (const QJsonValue &v : array) {
            if (jsonContainsString(v, needle))
                return true;
        }
        return false;
    }
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            if (jsonContainsString(it.value(), needle))
                return true;
        }
    }
    return false;
}

} // namespace

PluginRegistry::PluginRegistry(const QString &pluginsDir, QObject *parent)
    : QObject(parent)
    , m_dir(pluginsDir)
    , m_configPath(QDir(pluginsDir).absoluteFilePath(QStringLiteral("config.json")))
{
}

bool PluginRegistry::rescan()
{
    const QJsonObject before = m_config;
    readConfig();
    const bool configDirty = m_config != before;

    scanManifests();

    // No config file / no "enabled" key enables everything, so
    // drop-a-folder-in works without touching config.json.
    m_enabled.clear();
    const QJsonValue enabledValue = m_config.value(QStringLiteral("enabled"));
    if (enabledValue.isUndefined()) {
        m_enabled = m_plugins.keys();
        m_enabled.sort();
    } else if (!enabledValue.isArray()) {
        if (configDirty)
            qWarning().noquote() << "config.json: \"enabled\" must be an array"
                                    " of plugin names - enabling all plugins";
        m_enabled = m_plugins.keys();
        m_enabled.sort();
    } else {
        QSet<QString> unknown;
        const QJsonArray array = enabledValue.toArray();
        for (const QJsonValue &v : array) {
            if (!v.isString()) {
                if (configDirty)
                    qWarning().noquote() << "config.json: ignoring non-string entry"
                                            " in \"enabled\"";
                continue;
            }
            const QString name = normalized(v.toString());
            if (!m_plugins.contains(name)) {
                unknown.insert(name);
                continue;
            }
            if (!m_enabled.contains(name))
                m_enabled << name;
        }
        for (const QString &name : unknown) {
            if (!m_loggedUnknownEnabled.contains(name))
                qWarning().noquote() << "config.json: enabled plugin" << name
                                     << "is not installed";
        }
        m_loggedUnknownEnabled = unknown;
    }

    return configDirty;
}

void PluginRegistry::readConfig()
{
    if (!QFileInfo::exists(m_configPath)) {
        m_config = QJsonObject();
        m_configLoggedInvalid = false;
        return;
    }

    QFile file(m_configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << "Cannot read" << m_configPath << "- keeping current config";
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        // Keep last-good, like colors.json.
        if (!m_configLoggedInvalid)
            qWarning().noquote() << "Invalid config.json - keeping current config:"
                                 << (doc.isNull() ? parseError.errorString()
                                                  : QStringLiteral("root must be an object"));
        m_configLoggedInvalid = true;
        return;
    }
    m_configLoggedInvalid = false;
    m_config = doc.object();
}

void PluginRegistry::scanManifests()
{
    m_plugins.clear();
    m_entryToName.clear();
    m_broken.clear();

    const auto subDirs = m_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                             QDir::Name);
    for (const QFileInfo &sub : subDirs) {
        const QDir dir(sub.absoluteFilePath());
        if (dir.dirName().compare(QStringLiteral("shared"), Qt::CaseInsensitive) == 0)
            continue; // importable components, never plugins

        const QString entryFile = dir.absoluteFilePath(QStringLiteral("plugin.qml"));
        const QString manifestFile = dir.absoluteFilePath(QStringLiteral("manifest.json"));
        const bool hasEntry = QFileInfo::exists(entryFile);
        const bool hasManifest = QFileInfo::exists(manifestFile);
        if (!hasEntry && !hasManifest)
            continue; // plain asset folder

        QString error;
        Plugin plugin;
        plugin.entryFile = entryFile;

        if (!hasManifest) {
            error = QStringLiteral("manifest.json missing - every plugin needs one:\n"
                                   "{ \"name\": \"my-plugin\", \"author\": \"me\", \"version\": \"1.0.0\" }");
        } else if (!hasEntry) {
            error = QStringLiteral("plugin.qml missing next to manifest.json.");
        } else {
            QFile file(manifestFile);
            QJsonParseError parseError;
            QJsonDocument doc;
            if (!file.open(QIODevice::ReadOnly)) {
                error = QStringLiteral("manifest.json: cannot read the file.");
            } else if ((doc = QJsonDocument::fromJson(file.readAll(), &parseError)).isNull()) {
                error = QStringLiteral("manifest.json: %1").arg(parseError.errorString());
            } else if (!doc.isObject()) {
                error = QStringLiteral("manifest.json: root must be an object.");
            } else {
                const QJsonObject obj = doc.object();
                const QString rawName = obj.value(QStringLiteral("name")).toString();
                plugin.name = normalized(rawName);
                plugin.author = obj.value(QStringLiteral("author")).toString();
                plugin.version = obj.value(QStringLiteral("version")).toString();

                if (plugin.name.isEmpty())
                    error = QStringLiteral("manifest.json: \"name\" (string) is required.");
                else if (!isValidName(plugin.name))
                    error = QStringLiteral("manifest.json: invalid name \"%1\" - use dashed "
                                           "lowercase (letters, digits, dashes).").arg(rawName);
                else if (plugin.name == QStringLiteral("enabled") || plugin.name == QStringLiteral("theme"))
                    error = QStringLiteral("manifest.json: \"%1\" is a reserved name.").arg(plugin.name);
                else if (m_plugins.contains(plugin.name))
                    error = QStringLiteral("manifest.json: duplicate plugin name \"%1\" - "
                                           "already provided by %2.")
                                .arg(plugin.name,
                                     QDir::toNativeSeparators(
                                         QFileInfo(m_plugins.value(plugin.name).entryFile)
                                             .absolutePath()));
            }
        }

        if (!error.isEmpty()) {
            m_broken.append({entryFile, error});
            continue;
        }
        m_plugins.insert(plugin.name, plugin);
        m_entryToName.insert(entryFile, plugin.name);
    }

    // Only announce news; rescan() runs on every reload tick.
    QSet<QString> namesNow;
    for (const Plugin &w : std::as_const(m_plugins)) {
        namesNow.insert(w.name);
        if (!m_loggedPlugins.contains(w.name)) {
            qInfo().noquote() << "Registered plugin" << w.name
                              << (w.version.isEmpty() ? QString() : w.version)
                              << (w.author.isEmpty() ? QString()
                                                     : QStringLiteral("by ") + w.author)
                              << QStringLiteral("(%1)").arg(QDir::toNativeSeparators(w.entryFile));
        }
    }
    m_loggedPlugins = namesNow;

    QHash<QString, QString> brokenNow;
    for (const Broken &b : std::as_const(m_broken)) {
        brokenNow.insert(b.entryFile, b.error);
        if (m_loggedBroken.value(b.entryFile) != b.error)
            qWarning().noquote() << "Cannot register" << b.entryFile << ":" << b.error;
    }
    m_loggedBroken = brokenNow;

    // A plugin is a folder with a manifest; warn once per loose leftover .qml.
    const auto looseFiles = m_dir.entryInfoList({QStringLiteral("*.qml")}, QDir::Files);
    QSet<QString> looseNow;
    for (const QFileInfo &f : looseFiles) {
        looseNow.insert(f.absoluteFilePath());
        if (!m_loggedTopLevelQml.contains(f.absoluteFilePath()))
            qWarning().noquote() << "Ignoring top-level" << f.fileName()
                                 << "- plugins are folders with a manifest.json and plugin.qml";
    }
    m_loggedTopLevelQml = looseNow;
}

QString PluginRegistry::entryFile(const QString &name) const
{
    return m_plugins.value(normalized(name)).entryFile;
}

QString PluginRegistry::nameForEntry(const QString &entryFile) const
{
    return m_entryToName.value(entryFile);
}

bool PluginRegistry::isEnabled(const QString &name) const
{
    return m_enabled.contains(normalized(name));
}

bool PluginRegistry::isEmbedded(const QString &name) const
{
    const QString needle = normalized(name);
    for (auto it = m_config.constBegin(); it != m_config.constEnd(); ++it) {
        const QString key = normalized(it.key());
        if (key == QStringLiteral("enabled") || key == needle)
            continue; // a plugin's own section does not embed it
        if (jsonContainsString(it.value(), needle))
            return true;
    }
    return false;
}

QJsonObject PluginRegistry::configSection(const QString &normalizedName) const
{
    for (auto it = m_config.constBegin(); it != m_config.constEnd(); ++it) {
        if (normalized(it.key()) == normalizedName && it.value().isObject())
            return it.value().toObject();
    }
    return QJsonObject();
}

QVariantMap PluginRegistry::config(const QString &name) const
{
    return configSection(normalized(name)).toVariantMap();
}

QUrl PluginRegistry::source(const QString &name) const
{
    const QString entry = entryFile(name);
    if (entry.isEmpty()) {
        qWarning().noquote() << "Plugins.source: no registered plugin named"
                             << normalized(name);
        return QUrl();
    }
    return QUrl::fromLocalFile(entry);
}

bool PluginRegistry::has(const QString &name) const
{
    return m_plugins.contains(normalized(name));
}
