#include "colorpalette.h"

#include <QColor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

namespace {

// Always present so the standard roles never bind to undefined, and written
// out as the initial colors.json - an array, not a QJsonObject, so the
// generated file keeps this reading order.
struct DefaultColor { const char *name; const char *value; };
const DefaultColor kDefaults[] = {
    { "background", "#F0101418" },
    { "surface",    "#455055" },
    { "text",       "#EAF2F2" },
    { "textMuted",  "#7D8C94" },
    { "accent",     "#55D6C2" },
    { "warning",    "#F0C674" },
    { "error",      "#FF7A66" },
};

// Keys become QML property names, so they must be lowercase-first:
// uppercase-first reads as a type lookup and would be unreachable.
bool isValidKey(const QString &key)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z][A-Za-z0-9_]*$"));
    return pattern.match(key).hasMatch();
}

} // namespace

ColorPalette::ColorPalette(const QString &pluginsDir, QObject *parent)
    : QQmlPropertyMap(this, parent)
    , m_filePath(QDir(pluginsDir).absoluteFilePath(QStringLiteral("colors.json")))
{
    for (const DefaultColor &c : kDefaults)
        insert(QLatin1String(c.name), QColor(QLatin1String(c.value)));

    if (!QFileInfo::exists(m_filePath))
        writeDefaultFile();

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, &ColorPalette::reload);
    // A save that replaces the file drops the file watch and only shows up
    // as a directory event, so watch the dir too and re-arm on every reload.
    m_watcher.addPath(QDir(pluginsDir).absolutePath());
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            &m_debounce, qOverload<>(&QTimer::start));
    refreshWatch();

    reload();
}

QVariant ColorPalette::updateValue(const QString &key, const QVariant &input)
{
    Q_UNUSED(input);
    qWarning().noquote() << "Colors is read-only from QML; edit"
                         << QDir::toNativeSeparators(m_filePath)
                         << "to change" << key;
    return value(key);
}

void ColorPalette::reload()
{
    refreshWatch();

    // Start from the defaults so a key deleted from the file falls back to
    // its built-in value; keys are only ever updated, never removed.
    QHash<QString, QVariant> merged;
    for (const DefaultColor &c : kDefaults)
        merged.insert(QLatin1String(c.name), QColor(QLatin1String(c.value)));

    if (QFileInfo::exists(m_filePath)) {
        QFile file(m_filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning().noquote() << "Cannot read" << m_filePath << "- keeping current colors";
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (doc.isNull() || !doc.isObject()) {
            qWarning().noquote() << "Invalid colors.json - keeping current colors:"
                                 << (doc.isNull() ? parseError.errorString()
                                                  : QStringLiteral("root must be an object"));
            return;
        }

        const QJsonObject obj = doc.object();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            const QColor color = it.value().isString()
                ? QColor::fromString(it.value().toString()) : QColor();
            if (!isValidKey(it.key()) || !color.isValid()) {
                qWarning().noquote() << "colors.json: ignoring" << it.key()
                                     << "=" << it.value().toVariant().toString();
                if (contains(it.key()))
                    merged.insert(it.key(), value(it.key())); // keep current
                continue;
            }
            merged.insert(it.key(), color);
        }
    }

    QStringList changed;
    for (auto it = merged.constBegin(); it != merged.constEnd(); ++it) {
        if (value(it.key()) != it.value()) {
            insert(it.key(), it.value());
            changed << it.key();
        }
    }
    if (!changed.isEmpty())
        qInfo().noquote() << "Colors updated:" << changed.join(QStringLiteral(", "));
}

void ColorPalette::writeDefaultFile()
{
    QFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning().noquote() << "Cannot create" << m_filePath;
        return;
    }
    QString json = QStringLiteral("{\n");
    const size_t count = sizeof(kDefaults) / sizeof(kDefaults[0]);
    for (size_t i = 0; i < count; ++i) {
        json += QStringLiteral("    \"%1\": \"%2\"%3\n")
                    .arg(QLatin1String(kDefaults[i].name),
                         QLatin1String(kDefaults[i].value),
                         i + 1 < count ? QStringLiteral(",") : QString());
    }
    json += QStringLiteral("}\n");
    file.write(json.toUtf8());
    qInfo().noquote() << "Created default" << QDir::toNativeSeparators(m_filePath);
}

void ColorPalette::refreshWatch()
{
    if (QFileInfo::exists(m_filePath) && !m_watcher.files().contains(m_filePath))
        m_watcher.addPath(m_filePath);
}
