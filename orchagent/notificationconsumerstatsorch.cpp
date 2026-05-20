#include "notificationconsumerstatsorch.h"

#include "logger.h"
#include "select.h"
#include "table.h"
#include "timer.h"

#include <vector>

using namespace swss;

NotificationConsumerStatsOrch *gNotifConsumerStatsOrch = nullptr;

#ifdef SWSS_NOTIFICATIONCONSUMER_HAS_LRU_DEDUP

namespace
{
constexpr const char *kStatsTable = "NOTIFICATION_CONSUMER_STATS";
}

NotificationConsumerStatsOrch::NotificationConsumerStatsOrch()
    : Orch()
{
    SWSS_LOG_ENTER();

    m_countersDb = std::make_shared<DBConnector>("COUNTERS_DB", 0);
    m_table      = std::make_shared<Table>(m_countersDb.get(), kStatsTable);

    auto interv = timespec { .tv_sec = kPublishIntervalSec, .tv_nsec = 0 };
    m_timer = new SelectableTimer(interv);

    auto executor = new ExecutableTimer(m_timer, this, "NOTIF_CONSUMER_STATS_TIMER");
    Orch::addExecutor(executor);
    m_timer->start();

    SWSS_LOG_NOTICE("NotificationConsumerStatsOrch: publishing to COUNTERS_DB:%s every %ds",
                    kStatsTable, kPublishIntervalSec);
}

void NotificationConsumerStatsOrch::registerConsumer(const std::string &name,
                                                     NotificationConsumer *consumer)
{
    if (consumer == nullptr)
    {
        SWSS_LOG_WARN("NotificationConsumerStatsOrch::registerConsumer: null consumer for %s",
                      name.c_str());
        return;
    }
    m_consumers.push_back({name, consumer});
    SWSS_LOG_NOTICE("NotificationConsumerStatsOrch: registered %s (channel=%s)",
                    name.c_str(), consumer->getChannel().c_str());
}

void NotificationConsumerStatsOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    for (const auto &entry : m_consumers)
    {
        auto admit_stats = entry.consumer->getStats();
        uint64_t admitted = admit_stats.received >= admit_stats.dropped_allowlist
                            ? admit_stats.received - admit_stats.dropped_allowlist
                            : 0;
        uint64_t admit_ratio = admit_stats.received
                               ? (100 * admitted / admit_stats.received)
                               : 0;

        std::vector<FieldValueTuple> fvs;
        fvs.emplace_back("channel", entry.consumer->getChannel());
        fvs.emplace_back("received",          std::to_string(admit_stats.received));
        fvs.emplace_back("dropped_allowlist", std::to_string(admit_stats.dropped_allowlist));
        fvs.emplace_back("admitted",          std::to_string(admitted));
        fvs.emplace_back("admit_ratio_pct",   std::to_string(admit_ratio));

        // If the consumer uses LruDedup, also publish the queue-side
        // dedup counters.  FIFO queues have no equivalent.
        if (auto *lru = entry.consumer->getLruDedupQueue())
        {
            auto qs = lru->getStats();
            // Integer percent for consistency: redis-cli scrapers see
            // a stable type ("0".."100") instead of a mix of "0" and
            // "75.000000".  Sub-integer granularity isn't useful here
            // (the counter is updated once per push and the publish
            // tick is 10 s).
            uint64_t ratio = qs.pushed ? (100 * qs.dedup_hits / qs.pushed) : 0;
            fvs.emplace_back("lru_pushed",          std::to_string(qs.pushed));
            fvs.emplace_back("lru_dedup_hits",      std::to_string(qs.dedup_hits));
            fvs.emplace_back("lru_dedup_ratio_pct", std::to_string(ratio));
            fvs.emplace_back("lru_current_depth",   std::to_string(qs.current_depth));
            fvs.emplace_back("lru_high_watermark",  std::to_string(qs.high_watermark));
            fvs.emplace_back("queue_policy", "LruDedup");
        }
        else
        {
            fvs.emplace_back("queue_policy", "Fifo");
        }

        m_table->set(entry.name, fvs);
    }
}

#else  // !SWSS_NOTIFICATIONCONSUMER_HAS_LRU_DEDUP

// Stub implementation when building against a libswsscommon that
// doesn't yet ship the LruDedup queue policy / getStats / getChannel
// APIs.  The class is non-functional in this configuration -- the
// global pointer is null and the orchdaemon construction is
// #ifdef-skipped.  These stubs exist only to keep the symbol resolved
// for any caller that's still wired to the global.
NotificationConsumerStatsOrch::NotificationConsumerStatsOrch()
    : Orch()
{
}

void NotificationConsumerStatsOrch::registerConsumer(const std::string &,
                                                     NotificationConsumer *)
{
}

void NotificationConsumerStatsOrch::doTask(SelectableTimer &)
{
}

#endif  // SWSS_NOTIFICATIONCONSUMER_HAS_LRU_DEDUP
