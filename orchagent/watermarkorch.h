#ifndef WATERMARKORCH_H
#define WATERMARKORCH_H

#include "orch.h"
#include "port.h"

#include "notificationconsumer.h"
#include "timer.h"


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

    shared_ptr<Table> getCountersTable(void)
    {
        return m_countersTable;
    }

    shared_ptr<DBConnector> getCountersDb(void)
    {
        return m_countersDb;
    }

private:
    bool m_isTimerEnabled = false;

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
