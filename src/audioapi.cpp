#include "audioapi.h"

#include <QDebug>
#include <QDesktopServices>
#include <QMetaObject>
#include <QUrl>
#include <QVariantMap>

#include <windows.h>

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propsys.h>

// INITGUID gives PKEY_Device_FriendlyName real storage here instead of
// requiring a link against Propsys.lib. Local to this TU, the only one that
// needs the key, so it cannot clash with another .cpp.
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <string>

namespace {

// Changing the default output device has no public API. This undocumented
// interface has been stable since Vista and is what most volume mixers use,
// but a future release could drop it - hence every call site treats a
// missing IPolicyConfig as "switching disabled", never as a crash.
struct IPolicyConfig : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

// {870af99c-171d-4f9e-af0d-e63df40c2bc9}
const CLSID CLSID_PolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };
// {f8679f50-850a-41cf-9c72-430f290290c8}
const IID IID_IPolicyConfig = {
    0xf8679f50, 0x850a, 0x41cf, { 0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8 } };

QString friendlyNameOf(IMMDevice *dev)
{
    if (!dev)
        return QString();
    IPropertyStore *store = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &store)) || !store)
        return QString();
    PROPVARIANT pv;
    PropVariantInit(&pv);
    QString name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
        name = QString::fromWCharArray(pv.pwszVal);
    PropVariantClear(&pv);
    store->Release();
    return name;
}

QString idOf(IMMDevice *dev)
{
    if (!dev)
        return QString();
    LPWSTR raw = nullptr;
    QString id;
    if (SUCCEEDED(dev->GetId(&raw)) && raw) {
        id = QString::fromWCharArray(raw);
        CoTaskMemFree(raw);
    }
    return id;
}

// A genuine cross-thread COM callback: it fires on a thread the audio engine
// owns, unlike VirtualDesktops' WndProc, which Qt's own loop dispatches on
// the main thread. So it must marshal before touching AudioApi state or
// emitting. AudioApi is the invokeMethod context object, so a queued call
// no-ops if AudioApi dies before the notification is delivered.
class EndpointVolumeCallback : public IAudioEndpointVolumeCallback
{
public:
    EndpointVolumeCallback(AudioApi *owner, const GUID &ownContext)
        : m_owner(owner)
        , m_ownContext(ownContext)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = InterlockedDecrement(&m_refs);
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override
    {
        // Skip the echo of our own writes, which stamp m_ownContext, so a
        // drag does not fight itself. See AudioApi::setVolume.
        if (data && data->guidEventContext == m_ownContext)
            return S_OK;

        AudioApi *owner = m_owner;
        QMetaObject::invokeMethod(owner, [owner] {
            // Re-read authoritatively rather than trust the notify payload.
            owner->refreshVolumeState();
        }, Qt::QueuedConnection);
        return S_OK;
    }

private:
    AudioApi *m_owner;
    GUID m_ownContext;
    LONG m_refs = 1;
};

// Same cross-thread situation as EndpointVolumeCallback: the endpoint
// builder service calls these on its own thread, so every override does
// nothing but queue a marshalled call back.
class AudioNotificationClient : public IMMNotificationClient
{
public:
    explicit AudioNotificationClient(AudioApi *owner) : m_owner(owner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = InterlockedDecrement(&m_refs);
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
    {
        queueDeviceListRefresh();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
    {
        queueDeviceListRefresh();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
    {
        queueDeviceListRefresh();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        // Only eConsole is ever read (refreshEndpoint uses the same pair);
        // the other roles setDefaultDevice writes would just duplicate.
        if (flow != eRender || role != eConsole)
            return S_OK;
        AudioApi *owner = m_owner;
        QMetaObject::invokeMethod(owner, [owner] {
            owner->refreshEndpoint();
            owner->refreshDeviceList();
        }, Qt::QueuedConnection);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        queueDeviceListRefresh(); // covers a friendly name changing
        return S_OK;
    }

private:
    void queueDeviceListRefresh()
    {
        AudioApi *owner = m_owner;
        QMetaObject::invokeMethod(owner, [owner] { owner->refreshDeviceList(); }, Qt::QueuedConnection);
    }

    AudioApi *m_owner;
    LONG m_refs = 1;
};

} // namespace

struct AudioApi::Impl
{
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioEndpointVolume *endpointVolume = nullptr;
    EndpointVolumeCallback *volumeCallback = nullptr; // registered on endpointVolume
    AudioNotificationClient *notifyClient = nullptr;  // registered on enumerator
    IPolicyConfig *policyConfig = nullptr;            // null if creation failed

    GUID ownContext = {};       // stamped on our own volume/mute writes
    bool comOwnedByUs = false;  // our CoInitializeEx needs a CoUninitialize
    bool policyConfigWarned = false;
    bool noEndpointLogged = false; // logged once, not once per refresh
};

AudioApi::AudioApi(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    // Defensive: QApplication already initializes COM (STA) here. S_FALSE
    // means we added a refcount the destructor must balance;
    // RPC_E_CHANGED_MODE means another concurrency model is already set - the
    // apartment is still usable, but we hold no refcount to give back.
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    d->comOwnedByUs = (coHr == S_OK || coHr == S_FALSE);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
        qWarning().noquote() << "Audio: CoInitializeEx failed, Audio.available = false";
        return;
    }

    CoCreateGuid(&d->ownContext);

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void **>(&d->enumerator));
    if (FAILED(hr) || !d->enumerator) {
        qWarning().noquote() << "Audio: cannot create MMDeviceEnumerator, Audio.available = false";
        return;
    }

    d->notifyClient = new AudioNotificationClient(this);
    d->enumerator->RegisterEndpointNotificationCallback(d->notifyClient);

    // A future Windows build breaking IPolicyConfig just fails here and
    // leaves canSwitchDevices false; QML falls back to the settings link.
    hr = CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
                           IID_IPolicyConfig, reinterpret_cast<void **>(&d->policyConfig));
    m_canSwitchDevices = SUCCEEDED(hr) && d->policyConfig != nullptr;
    if (!m_canSwitchDevices) {
        d->policyConfigWarned = true;
        qWarning().noquote() << "Audio: IPolicyConfig unavailable, output device switching disabled";
    }

    refreshEndpoint();
    refreshDeviceList();
}

AudioApi::~AudioApi()
{
    if (d->enumerator && d->notifyClient) {
        d->enumerator->UnregisterEndpointNotificationCallback(d->notifyClient);
        d->notifyClient->Release();
    }
    if (d->endpointVolume) {
        if (d->volumeCallback) {
            d->endpointVolume->UnregisterControlChangeNotify(d->volumeCallback);
            d->volumeCallback->Release();
        }
        d->endpointVolume->Release();
    }
    if (d->device)
        d->device->Release();
    if (d->policyConfig)
        d->policyConfig->Release();
    if (d->enumerator)
        d->enumerator->Release();

    if (d->comOwnedByUs)
        CoUninitialize();
}

void AudioApi::refreshVolumeState()
{
    if (!d->endpointVolume)
        return;
    FLOAT scalar = 0.0f;
    BOOL muteRaw = FALSE;
    if (FAILED(d->endpointVolume->GetMasterVolumeLevelScalar(&scalar))
        || FAILED(d->endpointVolume->GetMute(&muteRaw)))
        return;

    const int newVolume = qBound(0, qRound(scalar * 100.0f), 100);
    const bool newMuted = (muteRaw != FALSE);
    if (newVolume != m_volume || newMuted != m_muted) {
        m_volume = newVolume;
        m_muted = newMuted;
        emit volumeChanged();
    }
}

void AudioApi::refreshEndpoint()
{
    // Called at startup and on every default-device change, so the previous
    // endpoint's callback and interfaces come down first.
    if (d->endpointVolume) {
        if (d->volumeCallback) {
            d->endpointVolume->UnregisterControlChangeNotify(d->volumeCallback);
            d->volumeCallback->Release();
            d->volumeCallback = nullptr;
        }
        d->endpointVolume->Release();
        d->endpointVolume = nullptr;
    }
    if (d->device) {
        d->device->Release();
        d->device = nullptr;
    }

    if (!d->enumerator)
        return;

    IMMDevice *dev = nullptr;
    if (FAILED(d->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev) {
        // No ACTIVE render endpoint - a normal state on a desktop with
        // nothing in the jack, a VM, or just after a driver update.
        // Everything stays inert and the volume plugin hides itself; logged
        // once so a vanished bar module is explicable rather than looking
        // like a misconfigured plugin.
        if (!d->noEndpointLogged) {
            d->noEndpointLogged = true;
            qInfo().noquote() << "Audio: no active output endpoint, Audio.available = false";
        }
        const bool wasAvailable = m_available;
        const bool hadName = !m_deviceName.isEmpty();
        m_available = false;
        m_deviceName.clear();
        if (wasAvailable || hadName)
            emit devicesChanged();
        return;
    }
    d->device = dev;

    IAudioEndpointVolume *ev = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&ev)))
        || !ev) {
        qWarning().noquote() << "Audio: IAudioEndpointVolume activation failed on the default endpoint";
        const bool wasAvailable = m_available;
        m_available = false;
        if (wasAvailable)
            emit devicesChanged();
        return;
    }
    d->endpointVolume = ev;
    d->volumeCallback = new EndpointVolumeCallback(this, d->ownContext);
    ev->RegisterControlChangeNotify(d->volumeCallback);

    const QString name = friendlyNameOf(dev);
    const bool changed = !m_available || name != m_deviceName;
    if (changed)
        qInfo().noquote() << "Audio: default output =" << name;
    d->noEndpointLogged = false; // a later disappearance is worth logging again
    m_deviceName = name;
    m_available = true;

    refreshVolumeState(); // primes volume/muted for the new endpoint
    if (changed)
        emit devicesChanged();
}

void AudioApi::refreshDeviceList()
{
    if (!d->enumerator)
        return;

    IMMDeviceCollection *collection = nullptr;
    if (FAILED(d->enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))
        || !collection)
        return;

    QString defaultId;
    IMMDevice *defaultDev = nullptr;
    if (SUCCEEDED(d->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDev)) && defaultDev) {
        defaultId = idOf(defaultDev);
        defaultDev->Release();
    }

    UINT count = 0;
    collection->GetCount(&count);
    QVariantList list;
    list.reserve(int(count));
    for (UINT i = 0; i < count; ++i) {
        IMMDevice *dev = nullptr;
        if (FAILED(collection->Item(i, &dev)) || !dev)
            continue;
        const QString id = idOf(dev);
        const QString name = friendlyNameOf(dev);
        dev->Release();
        if (id.isEmpty())
            continue;

        QVariantMap entry;
        entry[QStringLiteral("id")] = id;
        entry[QStringLiteral("name")] = name;
        entry[QStringLiteral("isDefault")] = (id == defaultId);
        list.append(entry);
    }
    collection->Release();

    if (list != m_devices) {
        m_devices = list;
        emit devicesChanged();
    }
}

void AudioApi::setVolume(int value)
{
    if (!d->endpointVolume)
        return;
    value = qBound(0, value, 100);
    // Our own GUID as the event context, so OnNotify ignores the echo;
    // without it a slider drag reads its own writes as external changes.
    if (FAILED(d->endpointVolume->SetMasterVolumeLevelScalar(float(value) / 100.0f, &d->ownContext))) {
        qWarning().noquote() << "Audio: SetMasterVolumeLevelScalar failed";
        return;
    }
    refreshVolumeState();
}

void AudioApi::adjustVolume(int delta)
{
    // Off the live value, so repeated scroll ticks accumulate exactly.
    setVolume(m_volume + delta);
}

void AudioApi::setMuted(bool muted)
{
    if (!d->endpointVolume)
        return;
    if (FAILED(d->endpointVolume->SetMute(muted ? TRUE : FALSE, &d->ownContext))) {
        qWarning().noquote() << "Audio: SetMute failed";
        return;
    }
    refreshVolumeState();
}

void AudioApi::toggleMute()
{
    setMuted(!m_muted);
}

void AudioApi::setDefaultDevice(const QString &id)
{
    if (!d->policyConfig) {
        if (!d->policyConfigWarned) {
            d->policyConfigWarned = true;
            qWarning().noquote() << "Audio: setDefaultDevice ignored, IPolicyConfig unavailable";
        }
        return;
    }
    if (id.isEmpty())
        return;

    const std::wstring wid = id.toStdWString();
    // All three: apps query different roles (players eMultimedia, chat apps
    // eCommunications), and leaving one behind is why "I switched but Zoom
    // stayed on the old speakers" happens.
    bool anyOk = false;
    const ERole roles[] = { eConsole, eMultimedia, eCommunications };
    for (ERole role : roles) {
        if (SUCCEEDED(d->policyConfig->SetDefaultEndpoint(wid.c_str(), role)))
            anyOk = true;
    }
    if (!anyOk)
        qWarning().noquote() << "Audio: SetDefaultEndpoint failed for" << id;
    // No refresh here: a successful switch fires OnDefaultDeviceChanged,
    // which queues one for us.
}

void AudioApi::openSoundSettings()
{
    QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:sound")));
}
