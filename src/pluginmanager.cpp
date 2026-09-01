#include "pluginmanager.h"
#include "pluginregistry.h"

#include <QCoreApplication>
#include <QEvent>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QDebug>

namespace {

// Wraps plugins whose root is a plain Item. The DragHandler makes them
// draggable by default, where the plugin's own items do not grab the press.
const char kWrapperQml[] = R"QML(
import QtQuick
import QtQuick.Window

Window {
    id: wrapper
    visible: true
    flags: Qt.FramelessWindowHint | Qt.Tool
    color: "transparent"

    // Alt+F4 must not destroy the plugin: only a reload could bring it back.
    onClosing: (close) => close.accepted = false

    DragHandler {
        target: null
        onActiveChanged: if (active) wrapper.startSystemMove()
    }
}
)QML";

const char kErrorQml[] = R"QML(
import QtQuick
import QtQuick.Window

Window {
    id: errWin
    required property string fileName
    required property string errorText

    width: 400
    height: Math.min(260, 100 + errBody.implicitHeight)
    visible: true
    flags: Qt.FramelessWindowHint | Qt.Tool
    color: "transparent"

    Rectangle {
        anchors.fill: parent
        radius: 10
        color: "#F02A1518"
        border.color: "#80FF5544"
        border.width: 1

        Column {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 8

            Text { text: "PLUGIN ERROR"; color: "#FF7A66"; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1 }
            Text {
                text: errWin.fileName
                color: "#D8C8C8"; font.pixelSize: 11
                width: parent.width; elide: Text.ElideMiddle
            }
            Text {
                id: errBody
                text: errWin.errorText
                color: "#F0E8E8"
                font.family: "Consolas"; font.pixelSize: 11
                width: parent.width
                wrapMode: Text.WrapAnywhere
            }
        }

        DragHandler {
            target: null
            onActiveChanged: if (active) errWin.startSystemMove()
        }
    }
}
)QML";

// Base URL for the built-in components above. MUST be file:// - under any
// other scheme QtQuick imports do not resolve and the component hangs in
// Loading state ("Component is not ready").
QUrl builtinUrl(const char *name)
{
    return QUrl::fromLocalFile(QCoreApplication::applicationDirPath()
                               + QStringLiteral("/.builtin/") + QLatin1String(name));
}

} // namespace

PluginManager::PluginManager(const QString &pluginsDir, PluginRegistry *registry,
                             QObject *parent)
    : QObject(parent)
    , m_engine(std::make_unique<QQmlApplicationEngine>())
    , m_registry(registry)
    , m_dir(pluginsDir)
{
    // Editors fire several fs events per save; reload 300 ms after the last.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, &PluginManager::applyPendingReloads);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &PluginManager::onFileChanged);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &PluginManager::onDirectoryChanged);
}

QHash<QString, QString> PluginManager::desiredEntries() const
{
    QHash<QString, QString> desired; // entry file -> error text ("" = load)
    const QStringList enabled = m_registry->enabledNames();
    for (const QString &name : enabled)
        desired.insert(m_registry->entryFile(name), QString());
    const auto broken = m_registry->broken();
    for (const PluginRegistry::Broken &b : broken)
        desired.insert(b.entryFile, b.error);
    return desired;
}

void PluginManager::loadAll()
{
    m_registry->rescan();
    const QHash<QString, QString> desired = desiredEntries();
    if (desired.isEmpty())
        qInfo().noquote() << "No plugins enabled in" << m_dir.absolutePath();

    for (auto it = desired.constBegin(); it != desired.constEnd(); ++it) {
        if (m_loaded.contains(it.key()))
            continue;
        if (it.value().isEmpty())
            loadPlugin(it.key());
        else
            showErrorWindow(it.key(), it.value());
    }
    refreshWatches();
}

// ~QObject deletes the child windows only after the engine member is gone,
// so tear the plugins down first: QML objects must not outlive their engine.
PluginManager::~PluginManager()
{
    const QStringList loaded = m_loaded.keys();
    for (const QString &entry : loaded)
        unloadPlugin(entry);
    flushDeferredDeletes();
}

// A fresh engine is the only dependable way to drop every cached
// compilation; see applyPendingReloads(). All plugins must already be unloaded.
void PluginManager::resetEngine()
{
    m_engine.reset(); // no engine may exist while registrations are redone
    if (m_reregisterHook)
        m_reregisterHook();
    m_engine = std::make_unique<QQmlApplicationEngine>();
}

void PluginManager::reloadAll()
{
    const QStringList loaded = m_loaded.keys();
    for (const QString &entry : loaded)
        unloadPlugin(entry);
    flushDeferredDeletes();
    resetEngine();
    loadAll();
}

// unloadPlugin uses deleteLater(), but the destructor is what releases
// exclusive resources (RegisterHotKey chords, AppBar slots). Without this
// flush the replacement races the old instance for them and loses
// nondeterministically: window churn pumps Win32 messages, which sometimes
// runs the deferred delete early and sometimes does not.
void PluginManager::flushDeferredDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void PluginManager::loadPlugin(const QString &entryFile)
{
    // Unique query per load: QML is cached per URL and clearComponentCache()
    // does not evict units the engine still references, so the same URL can
    // serve the old program. The query is stripped on open and ignored when
    // relative paths resolve against it.
    QUrl url = QUrl::fromLocalFile(entryFile);
    url.setQuery(QStringLiteral("reload=") + QString::number(++m_loadSerial));
    QQmlComponent component(m_engine.get(), url);

    QObject *root = component.isError() ? nullptr : component.create();
    if (!root) {
        QStringList lines;
        const auto errors = component.errors();
        for (const QQmlError &e : errors)
            lines << e.toString();
        const QString text = lines.isEmpty() ? QStringLiteral("Unknown error.")
                                             : lines.join(QStringLiteral("\n"));
        qWarning().noquote() << "Failed to load" << entryFile << ":\n" << text;
        showErrorWindow(entryFile, text);
        return;
    }

    // We own plugin lifetimes; keep the JS GC away from the roots.
    QQmlEngine::setObjectOwnership(root, QQmlEngine::CppOwnership);
    root->setParent(this);

    if (auto *window = qobject_cast<QQuickWindow *>(root)) {
        // A Window root controls its own visibility - one still hidden after
        // create() meant it (hotkey-summoned overlays); never force it visible.
        m_loaded.insert(entryFile, window);
        qInfo().noquote() << "Loaded plugin" << entryFile
                          << (window->isVisible() ? "" : "(starts hidden)");
        return;
    }

    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        QQmlComponent wrapperComponent(m_engine.get());
        wrapperComponent.setData(kWrapperQml, builtinUrl("wrapper.qml"));
        auto *wrapper = qobject_cast<QQuickWindow *>(wrapperComponent.create());
        if (!wrapper) {
            qWarning().noquote() << "Internal error creating wrapper window:"
                                 << wrapperComponent.errorString();
            delete root;
            return;
        }
        QQmlEngine::setObjectOwnership(wrapper, QQmlEngine::CppOwnership);
        // QWindow::setParent(QWindow*) shadows the QObject ownership overload.
        static_cast<QObject *>(wrapper)->setParent(this);

        const int w = int(item->width() > 0 ? item->width() : item->implicitWidth());
        const int h = int(item->height() > 0 ? item->height() : item->implicitHeight());
        wrapper->resize(qMax(w, 50), qMax(h, 50));

        item->setParentItem(wrapper->contentItem());
        item->setParent(wrapper); // wrapper owns the item now
        wrapper->show();

        m_loaded.insert(entryFile, wrapper);
        qInfo().noquote() << "Loaded plugin (wrapped Item)" << entryFile;
        return;
    }

    showErrorWindow(entryFile,
                    QStringLiteral("Root object must be a Window or an Item, got %1.")
                        .arg(QString::fromLatin1(root->metaObject()->className())));
    delete root;
}

void PluginManager::unloadPlugin(const QString &entryFile)
{
    QObject *root = m_loaded.take(entryFile);
    if (!root)
        return;
    if (auto *window = qobject_cast<QQuickWindow *>(root))
        window->close();
    root->deleteLater();
}

// Any file in a plugin's sub-directory affects that plugin's entry. Top-level
// files are not entries; callers handle config.json separately.
QString PluginManager::entryFileForPath(const QString &path) const
{
    const QString parent = QFileInfo(path).absolutePath();
    if (QFileInfo(parent).absolutePath() == m_dir.absolutePath())
        return parent + QStringLiteral("/plugin.qml");
    return QString();
}

// <pluginsDir>/shared and anything inside it: importable by any plugin, so
// edits there reload all plugins. Never a plugin itself.
bool PluginManager::isSharedPath(const QString &path) const
{
    const QString shared = m_dir.absoluteFilePath(QStringLiteral("shared"));
    const QString p = QDir::cleanPath(path);
    return p.compare(shared, Qt::CaseInsensitive) == 0
           || p.startsWith(shared + QLatin1Char('/'), Qt::CaseInsensitive);
}

void PluginManager::onFileChanged(const QString &path)
{
    if (isSharedPath(path)) {
        m_reloadAllPending = true;
    } else if (QFileInfo(path).absoluteFilePath()
               == m_dir.absoluteFilePath(QStringLiteral("config.json"))) {
        // applyPendingReloads()'s rescan escalates if it actually changed.
        m_rescanPending = true;
    } else {
        const QString entry = entryFileForPath(path);
        if (!entry.isEmpty())
            m_pending.insert(entry);
    }
    m_debounce.start();
}

void PluginManager::onDirectoryChanged(const QString &path)
{
    if (isSharedPath(path)) {
        m_reloadAllPending = true;
    } else if (path == m_dir.absolutePath()) {
        // Plugins may have been added or removed.
        m_rescanPending = true;
    } else {
        // A file inside a plugin folder changed; "/x" stands in for it so
        // entryFileForPath sees a file path.
        const QString entry = entryFileForPath(path + QStringLiteral("/x"));
        if (!entry.isEmpty())
            m_pending.insert(entry);
    }
    m_debounce.start();
}

void PluginManager::applyPendingReloads()
{
    m_engine->clearComponentCache();

    // A changed config.json means any plugin may see different config()
    // values - only a full reload gets everything back in sync.
    if (m_registry->rescan())
        m_reloadAllPending = true;

    const QHash<QString, QString> desired = desiredEntries();

    if (m_rescanPending) {
        m_rescanPending = false;
        const QStringList loaded = m_loaded.keys();
        for (const QString &entry : loaded) {
            if (!desired.contains(entry)) {
                qInfo().noquote() << "Plugin removed or disabled:" << entry;
                unloadPlugin(entry);
            }
        }
        for (auto it = desired.constBegin(); it != desired.constEnd(); ++it) {
            if (!m_loaded.contains(it.key()))
                m_pending.insert(it.key());
        }
    }

    // An embedded plugin's embedders must reload too - and embedded entry
    // files compile under query-less URLs (Loader source), which only the
    // engine reset below evicts.
    for (const QString &entry : std::as_const(m_pending)) {
        const QString name = m_registry->nameForEntry(entry);
        if (!name.isEmpty() && m_registry->isEmbedded(name))
            m_reloadAllPending = true;
    }

    const bool fullReload = m_reloadAllPending;
    m_reloadAllPending = false;

    QSet<QString> toUnload = m_pending;
    m_pending.clear();
    if (fullReload) {
        const QStringList loaded = m_loaded.keys();
        for (const QString &entry : loaded)
            toUnload.insert(entry);
    }

    for (const QString &entry : std::as_const(toUnload))
        unloadPlugin(entry);
    flushDeferredDeletes();

    // Imported files compile under query-less URLs, so loadPlugin()'s
    // unique-URL trick misses them, and clearComponentCache() cannot evict a
    // unit anything still references (verified: edited shared files reloaded
    // stale, surviving even collectGarbage()). A fresh engine is the only
    // dependable eviction, and everything is unloaded by now.
    if (fullReload)
        resetEngine();

    QSet<QString> toLoad;
    if (fullReload) {
        for (auto it = desired.constBegin(); it != desired.constEnd(); ++it)
            toLoad.insert(it.key());
    } else {
        for (const QString &entry : std::as_const(toUnload)) {
            if (desired.contains(entry))
                toLoad.insert(entry);
        }
    }

    for (const QString &entry : std::as_const(toLoad)) {
        const QString error = desired.value(entry);
        if (!error.isEmpty()) {
            showErrorWindow(entry, error);
        } else if (QFileInfo::exists(entry)) {
            qInfo().noquote() << "Reloading plugin" << entry;
            loadPlugin(entry);
        }
    }

    // A save that replaces the file silently drops it from the watcher.
    refreshWatches();
}

void PluginManager::refreshWatches()
{
    QStringList paths;
    paths << m_dir.absolutePath();

    const QString configPath = m_dir.absoluteFilePath(QStringLiteral("config.json"));
    if (QFileInfo::exists(configPath))
        paths << configPath;

    const auto subDirs = m_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &sub : subDirs) {
        paths << sub.absoluteFilePath();
        const QDir dir(sub.absoluteFilePath());
        const auto qmlFiles = dir.entryInfoList({QStringLiteral("*.qml")}, QDir::Files);
        for (const QFileInfo &f : qmlFiles)
            paths << f.absoluteFilePath();
        const QString manifest = dir.absoluteFilePath(QStringLiteral("manifest.json"));
        if (QFileInfo::exists(manifest))
            paths << manifest;
    }

    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    m_watcher.addPaths(paths);
}

void PluginManager::showErrorWindow(const QString &entryFile, const QString &errorText)
{
    QQmlComponent errorComponent(m_engine.get());
    errorComponent.setData(kErrorQml, builtinUrl("error.qml"));
    QObject *win = errorComponent.createWithInitialProperties({
        { QStringLiteral("fileName"), QDir::toNativeSeparators(entryFile) },
        { QStringLiteral("errorText"), errorText },
    });
    if (!win) {
        qWarning().noquote() << "Internal error creating error window:"
                             << errorComponent.errorString();
        return;
    }
    QQmlEngine::setObjectOwnership(win, QQmlEngine::CppOwnership);
    win->setParent(this);

    // Keyed by entry file, so reloading it swaps in the real plugin.
    m_loaded.insert(entryFile, win);
}
