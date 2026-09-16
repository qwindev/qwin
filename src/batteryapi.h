#pragma once

#include <QObject>
#include <QTimer>

// The `Battery` QML singleton: charge, AC/charging state, time remaining and
// battery saver, polled once a second via GetSystemPowerStatus.
class BatteryApi : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(int percent READ percent NOTIFY changed)
    Q_PROPERTY(bool charging READ charging NOTIFY changed)
    Q_PROPERTY(bool acPower READ acPower NOTIFY changed)
    Q_PROPERTY(int timeLeft READ timeLeft NOTIFY changed)
    Q_PROPERTY(bool saver READ saver NOTIFY changed)
public:
    explicit BatteryApi(QObject *parent = nullptr);

    bool available() const { return m_available; }
    int percent() const { return m_percent; }
    bool charging() const { return m_charging; }
    bool acPower() const { return m_acPower; }
    int timeLeft() const { return m_timeLeft; }
    bool saver() const { return m_saver; }

signals:
    // Only on a real change: battery rarely moves, and re-emitting every
    // poll would re-run every binding once a second for nothing.
    void changed();

private:
    void update();

    QTimer m_timer;
    bool m_available = false;
    int m_percent = -1;
    bool m_charging = false;
    bool m_acPower = false;
    int m_timeLeft = -1;
    bool m_saver = false;
};
