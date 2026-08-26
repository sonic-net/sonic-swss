#pragma once

#include "orch.h"
#include "notifications.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

/*
 * SaiNotificationOrch
 *
 * Drains per-consumer in-process SaiNotificationQueues on the orchagent main
 * loop and dispatches entries to handlers registered by feature orchs.
 *
 * Lifecycle:
 *
 *   - OrchDaemon constructs this orch once in ZMQ mode, before any orch that
 *     registers an in-process SAI notification handler.
 *   - Each <X>Orch that handles a queued notification calls
 *         gSaiNotificationOrch->registerHandler(op, handler, readiness)
 *     from its constructor.  Registration is a no-op when the global pointer
 *     is null (non-ZMQ mode).
 */
class SaiNotificationOrch : public Orch
{
public:
    SaiNotificationOrch();

    void registerHandler(const std::string &op,
                         SaiNotificationDispatcher::Handler handler,
                         SaiNotificationQueue::ReadinessPredicate ready = nullptr);

    SaiNotificationQueue *getSaiNotificationQueue(const std::string &op);

private:
    struct ConsumerMetadata
    {
        std::string consumerName;
        swss::NotificationQueuePolicy policy;
    };

    struct ConsumerEntry
    {
        std::unique_ptr<SaiNotificationQueue> queue;
        std::unique_ptr<Executor> executor;
    };

    void initMetadata();

    SaiNotificationQueue *getOrCreateQueueForOp(const std::string &op);

    std::unordered_map<std::string, ConsumerEntry> m_consumersByName;
    std::unordered_map<std::string, ConsumerMetadata> m_metadataByOp;
    SaiNotificationDispatcher m_dispatcher;
};

extern SaiNotificationOrch *gSaiNotificationOrch;
