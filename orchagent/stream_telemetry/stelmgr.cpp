#include "stelmgr.h"

#include <swss/logger.h>
#include <swss/redisutility.h>
#include <saihelper.h>
#include <sai_serialize.h>

#include <boost/tokenizer.hpp>
#include <boost/algorithm/string.hpp>
#include <regex>

using namespace std;
using namespace swss;

extern sai_object_id_t gSwitchId;
extern sai_tam_api_t *sai_tam_api;

static set<sai_stat_id_t> object_counters_to_stats_ids(
    const string &group_name,
    const set<string> &object_counters)
{
    SWSS_LOG_ENTER();
    sai_object_type_t sai_object_type = STelProfile::group_name_to_sai_type(group_name);
    set<sai_stat_id_t> stats_ids_set;

    auto info = sai_metadata_get_object_type_info(sai_object_type);
    if (info == nullptr)
    {
        SWSS_LOG_THROW("Failed to get the object type info for %s", group_name.c_str());
    }

    auto state_enum = info->statenum;
    if (state_enum == nullptr)
    {
        SWSS_LOG_THROW("The object type %s does not support stats", group_name.c_str());
    }

    string type_prefix = "SAI_" + group_name + "_STAT_";

    for (size_t i = 0; i < state_enum->valuescount; i++)
    {
        string state_name = type_prefix + state_enum->valuesnames[i];
        if (object_counters.find(state_name) != object_counters.end())
        {
            SWSS_LOG_DEBUG("Found the object counter %s", state_name.c_str());
            stats_ids_set.insert(state_enum->values[i]);
        }
    }

    if (stats_ids_set.size() != object_counters.size())
    {
        SWSS_LOG_THROW("Failed to convert the object counters to stats ids for %s", group_name.c_str());
    }

    return stats_ids_set;
}

// static uint16_t get_sai_label(const string &object_name)
// {
//     SWSS_LOG_ENTER();
//     uint16_t label = 0;

//     if (object_name.rfind("Ethernet", 0) == 0)
//     {
//         const static regex re("Ethernet(\\d+)(?:\\|(\\d+))?");
//         smatch match;
//         if (regex_match(object_name, match, re))
//         {
//             label = static_cast<uint16_t>(stoi(match[1]));
//             if (match.size() == 3)
//             {
//                 label = static_cast<uint16_t>(label * 100 + stoi(match[2]));
//             }
//         }
//     }
//     else
//     {
//         SWSS_LOG_THROW("The object %s is not supported", object_name.c_str());
//     }

//     return label;
// }

// static int32_t get_stats_mode(sai_object_type_t object_type, sai_stat_id_t stat_id)
// {
//     SWSS_LOG_ENTER();

//     switch(object_type)
//     {
//         case SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP:
//             switch(stat_id)
//             {
//                 case SAI_INGRESS_PRIORITY_GROUP_STAT_WATERMARK_BYTES:
//                 case SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES:
//                 case SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES:
//                     return SAI_STATS_MODE_READ_AND_CLEAR;
//                 default:
//                     break;
//             }
//         case SAI_OBJECT_TYPE_BUFFER_POOL:
//             switch(stat_id)
//             {
//                 case SAI_BUFFER_POOL_STAT_WATERMARK_BYTES:
//                 case SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES:
//                     return SAI_STATS_MODE_READ_AND_CLEAR;
//                 default:
//                     break;
//             }
//         default:
//             break;
//     }

//     return SAI_STATS_MODE_READ;
// }

STelProfile::STelProfile(
    const std::string &profile_name,
    sai_object_id_t sai_tam_obj,
    sai_object_id_t sai_tam_collector_obj,
    const CounterNameCache &cache)
    : m_profile_name(profile_name),
      m_setting_state(STREAM_STATE_DISABLED),
      m_poll_interval(0),
      m_bulk_size(0),
      m_counter_name_cache(cache),
      m_state(STREAM_STATE_DISABLED),
      m_object_count(0),
      m_needed_to_be_deployed(false),
      m_sai_tam_obj(sai_tam_obj),
      m_sai_tam_collector_obj(sai_tam_collector_obj),
      m_sai_tam_report_obj(SAI_NULL_OBJECT_ID),
      m_sai_tam_tel_type_obj(SAI_NULL_OBJECT_ID),
      m_sai_tam_telemetry_obj(SAI_NULL_OBJECT_ID)
{
    SWSS_LOG_ENTER();

    if (m_sai_tam_obj == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_THROW("The SAI TAM object is not valid");
    }
    if (m_sai_tam_collector_obj == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_THROW("The SAI TAM collector object is not valid");
    }

    deployTAM();
}

STelProfile::~STelProfile()
{
    SWSS_LOG_ENTER();

    undeployTAM();
}

void STelProfile::setStreamState(STREAM_STATE state)
{
    SWSS_LOG_ENTER();

    if (state == m_setting_state)
    {
        return;
    }
    m_setting_state = state;
    m_needed_to_be_deployed = true;
}

void STelProfile::setPollInterval(uint32_t poll_interval)
{
    SWSS_LOG_ENTER();

    if (poll_interval == m_poll_interval)
    {
        return;
    }
    m_poll_interval = poll_interval;
    m_needed_to_be_deployed = true;
}

void STelProfile::setBulkSize(uint32_t bulk_size)
{
    SWSS_LOG_ENTER();

    if (bulk_size == m_bulk_size)
    {
        return;
    }
    m_bulk_size = bulk_size;
    m_needed_to_be_deployed = true;
}

void STelProfile::setObjectNames(const string &group_name, set<string> &&object_names)
{
    SWSS_LOG_ENTER();

    sai_object_type_t sai_object_type = group_name_to_sai_type(group_name);
    auto itr = m_groups.lower_bound(sai_object_type);

    if (itr == m_groups.end() || itr->first != sai_object_type)
    {
        m_groups.insert(itr, {sai_object_type, StelGroup{move(object_names), {}}});
        m_object_count += object_names.size();
    }
    else
    {
        if (itr->second.m_object_names == object_names)
        {
            return;
        }
        for (const auto &name : itr->second.m_object_names)
        {
            delObjectSAIID(sai_object_type, name.c_str());
        }
        m_object_count -= itr->second.m_object_names.size();
        itr->second.m_object_names = move(object_names);
        m_object_count += itr->second.m_object_names.size();
    }
    loadCounterNameCache(sai_object_type);
    m_needed_to_be_deployed = true;
}

void STelProfile::setStatsIDs(const string &group_name, const set<string> &object_counters)
{
    SWSS_LOG_ENTER();

    sai_object_type_t sai_object_type = group_name_to_sai_type(group_name);
    auto itr = m_groups.lower_bound(sai_object_type);
    set<sai_stat_id_t> stats_ids_set = object_counters_to_stats_ids(group_name, object_counters);

    if (itr == m_groups.end() || itr->first != sai_object_type)
    {
        m_groups.insert(itr, {sai_object_type, StelGroup{{}, stats_ids_set}});
    }
    else
    {
        if (itr->second.m_stats_ids == stats_ids_set)
        {
            return;
        }
        itr->second.m_stats_ids = move(stats_ids_set);
    }
    m_needed_to_be_deployed = true;
}

void STelProfile::setObjectSAIID(sai_object_type_t object_type, const char *object_name, sai_object_id_t object_id)
{
    SWSS_LOG_ENTER();

    if (!isObjectInProfile(object_type, object_name))
    {
        return;
    }

    auto &objs = m_name_sai_map[object_type];
    auto itr = objs.find(object_name);
    if (itr != objs.end())
    {
        if (itr->second == object_id)
        {
            return;
        }
    }
    objs[object_name] = object_id;

    m_needed_to_be_deployed = true;
}

void STelProfile::delObjectSAIID(sai_object_type_t object_type, const char *object_name)
{
    SWSS_LOG_ENTER();

    if (!isObjectInProfile(object_type, object_name))
    {
        return;
    }

    auto &objs = m_name_sai_map[object_type];
    auto itr = objs.find(object_name);
    if (itr == objs.end())
    {
        return;
    }
    objs.erase(itr);
    if (objs.empty())
    {
        m_name_sai_map.erase(object_type);
    }
    if (m_state == STREAM_STATE_ENABLED)
    {
        // TODO: Disable the stream, delete the subscription counter
    }
}

std::vector<sai_object_type_t> STelProfile::getObjectTypes() const
{
    std::vector<sai_object_type_t> types;

    for (const auto &group : m_groups)
    {
        types.push_back(group.first);
    }

    return types;
}

void STelProfile::loadGroupFromCfgDB(Table &group_tbl)
{
    SWSS_LOG_ENTER();

    vector<string> keys;
    group_tbl.getKeys(keys);

    boost::char_separator<char> key_sep(group_tbl.getTableNameSeparator().c_str());
    for (const auto &key : keys)
    {
        boost::tokenizer<boost::char_separator<char>> tokens(key, key_sep);

        auto profile_name = (tokens.begin());
        if (profile_name == tokens.end())
        {
            SWSS_LOG_THROW("Invalid key %s in the %s", key.c_str(), group_tbl.getTableName().c_str());
        }
        if (*profile_name != m_profile_name)
        {
            // Not the profile we are interested in
            continue;
        }

        auto group_name = ++tokens.begin();
        if (group_name == tokens.end())
        {
            SWSS_LOG_THROW("Invalid key %s in the %s", key.c_str(), group_tbl.getTableName().c_str());
        }

        vector<FieldValueTuple> items;
        if (!group_tbl.get(key, items))
        {
            SWSS_LOG_WARN("Failed to get the stream telemetry group: %s.", key.c_str());
            continue;
        }

        auto names = fvsGetValue(items, "object_names", true);
        if (!names || names->empty())
        {
            // TODO: If the object names are empty, implicitly select all objects of the group
            SWSS_LOG_WARN("No object names in the stream telemetry group: %s", key.c_str());
            continue;
        }

        auto counters = fvsGetValue(items, "object_counters", true);
        if (!counters || counters->empty())
        {
            SWSS_LOG_ERROR("No object counters in the stream telemetry group: %s", key.c_str());
            continue;
        }

        vector<string> buffer;
        boost::split(buffer, *names, boost::is_any_of(","));
        set<string> object_names(buffer.begin(), buffer.end());
        setObjectNames(*group_name, move(object_names));

        buffer.clear();
        boost::split(buffer, *counters, boost::is_any_of(","));
        set<string> object_counters(buffer.begin(), buffer.end());
        setStatsIDs(*group_name, object_counters);
    }
}

void STelProfile::loadCounterNameCache(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    auto itr = m_counter_name_cache.find(object_type);
    if (itr == m_counter_name_cache.end())
    {
        return;
    }
    auto group = m_groups.find(object_type);
    if (group == m_groups.end())
    {
        return;
    }
    const auto &sai_objs = itr->second;
    for (const auto &name : group->second.m_object_names)
    {
        auto obj = sai_objs.find(name);
        if (obj != sai_objs.end())
        {
            setObjectSAIID(object_type, name.c_str(), obj->second);
        }
    }
}

void STelProfile::tryCommitConfig()
{
    SWSS_LOG_ENTER();

    m_needed_to_be_deployed &= (m_setting_state == STREAM_STATE_ENABLED);
    m_needed_to_be_deployed &= (m_object_count != 0);
    m_needed_to_be_deployed &= (m_object_count == m_name_sai_map.size());

    if (m_needed_to_be_deployed)
    {
        if (m_state == STREAM_STATE_ENABLED)
        {
            undeployFromSAI();
        }
        deployToSAI();
    }

    m_needed_to_be_deployed = false;
}

sai_object_type_t STelProfile::group_name_to_sai_type(const string &group_name)
{
    SWSS_LOG_ENTER();

    sai_object_type_t sai_object_type;

    sai_deserialize_object_type(string("SAI_OBJECT_TYPE_") + group_name, sai_object_type);
    return sai_object_type;
}

bool STelProfile::isObjectInProfile(sai_object_type_t object_type, const std::string &object_name) const
{
    SWSS_LOG_ENTER();

    auto group = m_groups.find(object_type);
    if (group == m_groups.end())
    {
        return false;
    }
    if (group->second.m_object_names.find(object_name) == group->second.m_object_names.end())
    {
        return false;
    }

    return false;
}

void STelProfile::deployToSAI()
{
    SWSS_LOG_ENTER();

    m_state = STREAM_STATE_ENABLED;
}

void STelProfile::undeployFromSAI()
{
    SWSS_LOG_ENTER();

    m_state = STREAM_STATE_DISABLED;
}

void STelProfile::deployTAM()
{
    SWSS_LOG_ENTER();

    // Delete the existing TAM objects
    undeployTAM();

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    // Create TAM report object
    attr.id = SAI_TAM_REPORT_ATTR_TYPE;
    attr.value.s32 = SAI_TAM_REPORT_TYPE_IPFIX;
    attrs.push_back(attr);

    attr.id = SAI_TAM_REPORT_ATTR_REPORT_MODE;
    attr.value.s32 = SAI_TAM_REPORT_MODE_BULK;
    attrs.push_back(attr);

    attr.id = SAI_TAM_REPORT_ATTR_TEMPLATE_REPORT_INTERVAL;
    attr.value.u32 = 0; // Don't push the template, Because we hope the template can be proactively queried by orchagent
    attrs.push_back(attr);

    attr.id = SAI_TAM_REPORT_ATTR_REPORT_INTERVAL_UNIT;
    attr.value.s32 = SAI_TAM_REPORT_INTERVAL_UNIT_USEC;

    handleSaiCreateStatus(
        SAI_API_TAM,
        sai_tam_api->create_tam_report(
            &m_sai_tam_report_obj,
            gSwitchId,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()));

    // Create TAM telemetry type object
    attrs.clear();

    attr.id = SAI_TAM_TEL_TYPE_ATTR_TAM_TELEMETRY_TYPE;
    attr.value.s32 = SAI_TAM_TELEMETRY_TYPE_COUNTER_SUBSCRIPTION;
    attrs.push_back(attr);

    attr.id = SAI_TAM_TEL_TYPE_ATTR_SWITCH_ENABLE_PORT_STATS;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_TAM_TEL_TYPE_ATTR_SWITCH_ENABLE_PORT_STATS_INGRESS;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_TAM_TEL_TYPE_ATTR_SWITCH_ENABLE_PORT_STATS_EGRESS;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_TAM_TEL_TYPE_ATTR_SWITCH_ENABLE_MMU_STATS;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_TAM_TEL_TYPE_ATTR_REPORT_ID;
    attr.value.oid = m_sai_tam_report_obj;
    attrs.push_back(attr);

    handleSaiCreateStatus(
        SAI_API_TAM,
        sai_tam_api->create_tam_tel_type(
            &m_sai_tam_tel_type_obj,
            gSwitchId,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()));

    // Create TAM telemetry object
    attrs.clear();

    sai_object_id_t sai_object = m_sai_tam_tel_type_obj;
    attr.id = SAI_TAM_TELEMETRY_ATTR_TAM_TYPE_LIST;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = &sai_object;
    attrs.push_back(attr);

    sai_object = m_sai_tam_collector_obj;
    attr.id = SAI_TAM_TELEMETRY_ATTR_COLLECTOR_LIST;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = &sai_object;
    attrs.push_back(attr);

    // TODO: Update SAI tam telemetry
    // attr.id = SAI_TAM_TELEMETRY_ATTR_REPORTING_TYPE;
    // attr.value.s32 = SAI_TAM_REPORTING_TYPE_COUNT_BASED;
    // attrs.push_back(attr);

    handleSaiCreateStatus(
        SAI_API_TAM,
        sai_tam_api->create_tam_telemetry(
            &m_sai_tam_telemetry_obj,
            gSwitchId, static_cast<uint32_t>(attrs.size()),
            attrs.data()));
}

void STelProfile::undeployTAM()
{
    SWSS_LOG_ENTER();

    if (m_sai_tam_telemetry_obj != SAI_NULL_OBJECT_ID)
    {
        handleSaiRemoveStatus(
            SAI_API_TAM,
            sai_tam_api->remove_tam_telemetry(m_sai_tam_telemetry_obj));
        m_sai_tam_telemetry_obj = SAI_NULL_OBJECT_ID;
    }

    if (m_sai_tam_tel_type_obj != SAI_NULL_OBJECT_ID)
    {
        handleSaiRemoveStatus(
            SAI_API_TAM,
            sai_tam_api->remove_tam_tel_type(m_sai_tam_tel_type_obj));
        m_sai_tam_tel_type_obj = SAI_NULL_OBJECT_ID;
    }

    if (m_sai_tam_report_obj != SAI_NULL_OBJECT_ID)
    {
        handleSaiRemoveStatus(
            SAI_API_TAM,
            sai_tam_api->remove_tam_report(m_sai_tam_report_obj));
        m_sai_tam_report_obj = SAI_NULL_OBJECT_ID;
    }
}

void STelProfile::deployCounterSubscription(sai_object_type_t object_type, sai_object_id_t sai_obj, sai_stat_id_t stat_id, uint16_t label)
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    auto itr = m_sai_tam_counter_subscription_objs.find(sai_obj);
    if (itr != m_sai_tam_counter_subscription_objs.end())
    {
        auto itr2 = itr->second.find(stat_id);
        if (itr2 != itr->second.end())
        {
            return;
        }
    }

    assert(m_sai_tam_tel_type_obj != SAI_NULL_OBJECT_ID);

    attr.id = SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_TEL_TYPE;
    attr.value.oid = m_sai_tam_tel_type_obj;
    attrs.push_back(attr);

    attr.id = SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_OBJECT_ID;
    attr.value.oid = stat_id;
    attrs.push_back(attr);

    attr.id = SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_STAT_ID;
    attr.value.oid = stat_id;
    attrs.push_back(attr);

    attr.id = SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_LABEL;
    attr.value.u64 = static_cast<uint64_t>(label);
    attrs.push_back(attr);

    // TODO: Update SAI counter subscription
    // attr.id = SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_STATS_MODE;
    // attr.value.s32 = get_stats_mode(object_type, stat_id);
    // attrs.push_back(attr);

    sai_object_id_t counter_id;

    handleSaiCreateStatus(
        SAI_API_TAM,
        sai_tam_api->create_tam_counter_subscription(
            &counter_id,
            gSwitchId,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()));

    m_sai_tam_counter_subscription_objs[sai_obj][stat_id] = move(
        unique_ptr<
            sai_object_id_t,
            function<void(sai_object_id_t *)>>(
            new sai_object_id_t(counter_id),
            [](sai_object_id_t *p)
            {
                handleSaiRemoveStatus(
                    SAI_API_TAM,
                    sai_tam_api->remove_tam_counter_subscription(*p));
                delete p;
            }));
}
