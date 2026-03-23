#include "hftelorch.h"
#include "hftelutils.h"

#include "notifications.h"

#include <swss/schema.h>
#include <swss/redisutility.h>
#include <swss/stringutility.h>
#include <swss/tokenize.h>
#include <saihelper.h>
#include <notifier.h>

#include <yaml-cpp/yaml.h>
#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/lexical_cast.hpp>

#include <algorithm>

using namespace std;
using namespace swss;

#define CONSTANTS_FILE "/et/sonic/constants.yml"

const unordered_map<string, sai_object_type_t> HFTelOrch::SUPPORT_COUNTER_TABLES = {
    {COUNTERS_PORT_NAME_MAP, SAI_OBJECT_TYPE_PORT},
    {COUNTERS_BUFFER_POOL_NAME_MAP, SAI_OBJECT_TYPE_BUFFER_POOL},
    {COUNTERS_QUEUE_NAME_MAP, SAI_OBJECT_TYPE_QUEUE},
    {COUNTERS_PG_NAME_MAP, SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP},
};

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;
extern sai_hostif_api_t *sai_hostif_api;
extern sai_tam_api_t *sai_tam_api;

namespace swss
{

    template <>
    inline void lexical_convert(const string &buffer, sai_tam_tel_type_state_t &stage)
    {
        SWSS_LOG_ENTER();

        if (buffer == "enabled")
        {
            stage = SAI_TAM_TEL_TYPE_STATE_START_STREAM;
        }
        else if (buffer == "disabled")
        {
            stage = SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
        }
        else
        {
            SWSS_LOG_THROW("Invalid stream state %s for high frequency telemetry", buffer.c_str());
        }
    }

}

HFTelOrch::HFTelOrch(
    DBConnector *cfg_db,
    DBConnector *state_db,
    const vector<string> &tables)
    : Orch(cfg_db, tables),
      m_state_telemetry_session(state_db, STATE_HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE_NAME),
      m_asic_db("ASIC_DB", 0),
      m_sai_hostif_obj(SAI_NULL_OBJECT_ID),
      m_sai_hostif_trap_group_obj(SAI_NULL_OBJECT_ID),
      m_sai_hostif_user_defined_trap_obj(SAI_NULL_OBJECT_ID),
      m_sai_hostif_table_entry_obj(SAI_NULL_OBJECT_ID),
      m_sai_tam_transport_obj(SAI_NULL_OBJECT_ID),
      m_sai_tam_collector_obj(SAI_NULL_OBJECT_ID),
      m_sai_tam_obj(SAI_NULL_OBJECT_ID)
{
    SWSS_LOG_ENTER();

    createNetlinkChannel("sonic_stel", "ipfix");
    createTAM();

    m_asic_notification_consumer = make_shared<NotificationConsumer>(&m_asic_db, "NOTIFICATIONS");
    auto notifier = new Notifier(m_asic_notification_consumer.get(), this, "TAM_TEL_TYPE_STATE");
    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_TAM_TEL_TYPE_CONFIG_CHANGE_NOTIFY;
    attr.value.ptr = (void *)on_tam_tel_type_config_change;
    if (sai_switch_api->set_switch_attribute(gSwitchId, &attr) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set SAI_SWITCH_ATTR_TAM_TEL_TYPE_CONFIG_CHANGE_NOTIFY");
        throw runtime_error("HFTelOrch initialization failure (failed to set tam tel type config change notify)");
    }

    Orch::addExecutor(notifier);
}

HFTelOrch::~HFTelOrch()
{
    SWSS_LOG_ENTER();

    m_name_profile_mapping.clear();
    m_type_profile_mapping.clear();

    deleteTAM();
    deleteNetlinkChannel();
}

void HFTelOrch::locallyNotify(const CounterNameMapUpdater::Message &msg)
{
    SWSS_LOG_ENTER();

    auto counter_itr = HFTelOrch::SUPPORT_COUNTER_TABLES.find(msg.m_table_name);
    if (counter_itr == HFTelOrch::SUPPORT_COUNTER_TABLES.end())
    {
        SWSS_LOG_WARN("The counter table %s is not supported by high frequency telemetry", msg.m_table_name);
        return;
    }

    SWSS_LOG_NOTICE("The counter table %s is updated, operation %d, object %s",
                    msg.m_table_name,
                    msg.m_operation,
                    msg.m_operation == CounterNameMapUpdater::SET ? msg.m_set.m_counter_name : msg.m_del.m_counter_name);

    // Update the local cache
    if (msg.m_operation == CounterNameMapUpdater::SET)
    {
        m_counter_name_cache[counter_itr->second][msg.m_set.m_counter_name] = msg.m_set.m_oid;
    }
    else if (msg.m_operation == CounterNameMapUpdater::DEL)
    {
        m_counter_name_cache[counter_itr->second].erase(msg.m_del.m_counter_name);
    }

    // Update the profile
    auto type_itr = m_type_profile_mapping.find(counter_itr->second);
    if (type_itr == m_type_profile_mapping.end())
    {
        return;
    }
    for (auto profile_itr = type_itr->second.begin(); profile_itr != type_itr->second.end(); profile_itr++)
    {
        auto profile = *profile_itr;
        const char *counter_name = msg.m_operation == CounterNameMapUpdater::SET ? msg.m_set.m_counter_name : msg.m_del.m_counter_name;

        if (!profile->canBeUpdated(counter_itr->second))
        {
            // TODO: Here is a potential issue, we might need to retry the task.
            // Because the Syncd is generating the configuration(template),
            // we cannot update the monitor objects at this time.
            SWSS_LOG_WARN("The high frequency telemetry profile %s is not ready to be updated, but the object %s want to be updated", profile->getProfileName().c_str(), counter_name);
            continue;
        }

        if (msg.m_operation == CounterNameMapUpdater::SET)
        {
            profile->setObjectSAIID(counter_itr->second, counter_name, msg.m_set.m_oid);
        }
        else if (msg.m_operation == CounterNameMapUpdater::DEL)
        {
            profile->delObjectSAIID(counter_itr->second, counter_name);
        }
        else
        {
            SWSS_LOG_THROW("Unknown operation type %d", msg.m_operation);
        }
        profile->tryCommitConfig(counter_itr->second);
    }
}

bool HFTelOrch::isSupportedHFTel(sai_object_id_t switch_id)
{
    SWSS_LOG_ENTER();

    sai_stat_st_capability_list_t stats_st_capability;
    stats_st_capability.count = 0;
    stats_st_capability.list = nullptr;
    sai_status_t status = sai_query_stats_st_capability(switch_id, SAI_OBJECT_TYPE_PORT, &stats_st_capability);

    return status == SAI_STATUS_SUCCESS || status == SAI_STATUS_BUFFER_OVERFLOW;
}

task_process_status HFTelOrch::profileTableSet(const string &profile_name, const vector<FieldValueTuple> &values)
{
    SWSS_LOG_ENTER();
    auto profile = getProfile(profile_name);

    if (!profile->canBeUpdated())
    {
        return task_process_status::task_need_retry;
    }

    auto value_opt = fvsGetValue(values, "stream_state", true);
    string stream_state = "disable";
    sai_tam_tel_type_state_t state = SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
    if (value_opt)
    {
        lexical_convert(*value_opt, state);
        profile->setStreamState(state);
        stream_state = *value_opt;

        // If the telemetry session state entry already exists, keep its stream_status in sync.
        // The entry is created/updated in doTask(NotificationConsumer&) when TAM notifies config readiness.
        // Here we only update existing entries to avoid creating partial/incomplete state entries.
        const auto desired_stream_status = (state == SAI_TAM_TEL_TYPE_STATE_START_STREAM) ? "enabled" : "disabled";
        for (const auto &type_profiles : m_type_profile_mapping)
        {
            const auto &type = type_profiles.first;
            if (type_profiles.second.find(profile) == type_profiles.second.end())
            {
                continue;
            }

            const auto session_key = profile_name + "|" + HFTelUtils::sai_type_to_group_name(type);
            vector<FieldValueTuple> existing_values;
            if (!m_state_telemetry_session.get(session_key, existing_values))
            {
                continue;
            }

            vector<FieldValueTuple> update_values;
            update_values.emplace_back("stream_status", desired_stream_status);
            m_state_telemetry_session.set(session_key, update_values);
        }
    }

    value_opt = fvsGetValue(values, "poll_interval", true);
    uint32_t poll_interval = 0;
    if (value_opt)
    {
        lexical_convert(*value_opt, poll_interval);
        profile->setPollInterval(poll_interval);
    }

    SWSS_LOG_NOTICE("The high frequency telemetry profile %s is set (stream_state: %s, poll_interval: %u)",
                    profile_name.c_str(),
                    state == SAI_TAM_TEL_TYPE_STATE_START_STREAM ? "enabled" : "disabled",
                    poll_interval);

    return task_process_status::task_success;
}

task_process_status HFTelOrch::profileTableDel(const std::string &profile_name)
{
    SWSS_LOG_ENTER();

    auto profile_itr = m_name_profile_mapping.find(profile_name);
    if (profile_itr == m_name_profile_mapping.end())
    {
        return task_process_status::task_success;
    }

    if (!profile_itr->second->canBeUpdated())
    {
        return task_process_status::task_need_retry;
    }

    if (!profile_itr->second->isEmpty() || isProfileInUse(profile_itr->second))
    {
        return task_process_status::task_need_retry;
    }

    m_name_profile_mapping.erase(profile_itr);

    SWSS_LOG_NOTICE("The high frequency telemetry profile %s is deleted", profile_name.c_str());

    return task_process_status::task_success;
}

task_process_status HFTelOrch::groupTableSet(const std::string &profile_name, const std::string &group_name, const std::vector<swss::FieldValueTuple> &values)
{
    SWSS_LOG_ENTER();

    auto profile = tryGetProfile(profile_name);
    if (!profile)
    {
        return task_process_status::task_need_retry;
    }

    auto type = HFTelUtils::group_name_to_sai_type(group_name);

    if (!profile->canBeUpdated(type))
    {
        return task_process_status::task_need_retry;
    }

    auto arg_object_names = fvsGetValue(values, "object_names", true);
    if (arg_object_names && !arg_object_names->empty())
    {
        vector<string> buffer;
        boost::split(buffer, *arg_object_names, boost::is_any_of(","));
        set<string> object_names(buffer.begin(), buffer.end());
        profile->setObjectNames(group_name, move(object_names));
    }

    auto arg_object_counters = fvsGetValue(values, "object_counters", true);
    if (arg_object_counters && !arg_object_counters->empty())
    {
        vector<string> buffer;
        boost::split(buffer, *arg_object_counters, boost::is_any_of(","));
        set<string> object_counters(buffer.begin(), buffer.end());
        profile->setStatsIDs(group_name, object_counters);
    }

    if (profile->getStreamState(type) != SAI_TAM_TEL_TYPE_STATE_STOP_STREAM)
    {
        SWSS_LOG_WARN("The high frequency telemetry group %s:%s is not in the stop stream state, it means no new configuration needs to be applied",
                    profile_name.c_str(),
                    group_name.c_str());
        return task_process_status::task_success;
    }

    profile->tryCommitConfig(type);

    m_type_profile_mapping[type].insert(profile);

    SWSS_LOG_NOTICE("The high frequency telemetry group %s with profile %s is set (object_names: %s, object_counters: %s)",
                    group_name.c_str(),
                    profile_name.c_str(),
                    arg_object_names ? arg_object_names->c_str() : "",
                    arg_object_counters ? arg_object_counters->c_str() : "");

    return task_process_status::task_success;
}

task_process_status HFTelOrch::groupTableDel(const std::string &profile_name, const std::string &group_name)
{
    SWSS_LOG_ENTER();

    auto profile = tryGetProfile(profile_name);

    if (!profile)
    {
        SWSS_LOG_WARN("The high frequency telemetry profile %s is not found", profile_name.c_str());
        return task_process_status::task_success;
    }

    auto type = HFTelUtils::group_name_to_sai_type(group_name);

    if (!profile->canBeUpdated(type))
    {
        return task_process_status::task_need_retry;
    }

    profile->clearGroup(group_name);
    m_type_profile_mapping[type].erase(profile);
    m_state_telemetry_session.del(profile_name + "|" + HFTelUtils::sai_type_to_group_name(type));

    SWSS_LOG_NOTICE("The high frequency telemetry group %s with profile %s is deleted", group_name.c_str(), profile_name.c_str());

    return task_process_status::task_success;
}

shared_ptr<HFTelProfile> HFTelOrch::getProfile(const string &profile_name)
{
    SWSS_LOG_ENTER();

    if (m_name_profile_mapping.find(profile_name) == m_name_profile_mapping.end())
    {
        m_name_profile_mapping.emplace(
            profile_name,
            make_shared<HFTelProfile>(
                profile_name,
                m_sai_tam_obj,
                m_sai_tam_collector_obj,
                m_counter_name_cache));
    }

    return m_name_profile_mapping.at(profile_name);
}

std::shared_ptr<HFTelProfile> HFTelOrch::tryGetProfile(const std::string &profile_name)
{
    SWSS_LOG_ENTER();

    auto itr = m_name_profile_mapping.find(profile_name);
    if (itr != m_name_profile_mapping.end())
    {
        return itr->second;
    }

    return std::shared_ptr<HFTelProfile>();
}

bool HFTelOrch::isProfileInUse(const std::shared_ptr<HFTelProfile> &profile) const
{
    SWSS_LOG_ENTER();

    for (const auto &type_profiles : m_type_profile_mapping)
    {
        if (type_profiles.second.find(profile) != type_profiles.second.end())
        {
            return true;
        }
    }

    return false;
}

void HFTelOrch::doTask(swss::NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();

    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    if (&consumer != m_asic_notification_consumer.get())
    {
        SWSS_LOG_DEBUG("Is not TAM notification");
        return;
    }

    consumer.pop(op, data, values);

    if (op != SAI_SWITCH_NOTIFICATION_NAME_TAM_TEL_TYPE_CONFIG_CHANGE)
    {
        SWSS_LOG_DEBUG("Unknown operation type %s for HFTel Orch", op.c_str());
        return;
    }

    sai_object_id_t tam_tel_type_obj = SAI_NULL_OBJECT_ID;

    sai_deserialize_object_id(data, tam_tel_type_obj);

    if (tam_tel_type_obj == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The TAM tel type object is not valid");
        return;
    }

    for (auto &profile : m_name_profile_mapping)
    {
        auto type = profile.second->getObjectType(tam_tel_type_obj);
        if (type == SAI_OBJECT_TYPE_NULL)
        {
            continue;
        }

        // TODO: A potential optimization
        // We need to notify Config Ready only when the message of State DB is delivered to the CounterSyncd
        profile.second->notifyConfigReady(type);

        // Update state db
        vector<FieldValueTuple> values;
        auto state = profile.second->getTelemetryTypeState(type);
        if (state == SAI_TAM_TEL_TYPE_STATE_START_STREAM)
        {
            values.emplace_back("stream_status", "enabled");
        }
        else if (state == SAI_TAM_TEL_TYPE_STATE_STOP_STREAM)
        {
            values.emplace_back("stream_status", "disabled");
        }
        else
        {
            SWSS_LOG_THROW("Unexpected state %d for high frequency telemetry", state);
        }


        values.emplace_back("object_names", boost::algorithm::join(profile.second->getObjectNames(type), ","));
        auto to_string = boost::adaptors::transformed([](sai_uint16_t n)
                                                        { return boost::lexical_cast<std::string>(n); });
        values.emplace_back("object_ids", boost::algorithm::join(profile.second->getObjectLabels(type) | to_string, ","));


        values.emplace_back("session_type", "ipfix");

        auto templates = profile.second->getTemplates(type);
        values.emplace_back("session_config", string(templates.begin(), templates.end()));

        m_state_telemetry_session.set(profile.first + "|" + HFTelUtils::sai_type_to_group_name(type), values);

        SWSS_LOG_NOTICE("The high frequency telemetry group %s with profile %s is ready",
                        HFTelUtils::sai_type_to_group_name(type).c_str(),
                        profile.first.c_str());

        return;
    }

    SWSS_LOG_ERROR("The TAM tel type object %s is not found in the profile", sai_serialize_object_id(tam_tel_type_obj).c_str());
}

void HFTelOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    auto itr = consumer.m_toSync.begin();
    while (itr != consumer.m_toSync.end())
    {
        task_process_status status = task_process_status::task_failed;
        KeyOpFieldsValuesTuple t = itr->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        try
        {
            if (table_name == CFG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE_NAME)
            {
                if (op == SET_COMMAND)
                {
                    status = profileTableSet(key, kfvFieldsValues(t));
                }
                else if (op == DEL_COMMAND)
                {
                    status = profileTableDel(key);
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
                }
            }
            else if (table_name == CFG_HIGH_FREQUENCY_TELEMETRY_GROUP_TABLE_NAME)
            {
                auto tokens = tokenize(key, '|');
                if (tokens.size() != 2)
                {
                    SWSS_LOG_THROW("Invalid key %s in the %s", key.c_str(), table_name.c_str());
                }
                if (op == SET_COMMAND)
                {
                    status = groupTableSet(tokens[0], tokens[1], kfvFieldsValues(t));
                }
                else if (op == DEL_COMMAND)
                {
                    status = groupTableDel(tokens[0], tokens[1]);
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
                }
            }
            else
            {
                SWSS_LOG_ERROR("Unknown table %s\n", table_name.c_str());
            }
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_ERROR("Failed to process the task for table %s, key %s, operation %s: %s",
                           table_name.c_str(),
                           key.c_str(),
                           op.c_str(),
                           e.what());
            status = task_process_status::task_failed;
        }

        if (status == task_process_status::task_need_retry)
        {
            ++itr;
        }
        else
        {
            itr = consumer.m_toSync.erase(itr);
        }
    }
}

void HFTelOrch::createNetlinkChannel(const string &genl_family, const string &genl_group)
{
    SWSS_LOG_ENTER();

    // Delete the existing netlink channel
    deleteNetlinkChannel();

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    // Create hostif object
    attr.id = SAI_HOSTIF_ATTR_TYPE;
    attr.value.s32 = SAI_HOSTIF_TYPE_GENETLINK;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_ATTR_OPER_STATUS;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_ATTR_NAME;
    strncpy(attr.value.chardata, genl_family.c_str(), sizeof(attr.value.chardata));
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_ATTR_GENETLINK_MCGRP_NAME;
    strncpy(attr.value.chardata, genl_group.c_str(), sizeof(attr.value.chardata));
    attrs.push_back(attr);

    sai_hostif_api->create_hostif(&m_sai_hostif_obj, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());

    // // Create hostif trap group object
    // sai_hostif_api->create_hostif_trap_group(&m_sai_hostif_trap_group_obj, gSwitchId, 0, nullptr);

    // Create hostif user defined trap object
    attrs.clear();

    attr.id = SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TYPE;
    attr.value.s32 = SAI_HOSTIF_USER_DEFINED_TRAP_TYPE_TAM;
    attrs.push_back(attr);

    // attr.id = SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TRAP_GROUP;
    // attr.value.oid = m_sai_hostif_trap_group_obj;
    // attrs.push_back(attr);

    sai_hostif_api->create_hostif_user_defined_trap(&m_sai_hostif_user_defined_trap_obj, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());

    // Create hostif table entry object
    attrs.clear();

    attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_TYPE;
    attr.value.s32 = SAI_HOSTIF_TABLE_ENTRY_TYPE_TRAP_ID;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_TRAP_ID;
    attr.value.oid = m_sai_hostif_user_defined_trap_obj;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_CHANNEL_TYPE;
    attr.value.s32 = SAI_HOSTIF_TABLE_ENTRY_CHANNEL_TYPE_GENETLINK;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_TABLE_ENTRY_ATTR_HOST_IF;
    attr.value.oid = m_sai_hostif_obj;
    attrs.push_back(attr);

    sai_hostif_api->create_hostif_table_entry(&m_sai_hostif_table_entry_obj, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());
}

void HFTelOrch::deleteNetlinkChannel()
{
    SWSS_LOG_ENTER();

    if (m_sai_hostif_table_entry_obj != SAI_NULL_OBJECT_ID)
    {
        sai_hostif_api->remove_hostif_table_entry(m_sai_hostif_table_entry_obj);
        m_sai_hostif_table_entry_obj = SAI_NULL_OBJECT_ID;
    }
    if (m_sai_hostif_user_defined_trap_obj != SAI_NULL_OBJECT_ID)
    {
        sai_hostif_api->remove_hostif_user_defined_trap(m_sai_hostif_user_defined_trap_obj);
        m_sai_hostif_user_defined_trap_obj = SAI_NULL_OBJECT_ID;
    }
    if (m_sai_hostif_trap_group_obj != SAI_NULL_OBJECT_ID)
    {
        sai_hostif_api->remove_hostif_trap_group(m_sai_hostif_trap_group_obj);
        m_sai_hostif_trap_group_obj = SAI_NULL_OBJECT_ID;
    }
    if (m_sai_hostif_obj != SAI_NULL_OBJECT_ID)
    {
        sai_hostif_api->remove_hostif(m_sai_hostif_obj);
        m_sai_hostif_obj = SAI_NULL_OBJECT_ID;
    }
}

void HFTelOrch::createTAM()
{
    SWSS_LOG_ENTER();

    // Delete the existing TAM
    deleteTAM();

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    // Create TAM transport object
    attr.id = SAI_TAM_TRANSPORT_ATTR_TRANSPORT_TYPE;
    attr.value.s32 = SAI_TAM_TRANSPORT_TYPE_NONE;
    attrs.push_back(attr);

    handleSaiCreateStatus(
        SAI_API_TAM,
        sai_tam_api->create_tam_transport(
            &m_sai_tam_transport_obj,
            gSwitchId,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()));

    // Create TAM collector object
    attrs.clear();

    attr.id = SAI_TAM_COLLECTOR_ATTR_SRC_IP;
    attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    attr.value.ipaddr.addr.ip4 = 0;
    attrs.push_back(attr);

    attr.id = SAI_TAM_COLLECTOR_ATTR_DST_IP;
    attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    attr.value.ipaddr.addr.ip4 = 0;
    attrs.push_back(attr);

    attr.id = SAI_TAM_COLLECTOR_ATTR_TRANSPORT;
    attr.value.oid = m_sai_tam_transport_obj;
    attrs.push_back(attr);

    attr.id = SAI_TAM_COLLECTOR_ATTR_LOCALHOST;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_TAM_COLLECTOR_ATTR_HOSTIF_TRAP;
    attr.value.oid = m_sai_hostif_user_defined_trap_obj;
    attrs.push_back(attr);

    attr.id = SAI_TAM_COLLECTOR_ATTR_DSCP_VALUE;
    attr.value.u8 = 0;
    attrs.push_back(attr);

    handleSaiCreateStatus(
        SAI_API_TAM,
        sai_tam_api->create_tam_collector(
            &m_sai_tam_collector_obj,
            gSwitchId,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()));

    // Create TAM object
    attrs.clear();
    attr.id = SAI_TAM_ATTR_TAM_BIND_POINT_TYPE_LIST;
    vector<sai_int32_t> bind_point_types = {
        SAI_TAM_BIND_POINT_TYPE_SWITCH,
    };
    attr.value.s32list.count = static_cast<uint32_t>(bind_point_types.size());
    attr.value.s32list.list = bind_point_types.data();
    attrs.push_back(attr);

    handleSaiCreateStatus(
        SAI_API_TAM,
        sai_tam_api->create_tam(
            &m_sai_tam_obj,
            gSwitchId,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()));

    // Bind the TAM object to switch
    // FIX: There is a bug for config reload
    // WARNING #syncd: :- logViewObjectCount: object count for SAI_OBJECT_TYPE_TAM on current view 1 is different than on temporary view: 2
    // WARNING #syncd: :- performObjectSetTransition: Present current attr SAI_TAM_ATTR_TAM_BIND_POINT_TYPE_LIST:1:SAI_TAM_BIND_POINT_TYPE_SWITCH has default that CAN'T be set to 0:null since it's CREATE_ONLY
    attr.id = SAI_SWITCH_ATTR_TAM_OBJECT_ID;
    vector<sai_object_id_t> obj_list = {m_sai_tam_obj};
    attr.value.objlist.count = static_cast<uint32_t>(obj_list.size());
    attr.value.objlist.list = obj_list.data();
    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set SAI_SWITCH_ATTR_TAM_OBJECT_ID, status: %s",
                       sai_serialize_status(status).c_str());
        throw runtime_error("HFTelOrch initialization failure (failed to set tam object id)");
    }
}

void HFTelOrch::deleteTAM()
{
    SWSS_LOG_ENTER();

    if (m_sai_tam_obj != SAI_NULL_OBJECT_ID)
    {
        // Unbind the TAM object from switch
        HFTELUTILS_DEL_SAI_OBJECT_LIST(
            gSwitchId,
            SAI_SWITCH_ATTR_TAM_OBJECT_ID,
            m_sai_tam_obj,
            SAI_API_SWITCH,
            switch,
            switch);
        handleSaiRemoveStatus(
            SAI_API_TAM,
            sai_tam_api->remove_tam(m_sai_tam_obj));
        m_sai_tam_obj = SAI_NULL_OBJECT_ID;
    }
    if (m_sai_tam_collector_obj != SAI_NULL_OBJECT_ID)
    {
        handleSaiRemoveStatus(
            SAI_API_TAM,
            sai_tam_api->remove_tam_collector(m_sai_tam_collector_obj));
        m_sai_tam_collector_obj = SAI_NULL_OBJECT_ID;
    }
    if (m_sai_tam_transport_obj != SAI_NULL_OBJECT_ID)
    {
        handleSaiRemoveStatus(
            SAI_API_TAM,
            sai_tam_api->remove_tam_transport(m_sai_tam_transport_obj));
        m_sai_tam_transport_obj = SAI_NULL_OBJECT_ID;
    }
}
