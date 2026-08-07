#pragma once

#include "orch.h"

class Notifier : public Executor {
public:
    Notifier(swss::NotificationConsumer *select, Orch *orch, const std::string &name)
        : Executor(select, orch, name)
    {
    }

    /*
     * Delegate to wrapped NotificationConsumer so Select dispatches
     * notifications before table consumers.
     */
    int getPri() const override
    {
        return getSelectable()->getPri();
    }

    /*
     * Yield the Select ready-set when the Orch stalls (defers doTask without
     * popping).  After STALL_THRESHOLD consecutive no-progress execute() calls,
     * report no cached data so lower-priority table consumers get dispatched.
     */
    bool hasCachedData() override
    {
        if (m_noProgressCount >= STALL_THRESHOLD)
            return false;
        return getSelectable()->hasCachedData();
    }

    swss::NotificationConsumer *getNotificationConsumer() const
    {
        return static_cast<swss::NotificationConsumer *>(getSelectable());
    }

    void execute() override
    {
        auto notificationConsumer = getNotificationConsumer();
        if (notificationConsumer->hasData())
        {
            m_orch->doTask(*notificationConsumer);

            /*
             * If queue drained, the Orch consumed — reset the counter.
             * If the Orch deferred (e.g. allPortsReady() guard), the queue
             * is unchanged and we increment toward the stall threshold.
             * Partial pops from a large backlog also increment, providing
             * natural fairness by eventually yielding to table consumers.
             */
            if (!notificationConsumer->hasCachedData())
                m_noProgressCount = 0;
            else if (m_noProgressCount < STALL_THRESHOLD)
                m_noProgressCount++;
        }
        else
        {
            m_noProgressCount = 0;
        }
    }

    void drain() override
    {
        this->execute();
    }

private:
    /* execute() is called twice per main-loop iteration (Select dispatch +
     * OrchDaemon sweep), so 2 gives the Orch one full iteration to consume. */
    static constexpr int STALL_THRESHOLD = 2;
    int m_noProgressCount = 0;
};
