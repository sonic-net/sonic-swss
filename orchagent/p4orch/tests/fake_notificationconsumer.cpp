#include "notificationconsumer.h"

namespace swss
{

NotificationConsumer::NotificationConsumer(swss::DBConnector *db, const std::string &channel, int pri,
                                           size_t popBatchSize)
    : Selectable(pri), POP_BATCH_SIZE(popBatchSize), m_db(db), m_subscribe(NULL), m_channel(channel)
{
    SWSS_LOG_ENTER();
}

} // namespace swss
