#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "macaddress.h"
#include "orchagent/orch.h"
#include "orchagent/p4orch/object_manager_interface.h"
#include "orchagent/p4orch/p4oidmapper.h"
#include "orchagent/p4orch/p4orch_util.h"
#include "orchagent/response_publisher_interface.h"
#include "orchagent/return_code.h"
extern "C"
{
#include "sai.h"
}

/**
 * A set of tables definition
 */
struct TablesInfo
{
    std::string context;
    nlohmann::json info;
    std::unordered_map<std::string, std::string> m_tableIdNameMap;
    std::unordered_map<std::string, TableInfo> m_tableInfoMap;
    std::map<int, std::string> m_tablePrecedenceMap;

    TablesInfo() {};
    TablesInfo(const std::string &context_key, const nlohmann::json &info_value)
        : context(context_key), info(info_value)
    {
    }
};

/**
 * Datastructure is designed to hold multiple set of table definition.
 * However, current support handles only one set of table definition.
 */
typedef std::unordered_map<std::string, TablesInfo> TablesInfoMap;

class TablesDefnManager : public ObjectManagerInterface
{
  public:
    TablesDefnManager(P4OidMapper *p4oidMapper, ResponsePublisherInterface *publisher)
    {
        SWSS_LOG_ENTER();

        assert(p4oidMapper != nullptr);
        m_p4OidMapper = p4oidMapper;
        assert(publisher != nullptr);
        m_publisher = publisher;
    }
    virtual ~TablesDefnManager() = default;

    void enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry) override;
    void drain() override;
    std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) override;
    ReturnCode getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                            std::string &object_key) override;

  private:
    ReturnCodeOr<TablesInfoAppDbEntry> deserializeTablesInfoEntry(const std::string &key,
                                                                  const std::vector<swss::FieldValueTuple> &attributes);
    TablesInfo *getTablesInfoEntry(const std::string &context_key);
    ReturnCode createTablesInfo(const std::string &context_key, TablesInfo &tablesinfo_entry);
    ReturnCode removeTablesInfo(const std::string &context_key);
    ReturnCode processAddRequest(const TablesInfoAppDbEntry &app_db_entry, const std::string &context_key);
    ReturnCode processUpdateRequest(const TablesInfoAppDbEntry &app_db_entry, const std::string &context_key);
    ReturnCode processDeleteRequest(const std::string &context_key);

    TablesInfoMap m_tablesinfoMap;
    P4OidMapper *m_p4OidMapper;
    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;
};
