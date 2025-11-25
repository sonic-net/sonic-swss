#include "hftelprofile.h"
#include "hftelutils.h"
#include "saihelper.h"

#include <swss/logger.h>
#include <swss/redisutility.h>
#include <sai_serialize.h>

#include <boost/tokenizer.hpp>
#include <boost/algorithm/string.hpp>

using namespace std;
using namespace swss;

extern sai_object_id_t gSwitchId;
extern sai_tam_api_t *sai_tam_api;

HFTelProfile::HFTelProfile(
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

HFTelProfile::~HFTelProfile()
{
    SWSS_LOG_ENTER();
}

const string &HFTelProfile::getProfileName() const
{
    SWSS_LOG_ENTER();

    return m_profile_name;
}

void HFTelProfile::setStreamState(sai_tam_tel_type_state_t state)
{
    SWSS_LOG_ENTER();
    m_setting_state = state;

    for (const auto &item : m_sai_tam_tel_type_objs)
    {
        setStreamState(item.first, state);
    }
}

void HFTelProfile::setStreamState(sai_object_type_t type, sai_tam_tel_type_state_t state)
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

    do
    {
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
                break;
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
                break;
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
                break;
            }
        }
        else
        {
            SWSS_LOG_THROW("Unknown state %d", stats->second);
        }

        sai_attribute_t attr;
        attr.id = SAI_TAM_TEL_TYPE_ATTR_STATE;
        attr.value.s32 = state;
        auto status = sai_tam_api->set_tam_tel_type_attribute(*type_itr->second, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            handleSaiSetStatus(SAI_API_TAM, status);
        }

        stats->second = state;
        return;

    } while(false);

    SWSS_LOG_THROW("Invalid state transfer from %d to %d", stats->second, state);
}

sai_tam_tel_type_state_t HFTelProfile::getStreamState(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();
    auto itr = m_sai_tam_tel_type_objs.find(object_type);
    if (itr == m_sai_tam_tel_type_objs.end())
    {
        return SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
    }
    auto state_itr = m_sai_tam_tel_type_states.find(itr->second);
    if (state_itr == m_sai_tam_tel_type_states.end())
    {
        return SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
    }
    return state_itr->second;
}

void HFTelProfile::notifyConfigReady(sai_object_type_t object_type)
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

sai_tam_tel_type_state_t HFTelProfile::getTelemetryTypeState(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();

    auto itr = m_sai_tam_tel_type_objs.find(object_type);
    if (itr == m_sai_tam_tel_type_objs.end())
    {
        return SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
    }
    auto state_itr = m_sai_tam_tel_type_states.find(itr->second);
    if (state_itr == m_sai_tam_tel_type_states.end())
    {
        return SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
    }
    return state_itr->second;
}

HFTelProfile::sai_guard_t HFTelProfile::getTAMTelTypeGuard(sai_object_id_t tam_tel_type_obj) const
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

sai_object_type_t HFTelProfile::getObjectType(sai_object_id_t tam_tel_type_obj) const
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

void HFTelProfile::setPollInterval(uint32_t poll_interval)
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
        sai_status_t status = sai_tam_api->set_tam_report_attribute(*report.second, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            handleSaiSetStatus(SAI_API_TAM, status);
        }
    }
}

void HFTelProfile::setObjectNames(const string &group_name, set<string> &&object_names)
{
    SWSS_LOG_ENTER();

    sai_object_type_t sai_object_type = HFTelUtils::group_name_to_sai_type(group_name);

    auto itr = m_groups.lower_bound(sai_object_type);

    if (itr == m_groups.end() || itr->first != sai_object_type)
    {
        HFTelGroup group(group_name);
        group.updateObjects(object_names);
        m_groups.insert(itr, {sai_object_type, move(group)});
    }
    else
    {
        if (itr->second.isSameObjects(object_names))
        {
            return;
        }
        for (const auto &obj : itr->second.getObjects())
        {
            delObjectSAIID(sai_object_type, obj.first.c_str());
        }
        itr->second.updateObjects(object_names);
    }
    loadCounterNameCache(sai_object_type);

    // TODO: In the phase 2, we don't need to stop the stream before update the object names
    setStreamState(sai_object_type, SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);
}

void HFTelProfile::setStatsIDs(const string &group_name, const set<string> &object_counters)
{
    SWSS_LOG_ENTER();

    sai_object_type_t sai_object_type = HFTelUtils::group_name_to_sai_type(group_name);
    auto itr = m_groups.lower_bound(sai_object_type);
    set<sai_stat_id_t> stats_ids_set = HFTelUtils::object_counters_to_stats_ids(group_name, object_counters);

    if (itr == m_groups.end() || itr->first != sai_object_type)
    {
        HFTelGroup group(group_name);
        group.updateStatsIDs(stats_ids_set);
        m_groups.insert(itr, {sai_object_type, move(group)});
    }
    else
    {
        if (itr->second.getStatsIDs() == stats_ids_set)
        {
            return;
        }
        itr->second.updateStatsIDs(stats_ids_set);
    }

    // TODO: In the phase 2, we don't need to stop the stream before update the stats
    setStreamState(sai_object_type, SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);

    deployCounterSubscriptions(sai_object_type);
}

void HFTelProfile::setObjectSAIID(sai_object_type_t object_type, const char *object_name, sai_object_id_t object_id)
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

    SWSS_LOG_DEBUG("Set object %s with ID %s in the name sai map", object_name, sai_serialize_object_id(object_id).c_str());

    // TODO: In the phase 2, we don't need to stop the stream before update the object
    setStreamState(object_type, SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);

    // Update the counter subscription
    deployCounterSubscriptions(object_type, object_id, m_groups.at(object_type).getObjects().at(object_name));
}

void HFTelProfile::delObjectSAIID(sai_object_type_t object_type, const char *object_name)
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
        SWSS_LOG_DEBUG("Delete object %s from the name sai map", object_name);
    }
}

bool HFTelProfile::canBeUpdated() const
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

bool HFTelProfile::canBeUpdated(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();


    if (getTelemetryTypeState(object_type) == SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG)
    {
        return false;
    }

    return true;
}

bool HFTelProfile::isEmpty() const
{
    SWSS_LOG_ENTER();

    return m_groups.empty();
}

void HFTelProfile::clearGroup(const std::string &group_name)
{
    SWSS_LOG_ENTER();

    sai_object_type_t sai_object_type = HFTelUtils::group_name_to_sai_type(group_name);

    auto itr = m_groups.find(sai_object_type);
    if (itr != m_groups.end())
    {
        for (const auto &obj : itr->second.getObjects())
        {
            delObjectSAIID(sai_object_type, obj.first.c_str());
        }
        m_groups.erase(itr);
    }
    m_sai_tam_tel_type_templates.erase(sai_object_type);
    m_sai_tam_tel_type_states.erase(m_sai_tam_tel_type_objs[sai_object_type]);
    m_sai_tam_tel_type_objs.erase(sai_object_type);
    m_sai_tam_report_objs.erase(sai_object_type);
    m_sai_tam_counter_subscription_objs.erase(sai_object_type);
    m_name_sai_map.erase(sai_object_type);

    SWSS_LOG_NOTICE("Cleared high frequency telemetry group %s with no objects", group_name.c_str());
}

const vector<uint8_t> &HFTelProfile::getTemplates(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();

    return m_sai_tam_tel_type_templates.at(object_type);
}

const vector<string> HFTelProfile::getObjectNames(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();

    vector<string> object_names;
    auto group = m_groups.find(object_type);
    if (group != m_groups.end())
    {
        object_names.resize(group->second.getObjects().size());
        transform(group->second.getObjects().begin(), group->second.getObjects().end(), object_names.begin(),
                  [](const auto &pair)
                  { return pair.first; });
    }

    return object_names;
}

const vector<uint16_t> HFTelProfile::getObjectLabels(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();

    vector<uint16_t> object_labels;
    auto group = m_groups.find(object_type);
    if (group != m_groups.end())
    {
        object_labels.resize(group->second.getObjects().size());
        transform(group->second.getObjects().begin(), group->second.getObjects().end(), object_labels.begin(),
                  [](const auto &pair)
                  { return pair.second; });
    }
    return object_labels;
}

pair<vector<string>, vector<string>> HFTelProfile::getObjectNamesAndLabels(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();

    auto group = m_groups.find(object_type);
    if (group == m_groups.end())
    {
        return {vector<string>(), vector<string>()};
    }

    return group->second.getObjectNamesAndLabels();
}

vector<sai_object_type_t> HFTelProfile::getObjectTypes() const
{
    vector<sai_object_type_t> types;
    types.reserve(m_groups.size());

    for (const auto &group : m_groups)
    {
        types.push_back(group.first);
    }

    return types;
}

void HFTelProfile::loadCounterNameCache(sai_object_type_t object_type)
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
    for (const auto &obj : group->second.getObjects())
    {
        auto sai_obj = sai_objs.find(obj.first);
        if (sai_obj != sai_objs.end())
        {
            setObjectSAIID(object_type, obj.first.c_str(), sai_obj->second);
        }
    }
}

bool HFTelProfile::tryCommitConfig(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    if (!canBeUpdated(object_type))
    {
        return false;
    }

    if (m_setting_state == SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG)
    {
        SWSS_LOG_THROW("Cannot commit the configuration in the state %d", m_setting_state);
    }

    auto group = m_groups.find(object_type);
    if (group == m_groups.end())
    {
        return false;
    }
    if (group->second.getObjects().empty())
    {
        // TODO: If the object names are empty, implicitly select all objects of the group
        return true;
    }
    if (!isMonitoringObjectReady(object_type))
    {
        deployCounterSubscriptions(object_type);
        if (!isMonitoringObjectReady(object_type))
        {
            // There are some objects still not ready
            return false;
        }
    }
    setStreamState(object_type, SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
    return true;
}

bool HFTelProfile::isObjectTypeInProfile(sai_object_type_t object_type, const string &object_name) const
{
    SWSS_LOG_ENTER();

    auto group = m_groups.find(object_type);
    if (group == m_groups.end())
    {
        return false;
    }
    if (!group->second.isObjectInGroup(object_name))
    {
        return false;
    }

    return true;
}

bool HFTelProfile::isMonitoringObjectReady(sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();

    auto group = m_groups.find(object_type);
    if (group == m_groups.end())
    {
        SWSS_LOG_THROW("The high frequency telemetry group for object type %s is not found", sai_serialize_object_type(object_type).c_str());
    }

    auto counters = m_sai_tam_counter_subscription_objs.find(object_type);

    if (counters == m_sai_tam_counter_subscription_objs.end() || group->second.getObjects().size() != counters->second.size())
    {
        // The monitoring counters are not ready
        return false;
    }

    return true;
}

sai_object_id_t HFTelProfile::getTAMReportObjID(sai_object_type_t object_type)
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
    // Don't push the template, Because we hope the template can be proactively queried by orchagent
    attr.value.u32 = 0;
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

    m_sai_tam_report_objs[object_type] = move(
        sai_guard_t(
            new sai_object_id_t(sai_object),
            [this](sai_object_id_t *p)
            {
                handleSaiRemoveStatus(
                    SAI_API_TAM,
                    sai_tam_api->remove_tam_report(*p));
                delete p;
            }));

    return sai_object;
}

sai_object_id_t HFTelProfile::getTAMTelTypeObjID(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    auto itr = m_sai_tam_tel_type_objs.find(object_type);
    if (itr != m_sai_tam_tel_type_objs.end())
    {
        return *itr->second;
    }

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

    attr.id = SAI_TAM_TEL_TYPE_ATTR_SWITCH_ENABLE_OUTPUT_QUEUE_STATS;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_TAM_TEL_TYPE_ATTR_MODE ;
    attr.value.s32 = SAI_TAM_TEL_TYPE_MODE_SINGLE_TYPE;
    attrs.push_back(attr);

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
                HFTELUTILS_DEL_SAI_OBJECT_LIST(
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

    HFTELUTILS_ADD_SAI_OBJECT_LIST(
        *m_sai_tam_telemetry_obj,
        SAI_TAM_TELEMETRY_ATTR_TAM_TYPE_LIST,
        sai_object,
        SAI_API_TAM,
        tam,
        tam_telemetry);

    return sai_object;
}

void HFTelProfile::initTelemetry()
{
    SWSS_LOG_ENTER();

    sai_object_id_t sai_object;
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_object_id_t sai_tam_collector_obj = m_sai_tam_collector_obj;

    // Create TAM telemetry object
    attr.id = SAI_TAM_TELEMETRY_ATTR_COLLECTOR_LIST;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = &sai_tam_collector_obj;
    attrs.push_back(attr);

    handleSaiCreateStatus(
        SAI_API_TAM,
        sai_tam_api->create_tam_telemetry(
            &sai_object,
            gSwitchId, static_cast<uint32_t>(attrs.size()),
            attrs.data()));

    HFTELUTILS_ADD_SAI_OBJECT_LIST(
        m_sai_tam_obj,
        SAI_TAM_ATTR_TELEMETRY_OBJECTS_LIST,
        sai_object,
        SAI_API_TAM,
        tam,
        tam);

    m_sai_tam_telemetry_obj = move(
        sai_guard_t(
            new sai_object_id_t(sai_object),
            [=](sai_object_id_t *p)
            {
                HFTELUTILS_DEL_SAI_OBJECT_LIST(
                    m_sai_tam_obj,
                    SAI_TAM_ATTR_TELEMETRY_OBJECTS_LIST,
                    *p,
                    SAI_API_TAM,
                    tam,
                    tam);

                handleSaiRemoveStatus(
                    SAI_API_TAM,
                    sai_tam_api->remove_tam_telemetry(*p));
                delete p;
            }));
}

void HFTelProfile::deployCounterSubscription(sai_object_type_t object_type, sai_object_id_t sai_obj, sai_stat_id_t stat_id, uint16_t label)
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

    attr.id = SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_TEL_TYPE;
    attr.value.oid = getTAMTelTypeObjID(object_type);
    attrs.push_back(attr);

    attr.id = SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_OBJECT_ID;
    attr.value.oid = sai_obj;
    attrs.push_back(attr);

    attr.id = SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_STAT_ID;
    attr.value.oid = stat_id;
    attrs.push_back(attr);

    attr.id = SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_LABEL;
    attr.value.u64 = static_cast<uint64_t>(label);
    attrs.push_back(attr);

    attr.id = SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_STATS_MODE;
    attr.value.s32 = HFTelUtils::get_stats_mode(object_type, stat_id);
    attrs.push_back(attr);

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

void HFTelProfile::deployCounterSubscriptions(sai_object_type_t object_type, sai_object_id_t sai_obj, std::uint16_t label)
{
    SWSS_LOG_ENTER();

    // TODO: Bulk create the counter subscriptions
    auto group = m_groups.find(object_type);
    if (group == m_groups.end())
    {
        return;
    }

    for (const auto &stat_id : group->second.getStatsIDs())
    {
        deployCounterSubscription(object_type, sai_obj, stat_id, label);
    }
}

void HFTelProfile::deployCounterSubscriptions(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    // TODO: Bulk create the counter subscriptions

    auto group = m_groups.find(object_type);
    if (group == m_groups.end())
    {
        return;
    }
    for (const auto &obj : group->second.getObjects())
    {
        auto itr = m_name_sai_map[object_type].find(obj.first);
        if (itr == m_name_sai_map[object_type].end())
        {
            continue;
        }
        for (const auto &stat_id : group->second.getStatsIDs())
        {
            deployCounterSubscription(object_type, itr->second, stat_id, obj.second);
        }
    }
}

void HFTelProfile::undeployCounterSubscriptions(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    // TODO: Bulk remove the counter subscriptions
    m_sai_tam_counter_subscription_objs.erase(object_type);
}

void HFTelProfile::updateTemplates(sai_object_id_t tam_tel_type_obj)
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

    const size_t COUNTER_SIZE (8LLU);
    const size_t IPFIX_TEMPLATE_MAX_SIZE (0xffffLLU);
    const size_t IPFIX_HEADER_SIZE (16LLU);
    const size_t IPFIX_TEMPLATE_METADATA_SIZE (12LLU);
    const size_t IPFIX_TEMPLATE_MAX_STATS_COUNT (((IPFIX_TEMPLATE_MAX_SIZE - IPFIX_HEADER_SIZE - IPFIX_TEMPLATE_METADATA_SIZE) / COUNTER_SIZE) - 1LLU);
    size_t estimated_template_size = (counters_count / IPFIX_TEMPLATE_MAX_STATS_COUNT + 1) * IPFIX_TEMPLATE_MAX_SIZE;

    vector<uint8_t> buffer(estimated_template_size, 0);

    sai_attribute_t attr;
    attr.id = SAI_TAM_TEL_TYPE_ATTR_IPFIX_TEMPLATES;
    attr.value.u8list.count = static_cast<uint32_t>(buffer.size());
    attr.value.u8list.list = buffer.data();

    auto status = sai_tam_api->get_tam_tel_type_attribute(tam_tel_type_obj, 1, &attr);
    if (status == SAI_STATUS_BUFFER_OVERFLOW)
    {
        buffer.resize(attr.value.u8list.count);
        attr.value.u8list.list = buffer.data();
        status = sai_tam_api->get_tam_tel_type_attribute(tam_tel_type_obj, 1, &attr);
    }

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_THROW("Failed to get the TAM telemetry type object %s attributes: %d",
                       sai_serialize_object_id(tam_tel_type_obj).c_str(), status);
    }

    buffer.resize(attr.value.u8list.count);

    m_sai_tam_tel_type_templates[object_type] = move(buffer);
}
