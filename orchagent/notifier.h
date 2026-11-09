#pragma once

#include "orch.h"

class NotificationDbHolder
{
protected:
    explicit NotificationDbHolder(const std::string &dbName)
        : m_ownedDb(std::make_unique<swss::DBConnector>(dbName, 0))
    {
    }

    std::unique_ptr<swss::DBConnector> m_ownedDb;
};

/* The holder base is constructed before and destroyed after the consumer that uses its connector. */
class OwningNotificationConsumer : private NotificationDbHolder, public swss::NotificationConsumer
{
public:
    OwningNotificationConsumer(const std::string &dbName, const std::string &channel)
        : NotificationDbHolder(dbName), swss::NotificationConsumer(m_ownedDb.get(), channel)
    {
    }
};

class Notifier : public Executor {
public:
    Notifier(swss::NotificationConsumer *select, Orch *orch, const std::string &name)
        : Executor(select, orch, name)
    {
    }

    swss::NotificationConsumer *getNotificationConsumer() const
    {
        return static_cast<swss::NotificationConsumer *>(getSelectable());
    }

    void execute() override
    {
        auto notificationConsumer = getNotificationConsumer();
        /* Check before triggering doTask because pop() can throw an exception if there is no data */
        if (notificationConsumer->hasData())
        {
            m_orch->doTask(*notificationConsumer);
        }
    }

    void drain() override
    {
        this->execute();
    }
};