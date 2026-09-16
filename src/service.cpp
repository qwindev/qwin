#include "service.h"

QQmlListProperty<QObject> Service::data()
{
    return QQmlListProperty<QObject>(this, nullptr, &Service::appendData, &Service::countData,
                                      &Service::atData, &Service::clearData);
}

void Service::appendData(QQmlListProperty<QObject> *property, QObject *object)
{
    auto *self = static_cast<Service *>(property->object);
    object->setParent(self); // so deleting the Service deletes its Hotkeys, etc.
    self->m_data.append(object);
}

qsizetype Service::countData(QQmlListProperty<QObject> *property)
{
    return static_cast<Service *>(property->object)->m_data.count();
}

QObject *Service::atData(QQmlListProperty<QObject> *property, qsizetype index)
{
    return static_cast<Service *>(property->object)->m_data.at(index);
}

void Service::clearData(QQmlListProperty<QObject> *property)
{
    static_cast<Service *>(property->object)->m_data.clear();
}
