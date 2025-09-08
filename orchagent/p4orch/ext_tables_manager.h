#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "macaddress.h"
#include "orch.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "p4orch/tables_definition_manager.h"
#include "response_publisher_interface.h"
#include "return_code.h"
#include "vrforch.h"
extern "C"
{
#include "sai.h"
}

struct P4ExtTableEntry
{
    std::string db_key;
    std::string table_name;
    std::string table_key;
    sai_object_id_t sai_entry_oid = SAI_NULL_OBJECT_ID;
    sai_object_id_t sai_counter_oid = SAI_NULL_OBJECT_ID;
    std::unordered_map<std::string, DepObject> action_dep_objects;

    P4ExtTableEntry() {};
    P4ExtTableEntry(const std::string &db_key, const std::string &table_name, const std::string &table_key)
        : db_key(db_key), table_name(table_name), table_key(table_key)
    {
    }
};

typedef std::unordered_map<std::string, P4ExtTableEntry> P4ExtTableEntryMap;
typedef std::unordered_map<std::string, P4ExtTableEntryMap> P4ExtTableMap;
typedef std::unordered_map<std::string, std::deque<swss::KeyOpFieldsValuesTuple>> m_entriesTableMap;

class ExtTablesManager : public ObjectManagerInterface
{
  public:
    ExtTablesManager(P4OidMapper *p4oidMapper, VRFOrch *vrfOrch, ResponsePublisherInterface *publisher)
        : m_vrfOrch(vrfOrch), m_countersDb(std::make_unique<swss::DBConnector>("COUNTERS_DB", 0)),
          m_countersTable(std::make_unique<swss::Table>(
              m_countersDb.get(), std::string(COUNTERS_TABLE) + DEFAULT_KEY_SEPARATOR + APP_P4RT_TABLE_NAME))
    {
        SWSS_LOG_ENTER();

        assert(p4oidMapper != nullptr);
        m_p4OidMapper = p4oidMapper;
        assert(publisher != nullptr);
        m_publisher = publisher;
    }
    virtual ~ExtTablesManager() = default;

    void enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry) override;
    ReturnCode drain() override;
    void drainWithNotExecuted() override;
    std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) override;
    ReturnCode getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                            std::string &object_key) override;

    // For every extension entry, update counters stats in COUNTERS_DB, if
    // counters are enabled for those entries
    void doExtCounterStatsTask();

  private:
    ReturnCodeOr<P4ExtTableAppDbEntry> deserializeP4ExtTableEntry(const std::string &table_name, const std::string &key,
                                                                  const std::vector<swss::FieldValueTuple> &attributes);
    ReturnCode validateActionParamsCrossRef(P4ExtTableAppDbEntry &app_db_entry, ActionInfo *action);
    ReturnCode validateP4ExtTableAppDbEntry(P4ExtTableAppDbEntry &app_db_entry);
    P4ExtTableEntry *getP4ExtTableEntry(const std::string &table_name, const std::string &table_key);
    ReturnCode prepareP4SaiExtAPIParams(const P4ExtTableAppDbEntry &app_db_entry, std::string &ext_table_entry_attr);
    ReturnCode createP4ExtTableEntry(const P4ExtTableAppDbEntry &app_db_entry, P4ExtTableEntry &ext_table_entry);
    ReturnCode updateP4ExtTableEntry(const P4ExtTableAppDbEntry &app_db_entry, P4ExtTableEntry *ext_table_entry);
    ReturnCode removeP4ExtTableEntry(const std::string &table_name, const std::string &table_key);
    ReturnCode processAddRequest(const P4ExtTableAppDbEntry &app_db_entry);
    ReturnCode processUpdateRequest(const P4ExtTableAppDbEntry &app_db_entry, P4ExtTableEntry *ext_table_entry);
    ReturnCode processDeleteRequest(const P4ExtTableAppDbEntry &app_db_entry);

    ReturnCode setExtTableCounterStats(P4ExtTableEntry *ext_table_entry);

    P4ExtTableMap m_extTables;
    P4OidMapper *m_p4OidMapper;
    VRFOrch *m_vrfOrch;
    ResponsePublisherInterface *m_publisher;
    m_entriesTableMap m_entriesTables;

    std::unique_ptr<swss::DBConnector> m_countersDb;
    std::unique_ptr<swss::Table> m_countersTable;
};
