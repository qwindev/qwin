#pragma once

#include <QList>
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
    Q_PROPERTY(QString description READ description WRITE setDescription NOTIFY descriptionChanged)
public:
    explicit Hotkey(QObject *parent = nullptr);
    ~Hotkey() override;

    QString sequence() const { return m_sequence; }
    void setSequence(const QString &sequence);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);

    bool registered() const { return m_registered; }

    QString description() const { return m_description; }
    void setDescription(const QString &description);

    // Local path of the QML file that declared this Hotkey, captured in
    // componentComplete() from the qmlContext's base URL - toLocalFile()
    // drops the "?reload=N" query the plugin manager adds, and baseUrl()
    // walks up to parent contexts, so Instantiator delegates and
    // Loader-embedded modules resolve to the file that declared them.
    QString sourceFile() const { return m_sourceFile; }

    // Every live Hotkey that has completed, regardless of enabled/registered
    // state - used by Hotkeys.list() to show chords the OS refused, which
    // never make it into the dispatcher's id -> Hotkey hash.
    static QList<Hotkey *> instances();

    void classBegin() override {}
    void componentComplete() override;

    void trigger() { emit activated(); } // called by the WM_HOTKEY dispatcher

signals:
    void activated();
    void sequenceChanged();
    void enabledChanged();
    void registeredChanged();
    void descriptionChanged();

private:
    void update();
    void unregister();

    QString m_sequence;
    QString m_description;
    QString m_sourceFile;
    bool m_enabled = true;
    bool m_registered = false;
    bool m_complete = false;
    const int m_id; // per-instance RegisterHotKey id, unique for the process
};
