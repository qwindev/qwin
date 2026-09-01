#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUuid>

// The `Wifi` QML singleton, deliberately minimal: a slow poll of the current
// connection via WlanQueryInterface plus a 1 s sample of the adapter's byte
// counters. Picking a network, passwords and the radio toggle are delegated
// to the native flyout (openNetworkFlyout); no WLAN interface means
// available == false.
//
// Windows gates the SSID-bearing query behind the Location capability, so
// `connected` comes from the interface state (never gated) and
// `detailsAvailable` says whether ssid/signalPercent could be read.
class WifiApi : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    // False while Location services are off: `connected` stays accurate, but
    // ssid is empty and signalPercent 0. Throughput is not gated.
    Q_PROPERTY(bool detailsAvailable READ detailsAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString ssid READ ssid NOTIFY stateChanged)
    // Not named `signal`: that is a QML keyword.
    Q_PROPERTY(int signalPercent READ signalPercent NOTIFY stateChanged)
    // Own signal: on stateChanged() this would re-evaluate every
    // ssid/connected binding in every plugin once a second.
    Q_PROPERTY(qreal rxBytesPerSec READ rxBytesPerSec NOTIFY throughputChanged)
    Q_PROPERTY(qreal txBytesPerSec READ txBytesPerSec NOTIFY throughputChanged)
public:
    explicit WifiApi(QObject *parent = nullptr);
    ~WifiApi() override;

    bool available() const { return m_available; }
    bool connected() const { return m_connected; }
    bool detailsAvailable() const { return m_detailsAvailable; }
    QString ssid() const { return m_ssid; }
    int signalPercent() const { return m_signalPercent; }
    qreal rxBytesPerSec() const { return m_rxBytesPerSec; }
    qreal txBytesPerSec() const { return m_txBytesPerSec; }

    // The same available-networks popup the taskbar tray opens.
    Q_INVOKABLE void openNetworkFlyout();

signals:
    void stateChanged();
    void throughputChanged();

private:
    void refreshState();
    void refreshThroughput();

    bool m_available = false;
    bool m_connected = false;
    bool m_detailsAvailable = false;
    bool m_deniedWarned = false; // Location warning is logged once
    QString m_ssid;
    int m_signalPercent = 0;

    QTimer m_pollTimer;
    void *m_handle = nullptr; // WLAN client HANDLE
    QUuid m_interfaceGuid;    // first interface; multi-adapter is out of scope

    // Throughput: deltas of the adapter's octet counters over real elapsed
    // time. m_luid is a NET_LUID::Value kept as a plain integer, so this
    // header stays clear of netioapi.h (see the .cpp's include order).
    QTimer m_throughputTimer;
    QElapsedTimer m_sinceLastSample;
    quint64 m_luid = 0;
    quint64 m_prevInOctets = 0;
    quint64 m_prevOutOctets = 0;
    bool m_haveSample = false;
    qreal m_rxBytesPerSec = 0;
    qreal m_txBytesPerSec = 0;
};
