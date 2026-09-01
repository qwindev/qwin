// winrt/* before Qt: mediaapi.h pulls in <QObject> and its unqualified
// `signals`/`slots`/`emit` macros. Nothing in GSMTC uses those identifiers
// today; this order means a future header update fails to compile rather
// than silently picking one meaning of `emit`.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

#include "mediaapi.h"

#include <QDebug>
#include <QMetaObject>

#include <atomic>
#include <memory>

namespace {

using winrt::Windows::Foundation::AsyncStatus;
using GsmtcManager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using GsmtcSession = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using GsmtcMediaProperties = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties;
using GsmtcPlaybackStatus = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

// Spelled out once: winrt's projected types always support `Type{ nullptr }`
// construction, but plain `= nullptr` assignment only for some shapes.
GsmtcSession nullSession()
{
    return GsmtcSession{ nullptr };
}

QString toQString(const winrt::hstring &s)
{
    return QString::fromWCharArray(s.c_str());
}

} // namespace

struct MediaApi::Impl
{
    // Every continuation and event handler below captures this rather than a
    // raw MediaApi*. They run on thread-pool threads and may still be queued
    // at exit: revoking a token does not wait for a callback already in
    // flight. ~MediaApi() nulls it first thing, so a late callback finds
    // nullptr; the shared_ptr keeps the box alive as long as any in-flight
    // callback holds a copy. This narrows, but cannot fully close, the gap
    // between a callback reading the pointer and destruction finishing -
    // that would need the destructor to block on WinRT drain-completion.
    std::shared_ptr<std::atomic<MediaApi *>> guard;

    GsmtcManager manager{ nullptr };
    GsmtcSession session{ nullptr };
    winrt::event_token currentSessionToken{};
    winrt::event_token mediaPropertiesToken{};
    winrt::event_token playbackInfoToken{};
    bool haveManager = false;
    bool haveSession = false;
};

MediaApi::MediaApi(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    d->guard = std::make_shared<std::atomic<MediaApi *>>(this);

    // Qt's platform plugin has already made this thread an STA, so
    // RPC_E_CHANGED_MODE here means "already the apartment we wanted", not a
    // failure. Anything else degrades to available == false.
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (const winrt::hresult_error &) {
    }

    requestManager();
}

MediaApi::~MediaApi()
{
    // Cuts off every queued callback before anything below runs; see Impl::guard.
    d->guard->store(nullptr);

    try {
        if (d->haveSession) {
            d->session.MediaPropertiesChanged(d->mediaPropertiesToken);
            d->session.PlaybackInfoChanged(d->playbackInfoToken);
        }
        if (d->haveManager)
            d->manager.CurrentSessionChanged(d->currentSessionToken);
    } catch (const winrt::hresult_error &) {
        // The COM objects can already be gone at shutdown (the last media
        // app closed); nothing left to revoke.
    }
}

void MediaApi::requestManager()
{
    try {
        auto op = GsmtcManager::RequestAsync();
        auto guard = d->guard;
        // Completes on a thread-pool thread, never synchronously and never
        // back here, STA notwithstanding: a WinRT async guarantee, not a COM
        // one. Hence the guard and the queued hop.
        op.Completed([guard](auto const &sender, AsyncStatus status) {
            if (status != AsyncStatus::Completed)
                return;
            try {
                GsmtcManager manager = sender.GetResults();
                MediaApi *self = guard->load();
                if (!self)
                    return;
                QMetaObject::invokeMethod(self, [guard, manager] {
                    MediaApi *self2 = guard->load();
                    if (!self2)
                        return;
                    self2->d->manager = manager;
                    self2->d->haveManager = true;

                    auto innerGuard = self2->d->guard;
                    self2->d->currentSessionToken = self2->d->manager.CurrentSessionChanged(
                        [innerGuard](GsmtcManager const &, auto const &) {
                            MediaApi *s = innerGuard->load();
                            if (!s)
                                return;
                            QMetaObject::invokeMethod(s, [innerGuard] {
                                MediaApi *s2 = innerGuard->load();
                                if (s2)
                                    s2->onCurrentSessionChanged();
                            }, Qt::QueuedConnection);
                        });

                    self2->onCurrentSessionChanged(); // whatever is already playing
                }, Qt::QueuedConnection);
            } catch (const winrt::hresult_error &) {
                // Throws when the manager could not be created at all (no
                // Media Feature Pack); available stays false.
            }
        });
    } catch (const winrt::hresult_error &) {
        qWarning().noquote() << "Media: RequestAsync failed, Media.available stays false";
    }
}

void MediaApi::onCurrentSessionChanged()
{
    // d->session and the tokens are mutated only here and in
    // requestManager()'s continuation, both on the GUI thread via the queued
    // hop - so this never races the destructor or another copy of itself.
    if (d->haveSession) {
        try {
            d->session.MediaPropertiesChanged(d->mediaPropertiesToken);
            d->session.PlaybackInfoChanged(d->playbackInfoToken);
        } catch (const winrt::hresult_error &) {
        }
        d->session = nullSession();
        d->haveSession = false;
    }

    GsmtcSession session{ nullptr };
    try {
        session = d->manager.GetCurrentSession();
    } catch (const winrt::hresult_error &) {
    }

    if (!session) {
        // Nothing playing anywhere: the common case, not a warning.
        applySnapshot(false, QString(), QString(), QString(), QString(),
                      false, false, false, false);
        return;
    }

    d->session = session;
    d->haveSession = true;

    auto guard = d->guard;
    d->mediaPropertiesToken = d->session.MediaPropertiesChanged(
        [guard](GsmtcSession const &, auto const &) {
            MediaApi *self = guard->load();
            if (!self)
                return;
            QMetaObject::invokeMethod(self, [guard] {
                MediaApi *self2 = guard->load();
                if (self2)
                    self2->refreshMediaProperties();
            }, Qt::QueuedConnection);
        });
    d->playbackInfoToken = d->session.PlaybackInfoChanged(
        [guard](GsmtcSession const &, auto const &) {
            MediaApi *self = guard->load();
            if (!self)
                return;
            QMetaObject::invokeMethod(self, [guard] {
                MediaApi *self2 = guard->load();
                if (self2)
                    self2->refreshPlaybackInfo();
            }, Qt::QueuedConnection);
        });

    refreshPlaybackInfo();
    refreshMediaProperties();
}

void MediaApi::refreshPlaybackInfo()
{
    if (!d->haveSession)
        return;
    try {
        const auto info = d->session.GetPlaybackInfo();
        const bool playing = info.PlaybackStatus() == GsmtcPlaybackStatus::Playing;
        const auto controls = info.Controls();
        // Play and pause are enabled separately, but playPause() is one
        // toggle call, so either is enough to offer it.
        const bool canPlayPause = controls.IsPlayEnabled() || controls.IsPauseEnabled();
        const bool canNext = controls.IsNextEnabled();
        const bool canPrev = controls.IsPreviousEnabled();
        const QString app = toQString(d->session.SourceAppUserModelId());

        applySnapshot(true, m_title, m_artist, m_album, app,
                      playing, canPlayPause, canNext, canPrev);
    } catch (const winrt::hresult_error &) {
        // Session went away since the event fired; the CurrentSessionChanged
        // that follows sorts it out.
    }
}

void MediaApi::refreshMediaProperties()
{
    if (!d->haveSession)
        return;
    try {
        auto op = d->session.TryGetMediaPropertiesAsync();
        auto guard = d->guard;
        op.Completed([guard](auto const &sender, AsyncStatus status) {
            if (status != AsyncStatus::Completed)
                return;
            try {
                GsmtcMediaProperties props = sender.GetResults();
                MediaApi *self = guard->load();
                if (!self)
                    return;
                QMetaObject::invokeMethod(self, [guard, props] {
                    MediaApi *self2 = guard->load();
                    if (!self2 || !self2->d->haveSession)
                        return; // session changed again before this arrived
                    self2->applySnapshot(true, toQString(props.Title()), toQString(props.Artist()),
                                         toQString(props.AlbumTitle()), self2->m_sourceApp,
                                         self2->m_playing, self2->m_canPlayPause,
                                         self2->m_canGoNext, self2->m_canGoPrevious);
                }, Qt::QueuedConnection);
            } catch (const winrt::hresult_error &) {
            }
        });
    } catch (const winrt::hresult_error &) {
    }
}

void MediaApi::applySnapshot(bool available, const QString &title, const QString &artist,
                              const QString &album, const QString &sourceApp, bool playing,
                              bool canPlayPause, bool canGoNext, bool canGoPrevious)
{
    const bool moved = available != m_available || title != m_title || artist != m_artist
                     || album != m_album || sourceApp != m_sourceApp || playing != m_playing
                     || canPlayPause != m_canPlayPause || canGoNext != m_canGoNext
                     || canGoPrevious != m_canGoPrevious;

    m_available = available;
    m_title = title;
    m_artist = artist;
    m_album = album;
    m_sourceApp = sourceApp;
    m_playing = playing;
    m_canPlayPause = canPlayPause;
    m_canGoNext = canGoNext;
    m_canGoPrevious = canGoPrevious;

    // Nothing here polls, so this dedup is not about tick spam: it is about
    // an event firing with metadata identical to what we already have.
    if (moved)
        Q_EMIT changed();
}

void MediaApi::playPause()
{
    if (!d->haveSession)
        return;
    try {
        // Toggle rather than tracking state and picking TryPlay/TryPause;
        // PlaybackInfoChanged reports the outcome. The empty Completed
        // handler discards the result on purpose - dropping `op` does not
        // cancel the operation. What must NOT appear here is a blocking
        // get(): this is the GUI thread's STA, where that deadlocks.
        auto op = d->session.TryTogglePlayPauseAsync();
        op.Completed([](auto const &, AsyncStatus) {});
    } catch (const winrt::hresult_error &) {
    }
}

void MediaApi::next()
{
    if (!d->haveSession)
        return;
    try {
        auto op = d->session.TrySkipNextAsync();
        op.Completed([](auto const &, AsyncStatus) {});
    } catch (const winrt::hresult_error &) {
    }
}

void MediaApi::previous()
{
    if (!d->haveSession)
        return;
    try {
        auto op = d->session.TrySkipPreviousAsync();
        op.Completed([](auto const &, AsyncStatus) {});
    } catch (const winrt::hresult_error &) {
    }
}
