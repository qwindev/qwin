#pragma once

#include <QObject>
#include <QString>
#include <QVariant>

#include <memory>

// The `Audio` QML singleton: master volume, mute, and the output-device list
// for the default render endpoint (MMDevice + IAudioEndpointVolume). COM
// types stay behind a PIMPL, so this header pulls in no windows.h.
//
// Two signals, split like WifiApi's throughput: volumeChanged() fires many
// times a second during a drag, while devicesChanged() is rare - one signal
// for both would re-run every device-list binding per scroll tick.
class AudioApi : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY devicesChanged)
    Q_PROPERTY(int volume READ volume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY volumeChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY devicesChanged)
    // Each entry: { id: string, name: string, isDefault: bool }.
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    // False when IPolicyConfig (see the .cpp) could not be created, so QML
    // falls back to the Sound settings link rather than offering dead rows.
    Q_PROPERTY(bool canSwitchDevices READ canSwitchDevices CONSTANT)
public:
    explicit AudioApi(QObject *parent = nullptr);
    ~AudioApi() override;

    bool available() const { return m_available; }
    int volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    QString deviceName() const { return m_deviceName; }
    QVariantList devices() const { return m_devices; }
    bool canSwitchDevices() const { return m_canSwitchDevices; }

    Q_INVOKABLE void setVolume(int value);
    Q_INVOKABLE void adjustVolume(int delta);
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void setMuted(bool muted);
    Q_INVOKABLE void setDefaultDevice(const QString &id);
    // The taskbar speaker icon's own deep link; cf. WifiApi::openNetworkFlyout.
    Q_INVOKABLE void openSoundSettings();

signals:
    void volumeChanged();
    void devicesChanged();

    // Called by the .cpp's COM callbacks once marshalled onto the Qt thread.
    // Plain methods, not Q_INVOKABLE or slots, so QML never sees them.
public:
    void refreshVolumeState();
    void refreshEndpoint();
    void refreshDeviceList();

private:
    struct Impl;
    std::unique_ptr<Impl> d;

    bool m_available = false;
    int m_volume = 0;
    bool m_muted = false;
    QString m_deviceName;
    QVariantList m_devices;
    bool m_canSwitchDevices = false;
};
