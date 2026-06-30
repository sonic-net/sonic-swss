#pragma once

#include "orch.h"

class Notifier : public Executor {
public:
    Notifier(swss::NotificationConsumer *select, Orch *orch, const std::string &name)
        : Executor(select, orch, name)
    {
    }

    // Delegate priority to the wrapped NotificationConsumer (pri=100)
    // so that Select dispatches notifications before table consumers (pri=0).
    int getPri() const override { return getSelectable()->getPri(); }

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