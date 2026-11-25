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
        size_t i = 0;
        /* Check before triggering doTask because pop() can throw an exception if there is no data */
        while (notificationConsumer->hasData() && (i < notificationConsumer->POP_BATCH_SIZE))
        {
            m_orch->doTask(*notificationConsumer);
            i++;
        }
    }

    void drain() override
    {
        this->execute();
    }
};
