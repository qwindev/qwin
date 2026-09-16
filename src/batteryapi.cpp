#include "batteryapi.h"

#include <windows.h>

BatteryApi::BatteryApi(QObject *parent)
    : QObject(parent)
{
    update(); // so the first frame shows real state, not the defaults

    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &BatteryApi::update);
    m_timer.start();
}

void BatteryApi::update()
{
    SYSTEM_POWER_STATUS status;
    if (!GetSystemPowerStatus(&status))
        return;

    // BatteryFlag bit 7 is "no system battery" - what desktops and some VMs
    // report instead of leaving the other fields meaningful.
    const bool available = (status.BatteryFlag & 128) == 0;
    // 255 = unknown.
    const int percent = (status.BatteryLifePercent == 255) ? -1 : int(status.BatteryLifePercent);
    // BatteryFlag 255 means "unknown" and sets every bit, charging's included.
    const bool charging = available && (status.BatteryFlag & 8) != 0;
    const bool ac = status.ACLineStatus == 1;
    // (DWORD)-1 = unknown, distinct from 0.
    const int timeLeft = (status.BatteryLifeTime == DWORD(-1)) ? -1 : int(status.BatteryLifeTime);
    // The battery-saver bit; older SDKs named this field Reserved1.
    const bool saver = status.SystemStatusFlag == 1;

    const bool moved = available != m_available
                     || percent != m_percent
                     || charging != m_charging
                     || ac != m_acPower
                     || timeLeft != m_timeLeft
                     || saver != m_saver;

    m_available = available;
    m_percent = percent;
    m_charging = charging;
    m_acPower = ac;
    m_timeLeft = timeLeft;
    m_saver = saver;

    if (moved)
        emit changed();
}
