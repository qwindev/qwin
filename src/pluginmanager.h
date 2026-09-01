#pragma once

#include <QDir>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>

class QQmlApplicationEngine;
class PluginRegistry;

// Instantiates the registry's enabled plugins on a shared engine and
// hot-reloads them as their files change; a change to config.json or to an
// embedded plugin escalates to a full reload with an engine rebuild.
// Bad user QML or a bad manifest never aborts the host: both get an error
// window in place of the plugin.
class PluginManager : public QObject
{
    Q_OBJECT
public:
    PluginManager(const QString &pluginsDir, PluginRegistry *registry,
                  QObject *parent = nullptr);
    ~PluginManager() override;

    QString pluginsDir() const { return m_dir.absolutePath(); }

    // Runs between destroying an engine and creating its replacement; must
    // redo every qmlRegister* (instance singletons serve one engine only).
    void setReregisterHook(std::function<void()> hook) { m_reregisterHook = std::move(hook); }

public slots:
    void loadAll();
    void reloadAll();

private slots:
    void onFileChanged(const QString &path);
    void onDirectoryChanged(const QString &path);
    void applyPendingReloads();

private:
    // Entry files that should have a window right now -> error text, empty
    // for an enabled plugin, set for a broken folder (gets an error window).
    QHash<QString, QString> desiredEntries() const;
    void loadPlugin(const QString &entryFile);
    void unloadPlugin(const QString &entryFile);
    static void flushDeferredDeletes();
    void showErrorWindow(const QString &entryFile, const QString &errorText);
    void refreshWatches();
    QString entryFileForPath(const QString &path) const;
    bool isSharedPath(const QString &path) const;
    void resetEngine();

    // Owned so it can be rebuilt wholesale: Qt 6 cannot dependably evict
    // cached compilations of imported files. See applyPendingReloads().
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    std::function<void()> m_reregisterHook;
    PluginRegistry *m_registry;
    QDir m_dir;
    QHash<QString, QObject *> m_loaded; // entry file -> root object (plugin window, wrapper, or error window)

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;          // editors fire several fs events per save
    QSet<QString> m_pending;    // entry files scheduled for reload
    bool m_rescanPending = false;
    bool m_reloadAllPending = false; // a shared/ file changed: reload every plugin
    int m_loadSerial = 0;       // cache-busting counter, see loadPlugin()
};
