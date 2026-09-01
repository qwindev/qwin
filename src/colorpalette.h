#pragma once

#include <QFileSystemWatcher>
#include <QQmlPropertyMap>
#include <QTimer>

// The `Colors` QML singleton. Every key in <pluginsDir>/colors.json becomes
// a per-key-notifying color property, so `Colors.accent` is a live binding
// and editing the watched file recolors all plugins without a reload. The
// built-in defaults always sit underneath, and seed the file on first run.
class ColorPalette : public QQmlPropertyMap
{
    Q_OBJECT
public:
    explicit ColorPalette(const QString &pluginsDir, QObject *parent = nullptr);

protected:
    // One-way (file -> plugins): writes from QML are rejected.
    QVariant updateValue(const QString &key, const QVariant &input) override;

private slots:
    void reload();

private:
    void writeDefaultFile();
    void refreshWatch();

    QString m_filePath;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
