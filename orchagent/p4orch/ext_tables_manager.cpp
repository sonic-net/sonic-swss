#include "p4orch/ext_tables_manager.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <boost/algorithm/string.hpp>

#include "directory.h"
#include <nlohmann/json.hpp>
#include "logger.h"
#include "tokenize.h"
#include "orch.h"
#include "crmorch.h"
#include "p4orch/p4orch.h"
#include "p4orch/p4orch_util.h"

extern sai_counter_api_t*   sai_counter_api;
extern sai_generic_programmable_api_t *sai_generic_programmable_api;

extern Directory<Orch *> gDirectory;
extern sai_object_id_t gSwitchId;
extern P4Orch *gP4Orch;
extern CrmOrch *gCrmOrch;

P4ExtTableEntry *ExtTablesManager::getP4ExtTableEntry(const std::string &table_name, const std::string &key)
{
    SWSS_LOG_ENTER();

    auto it = m_extTables.find(table_name);
    if (it == m_extTables.end())
        return nullptr;

    if (it->second.find(key) == it->second.end())
        return nullptr;

    return &it->second[key];
}

std::string getCrossRefTableName(const std::string table_name)
{
    auto it = FixedTablesMap.find(table_name);
    if (it != FixedTablesMap.end())
    {
        return(it->second);
    }

    return(table_name);
}

ReturnCode ExtTablesManager::validateActionParamsCrossRef(P4ExtTableAppDbEntry &app_db_entry, ActionInfo *action)
{
    const std::string action_name = action->name;
    std::unordered_map<std::string, nlohmann::json> cross_ref_key_j;
    ReturnCode status;

    for (auto param_defn_it = action->params.begin();
              param_defn_it != action->params.end(); param_defn_it++)
    {
        ActionParamInfo action_param_defn = param_defn_it->second;
        if (action_param_defn.table_reference_map.empty())
        {
            continue;
        }

        std::string param_name = param_defn_it->first;

        auto app_db_param_it = app_db_entry.action_params[action_name].find(param_name);
        if (app_db_param_it == app_db_entry.action_params[action_name].end())
        {
            SWSS_LOG_ERROR("Required param not specified for action %s\n", action_name.c_str());
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                     << "Required param not specified for action %s " << action_name.c_str();
        }

        for (auto cross_ref_it = action_param_defn.table_reference_map.begin();
                  cross_ref_it != action_param_defn.table_reference_map.end(); cross_ref_it++)
        {
            cross_ref_key_j[cross_ref_it->first].push_back(nlohmann::json::object_t::value_type(prependMatchField(cross_ref_it->second), app_db_param_it->second));
        }
    }


    for (auto it = cross_ref_key_j.begin(); it != cross_ref_key_j.end(); it++)
    {
        const std::string table_name = getCrossRefTableName(it->first);
        const std::string table_key = it->second.dump();
        std::string key;
        sai_object_type_t object_type;
        sai_object_id_t   oid;
        DepObject dep_object = {};

        if (gP4Orch->m_p4TableToManagerMap.find(table_name) != gP4Orch->m_p4TableToManagerMap.end())
        {
            status = gP4Orch->m_p4TableToManagerMap[table_name]->getSaiObject(table_key, object_type, key);
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Cross-table reference validation failed from fixed-table %s", table_name.c_str());
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                                   << "Cross-table reference valdiation failed from fixed-table";
            }
        }
       	else 
        {
            if (getTableInfo(table_name))
            {
                auto ext_table_key = KeyGenerator::generateExtTableKey(table_name, table_key);
                status = getSaiObject(ext_table_key, object_type, key);
                if (!status.ok())
                {
                    SWSS_LOG_ERROR("Cross-table reference validation failed from extension-table %s", table_name.c_str());
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                                       << "Cross-table reference valdiation failed from extension table";
                }
            }
            else
            {
                SWSS_LOG_ERROR("Cross-table reference validation failed due to non-existent table %s", table_name.c_str());
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                                       << "Cross-table reference valdiation failed due to non-existent table";
            }
        }

        if (!m_p4OidMapper->getOID(object_type, key, &oid))
        {
            SWSS_LOG_ERROR("Cross-table reference validation failed, no OID found from table %s", table_name.c_str());
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                               << "Cross-table reference valdiation failed, no OID found";
        }

        if (oid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Cross-table reference validation failed, null OID expected from table %s", table_name.c_str());
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                               << "Cross-table reference valdiation failed, null OID";
        }

        dep_object.sai_object  = object_type;
        dep_object.key         = key;
        dep_object.oid         = oid;
        app_db_entry.action_dep_objects[action_name] = dep_object;
    }

    return ReturnCode();
}

ReturnCode ExtTablesManager::validateP4ExtTableAppDbEntry(P4ExtTableAppDbEntry &app_db_entry)
{
    // Perform generic APP DB entry validations. Operation specific validations
    // will be done by the respective request process methods.
    ReturnCode status;

    TableInfo *table;
    table = getTableInfo(app_db_entry.table_name);
    if (table == nullptr)
    {
        SWSS_LOG_ERROR("Not a valid extension table %s", app_db_entry.table_name.c_str());
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Not a valid extension table " << app_db_entry.table_name.c_str();
    }

    if (table->action_ref_tables.empty())
    {
        return ReturnCode();
    }

    ActionInfo *action;
    for (auto app_db_action_it = app_db_entry.action_params.begin();
              app_db_action_it != app_db_entry.action_params.end(); app_db_action_it++)
    {
        auto action_name = app_db_action_it->first;
        action = getTableActionInfo(table, action_name);
        if (action == nullptr)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                     << "Not a valid action " << action_name.c_str()
                     << " in extension table " << app_db_entry.table_name.c_str();
        }

        if (!action->refers_to)
        {
            continue;
        }

        status = validateActionParamsCrossRef(app_db_entry, action);
        if (!status.ok())
        {
           return status;
        }
    }

    return ReturnCode();
}


ReturnCodeOr<P4ExtTableAppDbEntry> ExtTablesManager::deserializeP4ExtTableEntry(
    const std::string &table_name,
    const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    std::string  action_name;

    SWSS_LOG_ENTER();

    P4ExtTableAppDbEntry app_db_entry_or = {};
    app_db_entry_or.table_name = table_name;
    app_db_entry_or.table_key  = key;

    action_name = "";
    for (const auto &it : attributes)
    {
        auto field = fvField(it);
        auto value = fvValue(it);

        if (field == p4orch::kAction)
        {
            action_name = value;
            continue;
        }

        const auto &tokenized_fields = tokenize(field, p4orch::kFieldDelimiter);
        if (tokenized_fields.size() <= 1)
        {
            SWSS_LOG_ERROR("Unknown extension entry field");
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                     << "Unknown extension entry field " << QuotedVar(field);
        }

        const auto &prefix = tokenized_fields[0];
        if (prefix == p4orch::kActionParamPrefix)
        {
            const auto &param_name = tokenized_fields[1];
            app_db_entry_or.action_params[action_name][param_name] = value;
            continue;
        }
        else
        {
            SWSS_LOG_ERROR("Unexpected extension entry field");
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected extension entry field " << QuotedVar(field);
        }
    }

    return app_db_entry_or;
}


ReturnCode ExtTablesManager::prepareP4SaiExtAPIParams(const P4ExtTableAppDbEntry &app_db_entry,
                                                      std::string &ext_table_entry_attr)
{
    nlohmann::json   sai_j, sai_metadata_j, sai_array_j = {}, sai_entry_j;

    SWSS_LOG_ENTER();

    try
    {
        TableInfo *table;
        table = getTableInfo(app_db_entry.table_name);
        if (!table)
        {
            SWSS_LOG_ERROR("extension entry for invalid table %s", app_db_entry.table_name.c_str());
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                         << "extension entry for invalid table " << app_db_entry.table_name.c_str();
        }

        nlohmann::json j = nlohmann::json::parse(app_db_entry.table_key);
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            std::string   match, value, prefix;
            std::size_t   pos;

            match = it.key();
            value = it.value();

	    prefix = p4orch::kMatchPrefix;
            pos = match.rfind(prefix);
            if (pos != std::string::npos)
            {
                match.erase(0, prefix.length());
            }
	    else
            {
                SWSS_LOG_ERROR("Failed to encode match fields for sai call");
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to encode match fields for sai call";
            }

	    prefix = p4orch::kFieldDelimiter;
            pos = match.rfind(prefix);
            if (pos != std::string::npos)
            {
                match.erase(0, prefix.length());
            }
	    else
            {
                SWSS_LOG_ERROR("Failed to encode match fields for sai call");
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to encode match fields for sai call";
            }

            auto match_defn_it = table->match_fields.find(match);
            if (match_defn_it == table->match_fields.end())
            {
                SWSS_LOG_ERROR("extension entry for invalid match field %s", match.c_str());
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                         << "extension entry for invalid match field " << match.c_str();
            }

            sai_metadata_j = nlohmann::json::object({});
            sai_metadata_j["sai_attr_value_type"] = match_defn_it->second.datatype;

            sai_j = nlohmann::json::object({});
            sai_j[match]["value"] = value;
            sai_j[match]["sai_metadata"] = sai_metadata_j;

            sai_array_j.push_back(sai_j);
        }

        for (auto app_db_action_it = app_db_entry.action_params.begin();
                  app_db_action_it != app_db_entry.action_params.end(); app_db_action_it++)
        {
            sai_j = nlohmann::json::object({});
            auto action_dep_object_it = app_db_entry.action_dep_objects.find(app_db_action_it->first);
            if (action_dep_object_it == app_db_entry.action_dep_objects.end())
            {
                auto action_defn_it = table->action_fields.find(app_db_action_it->first);
                for (auto app_db_param_it = app_db_action_it->second.begin();
                          app_db_param_it != app_db_action_it->second.end(); app_db_param_it++)
                {
                    nlohmann::json  params_j = nlohmann::json::object({});
                    if (action_defn_it != table->action_fields.end())
                    {
                        auto param_defn_it = action_defn_it->second.params.find(app_db_param_it->first);
                        if (param_defn_it != action_defn_it->second.params.end())
                        {
                            sai_metadata_j = nlohmann::json::object({});
                            sai_metadata_j["sai_attr_value_type"] = param_defn_it->second.datatype;

                            params_j[app_db_param_it->first]["sai_metadata"] = sai_metadata_j;
                        }
                    }
                    params_j[app_db_param_it->first]["value"] = app_db_param_it->second;
                    sai_j[app_db_action_it->first].push_back(params_j);
                }
            }
            else
            {
                auto action_dep_object = action_dep_object_it->second;

                sai_metadata_j = nlohmann::json::object({});
                sai_metadata_j["sai_attr_value_type"] = "SAI_ATTR_VALUE_TYPE_OBJECT_ID";

                sai_j[app_db_action_it->first]["sai_metadata"] = sai_metadata_j;
                sai_j[app_db_action_it->first]["value"] = action_dep_object.oid;
            }

            sai_array_j.push_back(sai_j);
        }
    }
    catch (std::exception &ex)
    {
        SWSS_LOG_ERROR("Failed to encode table %s entry for sai call", app_db_entry.table_name.c_str());
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to encode table entry for sai call";
    }

    sai_entry_j = nlohmann::json::object({});
    sai_entry_j.push_back(nlohmann::json::object_t::value_type("attributes", sai_array_j));
    SWSS_LOG_ERROR("table: %s, sai entry: %s", app_db_entry.table_name.c_str(), sai_entry_j.dump().c_str());
    ext_table_entry_attr = sai_entry_j.dump();

    return ReturnCode();
}

bool removeGenericCounter(sai_object_id_t counter_id)
{
    sai_status_t sai_status = sai_counter_api->remove_counter(counter_id);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove generic counter: %" PRId64 "", counter_id);
        return false;
    }

    return true;
}

bool createGenericCounter(sai_object_id_t &counter_id)
{
    sai_attribute_t counter_attr;
    counter_attr.id = SAI_COUNTER_ATTR_TYPE;
    counter_attr.value.s32 = SAI_COUNTER_TYPE_REGULAR;
    sai_status_t sai_status = sai_counter_api->create_counter(&counter_id, gSwitchId, 1, &counter_attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to create generic counter");
        return false;
    }

    return true;
}


ReturnCode ExtTablesManager::createP4ExtTableEntry(const P4ExtTableAppDbEntry &app_db_entry,
                                              P4ExtTableEntry &ext_table_entry)
{
    ReturnCode status;
    sai_object_type_t object_type;
    std::string key;
    std::string ext_table_entry_attr;
    sai_object_id_t counter_id;

    SWSS_LOG_ENTER();

    status = prepareP4SaiExtAPIParams(app_db_entry, ext_table_entry_attr);
    if (!status.ok())
    {
        return status;
    }

    // Prepare attributes for the SAI create call.
    std::vector<sai_attribute_t> generic_programmable_attrs;
    sai_attribute_t generic_programmable_attr;

    generic_programmable_attr.id = SAI_GENERIC_PROGRAMMABLE_ATTR_OBJECT_NAME;
    generic_programmable_attr.value.s8list.count = (uint32_t)app_db_entry.table_name.size();
    generic_programmable_attr.value.s8list.list = (int8_t *)const_cast<char *>(app_db_entry.table_name.c_str());
    generic_programmable_attrs.push_back(generic_programmable_attr);

    generic_programmable_attr.id = SAI_GENERIC_PROGRAMMABLE_ATTR_ENTRY;
    generic_programmable_attr.value.json.json.count = (uint32_t)ext_table_entry_attr.size();
    generic_programmable_attr.value.json.json.list = (int8_t *)const_cast<char *>(ext_table_entry_attr.c_str());
    generic_programmable_attrs.push_back(generic_programmable_attr);


    auto *table = getTableInfo(app_db_entry.table_name);
    if (!table)
    {
        SWSS_LOG_ERROR("extension entry for invalid table %s", app_db_entry.table_name.c_str());
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                          << "extension entry for invalid table " << app_db_entry.table_name.c_str();
    }

    if (table->counter_bytes_enabled || table->counter_packets_enabled)
    {
        if (!createGenericCounter(counter_id))
        {
            SWSS_LOG_WARN("Failed to create counter for table %s, key %s\n",
       	                  app_db_entry.table_name.c_str(),
       	                  app_db_entry.table_key.c_str());
        }
        else
        {
            ext_table_entry.sai_counter_oid = counter_id;
        }

        generic_programmable_attr.id = SAI_GENERIC_PROGRAMMABLE_ATTR_COUNTER_ID;
        generic_programmable_attr.value.oid = counter_id;
        generic_programmable_attrs.push_back(generic_programmable_attr);
    }

    sai_object_id_t sai_generic_programmable_oid = SAI_NULL_OBJECT_ID;
    sai_status_t sai_status = sai_generic_programmable_api->create_generic_programmable(
                              &sai_generic_programmable_oid, gSwitchId,
                              (uint32_t)generic_programmable_attrs.size(),
                              generic_programmable_attrs.data());
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("create sai api call failed for extension entry table %s, entry %s",
      	                app_db_entry.table_name.c_str(), app_db_entry.table_key.c_str());
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "create sai api call failed for extension entry table "
                           << app_db_entry.table_name.c_str()
                           << " , entry " << app_db_entry.table_key.c_str();
    }
    std::string crm_table_name = "EXT_" + app_db_entry.table_name;
    boost::algorithm::to_upper(crm_table_name);
    gCrmOrch->incCrmExtTableUsedCounter(CrmResourceType::CRM_EXT_TABLE, crm_table_name);


    ext_table_entry.sai_entry_oid = sai_generic_programmable_oid;
    for (auto action_dep_object_it = app_db_entry.action_dep_objects.begin();
              action_dep_object_it != app_db_entry.action_dep_objects.end(); action_dep_object_it++)
    {
        auto action_dep_object = action_dep_object_it->second;
        m_p4OidMapper->increaseRefCount(action_dep_object.sai_object, action_dep_object.key);
        ext_table_entry.action_dep_objects[action_dep_object_it->first] = action_dep_object;
    }


    auto ext_table_key = KeyGenerator::generateExtTableKey(app_db_entry.table_name, app_db_entry.table_key);
    status = getSaiObject(ext_table_key, object_type, key);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Invalid formation of a key %s", ext_table_key.c_str());
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid formation of a key";
    }

    m_p4OidMapper->setOID(object_type, key, ext_table_entry.sai_entry_oid);
    m_extTables[app_db_entry.table_name][app_db_entry.table_key] = ext_table_entry;
    return ReturnCode();
}


ReturnCode ExtTablesManager::updateP4ExtTableEntry(const P4ExtTableAppDbEntry &app_db_entry,
                                              P4ExtTableEntry *ext_table_entry)
{
    ReturnCode status;
    std::string ext_table_entry_attr;
    std::unordered_map<std::string, DepObject> old_action_dep_objects;

    SWSS_LOG_ENTER();

    if (ext_table_entry->sai_entry_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("update sai api call for NULL extension entry table %s, entry %s",
      	                app_db_entry.table_name.c_str(), ext_table_entry->table_key.c_str());
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "update sai api call for NULL extension entry table "
                           << app_db_entry.table_name.c_str()
                           << " , entry " << ext_table_entry->table_key.c_str();
    }

    status = prepareP4SaiExtAPIParams(app_db_entry, ext_table_entry_attr);
    if (!status.ok())
    {
        return status;
    }

    // Prepare attribute for the SAI update call.
    sai_attribute_t generic_programmable_attr;

    generic_programmable_attr.id = SAI_GENERIC_PROGRAMMABLE_ATTR_ENTRY;
    generic_programmable_attr.value.json.json.count = (uint32_t)ext_table_entry_attr.length();
    generic_programmable_attr.value.json.json.list = (int8_t *)const_cast<char *>(ext_table_entry_attr.c_str());

    sai_status_t sai_status = sai_generic_programmable_api->set_generic_programmable_attribute(
                              ext_table_entry->sai_entry_oid,
                              &generic_programmable_attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("update sai api call failed for extension entry table %s, entry %s",
      	                app_db_entry.table_name.c_str(), ext_table_entry->table_key.c_str());
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "update sai api call failed for extension entry table "
                           << app_db_entry.table_name.c_str()
                           << " , entry " << ext_table_entry->table_key.c_str();
    }


    old_action_dep_objects = ext_table_entry->action_dep_objects;
    ext_table_entry->action_dep_objects.clear();

    for (auto action_dep_object_it = app_db_entry.action_dep_objects.begin();
              action_dep_object_it != app_db_entry.action_dep_objects.end(); action_dep_object_it++)
    {
        auto action_dep_object = action_dep_object_it->second;
        m_p4OidMapper->increaseRefCount(action_dep_object.sai_object, action_dep_object.key);
        ext_table_entry->action_dep_objects[action_dep_object_it->first] = action_dep_object;
    }

    for (auto old_action_dep_object_it = old_action_dep_objects.begin();
              old_action_dep_object_it != old_action_dep_objects.end(); old_action_dep_object_it++)
    {
        auto old_action_dep_object = old_action_dep_object_it->second;
        m_p4OidMapper->decreaseRefCount(old_action_dep_object.sai_object, old_action_dep_object.key);
    }

    return ReturnCode();
}

ReturnCode ExtTablesManager::removeP4ExtTableEntry(const std::string &table_name,
                                              const std::string &table_key)
{
    ReturnCode status;
    sai_object_type_t object_type;
    std::string key;

    SWSS_LOG_ENTER();

    auto *ext_table_entry = getP4ExtTableEntry(table_name, table_key);
    if (!ext_table_entry)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "extension entry with key " << QuotedVar(table_key)
                             << " does not exist for table " << QuotedVar(table_name));
    }

    if (ext_table_entry->sai_entry_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("remove sai api call for NULL extension entry table %s, entry %s",
      	                table_name.c_str(), table_key.c_str());
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "remove sai api call for NULL extension entry table "
                           << table_name.c_str() << " , entry " << table_key.c_str();
    }

    SWSS_LOG_ERROR("table: %s, key: %s", ext_table_entry->table_name.c_str(),
                                         ext_table_entry->table_key.c_str());
    sai_status_t sai_status = sai_generic_programmable_api->remove_generic_programmable(
                              ext_table_entry->sai_entry_oid);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("remove sai api call failed for extension entry table %s, entry %s",
      	                table_name.c_str(), table_key.c_str());
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "remove sai api call failed for extension entry table "
                           << table_name.c_str() << " , entry " << table_key.c_str();
    }
    std::string crm_table_name = "EXT_" + table_name;
    boost::algorithm::to_upper(crm_table_name);
    gCrmOrch->decCrmExtTableUsedCounter(CrmResourceType::CRM_EXT_TABLE, crm_table_name);


    auto ext_table_key = KeyGenerator::generateExtTableKey(table_name, table_key);
    status = getSaiObject(ext_table_key, object_type, key);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Invalid formation of a key %s", ext_table_key.c_str());
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid formation of a key";
    }

    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(object_type, key, &ref_count))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to get reference count for " << QuotedVar(key));
    }
    if (ref_count > 0)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "extension entry " << QuotedVar(key)
                             << " referenced by other objects (ref_count = " << ref_count);
    }
    m_p4OidMapper->eraseOID(object_type, key);

    for (auto action_dep_object_it = ext_table_entry->action_dep_objects.begin();
              action_dep_object_it != ext_table_entry->action_dep_objects.end(); action_dep_object_it++)
    {
        auto action_dep_object = action_dep_object_it->second;
        m_p4OidMapper->decreaseRefCount(action_dep_object.sai_object, action_dep_object.key);
    }

    if (ext_table_entry->sai_counter_oid != SAI_NULL_OBJECT_ID)
    {
        m_countersTable->del(ext_table_entry->db_key);
        removeGenericCounter(ext_table_entry->sai_counter_oid);
    }

    m_extTables[table_name].erase(table_key);

    return ReturnCode();
}


ReturnCode ExtTablesManager::processAddRequest(const P4ExtTableAppDbEntry &app_db_entry)
{
    SWSS_LOG_ENTER();

    P4ExtTableEntry ext_table_entry(app_db_entry.db_key, app_db_entry.table_name, app_db_entry.table_key);
    auto status = createP4ExtTableEntry(app_db_entry, ext_table_entry);
    if (!status.ok())
    {
        return status;
    }
    return ReturnCode();
}

ReturnCode ExtTablesManager::processUpdateRequest(const P4ExtTableAppDbEntry &app_db_entry,
                                               P4ExtTableEntry *ext_table_entry)
{
    SWSS_LOG_ENTER();

    auto status = updateP4ExtTableEntry(app_db_entry, ext_table_entry);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to update extension entry with key %s",
       	                app_db_entry.table_key.c_str());
    }
    return ReturnCode();
}

ReturnCode ExtTablesManager::processDeleteRequest(const P4ExtTableAppDbEntry &app_db_entry)
{
    SWSS_LOG_ENTER();

    auto status = removeP4ExtTableEntry(app_db_entry.table_name, app_db_entry.table_key);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to remove extension entry with key %s",
       	                app_db_entry.table_key.c_str());
    }
    return ReturnCode();
}


ReturnCode ExtTablesManager::getSaiObject(const std::string &json_key, sai_object_type_t &object_type, std::string &object_key)
{
    object_type = SAI_OBJECT_TYPE_GENERIC_PROGRAMMABLE;
    object_key = json_key;

    return ReturnCode();
}

void ExtTablesManager::enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entriesTables[table_name].push_back(entry);
}

void ExtTablesManager::drain()
{
    SWSS_LOG_ENTER();
    std::string table_prefix = "EXT_";

    if (gP4Orch->tablesinfo) {
      for (auto table_it = gP4Orch->tablesinfo->m_tablePrecedenceMap.begin();
                table_it != gP4Orch->tablesinfo->m_tablePrecedenceMap.end(); ++table_it)
      {
        auto table_name = table_prefix + table_it->second;
        boost::algorithm::to_upper(table_name);
        auto it_m = m_entriesTables.find(table_name);
        if (it_m == m_entriesTables.end())
        {
            continue;
        }

        for (const auto &key_op_fvs_tuple : it_m->second)
        {
            std::string table_name;
            std::string table_key;

            parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &table_key);
            const std::vector<swss::FieldValueTuple> &attributes = kfvFieldsValues(key_op_fvs_tuple);

            if (table_name.rfind(table_prefix, 0) == std::string::npos)
            {
                SWSS_LOG_ERROR("Table %s is without prefix %s", table_name.c_str(), table_prefix.c_str());
                m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                     StatusCode::SWSS_RC_INVALID_PARAM, /*replace=*/true);
                continue;
            }
            table_name = table_name.substr(table_prefix.length());
            boost::algorithm::to_lower(table_name);

            ReturnCode status;
            auto app_db_entry_or = deserializeP4ExtTableEntry(table_name, table_key, attributes);
            if (!app_db_entry_or.ok())
            {
                status = app_db_entry_or.status();
                SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                               QuotedVar(kfvKey(key_op_fvs_tuple)).c_str(), status.message().c_str());
                m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                     status, /*replace=*/true);
                continue;
            }

            auto &app_db_entry = *app_db_entry_or;
            status = validateP4ExtTableAppDbEntry(app_db_entry);
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Validation failed for extension APP DB entry with key %s: %s",
                               QuotedVar(kfvKey(key_op_fvs_tuple)).c_str(), status.message().c_str());
                m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                     status, /*replace=*/true);
                continue;
            }

            const std::string &operation = kfvOp(key_op_fvs_tuple);
            if (operation == SET_COMMAND)
            {
                auto *ext_table_entry = getP4ExtTableEntry(app_db_entry.table_name, app_db_entry.table_key);
                if (ext_table_entry == nullptr)
                {
                    // Create extension entry
                    app_db_entry.db_key = kfvKey(key_op_fvs_tuple);
                    status = processAddRequest(app_db_entry);
                }
                else
                {
                    // Modify existing extension entry
                    status = processUpdateRequest(app_db_entry, ext_table_entry);
                }
            }
            else if (operation == DEL_COMMAND)
            {
                // Delete extension entry
                status = processDeleteRequest(app_db_entry);
            }
            else
            {
                status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
       	                   << "Unknown operation type " << QuotedVar(operation);
                SWSS_LOG_ERROR("%s", status.message().c_str());
            }
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Processing failed for extension APP_DB entry with key %s: %s",
                               QuotedVar(kfvKey(key_op_fvs_tuple)).c_str(), status.message().c_str());
            }
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status, /*replace=*/true);
        }

        it_m->second.clear();
      }
    }

    // Now report error for all remaining un-processed entries
    for (auto it_m = m_entriesTables.begin(); it_m != m_entriesTables.end(); it_m++)
    {
        for (const auto &key_op_fvs_tuple : it_m->second)
        {
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 StatusCode::SWSS_RC_INVALID_PARAM, /*replace=*/true);
        }

        it_m->second.clear();
    }
}


void ExtTablesManager::doExtCounterStatsTask()
{
    SWSS_LOG_ENTER();

    if (!gP4Orch->tablesinfo)
    {
        return;
    }

    sai_stat_id_t stat_ids[] = { SAI_COUNTER_STAT_PACKETS, SAI_COUNTER_STAT_BYTES };
    uint64_t stats[2];
    std::vector<swss::FieldValueTuple> counter_stats_values;

    for (auto table_it = gP4Orch->tablesinfo->m_tableInfoMap.begin();
              table_it != gP4Orch->tablesinfo->m_tableInfoMap.end(); ++table_it)
    {
        if (!table_it->second.counter_bytes_enabled && !table_it->second.counter_packets_enabled)
        {
            continue;
        }

        auto table_name = table_it->second.name;
        auto ext_table_it = m_extTables.find(table_name);
        if (ext_table_it == m_extTables.end())
        {
            continue;
        }

        for (auto ext_table_entry_it = ext_table_it->second.begin();
                  ext_table_entry_it != ext_table_it->second.end(); ++ext_table_entry_it)
        {
            auto *ext_table_entry = &ext_table_entry_it->second;
            if (ext_table_entry->sai_counter_oid == SAI_NULL_OBJECT_ID)
            {
                continue;
            }

            sai_status_t sai_status =
                    sai_counter_api->get_counter_stats(ext_table_entry->sai_counter_oid, 2, stat_ids, stats);
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_WARN("Failed to set counters stats for extension entry %s:%s in COUNTERS_DB: ",
       	                       table_name.c_str(), ext_table_entry->table_key.c_str());
                continue;
            }

            counter_stats_values.push_back(
                    swss::FieldValueTuple{P4_COUNTER_STATS_PACKETS, std::to_string(stats[0])});
            counter_stats_values.push_back(
                    swss::FieldValueTuple{P4_COUNTER_STATS_BYTES, std::to_string(stats[1])});

            // Set field value tuples for counters stats in COUNTERS_DB
            m_countersTable->set(ext_table_entry->db_key, counter_stats_values);
        }
    }
}

std::string ExtTablesManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
{
    std::string result = "";
    SWSS_LOG_ENTER();

    return result;
}

