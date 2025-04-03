#include "orchnotificationconsumer.h"

using namespace swss;

OrchNotificationConsumer::OrchNotificationConsumer(DBConnector *db, const std::string &channel, int pri, size_t popBatchSize)
:NotificationConsumer(db, channel, pri, popBatchSize)
{
}

OrchNotificationConsumer::~OrchNotificationConsumer()
{
}

void OrchNotificationConsumer::saveToSync()
{
    std::deque<KeyOpFieldsValuesTuple> tmpVkco;
    pops(tmpVkco);

    m_toSync.insert(m_toSync.end(), tmpVkco.begin(), tmpVkco.end());
}

KeyOpFieldsValuesTuple OrchNotificationConsumer::getSyncFront()
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
