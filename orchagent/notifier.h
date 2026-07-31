#pragma once

#include "orch.h"

class Notifier : public Executor {
public:
    Notifier(swss::NotificationConsumer *select, Orch *orch, const std::string &name)
        : Executor(select, orch, name)
    {
    }

    /* Delegate priority to the wrapped NotificationConsumer (pri=100).
     * Must be constant — Select::cmp uses getPri() to order std::set m_ready;
     * a mutable return would violate the ordering invariant (UB). */
    int getPri() const override
    {
        return getSelectable()->getPri();
    }

    /* Yield the Select ready-set when the Orch stalls (defers doTask without
     * popping).  After STALL_THRESHOLD consecutive no-progress execute() calls,
     * report no cached data so lower-priority table consumers get dispatched.
     * Safe: Select checks hasCachedData() AFTER erasing from m_ready. */
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

            /* If queue drained, the Orch consumed — reset the counter.
             * If the Orch deferred (allPortsReady() guard), the queue remains
             * unchanged and we increment toward the stall threshold.
             * Partial pops from a large backlog also increment; this provides
             * natural fairness by eventually yielding to table consumers. */
            if (!notificationConsumer->hasCachedData())
                m_noProgressCount = 0;
            else
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

    static constexpr int STALL_THRESHOLD = 2;
    int m_noProgressCount = 0;
};
