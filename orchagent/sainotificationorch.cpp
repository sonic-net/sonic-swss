#include "sainotificationorch.h"

#include "logger.h"

SaiNotificationOrch *gSaiNotificationOrch = nullptr;

SaiNotificationOrch::SaiNotificationOrch()
    : Orch()
{
    SWSS_LOG_ENTER();

    m_queue = getSaiNotificationQueue();
    m_dispatcher = getSaiNotificationDispatcher();

    Orch::addExecutor(createSaiNotificationQueueExecutor(
        m_queue, this, m_dispatcher, "SAI_NOTIFICATION_QUEUE"));
}

void SaiNotificationOrch::registerHandler(const std::string &op,
                                           SaiNotificationDispatcher::Handler handler,
                                           SaiNotificationDispatcher::ReadinessPredicate ready)
{
    m_dispatcher->registerHandler(op, std::move(handler), std::move(ready));
}
