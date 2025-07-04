#include "p4orch/tables_definition_manager.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "directory.h"
#include "logger.h"
#include "orch.h"
#include "p4orch/p4orch.h"
#include "p4orch/p4orch_util.h"
#include "tokenize.h"
extern "C"
{
#include "saitypes.h"
}

extern Directory<Orch *> gDirectory;
extern P4Orch *gP4Orch;
const std::map<std::string, std::string> format_datatype_map = {
    {"MAC", "SAI_ATTR_VALUE_TYPE_MAC"}, {"IPV4", "SAI_ATTR_VALUE_TYPE_IPV4"}, {"IPV6", "SAI_ATTR_VALUE_TYPE_IPV6"}};

std::string BitwidthToDatatype(int bitwidth)
{
    std::string datatype = "SAI_ATTR_VALUE_TYPE_CHARDATA";

    if (bitwidth <= 0)
    {
        datatype = "SAI_ATTR_VALUE_TYPE_CHARDATA";
    }
    else if (bitwidth <= 8)
    {
        datatype = "SAI_ATTR_VALUE_TYPE_UINT8";
    }
    else if (bitwidth <= 16)
    {
        datatype = "SAI_ATTR_VALUE_TYPE_UINT16";
    }
    else if (bitwidth <= 32)
    {
        datatype = "SAI_ATTR_VALUE_TYPE_UINT32";
    }
    else if (bitwidth <= 64)
    {
        datatype = "SAI_ATTR_VALUE_TYPE_UINT64";
    }

    return datatype;
}

std::string parseBitwidthToDatatype(const nlohmann::json &json)
{
    int bitwidth;
    std::string datatype = "SAI_ATTR_VALUE_TYPE_CHARDATA";

    if (json.find(p4orch::kBitwidth) != json.end())
    {
        bitwidth = json.at(p4orch::kBitwidth).get<int>();
        datatype = BitwidthToDatatype(bitwidth);
    }

    return datatype;
}

std::string parseFormatToDatatype(const nlohmann::json &json, std::string datatype)
{
    std::string format;

    if (json.find(p4orch::kFormat) != json.end())
    {
        format = json.at(p4orch::kFormat).get<std::string>();

        auto it = format_datatype_map.find(format);
        if (it != format_datatype_map.end())
        {
            datatype = it->second;
        }
    }

    return datatype;
}

ReturnCode parseTableMatchReferences(const nlohmann::json &match_json, TableMatchInfo &match)
{
    std::string table, field;

    if (match_json.find(p4orch::kReferences) != match_json.end())
    {
        for (const auto &ref_json : match_json[p4orch::kReferences])
        {
            try
            {
                table = ref_json.at(p4orch::kTableRef).get<std::string>();
                field = ref_json.at(p4orch::kMatchRef).get<std::string>();
                match.table_reference_map[table] = field;
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "can not parse tables from app-db supplied table definition "
                          "info";
            }
        }
    }

    return ReturnCode();
}

ReturnCode parseActionParamReferences(const nlohmann::json &param_json, ActionParamInfo &param)
{
    std::string table, field;

    if (param_json.find(p4orch::kReferences) != param_json.end())
    {
        for (const auto &ref_json : param_json[p4orch::kReferences])
        {
            try
            {
                table = ref_json.at(p4orch::kTableRef).get<std::string>();
                field = ref_json.at(p4orch::kMatchRef).get<std::string>();
                param.table_reference_map[table] = field;
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "can not parse tables from app-db supplied table definition "
                          "info";
            }
        }
    }

    return ReturnCode();
}

ReturnCode parseTableActionParams(const nlohmann::json &action_json, ActionInfo &action)
{
    action.refers_to = false;
    if (action_json.find(p4orch::kActionParams) != action_json.end())
    {
        for (const auto &param_json : action_json[p4orch::kActionParams])
        {
            try
            {
                ActionParamInfo param;
                std::string param_name;

                param_name = param_json.at(p4orch::kName).get<std::string>();
                param.name = param_name;
                param.datatype = parseBitwidthToDatatype(param_json);
                param.datatype = parseFormatToDatatype(param_json, param.datatype);
                parseActionParamReferences(param_json, param);
                action.params[param_name] = param;

                if (!param.table_reference_map.empty())
                {
                    /**
                     * Helps avoid walk of action parameters if this is set to false at
                     * action level
                     */
                    action.refers_to = true;
                }
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "can not parse tables from app-db supplied table definition "
                          "info";
            }
        }
    }

    return ReturnCode();
}

ReturnCode parseTableCounter(const nlohmann::json &table_json, TableInfo &table)
{
    if (table_json.find(p4orch::kCounterUnit) != table_json.end())
    {
        auto unit = table_json.at(p4orch::kCounterUnit);
        if (unit == "PACKETS")
        {
            table.counter_packets_enabled = true;
        }
        else if (unit == "BYTES")
        {
            table.counter_bytes_enabled = true;
        }
        else
        {
            table.counter_packets_enabled = true;
            table.counter_bytes_enabled = true;
        }
    }

    return ReturnCode();
}

ReturnCode parseTablesInfo(const nlohmann::json &info_json, TablesInfo &info_entry)
{
    ReturnCode status;
    int table_id;
    std::string table_name, field_name;

    if (info_json.find(p4orch::kTables) == info_json.end())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "no tables in app-db supplied table definition info";
    }

    for (const auto &table_json : info_json[p4orch::kTables])
    {
        try
        {
            table_id = table_json.at(p4orch::kId).get<int>();
            table_name = table_json.at(p4orch::kAlias).get<std::string>();
        }
        catch (std::exception &ex)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "can not parse tables from app-db supplied table definition "
                      "info";
        }

        TableInfo table = {};
        table.name = table_name;
        table.id = table_id;
        try
        {
            for (const auto &match_json : table_json[p4orch::kmatchFields])
            {
                TableMatchInfo match = {};
                std::string match_name;

                match_name = match_json.at(p4orch::kName).get<std::string>();
                match.name = match_name;
                match.datatype = parseBitwidthToDatatype(match_json);
                match.datatype = parseFormatToDatatype(match_json, match.datatype);
                parseTableMatchReferences(match_json, match);
                table.match_fields[match_name] = match;
            }

            for (const auto &action_json : table_json[p4orch::kActions])
            {
                ActionInfo action = {};
                std::string action_name;

                action_name = action_json.at(p4orch::kAlias).get<std::string>();
                action.name = action_name;
                parseTableActionParams(action_json, action);
                table.action_fields[action_name] = action;

                /**
                 * If any parameter of action refers to another table, add that one in
                 * the cross-reference list of current table
                 */
                for (auto param_it = action.params.begin(); param_it != action.params.end(); param_it++)
                {
                    ActionParamInfo action_param = param_it->second;
                    for (auto ref_it = action_param.table_reference_map.begin();
                         ref_it != action_param.table_reference_map.end(); ref_it++)
                    {
                        if (std::find(table.action_ref_tables.begin(), table.action_ref_tables.end(), ref_it->first) ==
                            table.action_ref_tables.end())
                        {
                            table.action_ref_tables.push_back(ref_it->first);
                        }
                    }
                }
            }

            parseTableCounter(table_json, table);
        }
        catch (std::exception &ex)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "can not parse table " << QuotedVar(table_name.c_str()) << "match fields";
        }

        info_entry.m_tableIdNameMap[std::to_string(table_id)] = table_name;
        info_entry.m_tableInfoMap[table_name] = table;
    }

    return ReturnCode();
}

ReturnCodeOr<TablesInfoAppDbEntry> TablesDefnManager::deserializeTablesInfoEntry(
    const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    SWSS_LOG_ENTER();

    TablesInfoAppDbEntry app_db_entry = {};
    try
    {
        nlohmann::json j = nlohmann::json::parse(key);
        app_db_entry.context = j["context"];
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to deserialize tables info";
    }

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        std::string value = fvValue(it);
        if (field == "info")
        {
            app_db_entry.info = value;
        }
        else
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(field) << " in table entry";
        }
    }

    return app_db_entry;
}

ReturnCode validateTablesInfoAppDbEntry(const TablesInfoAppDbEntry &app_db_entry)
{
    // Perform generic APP DB entry validations. Operation specific validations
    // will be done by the respective request process methods.

    return ReturnCode();
}

TablesInfo *TablesDefnManager::getTablesInfoEntry(const std::string &context_key)
{
    SWSS_LOG_ENTER();

    if (m_tablesinfoMap.find(context_key) == m_tablesinfoMap.end())
        return nullptr;

    return &m_tablesinfoMap[context_key];
}

ReturnCode TablesDefnManager::processAddRequest(const TablesInfoAppDbEntry &app_db_entry,
                                                const std::string &context_key)
{
    nlohmann::json tablesinfo_json;
    ReturnCode status;

    SWSS_LOG_ENTER();

    if (!m_tablesinfoMap.empty())
    {
        // For now p4rt can send only same table-definition, so ignore it silently
        return ReturnCode();
    }

    try
    {
        tablesinfo_json = nlohmann::json::parse(app_db_entry.info);
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "tables info from appdb can not be parsed\n";
    }

    TablesInfo tablesinfo_entry(app_db_entry.context, tablesinfo_json);

    status = parseTablesInfo(tablesinfo_json, tablesinfo_entry);
    if (!status.ok())
    {
        return status;
    }

    m_tablesinfoMap[app_db_entry.context] = tablesinfo_entry;
    gP4Orch->tablesinfo = &m_tablesinfoMap[app_db_entry.context];
    return ReturnCode();
}

ReturnCode TablesDefnManager::processUpdateRequest(const TablesInfoAppDbEntry &app_db_entry,
                                                   const std::string &context_key)
{
    SWSS_LOG_ENTER();

    return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED) << "update of Tables Definition not supported";
}

ReturnCode TablesDefnManager::processDeleteRequest(const std::string &context_key)
{
    SWSS_LOG_ENTER();

    auto *tablesinfo = getTablesInfoEntry(context_key);

    if (tablesinfo)
    {
        if (gP4Orch->tablesinfo == tablesinfo)
        {
            gP4Orch->tablesinfo = nullptr;
        }

        tablesinfo->m_tableIdNameMap.clear();
    }

    m_tablesinfoMap.erase(context_key);
    return ReturnCode();
}

ReturnCode TablesDefnManager::getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                                           std::string &object_key)
{
    return StatusCode::SWSS_RC_INVALID_PARAM;
}

std::unordered_map<int, std::unordered_set<int>> createGraph(std::vector<std::pair<int, int>> preReq)
{
    std::unordered_map<int, std::unordered_set<int>> graph;

    for (auto pre : preReq)
    {
        auto it = graph.find(pre.second);
        if (it != graph.end())
        {
            it->second.insert(pre.first);
        }
        else
        {
            graph[pre.second].insert(pre.first);
        }
    }

    return graph;
}

std::unordered_map<int, int> computeIndegree(std::unordered_map<int, std::unordered_set<int>> &graph)
{
    std::unordered_map<int, int> degrees;

    for (auto g_it = graph.begin(); g_it != graph.end(); g_it++)
    {
        for (int neigh : g_it->second)
        {
            auto n_it = degrees.find(neigh);
            if (n_it != degrees.end())
            {
                n_it->second++;
            }
            else
            {
                degrees.insert({neigh, 0});
            }
        }
    }

    return degrees;
}

std::vector<int> findTablePrecedence(int tables, std::vector<std::pair<int, int>> preReq, TablesInfo *tables_info)
{
    std::unordered_map<int, std::unordered_set<int>> graph = createGraph(preReq);
    std::unordered_map<int, int> degrees = computeIndegree(graph);
    std::vector<int> visited;
    std::vector<int> toposort;
    std::queue<int> zeros;

    // initialize queue with tables having no dependencies
    for (auto table_it = tables_info->m_tableInfoMap.begin(); table_it != tables_info->m_tableInfoMap.end(); table_it++)
    {
        TableInfo table_info = table_it->second;
        if (degrees.find(table_info.id) == degrees.end())
        {
            zeros.push(table_info.id);
            visited.push_back(table_info.id);
        }
    }

    for (int i = 0; i < tables; i++)
    {
        // Err input data like possible cyclic dependencies, could not build
        // precedence order
        if (zeros.empty())
        {
            SWSS_LOG_ERROR("Filed to build table precedence order");
            return {};
        }

        // Run BFS
        int zero = zeros.front();
        zeros.pop();
        toposort.push_back(zero);
        auto g_it = graph.find(zero);
        if (g_it != graph.end())
        {
            for (int neigh : g_it->second)
            {
                auto n_it = degrees.find(neigh);
                if (n_it != degrees.end())
                {
                    if (!n_it->second)
                    {
                        if (std::find(visited.begin(), visited.end(), neigh) == visited.end())
                        {
                            zeros.push(neigh);
                            visited.push_back(neigh);
                        }
                    }
                    else
                    {
                        n_it->second--;
                    }
                }
            }
        }
    }

    return toposort;
}

void buildTablePrecedence(TablesInfo *tables_info)
{
    std::vector<std::pair<int, int>> preReq;
    std::vector<int> orderedTables;
    int tables = 0;

    if (!tables_info)
    {
        return;
    }

    // build dependencies
    for (auto table_it = tables_info->m_tableInfoMap.begin(); table_it != tables_info->m_tableInfoMap.end(); table_it++)
    {
        TableInfo table_info = table_it->second;
        tables++;

        for (std::size_t i = 0; i < table_info.action_ref_tables.size(); i++)
        {
            /**
             * For now processing precedence order is only amongst extension tables
             * Skip fixed tables, include them in precedence calculations when fixed
             * and extension tables processing precedence may be interleaved
             */
            if (FixedTablesMap.find(table_info.action_ref_tables[i]) != FixedTablesMap.end())
            {
                continue;
            }

            TableInfo ref_table_info = tables_info->m_tableInfoMap[table_info.action_ref_tables[i]];
            if (std::find(preReq.begin(), preReq.end(), std::make_pair(table_info.id, ref_table_info.id)) ==
                preReq.end())
            {
                preReq.push_back(std::make_pair(table_info.id, ref_table_info.id));
            }
        }
    }

    // find precedence of tables based on dependencies
    orderedTables = findTablePrecedence(tables, preReq, tables_info);

    // update each table with calculated precedence value and build table
    // precedence map
    for (std::size_t i = 0; i < orderedTables.size(); i++)
    {
        auto table_id = orderedTables[i];
        auto id_it = tables_info->m_tableIdNameMap.find(std::to_string(table_id));
        if (id_it == tables_info->m_tableIdNameMap.end())
        {
            continue;
        }

        auto table_it = tables_info->m_tableInfoMap.find(id_it->second);
        if (table_it == tables_info->m_tableInfoMap.end())
        {
            continue;
        }

        table_it->second.precedence = (int)i;
        tables_info->m_tablePrecedenceMap[(int)i] = table_it->second.name;
    }

    return;
}

void TablesDefnManager::enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void TablesDefnManager::drainWithNotExecuted() {
    drainMgmtWithNotExecuted(m_entries, m_publisher);
}

ReturnCode TablesDefnManager::drain() {
    SWSS_LOG_ENTER();

    ReturnCode status;
    while (!m_entries.empty()) {
        auto key_op_fvs_tuple = m_entries.front();
        m_entries.pop_front();
        std::string table_name;
        std::string key;
        parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
        const std::vector<swss::FieldValueTuple> &attributes = kfvFieldsValues(key_op_fvs_tuple);

        auto app_db_entry_or = deserializeTablesInfoEntry(key, attributes);
        if (!app_db_entry_or.ok())
        {
            status = app_db_entry_or.status();
            SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                           QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status,
                                 /*replace=*/true);
            break;
        }
        auto &app_db_entry = *app_db_entry_or;

        status = validateTablesInfoAppDbEntry(app_db_entry);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Validation failed for tables definition APP DB entry with key %s: "
                           "%s",
                           QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status,
                                 /*replace=*/true);
            break;
        }

        const std::string context_key = KeyGenerator::generateTablesInfoKey(app_db_entry.context);

        const std::string &operation = kfvOp(key_op_fvs_tuple);
        if (operation == SET_COMMAND)
        {
            auto *tablesinfo = getTablesInfoEntry(context_key);
            if (tablesinfo == nullptr)
            {
                // Create TablesInfo
                status = processAddRequest(app_db_entry, context_key);
            }
            else
            {
                // Modify existing TablesInfo
                status = processUpdateRequest(app_db_entry, context_key);
            }
        }
        else if (operation == DEL_COMMAND)
        {
            // Delete TablesInfo
            status = processDeleteRequest(context_key);
        }
        else
        {
            status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Unknown operation type " << QuotedVar(operation);
            SWSS_LOG_ERROR("%s", status.message().c_str());
        }
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Processing failed for tables definition APP DB entry with key %s: "
                           "%s",
                           QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
        }
        else
        {
            buildTablePrecedence(gP4Orch->tablesinfo);
        }
        m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple), status,
                             /*replace=*/true);
        if (!status.ok()) {
            break;
        }
    }
    drainWithNotExecuted();
    return status;
}

std::string TablesDefnManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
{
    std::string result = "";
    SWSS_LOG_ENTER();

    return result;
}
