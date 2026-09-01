// No persistent handle, unlike WifiApi: BluetoothFindFirstRadio and
// BluetoothFindFirstDevice are cheap and self-contained, so every tick opens
// and closes what it touches. windows.h must come first - bluetoothapis.h
// and setupapi.h assume its types and do not include it themselves.
#include <windows.h>
#include <bluetoothapis.h>
#include <setupapi.h>

#include "bluetoothapi.h"

#include <QString>
#include <QVariantMap>

#include <cstdio>

namespace {

// Best-effort BLE battery level. Windows exposes it as an undocumented (but
// stable, and used by its own settings page) property on the device's PnP
// node, with no SDK constant, so the DEVPROPKEY is spelled out below.
//
// Runs per connected device on every 5 s poll and can only return a value or
// -1: no battery is the normal case for classic Bluetooth. Everything here
// is a local property read - no device I/O, nothing that blocks.
int lookupBatteryPercent(const BLUETOOTH_ADDRESS &address)
{
    // The instance ID carries the address as "DEV_AABBCCDDEEFF", most
    // significant byte first - 17 wchar_t with the NUL. Undersizing this
    // does not truncate quietly: swprintf_s routes it to the CRT
    // invalid-parameter handler, which terminates the process.
    wchar_t addressFragment[24] = {};
    swprintf_s(addressFragment, L"DEV_%02X%02X%02X%02X%02X%02X",
               address.rgBytes[5], address.rgBytes[4], address.rgBytes[3],
               address.rgBytes[2], address.rgBytes[1], address.rgBytes[0]);

    // "Bluetooth" is a PnP enumerator prefix, not a setup class, and
    // SetupDiGetClassDevs demands a null ClassGuid with DIGCF_ALLCLASSES
    // whenever Enumerator is used instead of a class GUID.
    const HDEVINFO devInfo = SetupDiGetClassDevsW(nullptr, L"Bluetooth", nullptr,
                                                  DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE)
        return -1;

    int result = -1;
    SP_DEVINFO_DATA info = {};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &info); ++i) {
        wchar_t instanceId[256] = {};
        if (!SetupDiGetDeviceInstanceIdW(devInfo, &info, instanceId, 256, nullptr))
            continue;
        if (!wcsstr(instanceId, addressFragment))
            continue; // a different device's PnP node

        static const DEVPROPKEY batteryKey = {
            {0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2
        };
        DEVPROPTYPE propType = 0;
        BYTE value = 0;
        DWORD required = 0;
        if (SetupDiGetDevicePropertyW(devInfo, &info, &batteryKey, &propType, &value,
                                      sizeof(value), &required, 0)
            && propType == DEVPROP_TYPE_BYTE && value <= 100) {
            result = int(value);
            break;
        }
        // One paired device enumerates as several PnP nodes and only one
        // carries the property, so a miss here is not the answer.
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

QString formatAddress(const BLUETOOTH_ADDRESS &address)
{
    return QStringLiteral("%1:%2:%3:%4:%5:%6")
        .arg(int(address.rgBytes[5]), 2, 16, QLatin1Char('0'))
        .arg(int(address.rgBytes[4]), 2, 16, QLatin1Char('0'))
        .arg(int(address.rgBytes[3]), 2, 16, QLatin1Char('0'))
        .arg(int(address.rgBytes[2]), 2, 16, QLatin1Char('0'))
        .arg(int(address.rgBytes[1]), 2, 16, QLatin1Char('0'))
        .arg(int(address.rgBytes[0]), 2, 16, QLatin1Char('0'))
        .toUpper();
}

} // namespace

BluetoothApi::BluetoothApi(QObject *parent)
    : QObject(parent)
{
    poll();
    m_pollTimer.setInterval(5000); // changes rarely, and there is no notification API
    connect(&m_pollTimer, &QTimer::timeout, this, [this] { poll(); });
    m_pollTimer.start();
}

BluetoothApi::~BluetoothApi() = default;

void BluetoothApi::refresh()
{
    poll();
}

// Always synchronous on the Qt main thread - constructor, timer tick or
// refresh() alike - so nothing here needs marshalling.
void BluetoothApi::poll()
{
    // Only answers "is a radio present": the search below passes hRadio =
    // NULL, so multi-radio machines see every paired device. The enumeration
    // and the radio are two handles and both need closing.
    BLUETOOTH_FIND_RADIO_PARAMS radioParams = {sizeof(radioParams)};
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND radioFind = BluetoothFindFirstRadio(&radioParams, &radio);
    if (!radioFind) {
        setState(false, {});
        return;
    }
    BluetoothFindRadioClose(radioFind);
    CloseHandle(radio);

    BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams = {};
    searchParams.dwSize = sizeof(searchParams);
    searchParams.fReturnAuthenticated = TRUE;
    searchParams.fReturnRemembered = TRUE;
    searchParams.fReturnConnected = TRUE;
    searchParams.fReturnUnknown = FALSE;
    // Must stay FALSE: an inquiry is a multi-second radio scan on the GUI
    // thread, and Remembered/Connected already cover every paired device.
    searchParams.fIssueInquiry = FALSE;
    searchParams.cTimeoutMultiplier = 0;
    searchParams.hRadio = nullptr; // NULL = search every radio, not just the first

    QVariantList devices;
    BLUETOOTH_DEVICE_INFO deviceInfo = {};
    deviceInfo.dwSize = sizeof(deviceInfo);
    HBLUETOOTH_DEVICE_FIND deviceFind = BluetoothFindFirstDevice(&searchParams, &deviceInfo);
    if (deviceFind) {
        do {
            QString name = QString::fromWCharArray(deviceInfo.szName);
            if (name.isEmpty())
                name = formatAddress(deviceInfo.Address); // some report no name

            const bool connected = deviceInfo.fConnected;
            QVariantMap entry;
            entry[QStringLiteral("name")] = name;
            entry[QStringLiteral("connected")] = connected;
            // Connected only: nothing populates the PnP property without a
            // live GATT battery service.
            entry[QStringLiteral("battery")] = connected ? lookupBatteryPercent(deviceInfo.Address) : -1;
            devices.append(entry);
        } while (BluetoothFindNextDevice(deviceFind, &deviceInfo));
        BluetoothFindDeviceClose(deviceFind);
    }

    setState(true, devices);
}

void BluetoothApi::setState(bool available, const QVariantList &devices)
{
    int connectedCount = 0;
    for (const QVariant &entry : devices) {
        if (entry.toMap().value(QStringLiteral("connected")).toBool())
            ++connectedCount;
    }

    const bool moved = available != m_available
                     || connectedCount != m_connectedCount
                     || devices != m_devices;
    m_available = available;
    m_connectedCount = connectedCount;
    m_devices = devices;
    if (moved)
        emit changed();
}
