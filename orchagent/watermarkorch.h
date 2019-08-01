#ifndef WATERMARKORCH_H
#define WATERMARKORCH_H

#include <map>

#include "orch.h"
#include "port.h"

#include "notificationconsumer.h"
#include "timer.h"

const uint8_t queue_wm_status_mask = 1 << 0;
const uint8_t pg_wm_status_mask = 1 << 1;

static const map<string, const uint8_t> groupToMask =
{
    { "QUEUE_WATERMARK",     queue_wm_status_mask },
    { "PG_WATERMARK",        pg_wm_status_mask }
};

class WatermarkOrch : public Orch
{
public:
    WatermarkOrch(DBConnector *db, const vector<string> &tables);
    virtual ~WatermarkOrch(void);

    void doTask(Consumer &consumer);
    void doTask(NotificationConsumer &consumer);
    void doTask(SelectableTimer &timer);

    void init_pg_ids();
    void init_queue_ids();

    void handleWmConfigUpdate(const std::string &key, const std::vector<FieldValueTuple> &fvt);
    void handleFcConfigUpdate(const std::string &key, const std::vector<FieldValueTuple> &fvt);

    void clearSingleWm(Table *table, string wm_name, vector<sai_object_id_t> &obj_ids);
    void clearSingleWm(Table *table, string wm_name, const object_map &nameOidMap);

    shared_ptr<Table> getCountersTable(void)
    {
        return m_countersTable;
    }

    shared_ptr<DBConnector> getCountersDb(void)
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

    shared_ptr<DBConnector> m_countersDb = nullptr;
    shared_ptr<DBConnector> m_appDb = nullptr;
    shared_ptr<Table> m_countersTable = nullptr;
    shared_ptr<Table> m_periodicWatermarkTable = nullptr;
    shared_ptr<Table> m_persistentWatermarkTable = nullptr;
    shared_ptr<Table> m_userWatermarkTable = nullptr;

    NotificationConsumer* m_clearNotificationConsumer = nullptr;
    SelectableTimer* m_telemetryTimer = nullptr;

    vector<sai_object_id_t> m_unicast_queue_ids;
    vector<sai_object_id_t> m_multicast_queue_ids;
    vector<sai_object_id_t> m_pg_ids;
};

#endif // WATERMARKORCH_H
