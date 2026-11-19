#pragma once

#include "orch.h"

#include "notificationconsumer.h"

#include <memory>
#include <string>
#include <vector>

/*
 * NotificationConsumerStatsOrch
 *
 * Periodically publishes admission + LRU-dedup counters from registered
 * swss::NotificationConsumer instances to COUNTERS_DB so they are
 * scrapable without bpftrace or syslog parsing.
 *
 * Lifecycle:
 *
 *   - OrchDaemon constructs this orch once, before any orch that owns a
 *     NotificationConsumer it cares to publish.
 *   - Each <X>Orch that wants its NotificationConsumer's stats visible
 *     calls
 *         gNotifConsumerStatsOrch->registerConsumer(name, consumer)
 *     right after constructing the consumer.  Registration is opt-in
 *     per consumer.
 *   - A SelectableTimer fires every kPublishIntervalSec seconds; on
 *     each tick the orch walks the registry and writes one HSET per
 *     consumer to COUNTERS_DB:NOTIFICATION_CONSUMER_STATS:<name>.
 *
 * If the global pointer is null (e.g. unit tests, fabric-only daemon)
 * the registerConsumer call is a no-op via a null check at every call
 * site -- the timer never runs, no DB connection is opened.
 */

class NotificationConsumerStatsOrch : public Orch
{
public:
    // Creates its own DBConnector to COUNTERS_DB; no orchdaemon-side
    // table needs to be wired up.  Constructed once by OrchDaemon::init.
    NotificationConsumerStatsOrch();

    // Add a consumer to the publish set.  Stable name -- becomes the
    // hash key in COUNTERS_DB:NOTIFICATION_CONSUMER_STATS:<name>.  The
    // pointer must outlive this orch.
    void registerConsumer(const std::string &name, swss::NotificationConsumer *consumer);

    // Fires the COUNTERS_DB publish on every tick of the internal timer.
    void doTask(swss::SelectableTimer &timer) override;

    // This orch has no Consumer-table executors registered -- it is
    // driven entirely by the internal SelectableTimer.  The Consumer
    // override is kept for clarity/completeness and is not expected to run
    // because no Consumer executors are registered.
    void doTask(Consumer &consumer) override { /* no Consumer executors registered */ }

private:
    struct Entry
    {
        std::string name;
        swss::NotificationConsumer *consumer;
    };

    std::shared_ptr<swss::DBConnector> m_countersDb;
    std::shared_ptr<swss::Table>       m_table;
    std::vector<Entry>                 m_consumers;
    swss::SelectableTimer             *m_timer = nullptr;

    static constexpr int kPublishIntervalSec = 10;
};

extern NotificationConsumerStatsOrch *gNotifConsumerStatsOrch;
