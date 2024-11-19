#include "stelorch.h"

#include <swss/schema.h>
#include <swss/redisutility.h>
#include <swss/stringutility.h>
#include <swss/tokenize.h>
#include <saihelper.h>

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
    inline void lexical_convert(const string &buffer, ::STelProfile::STREAM_STATE &stage)
    {
        SWSS_LOG_ENTER();

        if (buffer == "enable")
        {
            stage = STelProfile::STREAM_STATE::STREAM_STATE_ENABLED;
        }
        else if (buffer == "disable")
        {
            stage = STelProfile::STREAM_STATE::STREAM_STATE_DISABLED;
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
      m_cfg_stream_telemetry_group(state_db, CFG_STREAM_TELEMETRY_GROUP_TABLE_NAME),
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
}

STelOrch::~STelOrch()
{
    SWSS_LOG_ENTER();

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
        if (msg.m_operation == CounterNameMapUpdater::SET)
        {
            profile->setObjectSAIID(itr->second, msg.m_set.m_counter_name, msg.m_set.m_oid);
        }
        else if (msg.m_operation == CounterNameMapUpdater::DEL)
        {
            profile->delObjectSAIID(itr->second, msg.m_del.m_counter_name);
        }
        else
        {
            SWSS_LOG_THROW("Unknown operation type %d", msg.m_operation);
        }
    }
}

void STelOrch::profileTableSet(const string &profile_name, const vector<FieldValueTuple> &values)
{
    SWSS_LOG_ENTER();
    auto profile = getProfile(profile_name);

    auto value_opt = fvsGetValue(values, "stream_state", true);
    if (value_opt)
    {
        STelProfile::STREAM_STATE state;
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

    value_opt = fvsGetValue(values, "bulk_size", true);
    if (value_opt)
    {
        uint32_t bulk_size;
        lexical_convert(*value_opt, bulk_size);
        profile->setBulkSize(bulk_size);
    }

    // Map the profile to types
    // This profile may be inserted by group table
    for (auto type : profile->getObjectTypes())
    {
        m_type_profile_mapping[type].insert(profile);
    }
}

void STelOrch::profileTableDel(const std::string &profile_name)
{
    SWSS_LOG_ENTER();

    auto itr = m_name_profile_mapping.find(profile_name);
    if (itr == m_name_profile_mapping.end())
    {
        return;
    }

    auto profile = itr->second;
    for (auto type : profile->getObjectTypes())
    {
        m_type_profile_mapping[type].erase(profile);
    }
    m_name_profile_mapping.erase(itr);
}

void STelOrch::groupTableSet(const std::string &profile_name, const std::string &group_name, const std::vector<swss::FieldValueTuple> &values)
{
    SWSS_LOG_ENTER();

    auto profile = getProfile(profile_name);
    auto type = STelProfile::group_name_to_sai_type(group_name);


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

    m_type_profile_mapping[type].insert(profile);
}

void STelOrch::groupTableDel(const std::string &profile_name, const std::string &group_name)
{
    SWSS_LOG_ENTER();

    auto profile = getProfile(profile_name);
    auto type = STelProfile::group_name_to_sai_type(group_name);
    m_type_profile_mapping[type].erase(profile);
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

void STelOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    auto itr = consumer.m_toSync.begin();
    while (itr != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = itr->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        if (table_name == CFG_STREAM_TELEMETRY_PROFILE_TABLE_NAME)
        {
            if (op == SET_COMMAND)
            {
                profileTableSet(key, kfvFieldsValues(t));
            }
            else if (op == DEL_COMMAND)
            {
                profileTableDel(key);
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
                groupTableSet(tokens[0], tokens[1], kfvFieldsValues(t));
            }
            else if (op == DEL_COMMAND)
            {
                groupTableDel(tokens[0], tokens[1]);
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

        itr = consumer.m_toSync.erase(itr);
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
    vector<sai_object_id_t> tam_objects(1024, SAI_NULL_OBJECT_ID);
    attrs.clear();
    attr.id = SAI_SWITCH_ATTR_TAM_OBJECT_ID;
    attr.value.objlist.count = static_cast<uint32_t>(tam_objects.size());
    attr.value.objlist.list = tam_objects.data();
    handleSaiGetStatus(
        SAI_API_SWITCH,
        sai_switch_api->get_switch_attribute(
            gSwitchId,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()));
    tam_objects[attr.value.objlist.count] = m_sai_tam_obj;
    attr.value.objlist.count++;
    handleSaiSetStatus(
        SAI_API_SWITCH,
        sai_switch_api->set_switch_attribute(
            gSwitchId,
            &attr));
}

void STelOrch::deleteTAM()
{
    SWSS_LOG_ENTER();

    if (m_sai_tam_obj != SAI_NULL_OBJECT_ID)
    {
        // Unbind the TAM object from switch
        vector<sai_object_id_t> tam_objects(1024, SAI_NULL_OBJECT_ID);
        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_TAM_OBJECT_ID;
        attr.value.objlist.count = static_cast<uint32_t>(tam_objects.size());
        attr.value.objlist.list = tam_objects.data();
        handleSaiGetStatus(
            SAI_API_SWITCH,
            sai_switch_api->get_switch_attribute(
                gSwitchId,
                1,
                &attr));
        tam_objects.erase(
            remove(
                tam_objects.begin(),
                tam_objects.begin() + attr.value.objlist.count,
                m_sai_tam_obj),
            tam_objects.end());
        handleSaiSetStatus(
            SAI_API_SWITCH,
            sai_switch_api->set_switch_attribute(
                gSwitchId,
                &attr));
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
