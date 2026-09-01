// Include order is load-bearing, twice over:
//   - winsock2.h before windows.h: iphlpapi/netioapi pull in ws2def.h, which
//     collides with the winsock.h windows.h includes on its own (C1189).
//   - ws2ipdef.h before netioapi.h, which hides MIB_IF_ROW2 and the rest of
//     the interface-table API behind `#ifdef _WS2IPDEF_`. Without it the
//     header parses cleanly and the type simply does not exist.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <windows.h>
#include <wlanapi.h>

#include "wifiapi.h"

#include <QDebug>
#include <QDesktopServices>
#include <QUrl>

WifiApi::WifiApi(QObject *parent)
    : QObject(parent)
{
    DWORD negotiated = 0;
    HANDLE handle = nullptr;
    if (WlanOpenHandle(2, nullptr, &negotiated, &handle) != ERROR_SUCCESS) {
        qInfo().noquote() << "Wifi: WLAN service unavailable, Wifi.available = false";
        return;
    }

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (WlanEnumInterfaces(handle, nullptr, &interfaces) != ERROR_SUCCESS
        || interfaces->dwNumberOfItems == 0) {
        if (interfaces)
            WlanFreeMemory(interfaces);
        WlanCloseHandle(handle, nullptr);
        qInfo().noquote() << "Wifi: no WLAN interface, Wifi.available = false";
        return;
    }
    const GUID guid = interfaces->InterfaceInfo[0].InterfaceGuid;
    m_interfaceGuid = QUuid(guid);
    WlanFreeMemory(interfaces);

    m_handle = handle;
    m_available = true;

    refreshState();
    m_pollTimer.setInterval(3000);
    connect(&m_pollTimer, &QTimer::timeout, this, [this] { refreshState(); });
    m_pollTimer.start();

    // The byte counters are addressed by LUID, not GUID. Throughput is a
    // bonus, so a failed lookup leaves the rest of the singleton working.
    NET_LUID luid = {};
    if (ConvertInterfaceGuidToLuid(&guid, &luid) == NO_ERROR) {
        m_luid = luid.Value;
        // Faster than the state poll: a 3 s average flattens bursts.
        m_throughputTimer.setInterval(1000);
        connect(&m_throughputTimer, &QTimer::timeout, this, [this] { refreshThroughput(); });
        m_throughputTimer.start();
    } else {
        qWarning().noquote() << "Wifi: interface LUID lookup failed, throughput stays at 0";
    }
}

WifiApi::~WifiApi()
{
    if (m_handle)
        WlanCloseHandle(m_handle, nullptr);
}

void WifiApi::refreshState()
{
    GUID guid = m_interfaceGuid;

    bool connected = false;
    bool detailsAvailable = false;
    QString ssid;
    int signalPercent = 0;
    DWORD size = 0;
    PVOID data = nullptr;

    // Connectivity from the interface state: it carries no SSID, so unlike
    // the query below it is not gated behind the Location capability.
    if (WlanQueryInterface(m_handle, &guid, wlan_intf_opcode_interface_state,
                           nullptr, &size, &data, nullptr) == ERROR_SUCCESS) {
        connected = *static_cast<WLAN_INTERFACE_STATE *>(data) == wlan_interface_state_connected;
        WlanFreeMemory(data);
    }

    size = 0;
    data = nullptr;
    // ERROR_INVALID_STATE while disconnected, ERROR_ACCESS_DENIED with
    // Location off. Either way the defaults stand and `connected` above keeps
    // its own answer.
    const DWORD result = WlanQueryInterface(m_handle, &guid, wlan_intf_opcode_current_connection,
                                            nullptr, &size, &data, nullptr);
    if (result == ERROR_SUCCESS) {
        auto *attrs = static_cast<PWLAN_CONNECTION_ATTRIBUTES>(data);
        if (attrs->isState == wlan_interface_state_connected) {
            connected = true;
            detailsAvailable = true;
            // Raw bytes; UTF-8 is the de-facto convention.
            const DOT11_SSID &raw = attrs->wlanAssociationAttributes.dot11Ssid;
            ssid = QString::fromUtf8(reinterpret_cast<const char *>(raw.ucSSID),
                                     int(raw.uSSIDLength));
            signalPercent = int(attrs->wlanAssociationAttributes.wlanSignalQuality);
        }
        WlanFreeMemory(data);
    } else if (result == ERROR_ACCESS_DENIED && !m_deniedWarned) {
        m_deniedWarned = true;
        qWarning().noquote() << "Wifi: network name and signal strength need Windows Location "
                                "services (Settings > Privacy & security > Location, including "
                                "\"Let desktop apps access your location\"); reporting "
                                "connectivity only";
    }

    if (connected != m_connected || detailsAvailable != m_detailsAvailable || ssid != m_ssid
        || signalPercent != m_signalPercent) {
        m_connected = connected;
        m_detailsAvailable = detailsAvailable;
        m_ssid = ssid;
        m_signalPercent = signalPercent;
        qInfo().noquote() << QStringLiteral("Wifi: state connected=%1 details=%2 ssid=%3 signal=%4")
                                 .arg(connected).arg(detailsAvailable).arg(ssid).arg(signalPercent);
        emit stateChanged();
    }
}

void WifiApi::refreshThroughput()
{
    MIB_IF_ROW2 row = {};
    row.InterfaceLuid.Value = m_luid;

    qreal rx = 0;
    qreal tx = 0;
    if (GetIfEntry2(&row) == NO_ERROR) {
        const quint64 in = row.InOctets;
        const quint64 out = row.OutOctets;
        if (m_haveSample) {
            // Real elapsed time, not the nominal 1000 ms: the timer drifts
            // under load and a flat divisor makes the reading spiky.
            const qint64 elapsedMs = m_sinceLastSample.restart();
            if (elapsedMs > 0) {
                // An adapter reset restarts the counters; unsigned
                // subtraction would read that as a huge spike.
                const quint64 inDelta = in >= m_prevInOctets ? in - m_prevInOctets : 0;
                const quint64 outDelta = out >= m_prevOutOctets ? out - m_prevOutOctets : 0;
                rx = qreal(inDelta) * 1000.0 / qreal(elapsedMs);
                tx = qreal(outDelta) * 1000.0 / qreal(elapsedMs);
            }
        } else {
            // Nothing to diff against; the first tick necessarily reads 0.
            m_sinceLastSample.start();
            m_haveSample = true;
        }
        m_prevInOctets = in;
        m_prevOutOctets = out;
    } else {
        // Adapter disabled or removed: re-baseline, so coming back does not
        // surface the whole gap as one spike.
        m_haveSample = false;
    }

    if (rx != m_rxBytesPerSec || tx != m_txBytesPerSec) {
        m_rxBytesPerSec = rx;
        m_txBytesPerSec = tx;
        emit throughputChanged();
    }
}

void WifiApi::openNetworkFlyout()
{
    QDesktopServices::openUrl(QUrl(QStringLiteral("ms-availablenetworks:")));
}
