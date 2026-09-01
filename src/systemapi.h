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
    Q_PROPERTY(bool batteryAvailable READ batteryAvailable NOTIFY batteryChanged)
    Q_PROPERTY(int batteryPercent READ batteryPercent NOTIFY batteryChanged)
    Q_PROPERTY(bool batteryCharging READ batteryCharging NOTIFY batteryChanged)
    Q_PROPERTY(bool acPower READ acPower NOTIFY batteryChanged)
    Q_PROPERTY(int batteryTimeLeft READ batteryTimeLeft NOTIFY batteryChanged)
    Q_PROPERTY(bool batterySaver READ batterySaver NOTIFY batteryChanged)
public:
    explicit SystemApi(const QString &pluginsDir, QObject *parent = nullptr);

    double cpuUsage() const { return m_cpuUsage; }
    double memoryUsagePercent() const { return m_memoryUsagePercent; }
    QString hostname() const;

    bool batteryAvailable() const { return m_batteryAvailable; }
    int batteryPercent() const { return m_batteryPercent; }
    bool batteryCharging() const { return m_batteryCharging; }
    bool acPower() const { return m_acPower; }
    int batteryTimeLeft() const { return m_batteryTimeLeft; }
    bool batterySaver() const { return m_batterySaver; }

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
    void batteryChanged();

private:
    void updateStats();
    void updateBattery();

    QString m_pluginsDir;
    QTimer m_timer;
    double m_cpuUsage = 0.0;
    double m_memoryUsagePercent = 0.0;
    quint64 m_lastIdle = 0;
    quint64 m_lastKernel = 0;
    quint64 m_lastUser = 0;
    void *m_savedFocus = nullptr; // HWND that had the keyboard pre-popup

    bool m_batteryAvailable = false;
    int m_batteryPercent = -1;
    bool m_batteryCharging = false;
    bool m_acPower = false;
    int m_batteryTimeLeft = -1;
    bool m_batterySaver = false;
};
