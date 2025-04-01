#include "orchnotificationconsumer.h"

using namespace swss;

OrchNotificationConsumer::OrchNotificationConsumer(swss::DBConnector *db, const std::string &channel, int pri = 100, size_t popBatchSize = swss::DEFAULT_NC_POP_BATCH_SIZE)
:NotificationConsumer(db, channel, pri, popBatchSize)
{
}

OrchNotificationConsumer::~OrchNotificationConsumer()
{
}

void OrchNotificationConsumer::saveToSync()
{
    std::deque<swss::KeyOpFieldsValuesTuple> tmpVkco;
    pops(tmpVkco);

    m_toSync.insert(m_toSync.end(), tmpVkco.begin(), tmpVkco.end());
}

swss:KeyOpFieldsValuesTuple& OrchNotificationConsumer::getSyncFront()
{
    return m_toSync.front();
}

void OrchNotificationConsumer::popSyncFront()
{
    m_toSync.pop_front();
}

bool OrchNotificationConsumer::syncIsEmpty()
{
    return m_toSync.empty();
}
