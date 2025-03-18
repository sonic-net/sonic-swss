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

    void execute()
    {
        swss::NotificationConsumer *nc = getNotificationConsumer();
        while(nc->hasData())
            m_orch->doTask(*nc);
    }
};
