#pragma once

#include <QObject>

// The `Power` QML singleton: the caffeine plugin's keep-awake toggle plus
// the session actions (lock/sign out/sleep/hibernate/restart/shut down) the
// power plugin offers. Confirmation is the QML popup's two-click job; this
// class stays dumb and acts immediately.
class PowerApi : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool keepAwake READ keepAwake WRITE setKeepAwake NOTIFY keepAwakeChanged)
    // So the popup can omit the entry rather than offer an action that fails.
    Q_PROPERTY(bool hibernateAvailable READ hibernateAvailable CONSTANT)
public:
    explicit PowerApi(QObject *parent = nullptr);
    ~PowerApi() override;

    bool keepAwake() const { return m_keepAwake; }
    void setKeepAwake(bool awake);

    bool hibernateAvailable() const { return m_hibernateAvailable; }

    Q_INVOKABLE void lock();
    Q_INVOKABLE void sleep();
    Q_INVOKABLE void hibernate();
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void restart();
    Q_INVOKABLE void shutdown();

signals:
    void keepAwakeChanged();

private:
    // Reboot/shutdown (not logoff) need this first; see the .cpp for why the
    // return value alone does not prove it worked.
    bool enableShutdownPrivilege() const;

    bool m_keepAwake = false;
    bool m_hibernateAvailable = false;
};
