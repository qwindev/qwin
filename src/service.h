#pragma once

#include <QList>
#include <QObject>
#include <QQmlListProperty>

// The `Service` QML type: a plugin root with no window, for plugins that
// only bind Hotkeys or push config into a singleton. Not QtObject: it has no
// default property, so `QtObject { Hotkey {} }` does not parse. Not Item: an
// Item root gets wrapped in a window (pluginmanager.cpp). Children are owned,
// so deleting the root on unload or reload releases their Hotkey chords.
class Service : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QQmlListProperty<QObject> data READ data CONSTANT)
    Q_CLASSINFO("DefaultProperty", "data")
public:
    explicit Service(QObject *parent = nullptr) : QObject(parent) {}

    QQmlListProperty<QObject> data();

private:
    static void appendData(QQmlListProperty<QObject> *property, QObject *object);
    static qsizetype countData(QQmlListProperty<QObject> *property);
    static QObject *atData(QQmlListProperty<QObject> *property, qsizetype index);
    static void clearData(QQmlListProperty<QObject> *property);

    QList<QObject *> m_data;
};
