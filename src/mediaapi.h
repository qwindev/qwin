#pragma once

#include <QObject>
#include <QString>

#include <memory>

// The `Media` QML singleton: whatever app owns the system "now playing"
// state, via the WinRT Global System Media Transport Controls. No per-app
// integration and no poll - GSMTC pushes CurrentSessionChanged /
// MediaPropertiesChanged / PlaybackInfoChanged.
//
// All WinRT state lives in Impl, defined only in the .cpp, so this header
// keeps the no-winrt/-in-headers rule with a PIMPL rather than the
// void*-handle trick the plain-Win32 singletons use.
//
// One `changed` signal covers every property: media state moves rarely, so a
// per-property split (as in WifiApi's per-second throughput) buys nothing.
class MediaApi : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString artist READ artist NOTIFY changed)
    Q_PROPERTY(QString album READ album NOTIFY changed)
    Q_PROPERTY(QString sourceApp READ sourceApp NOTIFY changed)
    Q_PROPERTY(bool playing READ playing NOTIFY changed)
    Q_PROPERTY(bool canPlayPause READ canPlayPause NOTIFY changed)
    Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY changed)
    Q_PROPERTY(bool canGoPrevious READ canGoPrevious NOTIFY changed)
public:
    explicit MediaApi(QObject *parent = nullptr);
    ~MediaApi() override;

    bool available() const { return m_available; }
    QString title() const { return m_title; }
    QString artist() const { return m_artist; }
    QString album() const { return m_album; }
    QString sourceApp() const { return m_sourceApp; }
    bool playing() const { return m_playing; }
    bool canPlayPause() const { return m_canPlayPause; }
    bool canGoNext() const { return m_canGoNext; }
    bool canGoPrevious() const { return m_canGoPrevious; }

    // Fire-and-forget; no-op with no session, or when the session refuses.
    Q_INVOKABLE void playPause();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();

Q_SIGNALS:
    // Q_SIGNALS/Q_EMIT, not the bare keywords: this class lives next to
    // winrt/* headers, and Qt's unqualified macros are a known collision
    // source. Nothing collided yet; the qualified form costs nothing.
    void changed();

private:
    // Manager::RequestAsync(), once, from the constructor.
    void requestManager();
    // Re-reads the current session, revoking the old tokens and wiring the
    // new ones. Always runs on the GUI thread.
    void onCurrentSessionChanged();
    void refreshMediaProperties(); // async
    void refreshPlaybackInfo();    // plain property reads, not async
    // The only writer of m_*: every method above hands it a full snapshot in
    // Qt types, and changed() is emitted only if something moved.
    void applySnapshot(bool available, const QString &title, const QString &artist,
                        const QString &album, const QString &sourceApp, bool playing,
                        bool canPlayPause, bool canGoNext, bool canGoPrevious);

    struct Impl;
    std::unique_ptr<Impl> d;

    bool m_available = false;
    QString m_title;
    QString m_artist;
    QString m_album;
    QString m_sourceApp;
    bool m_playing = false;
    bool m_canPlayPause = false;
    bool m_canGoNext = false;
    bool m_canGoPrevious = false;
};
