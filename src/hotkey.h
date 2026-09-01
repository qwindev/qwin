#pragma once

#include <QObject>
#include <QQmlParserStatus>
#include <QString>

// The `Hotkey` QML type: a system-wide chord via RegisterHotKey that emits
// activated() whoever has focus. The chord is grabbed exclusively; if
// another application already owns it, registration warns and `registered`
// stays false, leaving the plugin itself working.
class Hotkey : public QObject, public QQmlParserStatus
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(QString sequence READ sequence WRITE setSequence NOTIFY sequenceChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool registered READ registered NOTIFY registeredChanged)
public:
    explicit Hotkey(QObject *parent = nullptr);
    ~Hotkey() override;

    QString sequence() const { return m_sequence; }
    void setSequence(const QString &sequence);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);

    bool registered() const { return m_registered; }

    void classBegin() override {}
    void componentComplete() override;

    void trigger() { emit activated(); } // called by the WM_HOTKEY dispatcher

signals:
    void activated();
    void sequenceChanged();
    void enabledChanged();
    void registeredChanged();

private:
    void update();
    void unregister();

    QString m_sequence;
    bool m_enabled = true;
    bool m_registered = false;
    bool m_complete = false;
    const int m_id; // per-instance RegisterHotKey id, unique for the process
};
