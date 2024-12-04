#include "stelmgr.h"
#include "stelutils.h"

#include <swss/logger.h>
#include <swss/redisutility.h>
#include <saihelper.h>
#include <sai_serialize.h>

#include <boost/tokenizer.hpp>
#include <boost/algorithm/string.hpp>

using namespace std;
using namespace swss;

extern sai_object_id_t gSwitchId;
extern sai_tam_api_t *sai_tam_api;

STelProfile::STelProfile(
    const string &profile_name,
    sai_object_id_t sai_tam_obj,
    sai_object_id_t sai_tam_collector_obj,
    const CounterNameCache &cache)
    : m_profile_name(profile_name),
      m_setting_state(SAI_TAM_TEL_TYPE_STATE_STOP_STREAM),
      m_poll_interval(0),
      m_counter_name_cache(cache),
      m_sai_tam_obj(sai_tam_obj),
      m_sai_tam_collector_obj(sai_tam_collector_obj)
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

    initTelemetry();
}

STelProfile::~STelProfile()
{
    SWSS_LOG_ENTER();
}

const string &STelProfile::getProfileName() const
{
    SWSS_LOG_ENTER();

    return m_profile_name;
}

void STelProfile::setStreamState(sai_tam_tel_type_state_t state)
{
    SWSS_LOG_ENTER();
    m_setting_state = state;

    for (const auto &item : m_sai_tam_tel_type_objs)
    {
        setStreamState(item.first, state);
    }
}

void STelProfile::setStreamState(sai_object_type_t type, sai_tam_tel_type_state_t state)
{
    SWSS_LOG_ENTER();

    auto type_itr = m_sai_tam_tel_type_objs.find(type);
    if (type_itr == m_sai_tam_tel_type_objs.end())
    {
        return;
    }

    auto stats = m_sai_tam_tel_type_states.find(type_itr->second);
    if (stats == m_sai_tam_tel_type_states.end())
    {
        return;
    }

    if (stats->second == state)
    {
        return;
    }

    if (stats->second == SAI_TAM_TEL_TYPE_STATE_STOP_STREAM)
    {
        if (state == SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG)
        {
            if (!isMonitoringObjectReady(type))
            {
                return;
            }
            // Clearup the previous templates
            m_sai_tam_tel_type_templates.erase(type);
        }
        else if (state == SAI_TAM_TEL_TYPE_STATE_START_STREAM)
        {
            if (m_sai_tam_tel_type_templates.find(type) == m_sai_tam_tel_type_templates.end())
            {
                // The template isn't ready
                return;
            }
            if (!isMonitoringObjectReady(type))
            {
                return;
            }
        }
        else
        {
            goto failed_state_transfer;
        }
    }
    else if (stats->second == SAI_TAM_TEL_TYPE_STATE_START_STREAM)
    {
        if (state == SAI_TAM_TEL_TYPE_STATE_STOP_STREAM)
        {
            // Nothing to do
        }
        else if (state == SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG)
        {
            // TODO: Implement the transition from started to config generating in Phase2
            SWSS_LOG_THROW("Transfer from start to create config hasn't been implemented yet");
        }
        else
        {
            goto failed_state_transfer;
        }
    }
    else if (stats->second == SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG)
    {
        if (state == SAI_TAM_TEL_TYPE_STATE_STOP_STREAM)
        {
            // Nothing to do
        }
        else if (state == SAI_TAM_TEL_TYPE_STATE_START_STREAM)
        {
            // Nothing to do
        }
        else
        {
            goto failed_state_transfer;
        }
    }
    else
    {
        SWSS_LOG_THROW("Unknown state %d", stats->second);
    }

    sai_attribute_t attr;
    // TODO: Update SAI
    // attr.id = SAI_TAM_TEL_TYPE_ATTR_STATE;
    attr.value.s32 = state;
    handleSaiSetStatus(
        SAI_API_TAM,
        sai_tam_api->set_tam_tel_type_attribute(*type_itr->second, &attr));

    stats->second = state;

    return;

failed_state_transfer:
    SWSS_LOG_THROW("Invalid state transfer from %d to %d", stats->second, state);
}

void STelProfile::notifyConfigReady(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    auto itr = m_sai_tam_tel_type_objs.find(object_type);
    if (itr == m_sai_tam_tel_type_objs.end())
    {
        return;
    }

    updateTemplates(*itr->second);
    setStreamState(object_type, m_setting_state);
}

sai_tam_tel_type_state_t STelProfile::getTelemetryTypeState(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();

    auto itr = m_sai_tam_tel_type_objs.find(object_type);
    return m_sai_tam_tel_type_states.at(itr->second);
}

STelProfile::sai_guard_t STelProfile::getTAMTelTypeGuard(sai_object_id_t tam_tel_type_obj) const
{
    SWSS_LOG_ENTER();

    for (const auto &item : m_sai_tam_tel_type_objs)
    {
        if (*item.second == tam_tel_type_obj)
        {
            return item.second;
        }
    }

    return sai_guard_t();
}

sai_object_type_t STelProfile::getObjectType(sai_object_id_t tam_tel_type_obj) const
{
    SWSS_LOG_ENTER();

    auto guard = getTAMTelTypeGuard(tam_tel_type_obj);
    if (guard)
    {
        for (const auto &item : m_sai_tam_tel_type_objs)
        {
            if (item.second == guard)
            {
                return item.first;
            }
        }
    }

    return SAI_OBJECT_TYPE_NULL;
}

void STelProfile::setPollInterval(uint32_t poll_interval)
{
    SWSS_LOG_ENTER();

    if (poll_interval == m_poll_interval)
    {
        return;
    }
    m_poll_interval = poll_interval;

    for (const auto &report : m_sai_tam_report_objs)
    {
        sai_attribute_t attr;
        attr.id = SAI_TAM_REPORT_ATTR_REPORT_INTERVAL;
        attr.value.u32 = m_poll_interval;
        handleSaiSetStatus(
            SAI_API_TAM,
            sai_tam_api->set_tam_report_attribute(*report.second, &attr));
    }
}

void STelProfile::setObjectNames(const string &group_name, set<string> &&object_names)
{
    SWSS_LOG_ENTER();

    sai_object_type_t sai_object_type = STelUtils::group_name_to_sai_type(group_name);
    auto itr = m_groups.lower_bound(sai_object_type);

    if (itr == m_groups.end() || itr->first != sai_object_type)
    {
        m_groups.insert(itr, {sai_object_type, STelGroup{move(object_names), {}}});
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
        itr->second.m_object_names = move(object_names);
    }
    loadCounterNameCache(sai_object_type);

    // TODO: In the phase 2, we don't need to stop the stream before update the object names
    setStreamState(sai_object_type, SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);

    undeployCounterSubscriptions(sai_object_type);
}

void STelProfile::setStatsIDs(const string &group_name, const set<string> &object_counters)
{
    SWSS_LOG_ENTER();

    sai_object_type_t sai_object_type = STelUtils::group_name_to_sai_type(group_name);
    auto itr = m_groups.lower_bound(sai_object_type);
    set<sai_stat_id_t> stats_ids_set = STelUtils::object_counters_to_stats_ids(group_name, object_counters);

    if (itr == m_groups.end() || itr->first != sai_object_type)
    {
        m_groups.insert(itr, {sai_object_type, STelGroup{{}, stats_ids_set}});
    }
    else
    {
        if (itr->second.m_stats_ids == stats_ids_set)
        {
            return;
        }
        itr->second.m_stats_ids = move(stats_ids_set);
    }

    // TODO: In the phase 2, we don't need to stop the stream before update the stats
    setStreamState(sai_object_type, SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);

    undeployCounterSubscriptions(sai_object_type);
}

void STelProfile::setObjectSAIID(sai_object_type_t object_type, const char *object_name, sai_object_id_t object_id)
{
    SWSS_LOG_ENTER();

    if (!isObjectTypeInProfile(object_type, object_name))
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

    // TODO: In the phase 2, we don't need to stop the stream before update the object
    setStreamState(object_type, SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);

    // Update the counter subscription
    deployCounterSubscriptions(object_type, object_id, STelUtils::get_sai_label(object_name));
}

void STelProfile::delObjectSAIID(sai_object_type_t object_type, const char *object_name)
{
    SWSS_LOG_ENTER();

    if (!isObjectTypeInProfile(object_type, object_name))
    {
        return;
    }

    auto &objs = m_name_sai_map[object_type];
    auto itr = objs.find(object_name);
    if (itr == objs.end())
    {
        return;
    }

    // TODO: In the phase 2, we don't need to stop the stream before removing the object
    setStreamState(object_type, SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);

    // Remove all counters bounded to the object
    auto counter_itr = m_sai_tam_counter_subscription_objs.find(object_type);
    if (counter_itr != m_sai_tam_counter_subscription_objs.end())
    {
        counter_itr->second.erase(itr->second);
        if (counter_itr->second.empty())
        {
            m_sai_tam_counter_subscription_objs.erase(counter_itr);
        }
    }

    objs.erase(itr);
    if (objs.empty())
    {
        m_name_sai_map.erase(object_type);
    }
}

bool STelProfile::canBeUpdated() const
{
    SWSS_LOG_ENTER();

    for (const auto &group : m_groups)
    {
        if (!canBeUpdated(group.first))
        {
            return false;
        }
    }

    return true;
}

bool STelProfile::canBeUpdated(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();

    if (getTelemetryTypeState(object_type) == SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG)
    {
        return false;
    }

    return true;
}

const vector<uint8_t> &STelProfile::getTemplates(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();

    return m_sai_tam_tel_type_templates.at(object_type);
}

vector<sai_object_type_t> STelProfile::getObjectTypes() const
{
    vector<sai_object_type_t> types;
    types.reserve(m_groups.size());

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

void STelProfile::tryCommitConfig(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    if (!canBeUpdated(object_type))
    {
        return;
    }

    if (m_setting_state == SAI_TAM_TEL_TYPE_STATE_START_STREAM)
    {
        auto group = m_groups.find(object_type);
        if (group == m_groups.end())
        {
            return;
        }
        if (group->second.m_object_names.empty())
        {
            // TODO: If the object names are empty, implicitly select all objects of the group
            return;
        }
        if (!isMonitoringObjectReady(object_type))
        {
            deployCounterSubscriptions(object_type);
        }
        setStreamState(object_type, SAI_TAM_TEL_TYPE_STATE_START_STREAM);
    }
    else if (m_setting_state == SAI_TAM_TEL_TYPE_STATE_STOP_STREAM)
    {
        setStreamState(object_type, SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);
        undeployCounterSubscriptions(object_type);
    }
    else
    {
        SWSS_LOG_THROW("Cannot commit the configuration in the state %d", m_setting_state);
    }
}

bool STelProfile::isObjectTypeInProfile(sai_object_type_t object_type, const string &object_name) const
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

bool STelProfile::isMonitoringObjectReady(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();

    auto group = m_groups.find(object_type);
    if (group == m_groups.end())
    {

        SWSS_LOG_THROW("The group for object type %s is not found", sai_serialize_object_type(object_type).c_str());
    }

    auto counters = m_sai_tam_counter_subscription_objs.find(object_type);

    if (counters == m_sai_tam_counter_subscription_objs.end() || group->second.m_object_names.size() != counters->second.size())
    {
        // The monitoring counters are not ready
        return false;
    }

    return true;
}

sai_object_id_t STelProfile::getTAMReportObjID(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    auto itr = m_sai_tam_report_objs.find(object_type);
    if (itr != m_sai_tam_report_objs.end())
    {
        return *itr->second;
    }

    sai_object_id_t sai_object;
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

    if (m_poll_interval != 0)
    {
        attr.id = SAI_TAM_REPORT_ATTR_REPORT_INTERVAL;
        attr.value.u32 = m_poll_interval;
        attrs.push_back(attr);
    }

    attr.id = SAI_TAM_REPORT_ATTR_REPORT_INTERVAL_UNIT;
    attr.value.s32 = SAI_TAM_REPORT_INTERVAL_UNIT_USEC;

    handleSaiCreateStatus(
        SAI_API_TAM,
        sai_tam_api->create_tam_report(
            &sai_object,
            gSwitchId,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()));

    m_sai_tam_report_objs[object_type] = make_unique<sai_object_id_t>(sai_object);

    return sai_object;
}

sai_object_id_t STelProfile::getTAMTelTypeObjID(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    auto itr = m_sai_tam_tel_type_objs.find(object_type);
    if (itr != m_sai_tam_tel_type_objs.end())
    {
        return *itr->second;
    }
    return sai_object_id_t();

    sai_object_id_t sai_object;
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    // Create TAM telemetry type object

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

    // attr.id = SAI_TAM_TEL_TYPE_ATTR_MODE ;
    // attr.value.boolean = SAI_TAM_TEL_TYPE_MODE_SINGLE_TYPE;
    // attrs.push_back(attr);

    attr.id = SAI_TAM_TEL_TYPE_ATTR_REPORT_ID;
    attr.value.oid = getTAMReportObjID(object_type);
    attrs.push_back(attr);

    handleSaiCreateStatus(
        SAI_API_TAM,
        sai_tam_api->create_tam_tel_type(
            &sai_object,
            gSwitchId,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()));

    m_sai_tam_tel_type_objs[object_type] = move(
        sai_guard_t(
            new sai_object_id_t(sai_object),
            [this](sai_object_id_t *p)
            {
                STELUTILS_DEL_SAI_OBJECT_LIST(
                    *this->m_sai_tam_telemetry_obj,
                    SAI_TAM_TELEMETRY_ATTR_TAM_TYPE_LIST,
                    *p,
                    SAI_API_TAM,
                    tam,
                    tam_telemetry);

                handleSaiRemoveStatus(
                    SAI_API_TAM,
                    sai_tam_api->remove_tam_tel_type(*p));
                delete p;
            }));
    m_sai_tam_tel_type_states[m_sai_tam_tel_type_objs[object_type]] = SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;

    STELUTILS_ADD_SAI_OBJECT_LIST(
        *m_sai_tam_telemetry_obj,
        SAI_TAM_TELEMETRY_ATTR_TAM_TYPE_LIST,
        sai_object,
        SAI_API_TAM,
        tam,
        tam_telemetry);

    return sai_object;
}

void STelProfile::initTelemetry()
{
    SWSS_LOG_ENTER();

    sai_object_id_t sai_object;
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    // Create TAM telemetry object
    sai_object = m_sai_tam_collector_obj;
    attr.id = SAI_TAM_TELEMETRY_ATTR_COLLECTOR_LIST;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = &sai_object;
    attrs.push_back(attr);

    handleSaiCreateStatus(
        SAI_API_TAM,
        sai_tam_api->create_tam_telemetry(
            &sai_object,
            gSwitchId, static_cast<uint32_t>(attrs.size()),
            attrs.data()));

    m_sai_tam_telemetry_obj = move(
        sai_guard_t(
            new sai_object_id_t(sai_object),
            [](sai_object_id_t *p)
            {
                handleSaiRemoveStatus(
                    SAI_API_TAM,
                    sai_tam_api->remove_tam_telemetry(*p));
                delete p;
            }));
}

void STelProfile::deployCounterSubscription(sai_object_type_t object_type, sai_object_id_t sai_obj, sai_stat_id_t stat_id, uint16_t label)
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    auto itr = m_sai_tam_counter_subscription_objs[object_type].find(sai_obj);
    if (itr != m_sai_tam_counter_subscription_objs[object_type].end())
    {
        auto itr2 = itr->second.find(stat_id);
        if (itr2 != itr->second.end())
        {
            return;
        }
    }

    assert(m_sai_tam_tel_type_obj != SAI_NULL_OBJECT_ID);

    attr.id = SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_TEL_TYPE;
    attr.value.oid = getTAMTelTypeObjID(object_type);
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

    m_sai_tam_counter_subscription_objs[object_type][sai_obj][stat_id] = move(
        sai_guard_t(
            new sai_object_id_t(counter_id),
            [](sai_object_id_t *p)
            {
                handleSaiRemoveStatus(
                    SAI_API_TAM,
                    sai_tam_api->remove_tam_counter_subscription(*p));
                delete p;
            }));
}

void STelProfile::deployCounterSubscriptions(sai_object_type_t object_type, sai_object_id_t sai_obj, std::uint16_t label)
{
    SWSS_LOG_ENTER();

    // TODO: Bulk create the counter subscriptions
    auto group = m_groups.find(object_type);
    if (group == m_groups.end())
    {
        return;
    }

    for (const auto &stat_id : group->second.m_stats_ids)
    {
        deployCounterSubscription(object_type, sai_obj, stat_id, label);
    }
}

void STelProfile::deployCounterSubscriptions(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    // TODO: Bulk create the counter subscriptions

    auto group = m_groups.find(object_type);
    if (group == m_groups.end())
    {
        return;
    }
    for (const auto &name : group->second.m_object_names)
    {
        auto itr = m_name_sai_map[object_type].find(name);
        if (itr == m_name_sai_map[object_type].end())
        {
            continue;
        }
        for (const auto &stat_id : group->second.m_stats_ids)
        {
            deployCounterSubscription(object_type, itr->second, stat_id, STelUtils::get_sai_label(name));
        }
    }
}

void STelProfile::undeployCounterSubscriptions(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    // TODO: Bulk remove the counter subscriptions
    m_sai_tam_counter_subscription_objs.erase(object_type);
}

void STelProfile::updateTemplates(sai_object_id_t tam_tel_type_obj)
{
    SWSS_LOG_ENTER();

    auto object_type = getObjectType(tam_tel_type_obj);
    if (object_type == SAI_OBJECT_TYPE_NULL)
    {
        SWSS_LOG_THROW("The object type is not found");
    }

    // Estimate the template size
    auto counters = m_sai_tam_counter_subscription_objs.find(object_type);
    if (counters == m_sai_tam_counter_subscription_objs.end())
    {
        SWSS_LOG_THROW("The counter subscription object is not found");
    }
    size_t counters_count = 0;
    for (const auto &item : counters->second)
    {
        counters_count += item.second.size();
    }
#define COUNTER_SIZE (8LLU)
#define IPFIX_TEMPLATE_MAX_SIZE (0xffffLLU)
#define IPFIX_HEADER_SIZE (16LLU)
#define IPFIX_TEMPLATE_METADATA_SIZE (12LLU)
#define IPFIX_TEMPLATE_MAX_STATS_COUNT (((IPFIX_TEMPLATE_MAX_SIZE - IPFIX_HEADER_SIZE - IPFIX_TEMPLATE_METADATA_SIZE) / COUNTER_SIZE) - 1LLU)
    size_t estimated_template_size = (counters_count / IPFIX_TEMPLATE_MAX_STATS_COUNT + 1) * IPFIX_TEMPLATE_MAX_SIZE;

    vector<uint8_t> buffer(estimated_template_size, 0);

    sai_attribute_t attr;
    // attr.id = SAI_TAM_TEL_TYPE_ATTR_IPFIX_TEMPLATES;
    attr.value.u8list.count = static_cast<uint32_t>(buffer.size());
    attr.value.u8list.list = buffer.data();
    handleSaiGetStatus(
        SAI_API_TAM,
        sai_tam_api->get_tam_tel_type_attribute(tam_tel_type_obj, 1, &attr));

    buffer.resize(attr.value.u8list.count);

    m_sai_tam_tel_type_templates[object_type] = move(buffer);
}
