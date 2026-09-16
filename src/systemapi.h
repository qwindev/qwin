#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

// The `System` QML singleton. Stats refresh once per second.
class SystemApi : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double cpuUsage READ cpuUsage NOTIFY statsChanged)
    Q_PROPERTY(double memoryUsagePercent READ memoryUsagePercent NOTIFY statsChanged)
    Q_PROPERTY(QString hostname READ hostname CONSTANT)
public:
    explicit SystemApi(const QString &pluginsDir, QObject *parent = nullptr);

    double cpuUsage() const { return m_cpuUsage; }
    double memoryUsagePercent() const { return m_memoryUsagePercent; }
    QString hostname() const;

    Q_INVOKABLE QString readTextFile(const QString &path) const;

    // Opens the Start menu - or closes it again, since it toggles.
    Q_INVOKABLE void openStartMenu();

    // Focus handover, bracketing a popup. A popup activates itself, taking
    // the keyboard from whatever the user was in, and hiding it does not give
    // it back: the foreground lands on the panel owning the popup, which has
    // nothing to type into (measured under Qt::Tool and Qt::Popup alike).
    // restoreFocus() hands over only under the rule in the .cpp.
    Q_INVOKABLE void rememberFocus();
    Q_INVOKABLE void restoreFocus();

signals:
    void statsChanged();

private:
    void updateStats();

    QString m_pluginsDir;
    QTimer m_timer;
    double m_cpuUsage = 0.0;
    double m_memoryUsagePercent = 0.0;
    quint64 m_lastIdle = 0;
    quint64 m_lastKernel = 0;
    quint64 m_lastUser = 0;
    void *m_savedFocus = nullptr; // HWND that had the keyboard pre-popup
};
