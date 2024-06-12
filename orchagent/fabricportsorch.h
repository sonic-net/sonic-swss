#ifndef SWSS_FABRICPORTSORCH_H
#define SWSS_FABRICPORTSORCH_H

#include <map>

#include "orch.h"
#include "observer.h"
#include "observer.h"
#include "producertable.h"
#include "flex_counter_manager.h"

#define STATE_FABRIC_CAPACITY_TABLE_NAME "FABRIC_CAPACITY_TABLE"
#define STATE_PORT_CAPACITY_TABLE_NAME "PORT_CAPACITY_TABLE"

class FabricPortsOrch : public Orch, public Subject
{
public:
    FabricPortsOrch(DBConnector *appl_db, vector<table_name_with_pri_t> &tableNames,
                    bool fabricPortStatEnabled=true, bool fabricQueueStatEnabled=true);
    bool allPortsReady();
    void generateQueueStats();

private:
    bool m_fabricPortStatEnabled;
    bool m_fabricQueueStatEnabled;

    shared_ptr<DBConnector> m_state_db;
    shared_ptr<DBConnector> m_counter_db;
    shared_ptr<DBConnector> m_appl_db;

    unique_ptr<Table> m_stateTable;
    unique_ptr<Table> m_portNameQueueCounterTable;
    unique_ptr<Table> m_portNamePortCounterTable;
    unique_ptr<Table> m_fabricCounterTable;
    unique_ptr<Table> m_applTable;
    unique_ptr<Table> m_fabricCapacityTable;
    unique_ptr<Table> m_applMonitorConstTable;
    unique_ptr<ProducerTable> m_flexCounterTable;
    shared_ptr<Table> m_counterNameToSwitchStatMap;

    swss::SelectableTimer *m_timer = nullptr;
    swss::SelectableTimer *m_debugTimer = nullptr;

    FlexCounterManager port_stat_manager;
    FlexCounterManager queue_stat_manager;
    FlexCounterManager *switch_drop_counter_manager = nullptr;

    sai_uint32_t m_fabricPortCount;
    map<int, sai_object_id_t> m_fabricLanePortMap;
    unordered_map<int, bool> m_portStatus;
    unordered_map<int, size_t> m_portDownCount;
    unordered_map<int, time_t> m_portDownSeenLastTime;

    bool m_getFabricPortListDone = false;
    bool m_isQueueStatsGenerated = false;
    bool m_debugTimerEnabled = false;
    bool m_isSwitchStatsGenerated = false;

    string m_defaultPollWithErrors = "0";
    string m_defaultPollWithNoErrors = "8";
    string m_defaultPollWithFecErrors = "0";
    string m_defaultPollWithNoFecErrors = "8";
    string m_defaultConfigIsolated = "0";
    string m_defaultIsolated = "0";
    string m_defaultAutoIsolated = "0";

    int getFabricPortList();
    void generatePortStats();
    void updateFabricPortState();
    void updateFabricDebugCounters();
    void updateFabricCapacity();
    bool checkFabricPortMonState();
    void updateFabricRate();
    void createSwitchDropCounters();

    void doTask() override;
    void doTask(Consumer &consumer);
    void doFabricPortTask(Consumer &consumer);
    void doTask(swss::SelectableTimer &timer);
};

#endif /* SWSS_FABRICPORTSORCH_H */
