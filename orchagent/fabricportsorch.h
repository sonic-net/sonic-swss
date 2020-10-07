#ifndef SWSS_FABRICPORTSORCH_H
#define SWSS_FABRICPORTSORCH_H

#include <map>

#include "orch.h"
#include "observer.h"
#include "observer.h"
#include "producertable.h"
#include "flex_counter_manager.h"

class FabricPortsOrch : public Orch, public Subject
{
public:
    FabricPortsOrch(DBConnector *appl_db, vector<table_name_with_pri_t> &tableNames);
    bool allPortsReady();
    void generateQueueStats();

private:
    shared_ptr<DBConnector> m_state_db;
    shared_ptr<DBConnector> m_counter_db;
    shared_ptr<DBConnector> m_flex_db;

    unique_ptr<Table> m_stateTable;
    unique_ptr<Table> m_portNameQueueCounterTable;
    unique_ptr<Table> m_queuePortCounterTable;
    unique_ptr<Table> m_queueIndexCounterTable;
    unique_ptr<Table> m_queueTypeCounterTable;
    unique_ptr<Table> m_portNamePortCounterTable;
    unique_ptr<ProducerTable> m_flexCounterTable;

    swss::SelectableTimer *m_timer = nullptr;

    FlexCounterManager port_stat_manager;
    FlexCounterManager queue_stat_manager;

    sai_uint32_t m_fabricPortCount;
    map<int, sai_object_id_t> m_fabricLanePortMap;

    bool m_getFabricPortListDone = false;
    bool m_isQueueStatsGenerated = false;
    int getFabricPortList();
    void generatePortStats();
    void updateFabricPortState();

    void doTask() override;
    void doTask(Consumer &consumer);
    void doTask(swss::SelectableTimer &timer);
};

#endif /* SWSS_FABRICPORTSORCH_H */
