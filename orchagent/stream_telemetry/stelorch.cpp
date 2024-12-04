#include "stelorch.h"
#include "stelutils.h"

#include <swss/schema.h>
#include <swss/redisutility.h>
#include <swss/stringutility.h>
#include <swss/tokenize.h>
#include <saihelper.h>
#include <notifier.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>

using namespace std;
using namespace swss;

#define CONSTANTS_FILE "/et/sonic/constants.yml"

const unordered_map<string, sai_object_type_t> STelOrch::SUPPORT_COUNTER_TABLES = {
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

        if (buffer == "enable")
        {
            stage = SAI_TAM_TEL_TYPE_STATE_START_STREAM;
        }
        else if (buffer == "disable")
        {
            stage = SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
        }
        else
        {
            SWSS_LOG_THROW("Invalid stream state %s", buffer.c_str());
        }
    }

}

STelOrch::STelOrch(
    DBConnector *cfg_db,
    DBConnector *state_db,
    const vector<string> &tables)
    : Orch(cfg_db, tables),
      m_state_telemetry_session(state_db, STREAM_TELEMETRY_SESSION),
      m_asic_db("ASIC_DB", 0),
      m_sai_hostif_obj(SAI_NULL_OBJECT_ID),
      m_sai_hostif_trap_group_obj(SAI_NULL_OBJECT_ID),
      m_sai_hostif_user_defined_trap_obj(SAI_NULL_OBJECT_ID),
      m_sai_hostif_table_entry_obj(SAI_NULL_OBJECT_ID),
      m_sai_tam_transport_obj(SAI_NULL_OBJECT_ID),
      m_sai_tam_collector_obj(SAI_NULL_OBJECT_ID)
{
    SWSS_LOG_ENTER();

    createNetlinkChannel("stel", "ipfix");
    createTAM();

    m_asic_notification_consumer = make_shared<NotificationConsumer>(&m_asic_db, "NOTIFICATIONS");
    auto notifier = new Notifier(m_asic_notification_consumer.get(), this, "TAM_TEL_TYPE_STATE");
    Orch::addExecutor(notifier);
}

STelOrch::~STelOrch()
{
    SWSS_LOG_ENTER();

    m_name_profile_mapping.clear();
    m_type_profile_mapping.clear();

    deleteTAM();
    deleteNetlinkChannel();
}

void STelOrch::locallyNotify(const CounterNameMapUpdater::Message &msg)
{
    SWSS_LOG_ENTER();

    auto itr = STelOrch::SUPPORT_COUNTER_TABLES.find(msg.m_table_name);
    if (itr == STelOrch::SUPPORT_COUNTER_TABLES.end())
    {
        SWSS_LOG_WARN("The counter table %s is not supported by stream telemetry", msg.m_table_name);
        return;
    }

    // Update the local cache
    if (msg.m_operation == CounterNameMapUpdater::SET)
    {
        m_counter_name_cache[itr->second][msg.m_set.m_counter_name] = msg.m_set.m_oid;
    }
    else if (msg.m_operation == CounterNameMapUpdater::DEL)
    {
        m_counter_name_cache[itr->second].erase(msg.m_del.m_counter_name);
    }

    // Update the profile
    auto itr2 = m_type_profile_mapping.find(itr->second);
    if (itr2 == m_type_profile_mapping.end())
    {
        return;
    }
    for (auto itr3 = itr2->second.begin(); itr3 != itr2->second.end(); itr3++)
    {
        auto profile = *itr3;
        const char *counter_name = msg.m_operation == CounterNameMapUpdater::SET ? msg.m_set.m_counter_name : msg.m_del.m_counter_name;

        if (!profile->canBeUpdated(itr->second))
        {
            // TODO: Here is a potential issue, we may need to retry the task.
            SWSS_LOG_WARN("The profile %s is not ready to be updated, but the object %s want to be updated", profile->getProfileName().c_str(), counter_name);
            continue;
        }

        if (msg.m_operation == CounterNameMapUpdater::SET)
        {
            profile->setObjectSAIID(itr->second, counter_name, msg.m_set.m_oid);
        }
        else if (msg.m_operation == CounterNameMapUpdater::DEL)
        {
            profile->delObjectSAIID(itr->second, counter_name);
        }
        else
        {
            SWSS_LOG_THROW("Unknown operation type %d", msg.m_operation);
        }
    }
}

task_process_status STelOrch::profileTableSet(const string &profile_name, const vector<FieldValueTuple> &values)
{
    SWSS_LOG_ENTER();
    auto profile = getProfile(profile_name);

    if (!profile->canBeUpdated())
    {
        return task_process_status::task_need_retry;
    }

    auto value_opt = fvsGetValue(values, "stream_state", true);
    if (value_opt)
    {
        sai_tam_tel_type_state_t state;
        lexical_convert(*value_opt, state);
        profile->setStreamState(state);
    }

    value_opt = fvsGetValue(values, "poll_interval", true);
    if (value_opt)
    {
        uint32_t poll_interval;
        lexical_convert(*value_opt, poll_interval);
        profile->setPollInterval(poll_interval);
    }

    // Map the profile to types
    // This profile may be inserted by group table
    for (auto type : profile->getObjectTypes())
    {
        m_type_profile_mapping[type].insert(profile);
        profile->tryCommitConfig(type);
    }

    return task_process_status::task_success;
}

task_process_status STelOrch::profileTableDel(const std::string &profile_name)
{
    SWSS_LOG_ENTER();

    auto itr = m_name_profile_mapping.find(profile_name);
    if (itr == m_name_profile_mapping.end())
    {
        return task_process_status::task_success;
    }

    if (!itr->second->canBeUpdated())
    {
        return task_process_status::task_need_retry;
    }

    auto profile = itr->second;
    for (auto type : profile->getObjectTypes())
    {
        profile->tryCommitConfig(type);
        m_type_profile_mapping[type].erase(profile);
        m_state_telemetry_session.del(profile_name + "|" + STelUtils::sai_type_to_group_name(type));
    }
    m_name_profile_mapping.erase(itr);

    return task_process_status::task_success;
}

task_process_status STelOrch::groupTableSet(const std::string &profile_name, const std::string &group_name, const std::vector<swss::FieldValueTuple> &values)
{
    SWSS_LOG_ENTER();

    auto profile = getProfile(profile_name);
    auto type = STelUtils::group_name_to_sai_type(group_name);

    if (!profile->canBeUpdated(type))
    {
        return task_process_status::task_need_retry;
    }

    auto value_opt = fvsGetValue(values, "object_names", true);
    if (value_opt)
    {
        vector<string> buffer;
        boost::split(buffer, *value_opt, boost::is_any_of(","));
        set<string> object_names(buffer.begin(), buffer.end());
        profile->setObjectNames(group_name, move(object_names));
    }

    value_opt = fvsGetValue(values, "object_counters", true);
    if (value_opt)
    {
        vector<string> buffer;
        boost::split(buffer, *value_opt, boost::is_any_of(","));
        set<string> object_counters(buffer.begin(), buffer.end());
        profile->setStatsIDs(group_name, object_counters);
    }

    profile->tryCommitConfig(type);

    m_type_profile_mapping[type].insert(profile);

    return task_process_status::task_success;
}

task_process_status STelOrch::groupTableDel(const std::string &profile_name, const std::string &group_name)
{
    SWSS_LOG_ENTER();

    auto profile = getProfile(profile_name);
    auto type = STelUtils::group_name_to_sai_type(group_name);

    if (!profile->canBeUpdated(type))
    {
        return task_process_status::task_need_retry;
    }

    profile->setObjectSAIID(type, group_name.c_str(), SAI_NULL_OBJECT_ID);
    profile->tryCommitConfig(type);

    m_type_profile_mapping[type].erase(profile);
    m_state_telemetry_session.del(profile_name + "|" + group_name);

    return task_process_status::task_success;
}

shared_ptr<STelProfile> STelOrch::getProfile(const string &profile_name)
{
    SWSS_LOG_ENTER();

    if (m_name_profile_mapping.find(profile_name) == m_name_profile_mapping.end())
    {
        m_name_profile_mapping.emplace(
            profile_name,
            make_shared<STelProfile>(
                profile_name,
                m_sai_tam_obj,
                m_sai_tam_collector_obj,
                m_counter_name_cache));
    }

    return m_name_profile_mapping.at(profile_name);
}

void STelOrch::doTask(swss::NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();

    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);

    if (&consumer != m_asic_notification_consumer.get())
    {
        SWSS_LOG_ERROR("Unknown consumer");
        return;
    }

    if (op != SAI_SWITCH_NOTIFICATION_NAME_TAM_TEL_TYPE_CONFIG_CHANGE)
    {
        SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        return;
    }

    sai_object_id_t tam_tel_type_obj = SAI_NULL_OBJECT_ID;

    // sai_deserialize_tam_tel_type_config_ntf(data, tam_tel_type_obj);

    if (tam_tel_type_obj == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The TAM tel type object is not valid");
        return;
    }

    for (auto &profile : m_name_profile_mapping)
    {
        auto type = profile.second->getObjectType(tam_tel_type_obj);
        if (type != SAI_OBJECT_TYPE_NULL)
        {
            profile.second->notifyConfigReady(type);
            // TODO: TryCommitConfig once the template has been applied by CounterSyncd in the phase2
            profile.second->tryCommitConfig(type);
            // Update state db
            vector<FieldValueTuple> values;
            auto state = profile.second->getTelemetryTypeState(type);
            if (state == SAI_TAM_TEL_TYPE_STATE_START_STREAM)
            {
                values.emplace_back("stream_status", "enable");
            }
            else if (state == SAI_TAM_TEL_TYPE_STATE_STOP_STREAM)
            {
                values.emplace_back("stream_status", "disable");
            }
            else
            {
                SWSS_LOG_THROW("Unexpected state %d", state);
            }

            values.emplace_back("session_type", "ipfix");

            auto templates = profile.second->getTemplates(type);
            values.emplace_back("session_config", string(templates.begin(), templates.end()));

            m_state_telemetry_session.set(profile.first + "|" + STelUtils::sai_type_to_group_name(type), values);
            break;
        }
    }
}

void STelOrch::doTask(Consumer &consumer)
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

        if (table_name == CFG_STREAM_TELEMETRY_PROFILE_TABLE_NAME)
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
        else if (table_name == CFG_STREAM_TELEMETRY_GROUP_TABLE_NAME)
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

void STelOrch::createNetlinkChannel(const string &genl_family, const string &genl_group)
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

    // Create hostif trap group object
    sai_hostif_api->create_hostif_trap_group(&m_sai_hostif_trap_group_obj, gSwitchId, 0, nullptr);

    // Create hostif user defined trap object
    attrs.clear();

    attr.id = SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TYPE;
    attr.value.s32 = SAI_HOSTIF_USER_DEFINED_TRAP_TYPE_TAM;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TRAP_GROUP;
    attr.value.oid = m_sai_hostif_trap_group_obj;
    attrs.push_back(attr);

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

void STelOrch::deleteNetlinkChannel()
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

void STelOrch::createTAM()
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
        SAI_TAM_BIND_POINT_TYPE_PORT,
        SAI_TAM_BIND_POINT_TYPE_QUEUE,
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

    STELUTILS_ADD_SAI_OBJECT_LIST(
        gSwitchId,
        SAI_SWITCH_ATTR_TAM_OBJECT_ID,
        m_sai_tam_obj,
        SAI_API_SWITCH,
        switch,
        switch);
}

void STelOrch::deleteTAM()
{
    SWSS_LOG_ENTER();

    if (m_sai_tam_obj != SAI_NULL_OBJECT_ID)
    {
        // Unbind the TAM object from switch
        STELUTILS_DEL_SAI_OBJECT_LIST(
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
