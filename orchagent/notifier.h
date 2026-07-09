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
    //
    // This value is CONSTANT for a given Notifier instance.  Do NOT make
    // it mutable -- Select::cmp uses getPri() to order its internal
    // std::set (m_ready).  If getPri() returns different values while the
    // element is in the set, the ordering invariant is violated (UB).
    int getPri() const override
    {
        return getSelectable()->getPri();
    }

    // Yield the Select ready-set when the Orch stalls (defers processing).
    //
    // Select::poll_descriptors() checks hasCachedData() AFTER erasing the
    // element from m_ready, so a mutable return value here is safe -- it
    // does not corrupt the set's ordering invariant.
    //
    // When the Orch defers processing (returns from doTask without consuming
    // for STALL_THRESHOLD consecutive execute() calls), we report no cached
    // data.  This prevents the Notifier from being re-inserted into m_ready,
    // allowing lower-priority table consumers to be dispatched.  The stalled
    // notification is still processed via the drain() path (Orch::doTask()
    // iterates all consumers each main-loop cycle).
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
        /* Check before triggering doTask because pop() can throw an exception if there is no data */
        if (notificationConsumer->hasData())
        {
            bool cachedBefore = notificationConsumer->hasCachedData();

            m_orch->doTask(*notificationConsumer);

            bool hasDataAfter = notificationConsumer->hasData();
            bool cachedAfter  = notificationConsumer->hasCachedData();

            /* Detect whether doTask() consumed at least one notification.
             *
             * Several Orchs (PortsOrch, FdbOrch, TwampOrch, WatermarkOrch,
             * P4Orch) guard doTask(NotificationConsumer&) behind
             * allPortsReady() and return without calling pop()/pops()
             * when the precondition is not yet met.  This is correct --
             * the notification must be deferred, not discarded.
             *
             * However, unconsumed data keeps the real hasCachedData() true,
             * and this high-priority Notifier would be perpetually
             * re-inserted into Select's m_ready set ahead of lower-priority
             * table consumers, starving them.
             *
             * Fix: after STALL_THRESHOLD consecutive execute() calls with
             * no detectable consumption, our hasCachedData() override
             * returns false so the Notifier drops out of m_ready and table
             * consumers get their turn.  The notification is still processed
             * via drain() on subsequent main-loop iterations.  Priority
             * self-restores once the Orch resumes consuming. */
            if (!hasDataAfter ||                  /* queue fully drained   */
                (cachedBefore && !cachedAfter))    /* queue visibly shrank  */
            {
                m_noProgressCount = 0;
            }
            else
            {
                m_noProgressCount++;
            }
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
    static constexpr int STALL_THRESHOLD = 2;
    int m_noProgressCount = 0;
};
