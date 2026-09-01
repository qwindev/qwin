#pragma once

#include <QHash>
#include <QObject>
#include <QString>

// The `ActiveWindow` QML singleton: title, process, app label and icon of
// whichever window has the keyboard. Purely event-driven; see the .cpp.
// `available` follows windowfocus::isFocusableAppWindow(), so the desktop,
// system flyouts and our own panels never surface here.
class ForegroundWindow : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString processName READ processName NOTIFY changed)
    Q_PROPERTY(QString appName READ appName NOTIFY changed)
    Q_PROPERTY(QString iconSource READ iconSource NOTIFY changed)
public:
    explicit ForegroundWindow(QObject *parent = nullptr);
    ~ForegroundWindow() override;

    bool available() const { return m_available; }
    QString title() const { return m_title; }
    QString processName() const { return m_processName; }
    QString appName() const { return m_appName; }
    QString iconSource() const { return m_iconSource; }

    // Entry points for the .cpp's WinEvent callback, routed through a
    // file-static instance pointer (main.cpp constructs exactly one). Plain
    // methods, not slots: only that callback should drive them, and QML
    // reaches nothing that is not Q_INVOKABLE or a Q_PROPERTY.
    void onForegroundChanged();
    void onNameChanged();
    void onMinimizeStateChanged();

signals:
    void changed();

private:
    void refresh();
    QString iconDataUrl(const QString &exePath, void *hwnd);

    bool m_available = false;
    QString m_title;
    QString m_processName;
    QString m_appName;
    QString m_iconSource;
    void *m_hwnd = nullptr; // the HWND the properties above describe

    // exe path -> data: URL, capped; see iconDataUrl().
    QHash<QString, QString> m_iconCache;

    void *m_hookForeground = nullptr; // EVENT_SYSTEM_FOREGROUND
    void *m_hookMinimize = nullptr;   // EVENT_SYSTEM_MINIMIZESTART..MINIMIZEEND
    void *m_hookNameChange = nullptr; // EVENT_OBJECT_NAMECHANGE
};
