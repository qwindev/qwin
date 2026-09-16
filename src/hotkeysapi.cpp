#include "hotkeysapi.h"

#include "hotkey.h"

#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QVariantMap>
#include <algorithm>

HotkeysApi::HotkeysApi(std::function<QString(const QString &file)> pluginForFile,
                       QObject *parent)
    : QObject(parent)
    , m_pluginForFile(std::move(pluginForFile))
{
}

QVariantList HotkeysApi::list() const
{
    struct Row {
        QString sequence;
        QString plugin;
        QString description;
        bool registered;
    };

    QList<Row> rows;
    for (Hotkey *hotkey : Hotkey::instances()) {
        // Instantiators (the tiler's workspace bindings) blank the sequence
        // while tearing a delegate down, and a disabled Hotkey was never
        // meant to be live - neither belongs in a listing of what is armed.
        if (!hotkey->enabled() || hotkey->sequence().isEmpty())
            continue;

        const QString raw = hotkey->sequence();
        const QKeySequence seq(raw, QKeySequence::PortableText);
        // An unparseable chord still gets listed (with registered false, so
        // the cheat sheet flags it) rather than silently vanishing.
        QString display = seq.isEmpty() ? raw : seq.toString(QKeySequence::PortableText);
        display.replace(QStringLiteral("Meta+"), QStringLiteral("Win+"));

        const QString file = hotkey->sourceFile();
        QString plugin = m_pluginForFile ? m_pluginForFile(file) : QString();
        if (plugin.isEmpty() && !file.isEmpty())
            plugin = QFileInfo(file).dir().dirName(); // e.g. a Hotkey declared in shared/

        rows.append({display, plugin, hotkey->description(), hotkey->registered()});
    }

    // Declaration order is not recoverable - QML runs componentComplete in
    // reverse and Instantiators create later - so sort deterministically.
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        const int byPlugin = a.plugin.compare(b.plugin, Qt::CaseInsensitive);
        if (byPlugin != 0)
            return byPlugin < 0;
        return a.sequence.compare(b.sequence, Qt::CaseInsensitive) < 0;
    });

    QVariantList out;
    out.reserve(rows.size());
    for (const Row &row : rows) {
        QVariantMap entry;
        entry[QStringLiteral("sequence")] = row.sequence;
        entry[QStringLiteral("plugin")] = row.plugin;
        entry[QStringLiteral("description")] = row.description;
        entry[QStringLiteral("registered")] = row.registered;
        out.append(entry);
    }
    return out;
}
