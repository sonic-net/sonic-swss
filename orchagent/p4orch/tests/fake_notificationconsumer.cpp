#include "notificationconsumer.h"

namespace swss
{

#ifdef SWSS_NOTIFICATIONCONSUMER_HAS_LRU_DEDUP

NotificationConsumer::NotificationConsumer(swss::DBConnector *db, const std::string &channel, int pri,
                                           size_t popBatchSize)
    : Selectable(pri), POP_BATCH_SIZE(popBatchSize), m_db(db), m_subscribe(NULL), m_channel(channel),
      m_queue(std::make_unique<FifoNotificationQueue>())
{
    SWSS_LOG_ENTER();
}

NotificationConsumer::NotificationConsumer(swss::DBConnector *db, const std::string &channel, int pri,
                                           size_t popBatchSize, NotificationQueuePolicy policy)
    : Selectable(pri), POP_BATCH_SIZE(popBatchSize), m_db(db), m_subscribe(NULL), m_channel(channel),
      m_queue(policy == NotificationQueuePolicy::LruDedup
              ? std::unique_ptr<NotificationQueueBase>(std::make_unique<LruDedupNotificationQueue>(channel))
              : std::unique_ptr<NotificationQueueBase>(std::make_unique<FifoNotificationQueue>()))
{
    SWSS_LOG_ENTER();
}

void NotificationConsumer::setStatsLabel(const std::string &label)
{
    m_stats_label = label;
}

void NotificationConsumer::setOpAllowList(std::unordered_set<std::string> ops)
{
    m_op_allowlist = std::move(ops);
}

LruDedupNotificationQueue* NotificationConsumer::getLruDedupQueue() const
{
    return dynamic_cast<LruDedupNotificationQueue*>(m_queue.get());
}

void NotificationConsumer::maybeLogStats()
{
    // no-op in tests
}

void LruDedupNotificationQueue::maybeLogStats()
{
    // no-op in tests
}

#else  // !SWSS_NOTIFICATIONCONSUMER_HAS_LRU_DEDUP

// Legacy stub: libswsscommon doesn't yet ship the LruDedup queue
// policy / setStatsLabel / setOpAllowList APIs.  Provide only the
// 2-arg ctor that exists in the upstream header.
NotificationConsumer::NotificationConsumer(swss::DBConnector *db, const std::string &channel, int pri,
                                           size_t popBatchSize)
    : Selectable(pri), POP_BATCH_SIZE(popBatchSize), m_db(db), m_subscribe(NULL), m_channel(channel)
{
    SWSS_LOG_ENTER();
}

#endif  // SWSS_NOTIFICATIONCONSUMER_HAS_LRU_DEDUP

} // namespace swss
