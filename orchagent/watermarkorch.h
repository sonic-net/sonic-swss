#ifndef WATERMARKORCH_H
#define WATERMARKORCH_H

#include <map>

#include "orch.h"
#include "port.h"

#include "notificationconsumer.h"
#include "timer.h"

const uint8_t queue_wm_status_mask = 1 << 0;
const uint8_t pg_wm_status_mask = 1 << 1;

static const std::map<std::string, const uint8_t> groupToMask =
{
    { "QUEUE_WATERMARK",     queue_wm_status_mask },
    { "PG_WATERMARK",        pg_wm_status_mask }
};

class WatermarkOrch : public Orch
{
public:
    WatermarkOrch(swss::DBConnector *db, const std::vector<std::string> &tables);
    virtual ~WatermarkOrch(void);

    void doTask(Consumer &consumer);
    void doTask(swss::NotificationConsumer &consumer);
    void doTask(swss::SelectableTimer &timer);

    void init_pg_ids();
    void init_queue_ids();

    void handleWmConfigUpdate(const std::string &key, const std::vector<swss::FieldValueTuple> &fvt);
    void handleFcConfigUpdate(const std::string &key, const std::vector<swss::FieldValueTuple> &fvt);

    void clearSingleWm(swss::Table *table, std::string wm_name, std::vector<sai_object_id_t> &obj_ids);
    void clearSingleWm(swss::Table *table, std::string wm_name, const object_map &nameOidMap);

    std::shared_ptr<swss::Table> getCountersTable(void)
    {
        return m_countersTable;
    }

    std::shared_ptr<swss::DBConnector> getCountersDb(void)
    {
        return m_countersDb;
    }

private:
    /*
    [7-2] - unused
    [1] - pg wm status
    [0] - queue wm status (least significant bit)
    */
    uint8_t m_wmStatus = 0;
    bool m_timerChanged = false;

    std::shared_ptr<swss::DBConnector> m_countersDb = nullptr;
    std::shared_ptr<swss::DBConnector> m_appDb = nullptr;
    std::shared_ptr<swss::Table> m_countersTable = nullptr;
    std::shared_ptr<swss::Table> m_periodicWatermarkTable = nullptr;
    std::shared_ptr<swss::Table> m_persistentWatermarkTable = nullptr;
    std::shared_ptr<swss::Table> m_userWatermarkTable = nullptr;

    swss::NotificationConsumer* m_clearNotificationConsumer = nullptr;
    swss::SelectableTimer* m_telemetryTimer = nullptr;

    std::vector<sai_object_id_t> m_unicast_queue_ids;
    std::vector<sai_object_id_t> m_multicast_queue_ids;
    std::vector<sai_object_id_t> m_pg_ids;
};

#endif // WATERMARKORCH_H
