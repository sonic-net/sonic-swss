#include "p4orch/acl_table_manager.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "SaiAttributeList.h"
#include "crmorch.h"
#include "dbconnector.h"
#include "logger.h"
#include "orch.h"
#include "p4orch.h"
#include "p4orch/p4orch_util.h"
#include "sai_serialize.h"
#include "switchorch.h"
#include "table.h"
#include "tokenize.h"
extern "C"
{
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

extern sai_object_id_t gSwitchId;
extern sai_acl_api_t *sai_acl_api;
extern sai_udf_api_t *sai_udf_api;
extern sai_switch_api_t *sai_switch_api;
extern CrmOrch *gCrmOrch;
extern P4Orch *gP4Orch;
extern SwitchOrch *gSwitchOrch;
extern int gBatchSize;

namespace p4orch
{
namespace
{

std::vector<sai_attribute_t> getGroupMemSaiAttrs(const P4AclTableDefinition &acl_table)
{
    std::vector<sai_attribute_t> acl_mem_attrs;
    sai_attribute_t acl_mem_attr;
    acl_mem_attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID;
    acl_mem_attr.value.oid = acl_table.group_oid;
    acl_mem_attrs.push_back(acl_mem_attr);

    acl_mem_attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID;
    acl_mem_attr.value.oid = acl_table.table_oid;
    acl_mem_attrs.push_back(acl_mem_attr);

    acl_mem_attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY;
    acl_mem_attr.value.u32 = acl_table.priority;
    acl_mem_attrs.push_back(acl_mem_attr);

    return acl_mem_attrs;
}

std::vector<sai_attribute_t> getUdfGroupSaiAttrs(const P4UdfField &udf_field)
{
    std::vector<sai_attribute_t> udf_group_attrs;
    sai_attribute_t udf_group_attr;
    udf_group_attr.id = SAI_UDF_GROUP_ATTR_TYPE;
    udf_group_attr.value.s32 = SAI_UDF_GROUP_TYPE_GENERIC;
    udf_group_attrs.push_back(udf_group_attr);

    udf_group_attr.id = SAI_UDF_GROUP_ATTR_LENGTH;
    udf_group_attr.value.u16 = udf_field.length;
    udf_group_attrs.push_back(udf_group_attr);

    return udf_group_attrs;
}

} // namespace

AclTableManager::AclTableManager(P4OidMapper *p4oidMapper, ResponsePublisherInterface *publisher)
    : m_p4OidMapper(p4oidMapper), m_publisher(publisher)
{
    SWSS_LOG_ENTER();

    assert(p4oidMapper != nullptr);
}

AclTableManager::~AclTableManager()
{
    sai_object_id_t udf_match_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_UDF_MATCH, P4_UDF_MATCH_DEFAULT, &udf_match_oid))
    {
        return;
    }
    auto status = removeDefaultUdfMatch();
    if (!status.ok())
    {
        status.prepend("Failed to remove default UDF match: ");
        SWSS_LOG_ERROR("%s", status.message().c_str());
    }
}

ReturnCodeOr<std::vector<sai_attribute_t>> AclTableManager::getTableSaiAttrs(const P4AclTableDefinition &acl_table)
{
    std::vector<sai_attribute_t> acl_attr_list;
    sai_attribute_t acl_attr;
    acl_attr.id = SAI_ACL_TABLE_ATTR_ACL_STAGE;
    acl_attr.value.s32 = acl_table.stage;
    acl_attr_list.push_back(acl_attr);

    if (acl_table.size > 0)
    {
        acl_attr.id = SAI_ACL_TABLE_ATTR_SIZE;
        acl_attr.value.u32 = acl_table.size;
        acl_attr_list.push_back(acl_attr);
    }

    std::set<acl_table_attr_union_t> table_match_fields_to_add;
    if (!acl_table.ip_type_bit_type_lookup.empty())
    {
        acl_attr.id = SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE;
        acl_attr.value.booldata = true;
        acl_attr_list.push_back(acl_attr);
        table_match_fields_to_add.insert(SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE);
    }

    for (const auto &match_field : acl_table.sai_match_field_lookup)
    {
        const auto &sai_match_field = fvValue(match_field);
        // Avoid duplicate match attribute to add
        if (table_match_fields_to_add.find(sai_match_field.table_attr) != table_match_fields_to_add.end())
            continue;
        acl_attr.id = sai_match_field.table_attr;
        acl_attr.value.booldata = true;
        acl_attr_list.push_back(acl_attr);
        table_match_fields_to_add.insert(sai_match_field.table_attr);
    }

    for (const auto &match_fields : acl_table.composite_sai_match_fields_lookup)
    {
        const auto &sai_match_fields = fvValue(match_fields);
        for (const auto &sai_match_field : sai_match_fields)
        {
            // Avoid duplicate match attribute to add
            if (table_match_fields_to_add.find(sai_match_field.table_attr) != table_match_fields_to_add.end())
                continue;
            acl_attr.id = sai_match_field.table_attr;
            acl_attr.value.booldata = true;
            acl_attr_list.push_back(acl_attr);
            table_match_fields_to_add.insert(sai_match_field.table_attr);
        }
    }

    // Add UDF group attributes
    for (const auto &udf_group_idx : acl_table.udf_group_attr_index_lookup)
    {
        acl_attr.id = SAI_ACL_TABLE_ATTR_USER_DEFINED_FIELD_GROUP_MIN + fvValue(udf_group_idx);
        if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_UDF_GROUP, fvField(udf_group_idx), &acl_attr.value.oid))
        {
            LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                                 << "THe UDF group with id " << QuotedVar(fvField(udf_group_idx)) << " was not found.");
        }
        acl_attr_list.push_back(acl_attr);
    }

    m_acl_action_list[0] = SAI_ACL_ACTION_TYPE_COUNTER;
    acl_attr.id = SAI_ACL_TABLE_ATTR_ACL_ACTION_TYPE_LIST;
    acl_attr.value.s32list.count = 1;
    acl_attr.value.s32list.list = m_acl_action_list;
    acl_attr_list.push_back(acl_attr);

    return acl_attr_list;
}

ReturnCodeOr<std::vector<sai_attribute_t>> AclTableManager::getUdfSaiAttrs(const P4UdfField &udf_field)
{
    sai_object_id_t udf_group_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_UDF_GROUP, udf_field.group_id, &udf_group_oid))
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "UDF group " << QuotedVar(udf_field.group_id) << " does not exist";
    }
    sai_object_id_t udf_match_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_UDF_MATCH, P4_UDF_MATCH_DEFAULT, &udf_match_oid))
    {
        // Create the default UDF match
        LOG_AND_RETURN_IF_ERROR(createDefaultUdfMatch()
                                << "Failed to create ACL UDF default match " << QuotedVar(P4_UDF_MATCH_DEFAULT));
        m_p4OidMapper->getOID(SAI_OBJECT_TYPE_UDF_MATCH, P4_UDF_MATCH_DEFAULT, &udf_match_oid);
    }
    std::vector<sai_attribute_t> udf_attrs;
    sai_attribute_t udf_attr;
    udf_attr.id = SAI_UDF_ATTR_GROUP_ID;
    udf_attr.value.oid = udf_group_oid;
    udf_attrs.push_back(udf_attr);

    udf_attr.id = SAI_UDF_ATTR_MATCH_ID;
    udf_attr.value.oid = udf_match_oid;
    udf_attrs.push_back(udf_attr);

    udf_attr.id = SAI_UDF_ATTR_BASE;
    udf_attr.value.s32 = udf_field.base;
    udf_attrs.push_back(udf_attr);

    udf_attr.id = SAI_UDF_ATTR_OFFSET;
    udf_attr.value.u16 = udf_field.offset;
    udf_attrs.push_back(udf_attr);

    return udf_attrs;
}

ReturnCode AclTableManager::getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                                         std::string &object_key)
{
    return StatusCode::SWSS_RC_UNIMPLEMENTED;
}

void AclTableManager::enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void AclTableManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

ReturnCode AclTableManager::drain() {
  SWSS_LOG_ENTER();

  ReturnCode status;
  while (!m_entries.empty()) {
    auto key_op_fvs_tuple = m_entries.front();
    m_entries.pop_front();
    std::string table_name;
    std::string db_key;

    parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &db_key);
    SWSS_LOG_NOTICE("P4AclTableManager drain tuple for table %s",
                    QuotedVar(table_name).c_str());
    if (table_name != APP_P4RT_ACL_TABLE_DEFINITION_NAME) {
      status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid table " << QuotedVar(table_name);
      SWSS_LOG_ERROR("%s", status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    const std::vector<swss::FieldValueTuple>& attributes =
        kfvFieldsValues(key_op_fvs_tuple);

    const std::string& operation = kfvOp(key_op_fvs_tuple);
    if (operation == SET_COMMAND) {
      auto app_db_entry_or =
          deserializeAclTableDefinitionAppDbEntry(db_key, attributes);
      if (!app_db_entry_or.ok()) {
        status = app_db_entry_or.status();
        SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                       QuotedVar(table_name + ":" + db_key).c_str(),
                       status.message().c_str());
        m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                             kfvFieldsValues(key_op_fvs_tuple), status,
                             /*replace=*/true);
        break;
      }
      auto& app_db_entry = *app_db_entry_or;

      status = validateAclTableDefinitionAppDbEntry(app_db_entry);
      if (!status.ok()) {
        SWSS_LOG_ERROR(
            "Validation failed for ACL definition APP DB entry with key %s: %s",
            QuotedVar(table_name + ":" + db_key).c_str(),
            status.message().c_str());
        m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                             kfvFieldsValues(key_op_fvs_tuple), status,
                             /*replace=*/true);
        break;
      }
      auto* acl_table_definition = getAclTable(app_db_entry.acl_table_name);
      if (acl_table_definition == nullptr) {
        SWSS_LOG_NOTICE("ACL table SET %s",
                        app_db_entry.acl_table_name.c_str());
        status = processAddTableRequest(app_db_entry);
      } else {
        // All attributes in sai_acl_table_attr_t are CREATE_ONLY.
        status =
            ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
            << "Unable to update ACL table definition in APP DB entry with key "
            << QuotedVar(table_name + ":" + db_key)
            << " : All attributes in sai_acl_table_attr_t are CREATE_ONLY.";
      }
    } else if (operation == DEL_COMMAND) {
      status = processDeleteTableRequest(db_key);
    } else {
      status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Unknown operation type " << QuotedVar(operation);
    }
    if (!status.ok()) {
      SWSS_LOG_ERROR("Processed DEFINITION entry status: %s",
                     status.message().c_str());
    }
    m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                         kfvFieldsValues(key_op_fvs_tuple), status,
                         /*replace=*/true);
    if (!status.ok()) {
      break;
    }
  }
  drainWithNotExecuted();
  return status;
}

ReturnCodeOr<P4AclTableDefinitionAppDbEntry> AclTableManager::deserializeAclTableDefinitionAppDbEntry(
    const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    SWSS_LOG_ENTER();

    P4AclTableDefinitionAppDbEntry app_db_entry = {};
    app_db_entry.acl_table_name = key;

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        const auto &value = fvValue(it);
        SWSS_LOG_INFO("ACL table definition attr string %s : %s\n", QuotedVar(field).c_str(), QuotedVar(value).c_str());
        if (field == kStage)
        {
            app_db_entry.stage = value;
            continue;
        }
        else if (field == kPriority)
        {
            int priority = std::stoi(value);
            if (priority < 0)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid ACL table priority " << QuotedVar(value);
            }
            app_db_entry.priority = static_cast<uint32_t>(priority);
            continue;
        }
        else if (field == kSize)
        {
            int size = std::stoi(value);
            if (size < 0)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid ACL table size " << QuotedVar(value);
            }
            app_db_entry.size = static_cast<uint32_t>(size);
            continue;
        }
        else if (field == kMeterUnit)
        {
            app_db_entry.meter_unit = value;
            continue;
        }
        else if (field == kCounterUnit)
        {
            app_db_entry.counter_unit = value;
            continue;
        }
        std::vector<std::string> tokenized_field = swss::tokenize(field, kFieldDelimiter);
        if (tokenized_field.size() <= 1)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unknown ACL table definition field string " << QuotedVar(field);
        }
        const auto &p4_field = tokenized_field[1];
        if (tokenized_field[0] == kMatchPrefix)
        {
            app_db_entry.match_field_lookup[p4_field] = value;
        }
        else if (tokenized_field[0] == kAction)
        {
            if (!parseAclTableAppDbActionField(value, &app_db_entry.action_field_lookup[p4_field],
                                               &app_db_entry.packet_action_color_lookup[p4_field]))
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Error parsing ACL table definition action " << QuotedVar(field) << ":" << QuotedVar(value);
            }
        }
        else
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unknown ACL table definition field string " << QuotedVar(field);
        }
    }
    return app_db_entry;
}

ReturnCode AclTableManager::validateAclTableDefinitionAppDbEntry(const P4AclTableDefinitionAppDbEntry &app_db_entry)
{
    // Perform generic APP DB entry validations. Operation specific
    // validations will be done by the respective request process methods.
    if (!app_db_entry.meter_unit.empty() && app_db_entry.meter_unit != P4_METER_UNIT_BYTES &&
        app_db_entry.meter_unit != P4_METER_UNIT_PACKETS)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table meter unit " << QuotedVar(app_db_entry.meter_unit) << " is invalid";
    }
    if (!app_db_entry.counter_unit.empty() && app_db_entry.counter_unit != P4_COUNTER_UNIT_BYTES &&
        app_db_entry.counter_unit != P4_COUNTER_UNIT_PACKETS && app_db_entry.counter_unit != P4_COUNTER_UNIT_BOTH)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table counter unit " << QuotedVar(app_db_entry.counter_unit) << " is invalid";
    }
    return ReturnCode();
}

P4AclTableDefinition *AclTableManager::getAclTable(const std::string &acl_table_name)
{
    SWSS_LOG_ENTER();
    if (m_aclTableDefinitions.find(acl_table_name) == m_aclTableDefinitions.end())
        return nullptr;
    return &m_aclTableDefinitions[acl_table_name];
}

ReturnCode AclTableManager::processAddTableRequest(const P4AclTableDefinitionAppDbEntry &app_db_entry)
{
    SWSS_LOG_ENTER();

    auto stage_it = aclStageLookup.find(app_db_entry.stage);
    sai_acl_stage_t stage;
    if (stage_it != aclStageLookup.end())
    {
        stage = stage_it->second;
    }
    else
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "ACL table stage " << QuotedVar(app_db_entry.stage) << " is invalid");
    }

    if (gSwitchOrch->getAclGroupsBindingToSwitch().empty())
    {
        // Create default ACL groups binding to switch
        gSwitchOrch->initAclGroupsBindToSwitch();
    }

    P4AclTableDefinition acl_table_definition(app_db_entry.acl_table_name, stage, app_db_entry.priority,
                                              app_db_entry.size, app_db_entry.meter_unit, app_db_entry.counter_unit);

    auto &group_map = gSwitchOrch->getAclGroupsBindingToSwitch();
    auto group_it = group_map.find(acl_table_definition.stage);
    if (group_it == group_map.end())
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to find ACL group binding to switch at stage "
                                                 << acl_table_definition.stage);
    }
    acl_table_definition.group_oid = group_it->second.m_saiObjectId;

    auto build_match_rc =
        buildAclTableDefinitionMatchFieldValues(app_db_entry.match_field_lookup, &acl_table_definition);

    LOG_AND_RETURN_IF_ERROR(
        build_match_rc.prepend("Failed to build ACL table definition match fields with table name " +
                               QuotedVar(app_db_entry.acl_table_name) + ": "));

    auto build_action_rc = buildAclTableDefinitionActionFieldValues(app_db_entry.action_field_lookup,
                                                                    &acl_table_definition.rule_action_field_lookup);

    LOG_AND_RETURN_IF_ERROR(
        build_action_rc.prepend("Failed to build ACL table definition action fields with table name " +
                                QuotedVar(app_db_entry.acl_table_name) + ": "));

    if (gP4Orch->getAclRuleManager()->m_userDefinedTraps.empty() &&
        isSetUserTrapActionInAclTableDefinition(acl_table_definition.rule_action_field_lookup))
    {
        // Set up User Defined Traps for QOS_QUEUE action
        auto status = gP4Orch->getAclRuleManager()->setUpUserDefinedTraps();
        if (!status.ok())
        {
            gP4Orch->getAclRuleManager()->cleanUpUserDefinedTraps();
            LOG_ERROR_AND_RETURN(status);
        }
    }

    auto build_action_color_rc = buildAclTableDefinitionActionColorFieldValues(
        app_db_entry.packet_action_color_lookup, &acl_table_definition.rule_action_field_lookup,
        &acl_table_definition.rule_packet_action_color_lookup);
    LOG_AND_RETURN_IF_ERROR(build_action_color_rc.prepend("Failed to build ACL table definition "
                                                          "action color fields with table name " +
                                                          QuotedVar(app_db_entry.acl_table_name) + ": "));

    if (!acl_table_definition.udf_fields_lookup.empty())
    {
        LOG_AND_RETURN_IF_ERROR(createUdfGroupsAndUdfsForAclTable(acl_table_definition));
    }

    auto status =
        createAclTable(acl_table_definition, &acl_table_definition.table_oid, &acl_table_definition.group_member_oid);
    if (!status.ok())
    {
        // Clean up newly created UDFs and UDF groups
        for (auto &udf_fields : acl_table_definition.udf_fields_lookup)
        {
            for (auto &udf_field : fvValue(udf_fields))
            {
                auto rc = removeUdf(udf_field.udf_id, udf_field.group_id);
                if (!rc.ok())
                {
                    SWSS_LOG_ERROR("Failed to remove UDF %s: %s", QuotedVar(udf_field.udf_id).c_str(),
                                   rc.message().c_str());
                    SWSS_RAISE_CRITICAL_STATE("Failed to remove UDF in recovery.");
                }
                rc = removeUdfGroup(udf_field.group_id);
                if (!rc.ok())
                {
                    SWSS_LOG_ERROR("Failed to remove UDF group %s: %s", QuotedVar(udf_field.group_id).c_str(),
                                   rc.message().c_str());
                    SWSS_RAISE_CRITICAL_STATE("Failed to remove UDF group in recovery.");
                }
            }
        }
        LOG_ERROR_AND_RETURN(
            status.prepend("Failed to create ACL table with key " + QuotedVar(app_db_entry.acl_table_name)));
    }
    return status;
}

ReturnCode AclTableManager::createDefaultUdfMatch()
{
    SWSS_LOG_ENTER();
    sai_object_id_t udf_match_oid;
    std::vector<sai_attribute_t> udf_match_attrs;
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_udf_api->create_udf_match(&udf_match_oid, gSwitchId, 0, udf_match_attrs.data()),
                                   "Failed to create default UDF match from SAI call "
                                   "sai_udf_api->create_udf_match");
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_UDF_MATCH, P4_UDF_MATCH_DEFAULT, udf_match_oid);
    SWSS_LOG_INFO("Suceeded to create default UDF match %s with object ID %s ", QuotedVar(P4_UDF_MATCH_DEFAULT).c_str(),
                  sai_serialize_object_id(udf_match_oid).c_str());
    return ReturnCode();
}

ReturnCode AclTableManager::removeDefaultUdfMatch()
{
    SWSS_LOG_ENTER();
    sai_object_id_t udf_match_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_UDF_MATCH, P4_UDF_MATCH_DEFAULT, &udf_match_oid))
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "Default UDF match " << QuotedVar(P4_UDF_MATCH_DEFAULT) << " was not found";
    }

    // Check if there is anything referring to the UDF match before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_UDF_MATCH, P4_UDF_MATCH_DEFAULT, &ref_count))
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "Default UDF match " << QuotedVar(P4_UDF_MATCH_DEFAULT) << " reference count was not found";
    }
    if (ref_count > 0)
    {
        return ReturnCode(StatusCode::SWSS_RC_IN_USE)
               << "Default UDF match " << QuotedVar(P4_UDF_MATCH_DEFAULT)
               << " is referenced by other objects (ref_count = " << ref_count << ")";
    }

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_udf_api->remove_udf_match(udf_match_oid),
                                   "Failed to remove default UDF match with id " << udf_match_oid);
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_UDF_MATCH, P4_UDF_MATCH_DEFAULT);

    SWSS_LOG_INFO("Suceeded to remove UDF match %s : %s", QuotedVar(P4_UDF_MATCH_DEFAULT).c_str(),
                  sai_serialize_object_id(udf_match_oid).c_str());
    return ReturnCode();
}

ReturnCode AclTableManager::createUdfGroup(const P4UdfField &udf_field)
{
    SWSS_LOG_ENTER();
    sai_object_id_t udf_group_oid;
    auto attrs = getUdfGroupSaiAttrs(udf_field);

    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_udf_api->create_udf_group(&udf_group_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data()),
        "Failed to create UDF group " << QuotedVar(udf_field.group_id)
                                      << " from SAI call sai_udf_api->create_udf_group");
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_UDF_GROUP, udf_field.group_id, udf_group_oid);
    SWSS_LOG_INFO("Suceeded to create UDF group %s with object ID %s ", QuotedVar(udf_field.group_id).c_str(),
                  sai_serialize_object_id(udf_group_oid).c_str());
    return ReturnCode();
}

ReturnCode AclTableManager::removeUdfGroup(const std::string &udf_group_id)
{
    SWSS_LOG_ENTER();
    sai_object_id_t group_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_UDF_GROUP, udf_group_id, &group_oid))
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "UDF group " << QuotedVar(udf_group_id) << " was not found";
    }

    // Check if there is anything referring to the UDF group before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_UDF_GROUP, udf_group_id, &ref_count))
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "UDF group " << QuotedVar(udf_group_id) << " reference count was not found";
    }
    if (ref_count > 0)
    {
        return ReturnCode(StatusCode::SWSS_RC_IN_USE)
               << "UDF group " << QuotedVar(udf_group_id) << " referenced by other objects (ref_count = " << ref_count
               << ")";
    }

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_udf_api->remove_udf_group(group_oid),
                                   "Failed to remove UDF group with id " << QuotedVar(udf_group_id));
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_UDF_GROUP, udf_group_id);

    SWSS_LOG_NOTICE("Suceeded to remove UDF group %s: %s", QuotedVar(udf_group_id).c_str(),
                    sai_serialize_object_id(group_oid).c_str());
    return ReturnCode();
}

ReturnCode AclTableManager::createUdf(const P4UdfField &udf_field)
{
    SWSS_LOG_ENTER();
    const auto &udf_id = udf_field.udf_id;

    ASSIGN_OR_RETURN(auto attrs, getUdfSaiAttrs(udf_field));

    sai_object_id_t udf_oid;
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_udf_api->create_udf(&udf_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data()),
                                   "Failed to create UDF " << QuotedVar(udf_id)
                                                           << " from SAI call sai_udf_api->create_udf");
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_UDF, udf_id, udf_oid);
    // Increase UDF group and match reference count
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_UDF_MATCH, P4_UDF_MATCH_DEFAULT);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_UDF_GROUP, udf_field.group_id);
    SWSS_LOG_NOTICE("Suceeded to create UDF %s with object ID %s ", QuotedVar(udf_id).c_str(),
                    sai_serialize_object_id(udf_oid).c_str());
    return ReturnCode();
}

ReturnCode AclTableManager::removeUdf(const std::string &udf_id, const std::string &udf_group_id)
{
    SWSS_LOG_ENTER();
    sai_object_id_t udf_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_UDF, udf_id, &udf_oid))
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "UDF " << QuotedVar(udf_id) << " was not found";
    }
    // Check if there is anything referring to the UDF before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_UDF, udf_id, &ref_count))
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "UDF " << QuotedVar(udf_id) << " reference count was not found";
    }
    if (ref_count > 0)
    {
        return ReturnCode(StatusCode::SWSS_RC_IN_USE)
               << "UDF " << QuotedVar(udf_id) << " referenced by other objects (ref_count = " << ref_count << ")";
    }

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_udf_api->remove_udf(udf_oid), "Failed to remove UDF with id "
                                                                         << udf_oid
                                                                         << " from SAI call sai_udf_api->remove_udf");
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_UDF, udf_id);
    // Decrease UDF group and match reference count
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_UDF_MATCH, P4_UDF_MATCH_DEFAULT);
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_UDF_GROUP, udf_group_id);
    SWSS_LOG_NOTICE("Suceeded to remove UDF %s: %s", QuotedVar(udf_id).c_str(),
                    sai_serialize_object_id(udf_oid).c_str());
    return ReturnCode();
}

ReturnCode AclTableManager::createUdfGroupsAndUdfsForAclTable(const P4AclTableDefinition &acl_table_definition)
{
    ReturnCode status;
    // Cache newly created UDFs
    std::vector<P4UdfField> created_udf_fields;
    // Cache newly created UDF groups,
    std::vector<std::string> created_udf_group_ids;
    // Create UDF groups and UDFs
    for (auto &udf_fields : acl_table_definition.udf_fields_lookup)
    {
        for (auto &udf_field : fvValue(udf_fields))
        {
            status = createUdfGroup(udf_field);
            if (!status.ok())
            {
                status.prepend("Failed to create ACL UDF group with group id " + QuotedVar(udf_field.group_id) + " : ");
                break;
            }
            created_udf_group_ids.push_back(udf_field.group_id);
            status = createUdf(udf_field);
            if (!status.ok())
            {
                status.prepend("Failed to create ACL UDF with id " + QuotedVar(udf_field.udf_id) + ": ");
                break;
            }
            created_udf_fields.push_back(udf_field);
        }
        if (!status.ok())
            break;
    }
    // Clean up created UDFs and UDF groups if fails to create all.
    if (!status.ok())
    {
        for (const auto &udf_field : created_udf_fields)
        {
            auto rc = removeUdf(udf_field.udf_id, udf_field.group_id);
            if (!rc.ok())
            {
                SWSS_LOG_ERROR("Failed to remove UDF %s: %s", QuotedVar(udf_field.udf_id).c_str(),
                               rc.message().c_str());
                SWSS_RAISE_CRITICAL_STATE("Failed to remove UDF in recovery.");
            }
        }
        for (const auto &udf_group_id : created_udf_group_ids)
        {
            auto rc = removeUdfGroup(udf_group_id);
            if (!rc.ok())
            {
                SWSS_LOG_ERROR("Failed to remove UDF group %s: %s", QuotedVar(udf_group_id).c_str(),
                               rc.message().c_str());
                SWSS_RAISE_CRITICAL_STATE("Failed to remove UDF group in recovery.");
            }
        }
        LOG_ERROR_AND_RETURN(status);
    }
    return ReturnCode();
}

ReturnCode AclTableManager::createAclTable(P4AclTableDefinition &acl_table, sai_object_id_t *acl_table_oid,
                                           sai_object_id_t *acl_group_member_oid)
{
    // Prepare SAI ACL attributes list to create ACL table
    ASSIGN_OR_RETURN(auto attrs, getTableSaiAttrs(acl_table));

    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_acl_api->create_acl_table(acl_table_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data()),
        "Failed to create ACL table " << QuotedVar(acl_table.acl_table_name));
    SWSS_LOG_NOTICE("Called SAI API to create ACL table %s ", sai_serialize_object_id(*acl_table_oid).c_str());
    auto status = createAclGroupMember(acl_table, acl_group_member_oid);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to create ACL group member for table %s", QuotedVar(acl_table.acl_table_name).c_str());
        auto sai_status = sai_acl_api->remove_acl_table(*acl_table_oid);
        if (sai_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove ACL table %s SAI_STATUS: %s", QuotedVar(acl_table.acl_table_name).c_str(),
                           sai_serialize_status(sai_status).c_str());
            SWSS_RAISE_CRITICAL_STATE("Failed to remove ACL table in recovery.");
        }
        return status;
    }
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_ACL_TABLE, acl_table.acl_table_name, *acl_table_oid);
    gCrmOrch->incCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE, (sai_acl_stage_t)acl_table.stage,
                                   SAI_ACL_BIND_POINT_TYPE_SWITCH);
    m_aclTablesByStage[acl_table.stage].push_back(acl_table.acl_table_name);
    m_aclTableDefinitions[acl_table.acl_table_name] = acl_table;
    // Add ACL table name to AclRuleManager mapping in p4orch
    if (!gP4Orch->addAclTableToManagerMapping(acl_table.acl_table_name))
    {
        SWSS_LOG_NOTICE("ACL table %s to AclRuleManager mapping already exists",
                        QuotedVar(acl_table.acl_table_name).c_str());
    }
    for (const auto &udf_group_idx : acl_table.udf_group_attr_index_lookup)
    {
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_UDF_GROUP, fvField(udf_group_idx));
    }

    SWSS_LOG_NOTICE("ACL table %s was created successfully : %s", QuotedVar(acl_table.acl_table_name).c_str(),
                    sai_serialize_object_id(acl_table.table_oid).c_str());
    return ReturnCode();
}

ReturnCode AclTableManager::removeAclTable(P4AclTableDefinition &acl_table)
{
    SWSS_LOG_ENTER();

    auto status = removeAclGroupMember(acl_table);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to remove ACL table with key %s : failed to delete group "
                       "member %s.",
                       QuotedVar(acl_table.acl_table_name).c_str(),
                       sai_serialize_object_id(acl_table.group_member_oid).c_str());
        return status;
    }
    auto sai_status = sai_acl_api->remove_acl_table(acl_table.table_oid);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        status = ReturnCode(sai_status) << "Failed to remove ACL table with key " << QuotedVar(acl_table.acl_table_name)
                                        << " by calling sai_acl_api->remove_acl_table";
        SWSS_LOG_ERROR("%s", status.message().c_str());
        auto rc = createAclGroupMember(acl_table, &acl_table.group_member_oid);
        if (!rc.ok())
        {
            SWSS_LOG_ERROR("%s", rc.message().c_str());
            SWSS_RAISE_CRITICAL_STATE("Failed to create ACL group member in recovery.");
        }
        return status;
    }
    for (const auto &udf_group_idx : acl_table.udf_group_attr_index_lookup)
    {
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_UDF_GROUP, fvField(udf_group_idx));
    }

    // Remove UDFs and UDF groups after ACL table deletion
    std::vector<P4UdfField> removed_udf_fields;
    std::vector<P4UdfField> removed_udf_group_ids;
    for (const auto &udf_fields : acl_table.udf_fields_lookup)
    {
        for (const auto &udf_field : fvValue(udf_fields))
        {
            status = removeUdf(udf_field.udf_id, udf_field.group_id);
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Failed to remove ACL UDF with id %s : %s", QuotedVar(udf_field.udf_id).c_str(),
                               status.message().c_str());
                break;
            }
            removed_udf_fields.push_back(udf_field);
            status = removeUdfGroup(udf_field.group_id);
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Failed to remove ACL UDF group with group id %s : %s",
                               QuotedVar(udf_field.group_id).c_str(), status.message().c_str());
                break;
            }
            removed_udf_group_ids.push_back(udf_field);
        }
        if (!status.ok())
        {
            break;
        }
    }
    if (!status.ok())
    {
        for (const auto &udf_field : removed_udf_group_ids)
        {
            auto rc = createUdfGroup(udf_field);
            if (!rc.ok())
            {
                SWSS_LOG_ERROR("Failed to create UDF group %s: %s", QuotedVar(udf_field.group_id).c_str(),
                               rc.message().c_str());
                SWSS_RAISE_CRITICAL_STATE("Failed to create UDF group in recovery.");
            }
        }
        for (const auto &udf_field : removed_udf_fields)
        {
            auto rc = createUdf(udf_field);
            if (!rc.ok())
            {
                SWSS_LOG_ERROR("Failed to create UDF %s: %s", QuotedVar(udf_field.udf_id).c_str(),
                               rc.message().c_str());
                SWSS_RAISE_CRITICAL_STATE("Failed to create UDF in recovery.");
            }
        }
    }

    gCrmOrch->decCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE, (sai_acl_stage_t)acl_table.stage,
                                   SAI_ACL_BIND_POINT_TYPE_SWITCH, acl_table.table_oid);
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ACL_TABLE, acl_table.acl_table_name);
    // Remove ACL table name to AclRuleManager mapping in p4orch
    if (!gP4Orch->removeAclTableToManagerMapping(acl_table.acl_table_name))
    {
        SWSS_LOG_NOTICE("ACL table %s to AclRuleManager mapping does not exist",
                        QuotedVar(acl_table.acl_table_name).c_str());
    }
    auto &table_keys = m_aclTablesByStage[acl_table.stage];
    auto position = std::find(table_keys.begin(), table_keys.end(), acl_table.acl_table_name);
    if (position != table_keys.end())
    {
        table_keys.erase(position);
    }
    P4AclTableDefinition rollback_acl_table = acl_table;
    m_aclTableDefinitions.erase(acl_table.acl_table_name);

    if (!status.ok())
    {
        auto rc =
            createAclTable(rollback_acl_table, &rollback_acl_table.table_oid, &rollback_acl_table.group_member_oid);
        if (!rc.ok())
        {
            SWSS_LOG_ERROR("Failed to create ACL table: %s", rc.message().c_str());
            SWSS_RAISE_CRITICAL_STATE("Failed to create ACL table in recovery.");
        }
        return status;
    }
    SWSS_LOG_NOTICE("ACL table %s(%s) was removed successfully.", QuotedVar(rollback_acl_table.acl_table_name).c_str(),
                    sai_serialize_object_id(rollback_acl_table.table_oid).c_str());
    return ReturnCode();
}

ReturnCode AclTableManager::processDeleteTableRequest(const std::string &acl_table_name)
{
    SWSS_LOG_ENTER();

    auto *acl_table = getAclTable(acl_table_name);
    if (acl_table == nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "ACL table with key " << QuotedVar(acl_table_name) << " does not exist");
    }
    // Check if there is anything referring to the ACL table before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_ACL_TABLE, acl_table->acl_table_name, &ref_count))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to get reference count of ACL table "
                                                 << QuotedVar(acl_table->acl_table_name));
    }
    if (ref_count > 0)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "ACL table " << QuotedVar(acl_table->acl_table_name)
                             << " referenced by other objects (ref_count = " << ref_count << ")");
    }
    return removeAclTable(*acl_table);
}

ReturnCode AclTableManager::createAclGroupMember(const P4AclTableDefinition &acl_table,
                                                 sai_object_id_t *acl_grp_mem_oid)
{
    SWSS_LOG_ENTER();

    auto attrs = getGroupMemSaiAttrs(acl_table);
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_acl_api->create_acl_table_group_member(acl_grp_mem_oid, gSwitchId, (uint32_t)attrs.size(), attrs.data()),
        "Failed to create ACL group member in group " << sai_serialize_object_id(acl_table.group_oid));
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, acl_table.acl_table_name, *acl_grp_mem_oid);
    // Add reference on the ACL group
    auto &group_map = gSwitchOrch->getAclGroupsBindingToSwitch();
    auto group_it = group_map.find(acl_table.stage);
    if (group_it == group_map.end())
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to find ACL group binding to switch at stage "
                                                 << acl_table.stage);
    }
    auto *referenced_group = &group_it->second;
    referenced_group->m_objsDependingOnMe.insert(sai_serialize_object_id(*acl_grp_mem_oid));
    SWSS_LOG_NOTICE("ACL group member for table %s was created successfully: %s",
                    QuotedVar(acl_table.acl_table_name).c_str(), sai_serialize_object_id(*acl_grp_mem_oid).c_str());
    return ReturnCode();
}

ReturnCode AclTableManager::removeAclGroupMember(P4AclTableDefinition &acl_table)
{
    SWSS_LOG_ENTER();

    sai_object_id_t grp_mem_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, acl_table.acl_table_name, &grp_mem_oid))
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "Failed to remove ACL group member " << sai_serialize_object_id(grp_mem_oid)
                             << " for table " << QuotedVar(acl_table.acl_table_name) << ": invalid table key.");
    }
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_acl_api->remove_acl_table_group_member(grp_mem_oid),
                                   "Failed to remove ACL group member " << sai_serialize_object_id(grp_mem_oid)
                                                                        << " for table "
                                                                        << QuotedVar(acl_table.acl_table_name));
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, acl_table.acl_table_name);
    // Remove reference on the ACL group
    auto &group_map = gSwitchOrch->getAclGroupsBindingToSwitch();
    auto group_it = group_map.find(acl_table.stage);
    if (group_it == group_map.end())
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to find ACL group binding to switch at stage "
                                                 << acl_table.stage);
    }
    auto *referenced_group = &group_it->second;
    referenced_group->m_objsDependingOnMe.erase(sai_serialize_object_id(grp_mem_oid));
    SWSS_LOG_NOTICE("ACL table member %s for table %s was removed successfully.",
                    sai_serialize_object_id(grp_mem_oid).c_str(), QuotedVar(acl_table.acl_table_name).c_str());
    return ReturnCode();
}

std::string AclTableManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
{
    SWSS_LOG_ENTER();

    auto pos = key.find_first_of(kTableKeyDelimiter);
    if (pos == std::string::npos)
    {
        return std::string("Invalid key: ") + key;
    }
    std::string p4rt_table = key.substr(0, pos);
    std::string p4rt_key = key.substr(pos + 1);
    if (p4rt_table != APP_P4RT_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }
    std::string table_name;
    std::string key_content;
    parseP4RTKey(p4rt_key, &table_name, &key_content);
    if (table_name != APP_P4RT_ACL_TABLE_DEFINITION_NAME)
    {
        return std::string("Invalid key: ") + key;
    }

    ReturnCode status;
    auto app_db_entry_or = deserializeAclTableDefinitionAppDbEntry(key_content, tuple);
    if (!app_db_entry_or.ok())
    {
        status = app_db_entry_or.status();
        std::stringstream msg;
        msg << "Unable to deserialize key " << QuotedVar(key) << ": " << status.message();
        return msg.str();
    }
    auto &app_db_entry = *app_db_entry_or;

    auto *acl_table_definition = getAclTable(app_db_entry.acl_table_name);
    if (acl_table_definition == nullptr)
    {
        std::stringstream msg;
        msg << "No entry found with key " << QuotedVar(key);
        return msg.str();
    }

    std::string cache_result = verifyStateCache(app_db_entry, acl_table_definition);
    std::string asic_db_result = verifyStateAsicDb(acl_table_definition);
    if (cache_result.empty())
    {
        return asic_db_result;
    }
    if (asic_db_result.empty())
    {
        return cache_result;
    }
    return cache_result + "; " + asic_db_result;
}

std::string AclTableManager::verifyStateCache(const P4AclTableDefinitionAppDbEntry &app_db_entry,
                                              const P4AclTableDefinition *acl_table)
{
    ReturnCode status = validateAclTableDefinitionAppDbEntry(app_db_entry);
    if (!status.ok())
    {
        std::stringstream msg;
        msg << "Validation failed for ACL table DB entry " << QuotedVar(app_db_entry.acl_table_name) << ": "
            << status.message();
        return msg.str();
    }

    auto stage_it = aclStageLookup.find(app_db_entry.stage);
    sai_acl_stage_t stage;
    if (stage_it != aclStageLookup.end())
    {
        stage = stage_it->second;
    }
    else
    {
        std::stringstream msg;
        msg << "Invalid stage " << QuotedVar(app_db_entry.stage) << " in ACL table manager.";
        return msg.str();
    }
    P4AclTableDefinition acl_table_definition_entry(app_db_entry.acl_table_name, stage, app_db_entry.priority,
                                                    app_db_entry.size, app_db_entry.meter_unit,
                                                    app_db_entry.counter_unit);

    if (acl_table->acl_table_name != app_db_entry.acl_table_name)
    {
        std::stringstream msg;
        msg << "ACL table " << QuotedVar(app_db_entry.acl_table_name) << " does not match internal cache "
            << QuotedVar(acl_table->acl_table_name) << " in ACL table manager.";
        return msg.str();
    }
    if (acl_table->stage != stage)
    {
        std::stringstream msg;
        msg << "ACL table " << QuotedVar(app_db_entry.acl_table_name) << " with stage " << stage
            << " does not match internal cache " << acl_table->stage << " in ACL table manager.";
        return msg.str();
    }
    if (acl_table->size != app_db_entry.size)
    {
        std::stringstream msg;
        msg << "ACL table " << QuotedVar(app_db_entry.acl_table_name) << " with size " << app_db_entry.size
            << " does not match internal cache " << acl_table->size << " in ACL table manager.";
        return msg.str();
    }
    if (acl_table->priority != app_db_entry.priority)
    {
        std::stringstream msg;
        msg << "ACL table " << QuotedVar(app_db_entry.acl_table_name) << " with priority " << app_db_entry.priority
            << " does not match internal cache " << acl_table->priority << " in ACL table manager.";
        return msg.str();
    }
    if (acl_table->meter_unit != app_db_entry.meter_unit)
    {
        std::stringstream msg;
        msg << "ACL table " << QuotedVar(app_db_entry.acl_table_name) << " with meter unit "
            << QuotedVar(app_db_entry.meter_unit) << " does not match internal cache "
            << QuotedVar(acl_table->meter_unit) << " in ACL table manager.";
        return msg.str();
    }
    if (acl_table->counter_unit != app_db_entry.counter_unit)
    {
        std::stringstream msg;
        msg << "ACL table " << QuotedVar(app_db_entry.acl_table_name) << " with counter unit "
            << QuotedVar(app_db_entry.counter_unit) << " does not match internal cache "
            << QuotedVar(acl_table->counter_unit) << " in ACL table manager.";
        return msg.str();
    }

    status = buildAclTableDefinitionMatchFieldValues(app_db_entry.match_field_lookup, &acl_table_definition_entry);
    if (!status.ok())
    {
        std::stringstream msg;
        msg << "Failed to build ACL table match field values for table " << QuotedVar(app_db_entry.acl_table_name);
        return msg.str();
    }
    status = buildAclTableDefinitionActionFieldValues(app_db_entry.action_field_lookup,
                                                      &acl_table_definition_entry.rule_action_field_lookup);
    if (!status.ok())
    {
        std::stringstream msg;
        msg << "Failed to build ACL table action field values for table " << QuotedVar(app_db_entry.acl_table_name);
        return msg.str();
    }
    status = buildAclTableDefinitionActionColorFieldValues(app_db_entry.packet_action_color_lookup,
                                                           &acl_table_definition_entry.rule_action_field_lookup,
                                                           &acl_table_definition_entry.rule_packet_action_color_lookup);
    if (!status.ok())
    {
        std::stringstream msg;
        msg << "Failed to build ACL table action color field values for table "
            << QuotedVar(app_db_entry.acl_table_name);
        return msg.str();
    }

    if (acl_table->composite_sai_match_fields_lookup != acl_table_definition_entry.composite_sai_match_fields_lookup)
    {
        std::stringstream msg;
        msg << "Composite SAI match fields mismatch on ACL table " << QuotedVar(app_db_entry.acl_table_name);
        return msg.str();
    }
    if (acl_table->udf_fields_lookup != acl_table_definition_entry.udf_fields_lookup)
    {
        std::stringstream msg;
        msg << "UDF fields lookup mismatch on ACL table " << QuotedVar(app_db_entry.acl_table_name);
        return msg.str();
    }
    if (acl_table->udf_group_attr_index_lookup != acl_table_definition_entry.udf_group_attr_index_lookup)
    {
        std::stringstream msg;
        msg << "UDF group attr index lookup mismatch on ACL table " << QuotedVar(app_db_entry.acl_table_name);
        return msg.str();
    }
    if (acl_table->sai_match_field_lookup != acl_table_definition_entry.sai_match_field_lookup)
    {
        std::stringstream msg;
        msg << "SAI match field lookup mismatch on ACL table " << QuotedVar(app_db_entry.acl_table_name);
        return msg.str();
    }
    if (acl_table->ip_type_bit_type_lookup != acl_table_definition_entry.ip_type_bit_type_lookup)
    {
        std::stringstream msg;
        msg << "IP type bit type lookup mismatch on ACL table " << QuotedVar(app_db_entry.acl_table_name);
        return msg.str();
    }
    if (acl_table->rule_action_field_lookup != acl_table_definition_entry.rule_action_field_lookup)
    {
        std::stringstream msg;
        msg << "Rule action field lookup mismatch on ACL table " << QuotedVar(app_db_entry.acl_table_name);
        return msg.str();
    }
    if (acl_table->rule_packet_action_color_lookup != acl_table_definition_entry.rule_packet_action_color_lookup)
    {
        std::stringstream msg;
        msg << "Rule packet action color lookup mismatch on ACL table " << QuotedVar(app_db_entry.acl_table_name);
        return msg.str();
    }

    std::string err_msg = m_p4OidMapper->verifyOIDMapping(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER,
                                                          app_db_entry.acl_table_name, acl_table->group_member_oid);
    if (!err_msg.empty())
    {
        return err_msg;
    }
    err_msg =
        m_p4OidMapper->verifyOIDMapping(SAI_OBJECT_TYPE_ACL_TABLE, app_db_entry.acl_table_name, acl_table->table_oid);
    if (!err_msg.empty())
    {
        return err_msg;
    }

    return "";
}

std::string AclTableManager::verifyStateAsicDb(const P4AclTableDefinition *acl_table)
{
    swss::DBConnector db("ASIC_DB", 0);
    swss::Table table(&db, "ASIC_STATE");

    // Verify table.
    auto attrs_or = getTableSaiAttrs(*acl_table);
    if (!attrs_or.ok())
    {
        return std::string("Failed to get SAI attrs: ") + attrs_or.status().message();
    }
    std::vector<sai_attribute_t> attrs = *attrs_or;
    std::vector<swss::FieldValueTuple> exp =
        saimeta::SaiAttributeList::serialize_attr_list(SAI_OBJECT_TYPE_ACL_TABLE, (uint32_t)attrs.size(), attrs.data(),
                                                       /*countOnly=*/false);
    std::string key =
        sai_serialize_object_type(SAI_OBJECT_TYPE_ACL_TABLE) + ":" + sai_serialize_object_id(acl_table->table_oid);
    std::vector<swss::FieldValueTuple> values;
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }
    std::string err_msg = verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                                      /*allow_unknown=*/false);
    if (!err_msg.empty())
    {
        return err_msg;
    }

    // Verify group member.
    attrs = getGroupMemSaiAttrs(*acl_table);
    exp = saimeta::SaiAttributeList::serialize_attr_list(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, (uint32_t)attrs.size(),
                                                         attrs.data(), /*countOnly=*/false);
    key = sai_serialize_object_type(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER) + ":" +
          sai_serialize_object_id(acl_table->group_member_oid);
    values.clear();
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }
    err_msg = verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                          /*allow_unknown=*/false);
    if (!err_msg.empty())
    {
        return err_msg;
    }

    for (auto &udf_fields : acl_table->udf_fields_lookup)
    {
        for (auto &udf_field : fvValue(udf_fields))
        {
            sai_object_id_t udf_group_oid;
            if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_UDF_GROUP, udf_field.group_id, &udf_group_oid))
            {
                return std::string("UDF group ") + udf_field.group_id + " does not exist";
            }
            sai_object_id_t udf_oid;
            if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_UDF, udf_field.udf_id, &udf_oid))
            {
                return std::string("UDF ") + udf_field.udf_id + " does not exist";
            }

            // Verify UDF group.
            attrs = getUdfGroupSaiAttrs(udf_field);
            exp = saimeta::SaiAttributeList::serialize_attr_list(SAI_OBJECT_TYPE_UDF_GROUP, (uint32_t)attrs.size(),
                                                                 attrs.data(),
                                                                 /*countOnly=*/false);
            key = sai_serialize_object_type(SAI_OBJECT_TYPE_UDF_GROUP) + ":" + sai_serialize_object_id(udf_group_oid);
            values.clear();
            if (!table.get(key, values))
            {
                return std::string("ASIC DB key not found ") + key;
            }
            err_msg = verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                                  /*allow_unknown=*/false);
            if (!err_msg.empty())
            {
                return err_msg;
            }

            // Verify UDF.
            attrs_or = getUdfSaiAttrs(udf_field);
            if (!attrs_or.ok())
            {
                return std::string("Failed to get SAI attrs: ") + attrs_or.status().message();
            }
            attrs = *attrs_or;
            exp = saimeta::SaiAttributeList::serialize_attr_list(SAI_OBJECT_TYPE_UDF, (uint32_t)attrs.size(),
                                                                 attrs.data(),
                                                                 /*countOnly=*/false);
            key = sai_serialize_object_type(SAI_OBJECT_TYPE_UDF) + ":" + sai_serialize_object_id(udf_oid);
            values.clear();
            if (!table.get(key, values))
            {
                return std::string("ASIC DB key not found ") + key;
            }
            err_msg = verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                                  /*allow_unknown=*/false);
            if (!err_msg.empty())
            {
                return err_msg;
            }
        }
    }

    return "";
}

} // namespace p4orch
