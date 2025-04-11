#pragma once

#include "orch.h"
#include "orchnotificationconsumer.h"

class Notifier : public Executor {
public:
    Notifier(OrchNotificationConsumer *select, Orch *orch, const std::string &name)
        : Executor(select, orch, name)
    {
    }

    OrchNotificationConsumer *getNotificationConsumer() const
    {
        return static_cast<OrchNotificationConsumer *>(getSelectable());
    }

    void execute()
    {
        // pops notification first, then high priority notification will not block low priority event
        OrchNotificationConsumer *consumer = getNotificationConsumer();
        consumer->saveToSync();
        m_orch->doTask(*getNotificationConsumer());
    }

    void drain()
    {
        OrchNotificationConsumer *consumer = getNotificationConsumer();
        if (!consumer->syncIsEmpty())
        {
            m_orch->doTask(*consumer);
        }
    }
};
