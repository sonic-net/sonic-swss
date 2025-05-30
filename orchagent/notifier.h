#pragma once

#include "orch.h"

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