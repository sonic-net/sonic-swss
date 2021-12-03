#include "p4orch/acl_rule_manager.h"

#include <sstream>
#include <string>
#include <vector>

#include "converter.h"
#include "crmorch.h"
#include "json.hpp"
#include "logger.h"
#include "orch.h"
#include "p4orch.h"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "sai_serialize.h"
#include "tokenize.h"
extern "C"
{
#include "sai.h"
}

extern sai_object_id_t gSwitchId;
extern sai_acl_api_t *sai_acl_api;
extern sai_policer_api_t *sai_policer_api;
extern sai_hostif_api_t *sai_hostif_api;
extern CrmOrch *gCrmOrch;
extern PortsOrch *gPortsOrch;
extern P4Orch *gP4Orch;

namespace p4orch
{
namespace
{

const std::string concatTableNameAndRuleKey(const std::string &table_name, const std::string &rule_key)
{
    return table_name + kTableKeyDelimiter + rule_key;
}

} // namespace

void AclRuleManager::enqueue(const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void AclRuleManager::drain()
{
    SWSS_LOG_ENTER();

    for (const auto &key_op_fvs_tuple : m_entries)
    {
        std::string table_name;
        std::string db_key;
        parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &db_key);
        const auto &op = kfvOp(key_op_fvs_tuple);
        const std::vector<swss::FieldValueTuple> &attributes = kfvFieldsValues(key_op_fvs_tuple);

        SWSS_LOG_NOTICE("OP: %s, RULE_KEY: %s", op.c_str(), QuotedVar(db_key).c_str());

        ReturnCode status;
        auto app_db_entry_or = deserializeAclRuleAppDbEntry(table_name, db_key, attributes);
        if (!app_db_entry_or.ok())
        {
            status = app_db_entry_or.status();
            SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                           QuotedVar(table_name + ":" + db_key).c_str(), status.message().c_str());
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status,
                                 /*replace=*/true);
            continue;
        }
        auto &app_db_entry = *app_db_entry_or;

        status = validateAclRuleAppDbEntry(app_db_entry);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Validation failed for ACL rule APP DB entry with key %s: %s",
                           QuotedVar(table_name + ":" + db_key).c_str(), status.message().c_str());
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status,
                                 /*replace=*/true);
            continue;
        }

        const auto &acl_table_name = app_db_entry.acl_table_name;
        const auto &acl_rule_key =
            KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, std::to_string(app_db_entry.priority));

        const auto &operation = kfvOp(key_op_fvs_tuple);
        if (operation == SET_COMMAND)
        {
            auto *acl_rule = getAclRule(acl_table_name, acl_rule_key);
            if (acl_rule == nullptr)
            {
                status = processAddRuleRequest(acl_rule_key, app_db_entry);
            }
            else
            {
                status = processUpdateRuleRequest(app_db_entry, *acl_rule);
            }
        }
        else if (operation == DEL_COMMAND)
        {
            status = processDeleteRuleRequest(acl_table_name, acl_rule_key);
        }
        else
        {
            status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Unknown operation type " << operation;
            SWSS_LOG_ERROR("%s", status.message().c_str());
        }
        m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple), status,
                             /*replace=*/true);
    }
    m_entries.clear();
}

ReturnCode AclRuleManager::setUpUserDefinedTraps()
{
    SWSS_LOG_ENTER();

    const auto trapGroupMap = m_coppOrch->getTrapGroupMap();
    const auto trapGroupHostIfMap = m_coppOrch->getTrapGroupHostIfMap();
    for (int queue_num = 1; queue_num <= P4_CPU_QUEUE_MAX_NUM; queue_num++)
    {
        auto trap_group_it = trapGroupMap.find(GENL_PACKET_TRAP_GROUP_NAME_PREFIX + std::to_string(queue_num));
        if (trap_group_it == trapGroupMap.end())
        {
            LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                                 << "Trap group was not found given trap group name: "
                                 << GENL_PACKET_TRAP_GROUP_NAME_PREFIX << queue_num);
        }
        const sai_object_id_t trap_group_oid = trap_group_it->second;
        auto hostif_oid_it = trapGroupHostIfMap.find(trap_group_oid);
        if (hostif_oid_it == trapGroupHostIfMap.end())
        {
            LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                                 << "Hostif object id was not found given trap group - " << trap_group_it->first);
        }
        // Create user defined trap
        std::vector<sai_attribute_t> trap_attrs;
        sai_attribute_t attr;
        attr.id = SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TRAP_GROUP;
        attr.value.oid = trap_group_oid;
        trap_attrs.push_back(attr);
        attr.id = SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TYPE;
        attr.value.s32 = SAI_HOSTIF_USER_DEFINED_TRAP_TYPE_ACL;
        trap_attrs.push_back(attr);
        P4UserDefinedTrapHostifTableEntry udt_hostif;
        CHECK_ERROR_AND_LOG_AND_RETURN(
            sai_hostif_api->create_hostif_user_defined_trap(&udt_hostif.user_defined_trap, gSwitchId,
                                                            (uint32_t)trap_attrs.size(), trap_attrs.data()),
            "Failed to create trap by calling "
            "sai_hostif_api->create_hostif_user_defined_trap");
        std::vector<sai_attribute_t> sai_host_table_attr;

        attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_TYPE;
        attr.value.s32 = SAI_HOSTIF_TABLE_ENTRY_TYPE_TRAP_ID;
        sai_host_table_attr.push_back(attr);

        attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_TRAP_ID;
        attr.value.oid = udt_hostif.user_defined_trap;
        sai_host_table_attr.push_back(attr);

        attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_CHANNEL_TYPE;
        attr.value.s32 = SAI_HOSTIF_TABLE_ENTRY_CHANNEL_TYPE_GENETLINK;
        sai_host_table_attr.push_back(attr);

        attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_HOST_IF;
        attr.value.oid = hostif_oid_it->second;
        sai_host_table_attr.push_back(attr);

        auto sai_status =
            sai_hostif_api->create_hostif_table_entry(&udt_hostif.hostif_table_entry, gSwitchId,
                                                      (uint32_t)sai_host_table_attr.size(), sai_host_table_attr.data());
        if (sai_status != SAI_STATUS_SUCCESS)
        {
            ReturnCode return_code = ReturnCode(sai_status) << "Failed to create hostif table entry by calling "
                                                               "sai_hostif_api->remove_hostif_user_defined_trap";
            sai_hostif_api->remove_hostif_user_defined_trap(udt_hostif.user_defined_trap);
            SWSS_LOG_ERROR("%s SAI_STATUS: %s", return_code.message().c_str(),
                           sai_serialize_status(sai_status).c_str());
            return return_code;
        }
        m_p4OidMapper->setOID(SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP, std::to_string(queue_num),
                              udt_hostif.user_defined_trap, /*ref_count=*/1);
        m_userDefinedTraps.push_back(udt_hostif);
        SWSS_LOG_NOTICE("Created user defined trap for QUEUE number %d: %s", queue_num,
                        sai_serialize_object_id(udt_hostif.user_defined_trap).c_str());
    }
    return ReturnCode();
}

ReturnCode AclRuleManager::cleanUpUserDefinedTraps()
{
    SWSS_LOG_ENTER();

    for (size_t queue_num = 1; queue_num <= m_userDefinedTraps.size(); queue_num++)
    {
        CHECK_ERROR_AND_LOG_AND_RETURN(
            sai_hostif_api->remove_hostif_table_entry(m_userDefinedTraps[queue_num - 1].hostif_table_entry),
            "Failed to create hostif table entry.");
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP, std::to_string(queue_num));
        sai_hostif_api->remove_hostif_user_defined_trap(m_userDefinedTraps[queue_num - 1].user_defined_trap);
        m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP, std::to_string(queue_num));
    }
    m_userDefinedTraps.clear();
    return ReturnCode();
}

void AclRuleManager::doAclCounterStatsTask()
{
    SWSS_LOG_ENTER();

    for (const auto &table_it : m_aclRuleTables)
    {
        const auto &table_name = fvField(table_it);
        for (const auto &rule_it : fvValue(table_it))
        {
            if (!fvValue(rule_it).counter.packets_enabled && !fvValue(rule_it).counter.bytes_enabled)
                continue;
            auto status = setAclRuleCounterStats(fvValue(rule_it));
            if (!status.ok())
            {
                status.prepend("Failed to set counters stats for ACL rule " + QuotedVar(table_name) + ":" +
                               QuotedVar(fvField(rule_it)) + " in COUNTERS_DB: ");
                SWSS_LOG_ERROR("%s", status.message().c_str());
                continue;
            }
        }
    }
}

ReturnCode AclRuleManager::createAclCounter(const std::string &acl_table_name, const std::string &counter_key,
                                            const P4AclCounter &p4_acl_counter, sai_object_id_t *counter_oid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    std::vector<sai_attribute_t> counter_attrs;
    sai_object_id_t acl_table_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ACL_TABLE, acl_table_name, &acl_table_oid))
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Invalid ACL counter to create: ACL table key " << QuotedVar(acl_table_name)
                             << " not found.");
    }
    attr.id = SAI_ACL_COUNTER_ATTR_TABLE_ID;
    attr.value.oid = acl_table_oid;
    counter_attrs.push_back(attr);

    if (p4_acl_counter.bytes_enabled)
    {
        attr.id = SAI_ACL_COUNTER_ATTR_ENABLE_BYTE_COUNT;
        attr.value.booldata = true;
        counter_attrs.push_back(attr);
    }

    if (p4_acl_counter.packets_enabled)
    {
        attr.id = SAI_ACL_COUNTER_ATTR_ENABLE_PACKET_COUNT;
        attr.value.booldata = true;
        counter_attrs.push_back(attr);
    }

    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_acl_api->create_acl_counter(counter_oid, gSwitchId, (uint32_t)counter_attrs.size(), counter_attrs.data()),
        "Faied to create counter for the rule in table " << sai_serialize_object_id(acl_table_oid));
    SWSS_LOG_NOTICE("Suceeded to create ACL counter %s ", sai_serialize_object_id(*counter_oid).c_str());
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_ACL_COUNTER, counter_key, *counter_oid);
    gCrmOrch->incCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_COUNTER, acl_table_oid);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ACL_TABLE, acl_table_name);
    return ReturnCode();
}

ReturnCode AclRuleManager::removeAclCounter(const std::string &acl_table_name, const std::string &counter_key)
{
    SWSS_LOG_ENTER();
    sai_object_id_t counter_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ACL_COUNTER, counter_key, &counter_oid))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to remove ACL counter by key " << QuotedVar(counter_key)
                                                                                        << ": invalid counter key.");
    }
    sai_object_id_t table_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ACL_TABLE, acl_table_name, &table_oid))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to remove ACL counter "
                                                 << sai_serialize_object_id(counter_oid) << " in table "
                                                 << QuotedVar(acl_table_name) << ": invalid table key.");
    }
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_acl_api->remove_acl_counter(counter_oid),
                                   "Failed to remove ACL counter " << sai_serialize_object_id(counter_oid)
                                                                   << " in table " << QuotedVar(acl_table_name));

    gCrmOrch->decCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_COUNTER, table_oid);
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ACL_COUNTER, counter_key);
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ACL_TABLE, acl_table_name);
    SWSS_LOG_NOTICE("Removing record about the counter %s from the DB", sai_serialize_object_id(counter_oid).c_str());
    return ReturnCode();
}

ReturnCode AclRuleManager::createAclMeter(const P4AclMeter &p4_acl_meter, const std::string &meter_key,
                                          sai_object_id_t *meter_oid)
{
    SWSS_LOG_ENTER();

    std::vector<sai_attribute_t> meter_attrs;
    sai_attribute_t meter_attr;
    meter_attr.id = SAI_POLICER_ATTR_METER_TYPE;
    meter_attr.value.s32 = p4_acl_meter.type;
    meter_attrs.push_back(meter_attr);

    meter_attr.id = SAI_POLICER_ATTR_MODE;
    meter_attr.value.s32 = p4_acl_meter.mode;
    meter_attrs.push_back(meter_attr);

    if (p4_acl_meter.enabled)
    {
        meter_attr.id = SAI_POLICER_ATTR_CBS;
        meter_attr.value.u64 = p4_acl_meter.cburst;
        meter_attrs.push_back(meter_attr);

        meter_attr.id = SAI_POLICER_ATTR_CIR;
        meter_attr.value.u64 = p4_acl_meter.cir;
        meter_attrs.push_back(meter_attr);

        meter_attr.id = SAI_POLICER_ATTR_PIR;
        meter_attr.value.u64 = p4_acl_meter.pir;
        meter_attrs.push_back(meter_attr);

        meter_attr.id = SAI_POLICER_ATTR_PBS;
        meter_attr.value.u64 = p4_acl_meter.pburst;
        meter_attrs.push_back(meter_attr);
    }

    for (const auto &packet_color_action : p4_acl_meter.packet_color_actions)
    {
        meter_attr.id = fvField(packet_color_action);
        meter_attr.value.s32 = fvValue(packet_color_action);
        meter_attrs.push_back(meter_attr);
    }

    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_policer_api->create_policer(meter_oid, gSwitchId, (uint32_t)meter_attrs.size(), meter_attrs.data()),
        "Failed to create ACL meter");
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_POLICER, meter_key, *meter_oid);
    SWSS_LOG_NOTICE("Suceeded to create ACL meter %s ", sai_serialize_object_id(*meter_oid).c_str());
    return ReturnCode();
}

ReturnCode AclRuleManager::updateAclMeter(const P4AclMeter &new_acl_meter, const P4AclMeter &old_acl_meter)
{
    SWSS_LOG_ENTER();

    std::vector<sai_attribute_t> meter_attrs;
    std::vector<sai_attribute_t> rollback_attrs;
    sai_attribute_t meter_attr;

    if (old_acl_meter.cburst != new_acl_meter.cburst)
    {
        meter_attr.id = SAI_POLICER_ATTR_CBS;
        meter_attr.value.u64 = new_acl_meter.cburst;
        meter_attrs.push_back(meter_attr);
        meter_attr.value.u64 = old_acl_meter.cburst;
        rollback_attrs.push_back(meter_attr);
    }
    if (old_acl_meter.cir != new_acl_meter.cir)
    {
        meter_attr.id = SAI_POLICER_ATTR_CIR;
        meter_attr.value.u64 = new_acl_meter.cir;
        meter_attrs.push_back(meter_attr);
        meter_attr.value.u64 = old_acl_meter.cir;
        rollback_attrs.push_back(meter_attr);
    }
    if (old_acl_meter.pir != new_acl_meter.pir)
    {
        meter_attr.id = SAI_POLICER_ATTR_PIR;
        meter_attr.value.u64 = new_acl_meter.pir;
        meter_attrs.push_back(meter_attr);
        meter_attr.value.u64 = old_acl_meter.pir;
        rollback_attrs.push_back(meter_attr);
    }
    if (old_acl_meter.pburst != new_acl_meter.pburst)
    {
        meter_attr.id = SAI_POLICER_ATTR_PBS;
        meter_attr.value.u64 = new_acl_meter.pburst;
        meter_attrs.push_back(meter_attr);
        meter_attr.value.u64 = old_acl_meter.pburst;
        rollback_attrs.push_back(meter_attr);
    }

    std::set<sai_policer_attr_t> colors_to_reset;
    for (const auto &old_color_action : old_acl_meter.packet_color_actions)
    {
        colors_to_reset.insert(fvField(old_color_action));
    }

    for (const auto &packet_color_action : new_acl_meter.packet_color_actions)
    {
        const auto &it = old_acl_meter.packet_color_actions.find(fvField(packet_color_action));
        if (it == old_acl_meter.packet_color_actions.end() || it->second != fvValue(packet_color_action))
        {
            meter_attr.id = fvField(packet_color_action);
            meter_attr.value.s32 = fvValue(packet_color_action);
            meter_attrs.push_back(meter_attr);
            meter_attr.value.s32 =
                (it == old_acl_meter.packet_color_actions.end()) ? SAI_PACKET_ACTION_FORWARD : it->second;
            rollback_attrs.push_back(meter_attr);
        }
        if (it != old_acl_meter.packet_color_actions.end())
        {
            colors_to_reset.erase(fvField(packet_color_action));
        }
    }

    for (const auto &packet_color : colors_to_reset)
    {
        meter_attr.id = packet_color;
        meter_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
        meter_attrs.push_back(meter_attr);
        const auto &it = old_acl_meter.packet_color_actions.find(packet_color);
        meter_attr.value.s32 = it->second;
        rollback_attrs.push_back(meter_attr);
    }

    ReturnCode status;
    int i;
    for (i = 0; i < static_cast<int>(meter_attrs.size()); ++i)
    {
        status = ReturnCode(sai_policer_api->set_policer_attribute(old_acl_meter.meter_oid, &meter_attrs[i]));
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to update ACL meter attributes: %s", status.message().c_str());
            break;
        }
    }
    if (!status.ok())
    {
        for (--i; i >= 0; --i)
        {
            auto sai_status = sai_policer_api->set_policer_attribute(old_acl_meter.meter_oid, &rollback_attrs[i]);
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set ACL policer attribute. SAI_STATUS: %s",
                               sai_serialize_status(sai_status).c_str());
                SWSS_RAISE_CRITICAL_STATE("Failed to set ACL policer attribute in recovery.");
            }
        }
        return status;
    }
    SWSS_LOG_NOTICE("Suceeded to update ACL meter %s ", sai_serialize_object_id(old_acl_meter.meter_oid).c_str());
    return ReturnCode();
}

ReturnCode AclRuleManager::removeAclMeter(const std::string &meter_key)
{
    SWSS_LOG_ENTER();
    sai_object_id_t meter_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_POLICER, meter_key, &meter_oid))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to get ACL meter object id for ACL rule "
                                                 << QuotedVar(meter_key));
    }
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_policer_api->remove_policer(meter_oid),
                                   "Failed to remove ACL meter for ACL rule " << QuotedVar(meter_key));
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_POLICER, meter_key);
    SWSS_LOG_NOTICE("Suceeded to remove ACL meter %s: %s ", QuotedVar(meter_key).c_str(),
                    sai_serialize_object_id(meter_oid).c_str());
    return ReturnCode();
}

ReturnCodeOr<P4AclRuleAppDbEntry> AclRuleManager::deserializeAclRuleAppDbEntry(
    const std::string &acl_table_name, const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    sai_object_id_t table_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ACL_TABLE, acl_table_name, &table_oid))
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "ACL table " << QuotedVar(acl_table_name) << " is not found";
    }
    P4AclRuleAppDbEntry app_db_entry = {};
    app_db_entry.acl_table_name = acl_table_name;
    app_db_entry.db_key = concatTableNameAndRuleKey(acl_table_name, key);
    // Parse rule key : match fields and priority
    try
    {
        const auto &rule_key_json = nlohmann::json::parse(key);
        if (!rule_key_json.is_object())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid ACL rule key: should be a JSON object.";
        }
        for (auto rule_key_it = rule_key_json.begin(); rule_key_it != rule_key_json.end(); ++rule_key_it)
        {
            if (rule_key_it.key() == kPriority)
            {
                if (!rule_key_it.value().is_number_unsigned())
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "Invalid ACL rule priority type: should be uint32_t";
                }
                app_db_entry.priority = rule_key_it.value();
                continue;
            }
            else
            {
                const auto &tokenized_match_field = tokenize(rule_key_it.key(), kFieldDelimiter);
                if (tokenized_match_field.size() <= 1 || tokenized_match_field[0] != kMatchPrefix)
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "Unknown ACL match field string " << QuotedVar(rule_key_it.key());
                }
                app_db_entry.match_fvs[tokenized_match_field[1]] = rule_key_it.value();
            }
        }
    }
    catch (std::exception &e)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to deserialize ACL rule match key";
    }

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        const auto &value = fvValue(it);
        if (field == kControllerMetadata)
            continue;
        if (field == kAction)
        {
            app_db_entry.action = value;
            continue;
        }
        const auto &tokenized_field = tokenize(field, kFieldDelimiter);
        if (tokenized_field.size() <= 1)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Unknown ACL rule field " << QuotedVar(field);
        }
        const auto &prefix = tokenized_field[0];
        if (prefix == kActionParamPrefix)
        {
            const auto &param_name = tokenized_field[1];
            app_db_entry.action_param_fvs[param_name] = value;
        }
        else if (prefix == kMeterPrefix)
        {
            const auto &meter_attr_name = tokenized_field[1];
            if (std::stoi(value) < 0)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid ACL meter field value " << QuotedVar(field) << ": " << QuotedVar(value);
            }
            if (meter_attr_name == kMeterCir)
            {
                app_db_entry.meter.cir = std::stoi(value);
            }
            else if (meter_attr_name == kMeterCburst)
            {
                app_db_entry.meter.cburst = std::stoi(value);
            }
            else if (meter_attr_name == kMeterPir)
            {
                app_db_entry.meter.pir = std::stoi(value);
            }
            else if (meter_attr_name == kMeterPburst)
            {
                app_db_entry.meter.pburst = std::stoi(value);
            }
            else
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Unknown ACL meter field " << QuotedVar(field);
            }
            app_db_entry.meter.enabled = true;
        }
        else
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Unknown ACL rule field " << QuotedVar(field);
        }
    }
    return app_db_entry;
}

ReturnCode AclRuleManager::validateAclRuleAppDbEntry(const P4AclRuleAppDbEntry &app_db_entry)
{
    if (app_db_entry.priority == 0)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL rule in table " << QuotedVar(app_db_entry.acl_table_name) << " is missing priority";
    }
    return ReturnCode();
}

P4AclRule *AclRuleManager::getAclRule(const std::string &acl_table_name, const std::string &acl_rule_key)
{
    if (m_aclRuleTables[acl_table_name].find(acl_rule_key) == m_aclRuleTables[acl_table_name].end())
    {
        return nullptr;
    }
    return &m_aclRuleTables[acl_table_name][acl_rule_key];
}

ReturnCode AclRuleManager::setAclRuleCounterStats(const P4AclRule &acl_rule)
{
    SWSS_LOG_ENTER();

    std::vector<swss::FieldValueTuple> counter_stats_values;
    // Query colored packets/bytes stats by ACL meter object id if packet color is
    // defined
    if (!acl_rule.meter.packet_color_actions.empty())
    {
        std::vector<sai_stat_id_t> counter_stats_ids;
        const auto &packet_colors = acl_rule.meter.packet_color_actions;
        for (const auto &pc : packet_colors)
        {
            if (acl_rule.counter.packets_enabled)
            {
                const auto &pkt_stats_id_it = aclCounterColoredPacketsStatsIdMap.find(fvField(pc));
                if (pkt_stats_id_it == aclCounterColoredPacketsStatsIdMap.end())
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "Invalid meter attribute " << pkt_stats_id_it->first << " for packet color in ACL rule "
                           << QuotedVar(acl_rule.db_key);
                }
                counter_stats_ids.push_back(pkt_stats_id_it->second);
            }
            if (acl_rule.counter.bytes_enabled)
            {
                const auto &byte_stats_id_it = aclCounterColoredBytesStatsIdMap.find(fvField(pc));
                if (byte_stats_id_it == aclCounterColoredBytesStatsIdMap.end())
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "Invalid meter attribute " << byte_stats_id_it->first << " for packet color in ACL rule "
                           << QuotedVar(acl_rule.db_key);
                }
                counter_stats_ids.push_back(byte_stats_id_it->second);
            }
        }
        std::vector<uint64_t> meter_stats(counter_stats_ids.size());
        CHECK_ERROR_AND_LOG_AND_RETURN(sai_policer_api->get_policer_stats(
                                           acl_rule.meter.meter_oid, static_cast<uint32_t>(counter_stats_ids.size()),
                                           counter_stats_ids.data(), meter_stats.data()),
                                       "Failed to get meter stats for ACL rule " << QuotedVar(acl_rule.db_key));
        for (size_t i = 0; i < counter_stats_ids.size(); i++)
        {
            counter_stats_values.push_back(swss::FieldValueTuple{aclCounterStatsIdNameMap.at(counter_stats_ids[i]),
                                                                 std::to_string(meter_stats[i])});
        }
    }
    else
    {
        // Query general packets/bytes stats by ACL counter object id.
        std::vector<sai_attribute_t> counter_attrs;
        sai_attribute_t counter_attr;
        if (acl_rule.counter.packets_enabled)
        {
            counter_attr.id = SAI_ACL_COUNTER_ATTR_PACKETS;
            counter_attrs.push_back(counter_attr);
        }
        if (acl_rule.counter.bytes_enabled)
        {
            counter_attr.id = SAI_ACL_COUNTER_ATTR_BYTES;
            counter_attrs.push_back(counter_attr);
        }
        CHECK_ERROR_AND_LOG_AND_RETURN(
            sai_acl_api->get_acl_counter_attribute(acl_rule.counter.counter_oid,
                                                   static_cast<uint32_t>(counter_attrs.size()), counter_attrs.data()),
            "Failed to get counters stats for " << QuotedVar(acl_rule.acl_table_name));
        for (const auto &counter_attr : counter_attrs)
        {
            if (counter_attr.id == SAI_ACL_COUNTER_ATTR_PACKETS)
            {
                counter_stats_values.push_back(
                    swss::FieldValueTuple{P4_COUNTER_STATS_PACKETS, std::to_string(counter_attr.value.u64)});
            }
            if (counter_attr.id == SAI_ACL_COUNTER_ATTR_BYTES)
            {
                counter_stats_values.push_back(
                    swss::FieldValueTuple{P4_COUNTER_STATS_BYTES, std::to_string(counter_attr.value.u64)});
            }
        }
    }
    // Set field value tuples for counters stats in COUNTERS_DB
    m_countersTable->set(acl_rule.db_key, counter_stats_values);
    return ReturnCode();
}

ReturnCode AclRuleManager::setMatchValue(const acl_entry_attr_union_t attr_name, const std::string &attr_value,
                                         sai_attribute_value_t *value, P4AclRule *acl_rule,
                                         const std::string &ip_type_bit_type)
{
    try
    {
        switch (attr_name)
        {
        case SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS: {
            const auto &ports = tokenize(attr_value, kPortsDelimiter);
            if (ports.empty())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "IN_PORTS are emtpy.";
            }
            for (const auto &alias : ports)
            {
                Port port;
                if (!gPortsOrch->getPort(alias, port))
                {
                    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "Failed to locate port " << QuotedVar(alias);
                }
                acl_rule->in_ports.push_back(alias);
                acl_rule->in_ports_oids.push_back(port.m_port_id);
            }
            value->aclfield.data.objlist.count = static_cast<uint32_t>(acl_rule->in_ports_oids.size());
            value->aclfield.data.objlist.list = acl_rule->in_ports_oids.data();
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORTS: {
            const auto &ports = tokenize(attr_value, kPortsDelimiter);
            if (ports.empty())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "OUT_PORTS are emtpy.";
            }
            for (const auto &alias : ports)
            {
                Port port;
                if (!gPortsOrch->getPort(alias, port))
                {
                    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "Failed to locate port " << QuotedVar(alias);
                }
                acl_rule->out_ports.push_back(alias);
                acl_rule->out_ports_oids.push_back(port.m_port_id);
            }
            value->aclfield.data.objlist.count = static_cast<uint32_t>(acl_rule->out_ports_oids.size());
            value->aclfield.data.objlist.list = acl_rule->out_ports_oids.data();
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_IN_PORT: {
            Port port;
            if (!gPortsOrch->getPort(attr_value, port))
            {
                return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "Failed to locate port " << QuotedVar(attr_value);
            }
            value->aclfield.data.oid = port.m_port_id;
            acl_rule->in_ports.push_back(attr_value);
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORT: {
            Port port;
            if (!gPortsOrch->getPort(attr_value, port))
            {
                return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "Failed to locate port " << QuotedVar(attr_value);
            }
            value->aclfield.data.oid = port.m_port_id;
            acl_rule->out_ports.push_back(attr_value);
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE: {
            if (!setMatchFieldIpType(attr_value, value, ip_type_bit_type))
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Failed to set IP_TYPE with value " << QuotedVar(attr_value);
            }
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS:
        case SAI_ACL_ENTRY_ATTR_FIELD_IP_FLAGS:
        case SAI_ACL_ENTRY_ATTR_FIELD_DSCP: {
            // Support both exact value match and value/mask match
            const auto &flag_data = tokenize(attr_value, kDataMaskDelimiter);
            value->aclfield.data.u8 = to_uint<uint8_t>(trim(flag_data[0]), 0, 0x3F);

            if (flag_data.size() == 2)
            {
                value->aclfield.mask.u8 = to_uint<uint8_t>(trim(flag_data[1]), 0, 0x3F);
            }
            else
            {
                value->aclfield.mask.u8 = 0x3F;
            }
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE:
        case SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT:
        case SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT:
        case SAI_ACL_ENTRY_ATTR_FIELD_IP_IDENTIFICATION:
        case SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_ID:
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_VLAN_ID:
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_ETHER_TYPE:
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_L4_SRC_PORT:
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_L4_DST_PORT: {
            const std::vector<std::string> &value_and_mask = tokenize(attr_value, kDataMaskDelimiter);
            value->aclfield.data.u16 = to_uint<uint16_t>(trim(value_and_mask[0]));
            if (value_and_mask.size() > 1)
            {
                value->aclfield.mask.u16 = to_uint<uint16_t>(trim(value_and_mask[1]));
            }
            else
            {
                value->aclfield.mask.u16 = 0xFFFF;
            }
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP:
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IP:
        case SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP:
        case SAI_ACL_ENTRY_ATTR_FIELD_DST_IP: {
            const auto &tokenized_ip = tokenize(attr_value, kDataMaskDelimiter);
            if (tokenized_ip.size() == 2)
            {
                // data & mask
                swss::IpAddress ip_data(trim(tokenized_ip[0]));
                if (!ip_data.isV4())
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "IP data type should be v4 type: " << QuotedVar(attr_value);
                }
                swss::IpAddress ip_mask(trim(tokenized_ip[1]));
                if (!ip_mask.isV4())
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "IP mask type should be v4 type: " << QuotedVar(attr_value);
                }
                value->aclfield.data.ip4 = ip_data.getV4Addr();
                value->aclfield.mask.ip4 = ip_mask.getV4Addr();
            }
            else
            {
                // LPM annotated value
                swss::IpPrefix ip_prefix(trim(attr_value));
                if (!ip_prefix.isV4())
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "IP type should be v6 type: " << QuotedVar(attr_value);
                }
                value->aclfield.data.ip4 = ip_prefix.getIp().getV4Addr();
                value->aclfield.mask.ip4 = ip_prefix.getMask().getV4Addr();
            }
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IPV6:
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IPV6:
        case SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6:
        case SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6: {
            const auto &tokenized_ip = tokenize(attr_value, kDataMaskDelimiter);
            if (tokenized_ip.size() == 2)
            {
                // data & mask
                swss::IpAddress ip_data(trim(tokenized_ip[0]));
                if (ip_data.isV4())
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "IP data type should be v6 type: " << QuotedVar(attr_value);
                }
                swss::IpAddress ip_mask(trim(tokenized_ip[1]));
                if (ip_mask.isV4())
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "IP mask type should be v6 type: " << QuotedVar(attr_value);
                }
                memcpy(value->aclfield.data.ip6, ip_data.getV6Addr(), sizeof(sai_ip6_t));
                memcpy(value->aclfield.mask.ip6, ip_mask.getV6Addr(), sizeof(sai_ip6_t));
            }
            else
            {
                // LPM annotated value
                swss::IpPrefix ip_prefix(trim(attr_value));
                if (ip_prefix.isV4())
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "IP type should be v6 type: " << QuotedVar(attr_value);
                }
                memcpy(value->aclfield.data.ip6, ip_prefix.getIp().getV6Addr(), sizeof(sai_ip6_t));
                memcpy(value->aclfield.mask.ip6, ip_prefix.getMask().getV6Addr(), sizeof(sai_ip6_t));
            }
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_SRC_MAC:
        case SAI_ACL_ENTRY_ATTR_FIELD_DST_MAC: {
            const std::vector<std::string> mask_and_value = tokenize(attr_value, kDataMaskDelimiter);
            swss::MacAddress mac(trim(mask_and_value[0]));
            memcpy(value->aclfield.data.mac, mac.getMac(), sizeof(sai_mac_t));
            if (mask_and_value.size() > 1)
            {
                swss::MacAddress mask(trim(mask_and_value[1]));
                memcpy(value->aclfield.mask.mac, mask.getMac(), sizeof(sai_mac_t));
            }
            else
            {
                const sai_mac_t mac_mask = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                memcpy(value->aclfield.mask.mac, mac_mask, sizeof(sai_mac_t));
            }
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_TC:
        case SAI_ACL_ENTRY_ATTR_FIELD_ICMP_TYPE:
        case SAI_ACL_ENTRY_ATTR_FIELD_ICMP_CODE:
        case SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_TYPE:
        case SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_CODE:
        case SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_PRI:
        case SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_CFI:
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_VLAN_PRI:
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_VLAN_CFI:
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_IP_PROTOCOL:
        case SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL:
        case SAI_ACL_ENTRY_ATTR_FIELD_ECN:
        case SAI_ACL_ENTRY_ATTR_FIELD_TTL:
        case SAI_ACL_ENTRY_ATTR_FIELD_TOS:
        case SAI_ACL_ENTRY_ATTR_FIELD_IPV6_NEXT_HEADER: {
            const std::vector<std::string> &value_and_mask = tokenize(attr_value, kDataMaskDelimiter);
            value->aclfield.data.u8 = to_uint<uint8_t>(trim(value_and_mask[0]));
            if (value_and_mask.size() > 1)
            {
                value->aclfield.mask.u8 = to_uint<uint8_t>(trim(value_and_mask[1]));
            }
            else
            {
                value->aclfield.mask.u8 = 0xFF;
            }
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI:
        case SAI_ACL_ENTRY_ATTR_FIELD_IPV6_FLOW_LABEL: {
            const std::vector<std::string> &value_and_mask = tokenize(attr_value, kDataMaskDelimiter);
            value->aclfield.data.u32 = to_uint<uint32_t>(trim(value_and_mask[0]));
            if (value_and_mask.size() > 1)
            {
                value->aclfield.mask.u32 = to_uint<uint32_t>(trim(value_and_mask[1]));
            }
            else
            {
                value->aclfield.mask.u32 = 0xFFFFFFFF;
            }
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_FRAG: {
            const auto &ip_frag_it = aclIpFragLookup.find(attr_value);
            if (ip_frag_it == aclIpFragLookup.end())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid IP frag " << QuotedVar(attr_value);
            }
            value->aclfield.data.u32 = ip_frag_it->second;
            value->aclfield.mask.u32 = 0xFFFFFFFF;
            break;
        }
        case SAI_ACL_ENTRY_ATTR_FIELD_PACKET_VLAN: {
            const auto &packet_vlan_it = aclPacketVlanLookup.find(attr_value);
            if (packet_vlan_it == aclPacketVlanLookup.end())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid Packet VLAN " << QuotedVar(attr_value);
            }
            value->aclfield.data.u32 = packet_vlan_it->second;
            value->aclfield.mask.u32 = 0xFFFFFFFF;
            break;
        }
        default: {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "ACL match field " << attr_name << " is not supported.";
        }
        }
    }
    catch (std::exception &e)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Failed to parse match attribute " << attr_name << " value: " << QuotedVar(attr_value);
    }
    value->aclfield.enable = true;
    return ReturnCode();
}

ReturnCode AclRuleManager::getRedirectActionPortOid(const std::string &target, sai_object_id_t *rediect_oid)
{
    // Try to parse physical port and LAG first
    Port port;
    if (gPortsOrch->getPort(target, port))
    {
        if (port.m_type == Port::PHY)
        {
            *rediect_oid = port.m_port_id;
            return ReturnCode();
        }
        else if (port.m_type == Port::LAG)
        {
            *rediect_oid = port.m_lag_id;
            return ReturnCode();
        }
        else
        {
            LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                                 << "Wrong port type for REDIRECT action. Only "
                                    "physical ports and LAG ports are supported");
        }
    }
    return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "Port " << QuotedVar(target) << " not found.";
}

ReturnCode AclRuleManager::getRedirectActionNextHopOid(const std::string &target, sai_object_id_t *rediect_oid)
{
    // Try to get nexthop object id
    const auto &next_hop_key = KeyGenerator::generateNextHopKey(target);
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key, rediect_oid))
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "ACL Redirect action target next hop ip: " << QuotedVar(target)
                             << " doesn't exist on the switch");
    }
    return ReturnCode();
}

ReturnCode AclRuleManager::setAllMatchFieldValues(const P4AclRuleAppDbEntry &app_db_entry,
                                                  const P4AclTableDefinition *acl_table, P4AclRule &acl_rule)
{
    for (const auto &match_fv : app_db_entry.match_fvs)
    {
        const auto &match_field = fvField(match_fv);
        const auto &match_value = fvValue(match_fv);
        ReturnCode set_match_rc;
        // Set UDF fields
        auto udf_fields_it = acl_table->udf_fields_lookup.find(match_field);
        if (udf_fields_it != acl_table->udf_fields_lookup.end())
        {
            // Bytes Offset to extract Hex value from match_value string
            uint16_t bytes_offset = 0;
            for (const auto &udf_field : udf_fields_it->second)
            {
                auto udf_group_index_it = acl_table->udf_group_attr_index_lookup.find(udf_field.group_id);
                if (udf_group_index_it == acl_table->udf_group_attr_index_lookup.end())
                {
                    RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
                        "No UDF group found in ACL table definition with id:" << QuotedVar(udf_field.group_id));
                }
                set_match_rc = setUdfMatchValue(
                    udf_field, match_value,
                    &acl_rule.match_fvs[SAI_ACL_ENTRY_ATTR_USER_DEFINED_FIELD_GROUP_MIN + udf_group_index_it->second],
                    &acl_rule
                         .udf_data_masks[SAI_ACL_ENTRY_ATTR_USER_DEFINED_FIELD_GROUP_MIN + udf_group_index_it->second],
                    bytes_offset);
                if (!set_match_rc.ok())
                {
                    set_match_rc.prepend("Invalid ACL rule match field " + QuotedVar(match_field) + ": " +
                                         QuotedVar(match_value) + " to add: ");
                    return set_match_rc;
                }
                bytes_offset = (uint16_t)(bytes_offset + udf_field.length);
            }
            continue;
        }
        // Set Composite SAI fields
        auto composite_sai_match_field_it = acl_table->composite_sai_match_fields_lookup.find(match_field);
        if (composite_sai_match_field_it != acl_table->composite_sai_match_fields_lookup.end())
        {
            // Handle composite SAI match fields
            for (const auto &sai_match_field : composite_sai_match_field_it->second)
            {
                set_match_rc = setCompositeSaiMatchValue(sai_match_field.entry_attr, match_value,
                                                         &acl_rule.match_fvs[sai_match_field.entry_attr]);
                if (!set_match_rc.ok())
                {
                    set_match_rc.prepend("Invalid ACL rule match field " + QuotedVar(match_field) + ": " +
                                         QuotedVar(match_value) + " to add: ");
                    return set_match_rc;
                }
            }
            continue;
        }
        auto sai_match_field_it = acl_table->sai_match_field_lookup.find(match_field);
        if (sai_match_field_it == acl_table->sai_match_field_lookup.end())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "ACL rule match field " << QuotedVar(match_field) << ": " << QuotedVar(match_value)
                   << " is an invalid ACL rule attribute";
        }
        auto &sai_match_field = sai_match_field_it->second;
        if (sai_match_field.entry_attr == SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE &&
            acl_table->ip_type_bit_type_lookup.find(sai_match_field_it->first) !=
                acl_table->ip_type_bit_type_lookup.end())
        {
            set_match_rc =
                setMatchValue(sai_match_field.entry_attr, match_value, &acl_rule.match_fvs[sai_match_field.entry_attr],
                              &acl_rule, acl_table->ip_type_bit_type_lookup.at(sai_match_field_it->first));
        }
        else
        {
            set_match_rc = setMatchValue(sai_match_field.entry_attr, match_value,
                                         &acl_rule.match_fvs[sai_match_field.entry_attr], &acl_rule);
        }
        if (!set_match_rc.ok())
        {
            set_match_rc.prepend("Invalid ACL rule match field " + QuotedVar(match_field) + ": " +
                                 QuotedVar(match_value) + " to add: ");
            return set_match_rc;
        }
    }
    if (!acl_table->ip_type_bit_type_lookup.empty() &&
        acl_rule.match_fvs.find(SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE) == acl_rule.match_fvs.end())
    {
        // Wildcard match on ip type bits
        sai_attribute_value_t ip_type_attr;
        ip_type_attr.aclfield.data.u32 = SAI_ACL_IP_TYPE_ANY;
        ip_type_attr.aclfield.mask.u32 = 0xFFFFFFFF;
        ip_type_attr.aclfield.enable = true;
        acl_rule.match_fvs.insert({SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE, ip_type_attr});
    }
    return ReturnCode();
}

ReturnCode AclRuleManager::setAllActionFieldValues(const P4AclRuleAppDbEntry &app_db_entry,
                                                   const P4AclTableDefinition *acl_table, P4AclRule &acl_rule)
{
    const auto &action_param_list_it = acl_table->rule_action_field_lookup.find(app_db_entry.action);
    if (action_param_list_it == acl_table->rule_action_field_lookup.end())
    {
        ReturnCode status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                            << "Invalid P4 ACL action " << QuotedVar(app_db_entry.action);
        return status;
    }
    SaiActionWithParam sai_action_param;
    for (const auto &action_param : action_param_list_it->second)
    {
        sai_action_param.action = action_param.action;
        sai_action_param.param_name = action_param.param_name;
        sai_action_param.param_value = action_param.param_value;
        if (!action_param.param_name.empty())
        {
            const auto &param_value_it = app_db_entry.action_param_fvs.find(action_param.param_name);
            if (param_value_it == app_db_entry.action_param_fvs.end())
            {
                ReturnCode status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                                    << "No action param found for action " << action_param.action;
                return status;
            }
            if (!param_value_it->second.empty())
            {
                sai_action_param.param_value = param_value_it->second;
            }
        }
        auto set_action_rc = setActionValue(sai_action_param.action, sai_action_param.param_value,
                                            &acl_rule.action_fvs[sai_action_param.action], &acl_rule);
        if (!set_action_rc.ok())
        {
            return set_action_rc;
        }
    }
    return ReturnCode();
}

ReturnCode AclRuleManager::setActionValue(const acl_entry_attr_union_t attr_name, const std::string &attr_value,
                                          sai_attribute_value_t *value, P4AclRule *acl_rule)
{
    switch (attr_name)
    {
    case SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION: {
        const auto it = aclPacketActionLookup.find(attr_value);
        if (it != aclPacketActionLookup.end())
        {
            value->aclaction.parameter.s32 = it->second;
        }
        else
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Invalid ACL packet action " << QuotedVar(attr_value) << " for "
                   << QuotedVar(acl_rule->acl_table_name);
        }
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT: {
        sai_object_id_t redirect_oid;
        if (getRedirectActionPortOid(attr_value, &redirect_oid).ok())
        {
            value->aclaction.parameter.oid = redirect_oid;
            break;
        }
        RETURN_IF_ERROR(getRedirectActionNextHopOid(attr_value, &redirect_oid));
        value->aclaction.parameter.oid = redirect_oid;
        acl_rule->action_redirect_nexthop_key = KeyGenerator::generateNextHopKey(attr_value);
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_ENDPOINT_IP: {
        try
        {
            swss::IpAddress ip(attr_value);
            if (ip.isV4())
            {
                value->aclaction.parameter.ip4 = ip.getV4Addr();
            }
            else
            {
                memcpy(value->aclaction.parameter.ip6, ip.getV6Addr(), sizeof(sai_ip6_t));
            }
        }
        catch (std::exception &e)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Action attribute " << QuotedVar(std::to_string(attr_name)) << " is invalid for "
                   << QuotedVar(acl_rule->acl_table_name) << ": Expect IP address but got " << QuotedVar(attr_value);
        }
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS:
    case SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS: {
        sai_object_id_t mirror_session_oid;
        std::string key = KeyGenerator::generateMirrorSessionKey(attr_value);
        if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_MIRROR_SESSION, key, &mirror_session_oid))
        {
            return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                   << "Mirror session " << QuotedVar(attr_value) << " does not exist for "
                   << QuotedVar(acl_rule->acl_table_name);
        }
        auto &mirror_session = acl_rule->action_mirror_sessions[attr_name];
        mirror_session.name = attr_value;
        mirror_session.key = key;
        mirror_session.oid = mirror_session_oid;
        value->aclaction.parameter.objlist.list = &mirror_session.oid;
        value->aclaction.parameter.objlist.count = 1;
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_PACKET_COLOR: {
        const auto &it = aclPacketColorLookup.find(attr_value);
        if (it == aclPacketColorLookup.end())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Invalid ACL packet color " << QuotedVar(attr_value) << " in action for "
                   << QuotedVar(acl_rule->acl_table_name);
        }
        value->aclaction.parameter.s32 = it->second;
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_MAC: {
        try
        {
            swss::MacAddress mac(attr_value);
            memcpy(value->aclaction.parameter.mac, mac.getMac(), sizeof(sai_mac_t));
        }
        catch (std::exception &e)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Action attribute " << QuotedVar(std::to_string(attr_name)) << " is invalid for "
                   << QuotedVar(acl_rule->acl_table_name) << ": Expect MAC address but got " << QuotedVar(attr_value);
        }
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_IP:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_IP: {
        try
        {
            swss::IpAddress ip(attr_value);
            if (!ip.isV4())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Action attribute " << QuotedVar(std::to_string(attr_name)) << " is invalid for "
                       << QuotedVar(acl_rule->acl_table_name) << ": Expect IPv4 address but got "
                       << QuotedVar(attr_value);
            }
            value->aclaction.parameter.ip4 = ip.getV4Addr();
        }
        catch (std::exception &e)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Action attribute " << QuotedVar(std::to_string(attr_name)) << " is invalid for "
                   << QuotedVar(acl_rule->acl_table_name) << ": Expect IP address but got " << QuotedVar(attr_value);
        }
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_IPV6:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_IPV6: {
        try
        {
            swss::IpAddress ip(attr_value);
            if (ip.isV4())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Action attribute " << QuotedVar(std::to_string(attr_name)) << " is invalid for "
                       << QuotedVar(acl_rule->acl_table_name) << ": Expect IPv6 address but got "
                       << QuotedVar(attr_value);
            }
            memcpy(value->aclaction.parameter.ip6, ip.getV6Addr(), sizeof(sai_ip6_t));
        }
        catch (std::exception &e)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Action attribute " << QuotedVar(std::to_string(attr_name)) << " is invalid for "
                   << QuotedVar(acl_rule->acl_table_name) << ": Expect IP address but got " << QuotedVar(attr_value);
        }
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID: {
        try
        {
            uint32_t queue_num = to_uint<uint32_t>(attr_value);
            if (queue_num < 1 || queue_num > m_userDefinedTraps.size())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid CPU queue number " << QuotedVar(attr_value) << " for "
                       << QuotedVar(acl_rule->acl_table_name)
                       << ". Queue number should >= 1 and <= " << m_userDefinedTraps.size();
            }
            value->aclaction.parameter.oid = m_userDefinedTraps[queue_num - 1].user_defined_trap;
            acl_rule->action_qos_queue_num = queue_num;
        }
        catch (std::exception &e)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Action attribute " << QuotedVar(std::to_string(attr_name)) << " is invalid for "
                   << QuotedVar(acl_rule->acl_table_name) << ": Expect integer but got " << QuotedVar(attr_value);
        }
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_TC:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_DSCP:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_ECN:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_PRI:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_OUTER_VLAN_PRI: {
        try
        {
            value->aclaction.parameter.u8 = to_uint<uint8_t>(attr_value);
        }
        catch (std::exception &e)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Action attribute " << QuotedVar(std::to_string(attr_name)) << " is invalid for "
                   << QuotedVar(acl_rule->acl_table_name) << ": Expect integer but got " << QuotedVar(attr_value);
        }
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_ID: {
        try
        {
            value->aclaction.parameter.u32 = to_uint<uint32_t>(attr_value);
        }
        catch (std::exception &e)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Action attribute " << QuotedVar(std::to_string(attr_name)) << " is invalid for "
                   << QuotedVar(acl_rule->acl_table_name) << ": Expect integer but got " << QuotedVar(attr_value);
        }
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_OUTER_VLAN_ID:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_L4_SRC_PORT:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_L4_DST_PORT: {
        try
        {
            value->aclaction.parameter.u16 = to_uint<uint16_t>(attr_value);
        }
        catch (std::exception &e)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Action attribute " << QuotedVar(std::to_string(attr_name)) << " is invalid for "
                   << QuotedVar(acl_rule->acl_table_name) << ": Expect integer but got " << QuotedVar(attr_value);
        }
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_FLOOD:
    case SAI_ACL_ENTRY_ATTR_ACTION_DECREMENT_TTL:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_DO_NOT_LEARN: {
        // parameter is not needed
        break;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_VRF: {
        if (!attr_value.empty() && !m_vrfOrch->isVRFexists(attr_value))
        {
            return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "No VRF found with name " << QuotedVar(attr_value)
                                                             << " for " << QuotedVar(acl_rule->acl_table_name);
        }
        value->aclaction.parameter.oid = m_vrfOrch->getVRFid(attr_value);
        break;
    }
    default: {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid ACL action " << attr_name << " for " << QuotedVar(acl_rule->acl_table_name);
    }
    }
    value->aclaction.enable = true;
    return ReturnCode();
}

ReturnCode AclRuleManager::setMeterValue(const P4AclTableDefinition *acl_table, const P4AclRuleAppDbEntry &app_db_entry,
                                         P4AclMeter &acl_meter)
{
    if (app_db_entry.meter.enabled)
    {
        acl_meter.cir = app_db_entry.meter.cir;
        acl_meter.cburst = app_db_entry.meter.cburst;
        acl_meter.pir = app_db_entry.meter.pir;
        acl_meter.pburst = app_db_entry.meter.pburst;
        acl_meter.mode = SAI_POLICER_MODE_TR_TCM;
        if (acl_table->meter_unit == P4_METER_UNIT_PACKETS)
        {
            acl_meter.type = SAI_METER_TYPE_PACKETS;
        }
        else if (acl_table->meter_unit == P4_METER_UNIT_BYTES)
        {
            acl_meter.type = SAI_METER_TYPE_BYTES;
        }
        else
        {
            ReturnCode status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                                << "Invalid ACL meter type " << QuotedVar(acl_table->meter_unit);
            return status;
        }
        acl_meter.enabled = true;
    }
    const auto &action_color_it = acl_table->rule_packet_action_color_lookup.find(app_db_entry.action);
    if (action_color_it != acl_table->rule_packet_action_color_lookup.end() && !action_color_it->second.empty())
    {
        acl_meter.packet_color_actions = action_color_it->second;
    }
    return ReturnCode();
}

ReturnCode AclRuleManager::createAclRule(P4AclRule &acl_rule)
{
    SWSS_LOG_ENTER();
    std::vector<sai_attribute_t> acl_entry_attrs;
    sai_attribute_t acl_entry_attr;
    acl_entry_attr.id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    acl_entry_attr.value.oid = acl_rule.acl_table_oid;
    acl_entry_attrs.push_back(acl_entry_attr);

    acl_entry_attr.id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    acl_entry_attr.value.u32 = acl_rule.priority;
    acl_entry_attrs.push_back(acl_entry_attr);

    acl_entry_attr.id = SAI_ACL_ENTRY_ATTR_ADMIN_STATE;
    acl_entry_attr.value.booldata = true;
    acl_entry_attrs.push_back(acl_entry_attr);

    // Add matches
    for (const auto &match_fv : acl_rule.match_fvs)
    {
        acl_entry_attr.id = fvField(match_fv);
        acl_entry_attr.value = fvValue(match_fv);
        acl_entry_attrs.push_back(acl_entry_attr);
    }

    // Add actions
    for (const auto &action_fv : acl_rule.action_fvs)
    {
        acl_entry_attr.id = fvField(action_fv);
        acl_entry_attr.value = fvValue(action_fv);
        acl_entry_attrs.push_back(acl_entry_attr);
    }

    // Track if the entry creats a new counter or meter
    bool created_meter = false;
    bool created_counter = false;
    const auto &table_name_and_rule_key = concatTableNameAndRuleKey(acl_rule.acl_table_name, acl_rule.acl_rule_key);

    // Add meter
    if (acl_rule.meter.enabled || !acl_rule.meter.packet_color_actions.empty())
    {
        if (acl_rule.meter.meter_oid == SAI_NULL_OBJECT_ID)
        {
            auto status = createAclMeter(acl_rule.meter, table_name_and_rule_key, &acl_rule.meter.meter_oid);
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Failed to create ACL meter for rule %s", QuotedVar(acl_rule.acl_rule_key).c_str());
                return status;
            }
            created_meter = true;
        }
        acl_entry_attr.id = SAI_ACL_ENTRY_ATTR_ACTION_SET_POLICER;
        acl_entry_attr.value.aclaction.parameter.oid = acl_rule.meter.meter_oid;
        acl_entry_attr.value.aclaction.enable = true;
        acl_entry_attrs.push_back(acl_entry_attr);
    }

    // Add counter
    if (acl_rule.counter.packets_enabled || acl_rule.counter.bytes_enabled)
    {
        if (acl_rule.counter.counter_oid == SAI_NULL_OBJECT_ID)
        {
            auto status = createAclCounter(acl_rule.acl_table_name, table_name_and_rule_key, acl_rule.counter,
                                           &acl_rule.counter.counter_oid);
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Failed to create ACL counter for rule %s", QuotedVar(acl_rule.acl_rule_key).c_str());
                if (created_meter)
                {
                    auto rc = removeAclMeter(table_name_and_rule_key);
                    if (!rc.ok())
                    {
                        SWSS_RAISE_CRITICAL_STATE("Failed to remove ACL meter in recovery.");
                    }
                }
                return status;
            }
            created_counter = true;
        }
        acl_entry_attr.id = SAI_ACL_ENTRY_ATTR_ACTION_COUNTER;
        acl_entry_attr.value.aclaction.enable = true;
        acl_entry_attr.value.aclaction.parameter.oid = acl_rule.counter.counter_oid;
        acl_entry_attrs.push_back(acl_entry_attr);
    }

    auto sai_status = sai_acl_api->create_acl_entry(&acl_rule.acl_entry_oid, gSwitchId,
                                                    (uint32_t)acl_entry_attrs.size(), acl_entry_attrs.data());
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        ReturnCode status = ReturnCode(sai_status)
                            << "Failed to create ACL entry in table " << QuotedVar(acl_rule.acl_table_name);
        SWSS_LOG_ERROR("%s SAI_STATUS: %s", status.message().c_str(), sai_serialize_status(sai_status).c_str());
        if (created_meter)
        {
            auto rc = removeAclMeter(table_name_and_rule_key);
            if (!rc.ok())
            {
                SWSS_RAISE_CRITICAL_STATE("Failed to remove ACL meter in recovery.");
            }
        }
        if (created_counter)
        {
            auto rc = removeAclCounter(acl_rule.acl_table_name, table_name_and_rule_key);
            if (!rc.ok())
            {
                SWSS_RAISE_CRITICAL_STATE("Failed to remove ACL counter in recovery.");
            }
        }
        return status;
    }
    return ReturnCode();
}

ReturnCode AclRuleManager::updateAclRule(const P4AclRule &acl_rule, const P4AclRule &old_acl_rule,
                                         std::vector<sai_attribute_t> &acl_entry_attrs,
                                         std::vector<sai_attribute_t> &rollback_attrs)
{
    SWSS_LOG_ENTER();

    sai_attribute_t acl_entry_attr;
    std::set<acl_entry_attr_union_t> actions_to_reset;
    for (const auto &old_action_fv : old_acl_rule.action_fvs)
    {
        actions_to_reset.insert(fvField(old_action_fv));
    }

    for (const auto &action_fv : acl_rule.action_fvs)
    {
        const auto &it = old_acl_rule.action_fvs.find(fvField(action_fv));
        if (it == old_acl_rule.action_fvs.end())
        {
            acl_entry_attr.id = fvField(action_fv);
            acl_entry_attr.value = fvValue(action_fv);
            acl_entry_attr.value.aclaction.enable = true;
            acl_entry_attrs.push_back(acl_entry_attr);
            acl_entry_attr.value.aclaction.enable = false;
            rollback_attrs.push_back(acl_entry_attr);
        }
        else if (isDiffActionFieldValue(fvField(action_fv), fvValue(action_fv), it->second, acl_rule, old_acl_rule))
        {
            acl_entry_attr.id = fvField(action_fv);
            acl_entry_attr.value = fvValue(action_fv);
            acl_entry_attr.value.aclaction.enable = true;
            acl_entry_attrs.push_back(acl_entry_attr);
            acl_entry_attr.value = it->second;
            rollback_attrs.push_back(acl_entry_attr);
        }
        if (it != old_acl_rule.action_fvs.end())
        {
            actions_to_reset.erase(fvField(action_fv));
        }
    }

    for (const auto &action : actions_to_reset)
    {
        acl_entry_attr.id = action;
        acl_entry_attr.value = old_acl_rule.action_fvs.at(action);
        acl_entry_attr.value.aclaction.enable = false;
        acl_entry_attrs.push_back(acl_entry_attr);
        acl_entry_attr.value.aclaction.enable = true;
        rollback_attrs.push_back(acl_entry_attr);
    }

    ReturnCode status;
    int i;
    for (i = 0; i < static_cast<int>(acl_entry_attrs.size()); ++i)
    {
        status = ReturnCode(sai_acl_api->set_acl_entry_attribute(old_acl_rule.acl_entry_oid, &acl_entry_attrs[i]));
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to update ACL rule attributes: %s", status.message().c_str());
            break;
        }
    }
    if (!status.ok())
    {
        for (--i; i >= 0; --i)
        {
            auto sai_status = sai_acl_api->set_acl_entry_attribute(old_acl_rule.acl_entry_oid, &rollback_attrs[i]);
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set ACL rule attribute. SAI_STATUS: %s",
                               sai_serialize_status(sai_status).c_str());
                SWSS_RAISE_CRITICAL_STATE("Failed to set ACL rule attribute in recovery.");
            }
        }
        return status;
    }

    // Clear old ACL rule dependent refcount and update refcount in new rule
    if (!old_acl_rule.action_redirect_nexthop_key.empty())
    {
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, old_acl_rule.action_redirect_nexthop_key);
    }
    if (!acl_rule.action_redirect_nexthop_key.empty())
    {
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, acl_rule.action_redirect_nexthop_key);
    }
    for (const auto &mirror_session : old_acl_rule.action_mirror_sessions)
    {
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_MIRROR_SESSION, fvValue(mirror_session).key);
    }
    for (const auto &mirror_session : acl_rule.action_mirror_sessions)
    {
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_MIRROR_SESSION, fvValue(mirror_session).key);
    }
    auto old_set_vrf_action_it = old_acl_rule.action_fvs.find(SAI_ACL_ENTRY_ATTR_ACTION_SET_VRF);
    if (old_set_vrf_action_it != old_acl_rule.action_fvs.end())
    {
        m_vrfOrch->decreaseVrfRefCount(old_set_vrf_action_it->second.aclaction.parameter.oid);
    }
    auto set_vrf_action_it = acl_rule.action_fvs.find(SAI_ACL_ENTRY_ATTR_ACTION_SET_VRF);
    if (set_vrf_action_it != acl_rule.action_fvs.end())
    {
        m_vrfOrch->increaseVrfRefCount(set_vrf_action_it->second.aclaction.parameter.oid);
    }
    auto set_user_trap_it = acl_rule.action_fvs.find(SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID);
    if (set_user_trap_it != acl_rule.action_fvs.end())
    {
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP,
                                        std::to_string(acl_rule.action_qos_queue_num));
    }
    auto old_set_user_trap_it = old_acl_rule.action_fvs.find(SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID);
    if (old_set_user_trap_it != old_acl_rule.action_fvs.end())
    {
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP,
                                        std::to_string(old_acl_rule.action_qos_queue_num));
    }
    return ReturnCode();
}

ReturnCode AclRuleManager::removeAclRule(const std::string &acl_table_name, const std::string &acl_rule_key)
{
    auto *acl_rule = getAclRule(acl_table_name, acl_rule_key);
    if (acl_rule == nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "ACL rule with key " << QuotedVar(acl_rule_key) << " in table "
                             << QuotedVar(acl_table_name) << " does not exist");
    }
    const auto &table_name_and_rule_key = concatTableNameAndRuleKey(acl_table_name, acl_rule_key);
    // Check if there is anything referring to the next hop before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_ACL_ENTRY, table_name_and_rule_key, &ref_count))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to get reference count for ACL rule "
                                                 << QuotedVar(table_name_and_rule_key));
    }
    if (ref_count > 0)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "ACL rule " << QuotedVar(acl_rule_key)
                             << " referenced by other objects (ref_count = " << ref_count << ")");
    }

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_acl_api->remove_acl_entry(acl_rule->acl_entry_oid),
                                   "Failed to remove ACL rule with key "
                                       << sai_serialize_object_id(acl_rule->acl_entry_oid) << " in table "
                                       << QuotedVar(acl_table_name));
    bool deleted_meter = false;
    if (acl_rule->meter.enabled || !acl_rule->meter.packet_color_actions.empty())
    {
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key);
        auto status = removeAclMeter(table_name_and_rule_key);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to remove ACL meter for rule with key %s in table %s.",
                           QuotedVar(acl_rule_key).c_str(), QuotedVar(acl_table_name).c_str());
            auto rc = createAclRule(*acl_rule);
            if (!rc.ok())
            {
                SWSS_RAISE_CRITICAL_STATE("Failed to create ACL rule in recovery.");
            }
            m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key);
            return status;
        }
        acl_rule->meter.meter_oid = SAI_NULL_OBJECT_ID;
        deleted_meter = true;
    }
    if (acl_rule->counter.packets_enabled || acl_rule->counter.bytes_enabled)
    {
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ACL_COUNTER, table_name_and_rule_key);
        auto status = removeAclCounter(acl_table_name, table_name_and_rule_key);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to remove ACL counter for rule with key %s.",
                           QuotedVar(table_name_and_rule_key).c_str());
            m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ACL_COUNTER, table_name_and_rule_key);
            if (deleted_meter)
            {
                auto rc = createAclMeter(acl_rule->meter, table_name_and_rule_key, &acl_rule->meter.meter_oid);
                m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key);
                if (!rc.ok())
                {
                    SWSS_RAISE_CRITICAL_STATE("Failed to create ACL rule in recovery.");
                    return status;
                }
            }
            auto rc = createAclRule(*acl_rule);
            if (!rc.ok())
            {
                SWSS_RAISE_CRITICAL_STATE("Failed to create ACL rule in recovery.");
            }
            return status;
        }
        // Remove counter stats
        m_countersTable->del(acl_rule->db_key);
    }
    gCrmOrch->decCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_ENTRY, acl_rule->acl_table_oid);
    if (!acl_rule->action_redirect_nexthop_key.empty())
    {
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, acl_rule->action_redirect_nexthop_key);
    }
    for (const auto &mirror_session : acl_rule->action_mirror_sessions)
    {
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_MIRROR_SESSION, fvValue(mirror_session).key);
    }
    auto set_vrf_action_it = acl_rule->action_fvs.find(SAI_ACL_ENTRY_ATTR_ACTION_SET_VRF);
    if (set_vrf_action_it != acl_rule->action_fvs.end())
    {
        m_vrfOrch->decreaseVrfRefCount(set_vrf_action_it->second.aclaction.parameter.oid);
    }
    auto set_user_trap_it = acl_rule->action_fvs.find(SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID);
    if (set_user_trap_it != acl_rule->action_fvs.end())
    {
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP,
                                        std::to_string(acl_rule->action_qos_queue_num));
    }
    for (const auto &port_alias : acl_rule->in_ports)
    {
        gPortsOrch->decreasePortRefCount(port_alias);
    }
    for (const auto &port_alias : acl_rule->out_ports)
    {
        gPortsOrch->decreasePortRefCount(port_alias);
    }
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ACL_TABLE, acl_table_name);
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ACL_ENTRY, table_name_and_rule_key);
    m_aclRuleTables[acl_table_name].erase(acl_rule_key);
    return ReturnCode();
}

ReturnCode AclRuleManager::processAddRuleRequest(const std::string &acl_rule_key,
                                                 const P4AclRuleAppDbEntry &app_db_entry)
{
    P4AclRule acl_rule;
    acl_rule.priority = app_db_entry.priority;
    acl_rule.acl_rule_key = acl_rule_key;
    acl_rule.p4_action = app_db_entry.action;
    acl_rule.db_key = app_db_entry.db_key;
    const auto *acl_table = gP4Orch->getAclTableManager()->getAclTable(app_db_entry.acl_table_name);
    acl_rule.acl_table_oid = acl_table->table_oid;
    acl_rule.acl_table_name = acl_table->acl_table_name;

    // Add match field values
    LOG_AND_RETURN_IF_ERROR(setAllMatchFieldValues(app_db_entry, acl_table, acl_rule));

    // Add action field values
    auto status = setAllActionFieldValues(app_db_entry, acl_table, acl_rule);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to add action field values for ACL rule %s: %s",
                       QuotedVar(acl_rule.acl_rule_key).c_str(), status.message().c_str());
        return status;
    }

    // Add meter
    LOG_AND_RETURN_IF_ERROR(setMeterValue(acl_table, app_db_entry, acl_rule.meter));

    // Add counter
    if (!acl_table->counter_unit.empty())
    {
        if (acl_table->counter_unit == P4_COUNTER_UNIT_PACKETS)
        {
            acl_rule.counter.packets_enabled = true;
        }
        else if (acl_table->counter_unit == P4_COUNTER_UNIT_BYTES)
        {
            acl_rule.counter.bytes_enabled = true;
        }
        else if (acl_table->counter_unit == P4_COUNTER_UNIT_BOTH)
        {
            acl_rule.counter.bytes_enabled = true;
            acl_rule.counter.packets_enabled = true;
        }
        else
        {
            LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                                 << "Invalid ACL counter type " << QuotedVar(acl_table->counter_unit));
        }
    }
    status = createAclRule(acl_rule);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to create ACL rule with key %s in table %s", QuotedVar(acl_rule.acl_rule_key).c_str(),
                       QuotedVar(app_db_entry.acl_table_name).c_str());
        return status;
    }
    // ACL entry created in HW, update refcount
    if (!acl_rule.action_redirect_nexthop_key.empty())
    {
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, acl_rule.action_redirect_nexthop_key);
    }
    for (const auto &mirror_session : acl_rule.action_mirror_sessions)
    {
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_MIRROR_SESSION, fvValue(mirror_session).key);
    }
    auto set_vrf_action_it = acl_rule.action_fvs.find(SAI_ACL_ENTRY_ATTR_ACTION_SET_VRF);
    if (set_vrf_action_it != acl_rule.action_fvs.end())
    {
        m_vrfOrch->increaseVrfRefCount(set_vrf_action_it->second.aclaction.parameter.oid);
    }
    auto set_user_trap_it = acl_rule.action_fvs.find(SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID);
    if (set_user_trap_it != acl_rule.action_fvs.end())
    {
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP,
                                        std::to_string(acl_rule.action_qos_queue_num));
    }
    for (const auto &port_alias : acl_rule.in_ports)
    {
        gPortsOrch->increasePortRefCount(port_alias);
    }
    for (const auto &port_alias : acl_rule.out_ports)
    {
        gPortsOrch->increasePortRefCount(port_alias);
    }
    gCrmOrch->incCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_ENTRY, acl_rule.acl_table_oid);
    m_aclRuleTables[acl_rule.acl_table_name][acl_rule.acl_rule_key] = acl_rule;
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ACL_TABLE, acl_rule.acl_table_name);
    const auto &table_name_and_rule_key = concatTableNameAndRuleKey(acl_rule.acl_table_name, acl_rule.acl_rule_key);
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_ACL_ENTRY, table_name_and_rule_key, acl_rule.acl_entry_oid);
    if (acl_rule.counter.packets_enabled || acl_rule.counter.bytes_enabled)
    {
        // Counter was created, increase ACL rule ref count
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ACL_COUNTER, table_name_and_rule_key);
    }
    if (acl_rule.meter.enabled || !acl_rule.meter.packet_color_actions.empty())
    {
        // Meter was created, increase ACL rule ref count
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key);
    }
    SWSS_LOG_NOTICE("Suceeded to create ACL rule %s : %s", QuotedVar(acl_rule.acl_rule_key).c_str(),
                    sai_serialize_object_id(acl_rule.acl_entry_oid).c_str());
    return status;
}

ReturnCode AclRuleManager::processDeleteRuleRequest(const std::string &acl_table_name, const std::string &acl_rule_key)
{
    SWSS_LOG_ENTER();
    auto status = removeAclRule(acl_table_name, acl_rule_key);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to remove ACL rule with key %s in table %s", QuotedVar(acl_rule_key).c_str(),
                       QuotedVar(acl_table_name).c_str());
    }
    return status;
}

ReturnCode AclRuleManager::processUpdateRuleRequest(const P4AclRuleAppDbEntry &app_db_entry,
                                                    const P4AclRule &old_acl_rule)
{
    SWSS_LOG_ENTER();

    P4AclRule acl_rule;
    const auto *acl_table = gP4Orch->getAclTableManager()->getAclTable(app_db_entry.acl_table_name);
    acl_rule.acl_table_oid = acl_table->table_oid;
    acl_rule.acl_table_name = acl_table->acl_table_name;
    acl_rule.db_key = app_db_entry.db_key;

    // Skip match field comparison because the acl_rule_key including match
    // field value and priority should be the same with old one.
    acl_rule.match_fvs = old_acl_rule.match_fvs;
    acl_rule.in_ports = old_acl_rule.in_ports;
    acl_rule.out_ports = old_acl_rule.out_ports;
    acl_rule.priority = app_db_entry.priority;
    acl_rule.acl_rule_key = old_acl_rule.acl_rule_key;
    // Skip Counter comparison since the counter unit is defined in table
    // definition
    acl_rule.counter = old_acl_rule.counter;

    std::vector<sai_attribute_t> acl_entry_attrs;
    std::vector<sai_attribute_t> rollback_attrs;
    sai_attribute_t acl_entry_attr;
    const auto &table_name_and_rule_key = concatTableNameAndRuleKey(acl_rule.acl_table_name, acl_rule.acl_rule_key);

    // Update action field
    acl_rule.p4_action = app_db_entry.action;
    acl_rule.acl_entry_oid = old_acl_rule.acl_entry_oid;
    auto set_actions_rc = setAllActionFieldValues(app_db_entry, acl_table, acl_rule);
    if (!set_actions_rc.ok())
    {
        SWSS_LOG_ERROR("Failed to add action field values for ACL rule %s: %s",
                       QuotedVar(acl_rule.acl_rule_key).c_str(), set_actions_rc.message().c_str());
        return set_actions_rc;
    }

    // Update meter
    bool remove_meter = false;
    bool created_meter = false;
    bool updated_meter = false;
    LOG_AND_RETURN_IF_ERROR(setMeterValue(acl_table, app_db_entry, acl_rule.meter));
    if (old_acl_rule.meter.meter_oid == SAI_NULL_OBJECT_ID &&
        (acl_rule.meter.enabled || !acl_rule.meter.packet_color_actions.empty()))
    {
        // Create new meter
        auto status = createAclMeter(acl_rule.meter, table_name_and_rule_key, &acl_rule.meter.meter_oid);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to create ACL meter for rule %s", QuotedVar(acl_rule.acl_rule_key).c_str());
            return status;
        }
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key);
        created_meter = true;
        acl_entry_attr.id = SAI_ACL_ENTRY_ATTR_ACTION_SET_POLICER;
        acl_entry_attr.value.aclaction.enable = true;
        acl_entry_attr.value.aclaction.parameter.oid = acl_rule.meter.meter_oid;
        acl_entry_attrs.push_back(acl_entry_attr);
        acl_entry_attr.value.aclaction.enable = false;
        acl_entry_attr.value.aclaction.parameter.oid = SAI_NULL_OBJECT_ID;
        rollback_attrs.push_back(acl_entry_attr);
    }
    else if (old_acl_rule.meter.meter_oid != SAI_NULL_OBJECT_ID && !acl_rule.meter.enabled &&
             acl_rule.meter.packet_color_actions.empty())
    {
        // Remove old meter
        remove_meter = true;
        acl_entry_attr.id = SAI_ACL_ENTRY_ATTR_ACTION_SET_POLICER;
        acl_entry_attr.value.aclaction.enable = false;
        acl_entry_attr.value.aclaction.parameter.oid = SAI_NULL_OBJECT_ID;
        acl_entry_attrs.push_back(acl_entry_attr);
        acl_entry_attr.value.aclaction.enable = true;
        acl_entry_attr.value.aclaction.parameter.oid = old_acl_rule.meter.meter_oid;
        rollback_attrs.push_back(acl_entry_attr);
    }
    else if (old_acl_rule.meter.meter_oid != SAI_NULL_OBJECT_ID)
    {
        // Update meter attributes
        auto status = updateAclMeter(acl_rule.meter, old_acl_rule.meter);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to update ACL meter for rule %s", QuotedVar(acl_rule.acl_rule_key).c_str());
            return status;
        }
        updated_meter = true;
        acl_rule.meter.meter_oid = old_acl_rule.meter.meter_oid;
    }

    auto status = updateAclRule(acl_rule, old_acl_rule, acl_entry_attrs, rollback_attrs);
    if (status.ok())
    {
        // Remove old meter.
        if (remove_meter)
        {
            m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key);
            auto rc = removeAclMeter(table_name_and_rule_key);
            if (!rc.ok())
            {
                SWSS_LOG_ERROR("Failed to remove ACL meter for rule %s", QuotedVar(acl_rule.acl_rule_key).c_str());
                for (const auto &entry_attr : rollback_attrs)
                {
                    auto sai_status = sai_acl_api->set_acl_entry_attribute(old_acl_rule.acl_entry_oid, &entry_attr);
                    if (sai_status != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("Failed to set ACL rule attribute. SAI_STATUS: %s",
                                       sai_serialize_status(sai_status).c_str());
                        SWSS_RAISE_CRITICAL_STATE("Failed to set ACL rule attribute in recovery.");
                    }
                }
                m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key);
                return rc;
            }
        }
    }
    else
    {
        SWSS_LOG_ERROR("Failed to update ACL rule %s", QuotedVar(acl_rule.acl_rule_key).c_str());
        // Clean up
        if (created_meter)
        {
            m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key);
            auto rc = removeAclMeter(table_name_and_rule_key);
            if (!rc.ok())
            {
                SWSS_RAISE_CRITICAL_STATE("Failed to remove ACL meter in recovery.");
            }
        }
        if (updated_meter)
        {
            auto rc = updateAclMeter(old_acl_rule.meter, acl_rule.meter);
            if (!rc.ok())
            {
                SWSS_RAISE_CRITICAL_STATE("Failed to update ACL meter in recovery.");
            }
        }
        return status;
    }

    m_aclRuleTables[acl_rule.acl_table_name][acl_rule.acl_rule_key] = acl_rule;
    return ReturnCode();
}

} // namespace p4orch
