#pragma once

#include <map>
#include <string>

#include "flex_counter_manager.h"
#include "dbconnector.h"
#include "orch.h"
#include "portsorch.h"

using namespace std;

#define POLICER_STAT_COUNTER_FLEX_COUNTER_GROUP "POLICER_STAT_COUNTER"
typedef map<string, sai_object_id_t> PolicerTable;
typedef map<string, int> PolicerRefCountTable;

class PolicerOrch : public Orch
{
public:
    PolicerOrch(vector<TableConnector> &tableNames, PortsOrch *portOrch);

    bool policerExists(const string &name);
    bool getPolicerOid(const string &name, sai_object_id_t &oid);

    bool increaseRefCount(const string &name);
    bool decreaseRefCount(const string &name);
    task_process_status handlePortStormControlTable(swss::KeyOpFieldsValuesTuple tuple);
    void addPolicerToFlexCounter(sai_object_id_t oid, const string &name);
    void generatePolicerCounterMap();
    
private:
    PortsOrch *m_portsOrch;
    virtual void doTask(Consumer& consumer);
    void doTask(swss::SelectableTimer&) override;

    PolicerTable m_syncdPolicers;
    PolicerRefCountTable m_policerRefCounts;
    FlexCounterManager m_policer_counter_manager;
    bool m_isPolicerCounterMapGenerated = false;

    void getPolicerCounter(void);
    void initPolicerCounterPlugin(void);
    void removePCFromFlexCounter(sai_object_id_t id, const string &name);
    string getPolicerFlexCounterTableKey(string key);
    sai_status_t querySupportedPolicerStats(std::unordered_set<sai_policer_stat_t>&);
    std::unordered_set<std::string> generatePCounterStats();

protected:
    std::shared_ptr<DBConnector> m_counter_db;
    std::shared_ptr<DBConnector> m_flex_db;
    std::shared_ptr<DBConnector> m_asic_db;
    std::unique_ptr<Table> m_counter_table;
    std::unique_ptr<Table> m_vidToRidTable;
    std::unique_ptr<ProducerTable> m_flex_counter_group_table;
    std::map<sai_object_id_t, std::string> m_pendingPcAddToFlexCntr;

    SelectableTimer* m_FlexCounterUpdTimer = nullptr;
};


