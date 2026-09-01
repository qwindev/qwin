#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>

// The `Bluetooth` QML singleton, scoped like WifiApi: what is paired and
// what is connected. Pairing, connect/disconnect and scanning are left to
// Windows' own settings page, which the plugin deep-links to.
class BluetoothApi : public QObject
{
    Q_OBJECT
    // BluetoothFindFirstRadio found radio HARDWARE, so this is expected to
    // stay true with Bluetooth merely switched off in quick settings - only
    // an absent or Device Manager-disabled adapter turns it false. Read off
    // the docs; not reproduced here.
    Q_PROPERTY(bool available READ available NOTIFY changed)
    // How many entries in `devices` currently have connected == true.
    Q_PROPERTY(int connectedCount READ connectedCount NOTIFY changed)
    // Each entry: { name: string, connected: bool, battery: int }, where
    // battery is 0-100 or -1 when unknown - the normal case when
    // unsupported, disconnected, or the lookup failed.
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
public:
    explicit BluetoothApi(QObject *parent = nullptr);
    ~BluetoothApi() override;

    bool available() const { return m_available; }
    int connectedCount() const { return m_connectedCount; }
    QVariantList devices() const { return m_devices; }

    // The popup calls this on open, so the list is not up to 5 s stale.
    Q_INVOKABLE void refresh();

signals:
    // Only on a real change: re-emitting per poll would re-run every binding
    // in the popup every 5 s for nothing.
    void changed();

private:
    void poll();
    void setState(bool available, const QVariantList &devices);

    QTimer m_pollTimer;
    bool m_available = false;
    int m_connectedCount = 0;
    QVariantList m_devices;
};
