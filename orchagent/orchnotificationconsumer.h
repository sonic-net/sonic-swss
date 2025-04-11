#ifndef SWSS_ORCH_NOTIFICATION_CONSUMER_H
#define SWSS_ORCH_NOTIFICATION_CONSUMER_H

#include "orch.h"
#include "notificationconsumer.h"
#include "table.h"

class OrchNotificationConsumer : public swss::NotificationConsumer {
    public:
        OrchNotificationConsumer(swss::DBConnector *db, const std::string &channel, int pri = 100, size_t popBatchSize = swss::DEFAULT_NC_POP_BATCH_SIZE);
        ~OrchNotificationConsumer() override;

        void saveToSync();
        swss::KeyOpFieldsValuesTuple getSyncFront();
        void popSyncFront();
        bool syncIsEmpty();

    private:
        std::deque<swss::KeyOpFieldsValuesTuple> m_toSync;
};

#endif /* SWSS_ORCH_NOTIFICATION_CONSUMER_H */