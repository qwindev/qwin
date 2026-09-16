#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <functional>

// The `Hotkeys` QML singleton: a snapshot of every declared `Hotkey`, read
// on demand. Deliberately no change signal - a reload destroys and
// recreates every Hotkey at once, and a caller that reads the list when it
// opens never sees that churn.
class HotkeysApi : public QObject
{
    Q_OBJECT
public:
    // pluginForFile resolves a Hotkey's source file to the plugin name that
    // owns its folder (PluginRegistry::nameForFile) - taken as a callback
    // rather than a dependency so this stays independent of
    // pluginregistry.*; the two are wired together only in main.cpp.
    explicit HotkeysApi(std::function<QString(const QString &file)> pluginForFile,
                        QObject *parent = nullptr);

    // Every enabled Hotkey with a non-empty sequence, as
    // { sequence, plugin, description, registered }, sorted by plugin then
    // by displayed sequence (both case-insensitive).
    Q_INVOKABLE QVariantList list() const;

private:
    std::function<QString(const QString &file)> m_pluginForFile;
};
