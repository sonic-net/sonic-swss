// #include "sai.h"
#include "policerorch.h"
#include "sai_serialize.h"
#include "flow_counter_handler.h"
#include "aclorch.h"
#include "converter.h"
#include <inttypes.h>
extern "C" {
#include "sai.h"
}


// #include "logger.h"
// #include <limits.h>
// #include <unordered_map>
// #include <algorithm>
// #include "schema.h"
// #include "timer.h"

using namespace std;
using namespace swss;

extern sai_policer_api_t*   sai_policer_api;
extern sai_port_api_t *sai_port_api;

extern sai_object_id_t gSwitchId;
extern PortsOrch* gPortsOrch;

#define ETHERNET_PREFIX "Ethernet"

static const string meter_type_field           = "METER_TYPE";
static const string mode_field                 = "MODE";
static const string color_source_field         = "COLOR_SOURCE";
static const string cbs_field                  = "CBS";
static const string cir_field                  = "CIR";
static const string pbs_field                  = "PBS";
static const string pir_field                  = "PIR";
static const string green_packet_action_field  = "GREEN_PACKET_ACTION";
static const string red_packet_action_field    = "RED_PACKET_ACTION";
static const string yellow_packet_action_field = "YELLOW_PACKET_ACTION";

static const string storm_control_kbps         = "KBPS";
static const string storm_broadcast            = "broadcast";
static const string storm_unknown_unicast      = "unknown-unicast";
static const string storm_unknown_mcast        = "unknown-multicast";

extern bool gTraditionalFlexCounter;

static const map<string, sai_meter_type_t> meter_type_map = {
    {"PACKETS", SAI_METER_TYPE_PACKETS},
    {"BYTES", SAI_METER_TYPE_BYTES}
};

static const map<string, sai_policer_mode_t> policer_mode_map = {
    {"SR_TCM", SAI_POLICER_MODE_SR_TCM},
    {"TR_TCM", SAI_POLICER_MODE_TR_TCM},
    {"STORM_CONTROL", SAI_POLICER_MODE_STORM_CONTROL}
};

static const map<string, sai_policer_color_source_t> policer_color_source_map = {
    {"AWARE", SAI_POLICER_COLOR_SOURCE_AWARE},
    {"BLIND", SAI_POLICER_COLOR_SOURCE_BLIND}
};

static const map<string, sai_packet_action_t> packet_action_map = {
    {"DROP", SAI_PACKET_ACTION_DROP},
    {"FORWARD", SAI_PACKET_ACTION_FORWARD},
    {"COPY", SAI_PACKET_ACTION_COPY},
    {"COPY_CANCEL", SAI_PACKET_ACTION_COPY_CANCEL},
    {"TRAP", SAI_PACKET_ACTION_TRAP},
    {"LOG", SAI_PACKET_ACTION_LOG},
    {"DENY", SAI_PACKET_ACTION_DENY},
    {"TRANSIT", SAI_PACKET_ACTION_TRANSIT}
};

const vector<sai_policer_stat_t> policer_stat_ids = {

    SAI_POLICER_STAT_PACKETS,
    SAI_POLICER_STAT_ATTR_BYTES,
    SAI_POLICER_STAT_GREEN_PACKETS,
    SAI_POLICER_STAT_GREEN_BYTES,
    SAI_POLICER_STAT_YELLOW_PACKETS,
    SAI_POLICER_STAT_YELLOW_BYTES,
    SAI_POLICER_STAT_RED_PACKETS,
    SAI_POLICER_STAT_RED_BYTES

};

bool PolicerOrch::policerExists(const string &name)
{
    SWSS_LOG_ENTER();

    return m_syncdPolicers.find(name) != m_syncdPolicers.end();
}

bool PolicerOrch::getPolicerOid(const string &name, sai_object_id_t &oid)
{
    SWSS_LOG_ENTER();

    if (policerExists(name))
    {
        oid = m_syncdPolicers[name];
        SWSS_LOG_NOTICE("Get policer %s oid:%" PRIx64, name.c_str(), oid);
        return true;
    }

    return false;
}

bool PolicerOrch::increaseRefCount(const string &name)
{
    SWSS_LOG_ENTER();

    if (!policerExists(name))
    {
        SWSS_LOG_WARN("Policer %s does not exist", name.c_str());
        return false;
    }

    ++m_policerRefCounts[name];

    SWSS_LOG_INFO("Policer %s reference count is increased to %d",
            name.c_str(), m_policerRefCounts[name]);
    return true;
}

bool PolicerOrch::decreaseRefCount(const string &name)
{
    SWSS_LOG_ENTER();

    if (!policerExists(name))
    {
        SWSS_LOG_WARN("Policer %s does not exist", name.c_str());
        return false;
    }

    --m_policerRefCounts[name];

    SWSS_LOG_INFO("Policer %s reference count is decreased to %d",
            name.c_str(), m_policerRefCounts[name]);
    return true;
}


PolicerOrch::PolicerOrch( vector<TableConnector> &tableNames, PortsOrch *portOrch) :
    Orch(tableNames), 
    m_portsOrch(portOrch),
    m_counter_db(std::shared_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0))),
    m_asic_db(std::shared_ptr<DBConnector>(new DBConnector("ASIC_DB", 0))),
    m_counter_table(std::unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_POLICER_NAME_MAP))),
    m_vidToRidTable(std::unique_ptr<Table>(new Table(m_asic_db.get(), "VIDTORID"))),
    m_policer_counter_manager(POLICER_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,10000,false)
{
    SWSS_LOG_ENTER();
    auto intervT = timespec { .tv_sec = 1 , .tv_nsec = 0 };
    m_FlexCounterUpdTimer = new SelectableTimer(intervT);
    auto executorT = new ExecutableTimer(m_FlexCounterUpdTimer, this, "FLEX_COUNTER_UPD_TIMER");
    Orch::addExecutor(executorT);
}


task_process_status PolicerOrch::handlePortStormControlTable(swss::KeyOpFieldsValuesTuple tuple)
{
    auto key = kfvKey(tuple);
    auto op = kfvOp(tuple);
    string storm_key = key;
    auto tokens = tokenize(storm_key, config_db_key_delimiter);
    auto interface_name = tokens[0];
    auto storm_type = tokens[1];
    Port port;

    /*Only proceed for Ethernet interfaces*/
    if (strncmp(interface_name.c_str(), ETHERNET_PREFIX, strlen(ETHERNET_PREFIX)))
    {
        SWSS_LOG_ERROR("%s: Unsupported / Invalid interface %s",
                storm_type.c_str(), interface_name.c_str());
        return task_process_status::task_success;
    }
    if (!gPortsOrch->getPort(interface_name, port))
    {
        SWSS_LOG_ERROR("Failed to apply storm-control %s to port %s. Port not found",
                storm_type.c_str(), interface_name.c_str());
        /*continue here as there can be more interfaces*/
        return task_process_status::task_success;
    }
    /*Policer Name: _<interface_name>_<storm_type>*/
    const auto storm_policer_name = "_"+interface_name+"_"+storm_type;

    if (op == SET_COMMAND)
    {
        // Mark the operation as an 'update', if the policer exists.
        bool update = m_syncdPolicers.find(storm_policer_name) != m_syncdPolicers.end();
        vector <sai_attribute_t> attrs;
        bool cir = false;
        sai_attribute_t attr;

        /*Meter type hardcoded to BYTES*/
        attr.id = SAI_POLICER_ATTR_METER_TYPE;
        attr.value.s32 = (sai_meter_type_t) meter_type_map.at("BYTES");
        attrs.push_back(attr);

        /*Policer mode hardcoded to STORM_CONTROL*/
        attr.id = SAI_POLICER_ATTR_MODE;
        attr.value.s32 = (sai_policer_mode_t) policer_mode_map.at("STORM_CONTROL");
        attrs.push_back(attr);

        /*Red Packet Action hardcoded to DROP*/
        attr.id = SAI_POLICER_ATTR_RED_PACKET_ACTION;
        attr.value.s32 = packet_action_map.at("DROP");
        attrs.push_back(attr);

        for (auto i = kfvFieldsValues(tuple).begin();
                i != kfvFieldsValues(tuple).end(); ++i)
        {
            auto field = to_upper(fvField(*i));
            auto value = to_upper(fvValue(*i));

            /*BPS value is used as CIR*/
            if (field == storm_control_kbps)
            {
                attr.id = SAI_POLICER_ATTR_CIR;
                /*convert kbps to bps*/
                attr.value.u64 = (stoul(value)*1000/8);
                cir = true;
                attrs.push_back(attr);
                SWSS_LOG_DEBUG("CIR %s",value.c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Unknown storm control attribute %s specified",
                        field.c_str());
                continue;
            }
        }
        /*CIR is mandatory parameter*/
        if (!cir)
        {
            SWSS_LOG_ERROR("Failed to create storm control policer %s,\
                    missing mandatory fields", storm_policer_name.c_str());
            return task_process_status::task_failed;
        }

        /*Enabling storm-control on port*/
        sai_attribute_t port_attr;
        if (storm_type == storm_broadcast)
        {
            port_attr.id = SAI_PORT_ATTR_BROADCAST_STORM_CONTROL_POLICER_ID;
        }
        else if (storm_type == storm_unknown_unicast)
        {
            port_attr.id = SAI_PORT_ATTR_FLOOD_STORM_CONTROL_POLICER_ID;
        }
        else if (storm_type == storm_unknown_mcast)
        {
            port_attr.id = SAI_PORT_ATTR_MULTICAST_STORM_CONTROL_POLICER_ID;
        }
        else
        {
            SWSS_LOG_ERROR("Unknown storm_type %s", storm_type.c_str());
            return task_process_status::task_failed;
        }

        sai_object_id_t policer_id;
        // Create a new policer
        if (!update)
        {
            sai_status_t status = sai_policer_api->create_policer(
                    &policer_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create policer %s, rv:%d",
                        storm_policer_name.c_str(), status);
                if (handleSaiCreateStatus(SAI_API_POLICER, status) == task_need_retry)
                {
                    return task_process_status::task_need_retry;
                }
            }

            SWSS_LOG_DEBUG("Created storm-control policer %s", storm_policer_name.c_str());
            m_syncdPolicers[storm_policer_name] = policer_id;
            m_policerRefCounts[storm_policer_name] = 0;
            
        }
        // Update an existing policer
        else
        {
            policer_id = m_syncdPolicers[storm_policer_name];

            // The update operation has limitations that it could only update
            // the rate and the size accordingly.
            // STORM_CONTROL: CIR, CBS
            for (auto & attr: attrs)
            {
                if (attr.id != SAI_POLICER_ATTR_CIR)
                {
                    continue;
                }

                sai_status_t status = sai_policer_api->set_policer_attribute(
                        policer_id, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to update policer %s attribute, rv:%d",
                            storm_policer_name.c_str(), status);
                    if (handleSaiSetStatus(SAI_API_POLICER, status) == task_need_retry)
                    {
                        return task_process_status::task_need_retry;
                    }

                }
            }
        }
        policer_id = m_syncdPolicers[storm_policer_name];

        if (update)
        {
            SWSS_LOG_NOTICE("update storm-control policer %s", storm_policer_name.c_str());
            port_attr.value.oid = SAI_NULL_OBJECT_ID;
            /*Remove and re-apply policer*/
            sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &port_attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove storm-control %s from port %s, rv:%d",
                        storm_type.c_str(), interface_name.c_str(), status);
                if (handleSaiSetStatus(SAI_API_POLICER, status) == task_need_retry)
                {
                    return task_process_status::task_need_retry;
                }
            }
        }
        port_attr.value.oid = policer_id;

        sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &port_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to apply storm-control %s to port %s, rv:%d",
                    storm_type.c_str(), interface_name.c_str(),status);

            /*TODO: Do the below policer cleanup in an API*/
            /*Remove the already created policer*/
            if (SAI_STATUS_SUCCESS != sai_policer_api->remove_policer(
                        m_syncdPolicers[storm_policer_name]))
            {
                SWSS_LOG_ERROR("Failed to remove policer %s, rv:%d",
                        storm_policer_name.c_str(), status);
                /*TODO: Just doing a syslog. */
            }

            SWSS_LOG_NOTICE("Removed policer %s as set_port_attribute for %s failed", 
                    storm_policer_name.c_str(),interface_name.c_str());
            m_syncdPolicers.erase(storm_policer_name);
            m_policerRefCounts.erase(storm_policer_name);

            return task_process_status::task_need_retry;
        }
    }
    else if (op == DEL_COMMAND)
    {
        if (m_syncdPolicers.find(storm_policer_name) == m_syncdPolicers.end())
        {
            SWSS_LOG_ERROR("Policer %s not configured", storm_policer_name.c_str());
            return task_process_status::task_success;
        }

        sai_attribute_t port_attr;
        if (storm_type == storm_broadcast)
        {
            port_attr.id = SAI_PORT_ATTR_BROADCAST_STORM_CONTROL_POLICER_ID;
        }
        else if (storm_type == storm_unknown_unicast)
        {
            port_attr.id = SAI_PORT_ATTR_FLOOD_STORM_CONTROL_POLICER_ID;
        }
        else if (storm_type == storm_unknown_mcast)
        {
            port_attr.id = SAI_PORT_ATTR_MULTICAST_STORM_CONTROL_POLICER_ID;
        }
        else
        {
            SWSS_LOG_ERROR("Unknown storm_type %s", storm_type.c_str());
            return task_process_status::task_failed;
        }

        port_attr.value.oid = SAI_NULL_OBJECT_ID;

        sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &port_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove storm-control %s from port %s, rv:%d",
                    storm_type.c_str(), interface_name.c_str(), status);
            if (handleSaiRemoveStatus(SAI_API_POLICER, status) == task_need_retry)
            {
                return task_process_status::task_need_retry;
            }
        }

        status = sai_policer_api->remove_policer(
                m_syncdPolicers[storm_policer_name]);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove policer %s, rv:%d",
                    storm_policer_name.c_str(), status);
            if (handleSaiRemoveStatus(SAI_API_POLICER, status) == task_need_retry)
            {
                return task_process_status::task_need_retry;
            }
        }

        SWSS_LOG_NOTICE("Removed policer %s", storm_policer_name.c_str());
        m_syncdPolicers.erase(storm_policer_name);
        m_policerRefCounts.erase(storm_policer_name);
    }
    return task_process_status::task_success;
}

string PolicerOrch::getPolicerFlexCounterTableKey(string key)
{
    return string(POLICER_STAT_COUNTER_FLEX_COUNTER_GROUP) + ":" + key;
}

void PolicerOrch::removePCFromFlexCounter(sai_object_id_t id, const string &name)
{
    SWSS_LOG_ENTER();

    std::string counter_oid_str;
    counter_oid_str = sai_serialize_object_id(id);

    auto update_iter = m_pendingPcAddToFlexCntr.find(id);
    if (update_iter == m_pendingPcAddToFlexCntr.end())
    {
        m_policer_counter_manager.clearCounterIdList(id);
    }
    else
    {
        m_pendingPcAddToFlexCntr.erase(update_iter);
    }

    /* remove it from COUNTERS_DB maps */
    m_counter_table->hdel("", name);
    m_vidToRidTable->hdel("", counter_oid_str);

    SWSS_LOG_DEBUG("Unregistered interface %s from Flex counter", name.c_str());
}

sai_status_t PolicerOrch::querySupportedPolicerStats(std::unordered_set<sai_policer_stat_t> &supported_stats)
{
    sai_stat_capability_list_t        stats_capability;
    vector<sai_stat_capability_t>       capabilities(8);
    stats_capability.count = 8;
    stats_capability.list = capabilities.data();
   
    sai_status_t status = sai_query_stats_capability(gSwitchId,
                                                SAI_OBJECT_TYPE_POLICER,
                                                &stats_capability);

    if(status == SAI_STATUS_SUCCESS)
    {
        for (uint32_t i = 0; i < stats_capability.count; i++)
        {
            supported_stats.insert(static_cast<sai_policer_stat_t>(stats_capability.list[i].stat_enum));
        }
    }

    return status;
}


void PolicerOrch::doTask(SelectableTimer &timer)
{
    string value;
    for (auto it = m_pendingPcAddToFlexCntr.begin(); it != m_pendingPcAddToFlexCntr.end(); )
    {
        const auto id = sai_serialize_object_id(it->first);
        if (!gTraditionalFlexCounter || m_vidToRidTable->hget("", id, value))
        {
            vector<FieldValueTuple> policerNameVector;

            policerNameVector.emplace_back(it->second.c_str(), id);
            m_counter_table->set("", policerNameVector);
            m_counter_db->hset(COUNTERS_POLICER_NAME_MAP, it->second.c_str(), id.c_str());

            auto pcounter_stats = generatePCounterStats();
            m_policer_counter_manager.setCounterIdList(it->first, CounterType::POLICER, pcounter_stats, gSwitchId);
            SWSS_LOG_DEBUG("inserted %s to flex counter", it->second.c_str());
            it = m_pendingPcAddToFlexCntr.erase(it);
        }
        else
        {
            ++it;
        }
    }
}


std::unordered_set<std::string> PolicerOrch::generatePCounterStats()
{
    std::unordered_set<std::string> counter_stats;
    std::unordered_set<sai_policer_stat_t> supported_stats;
    sai_status_t status = querySupportedPolicerStats(supported_stats);

    if (status == SAI_STATUS_NOT_IMPLEMENTED)
    {
        for (auto& it: policer_stat_ids)
        {
            counter_stats.emplace(sai_serialize_policer_stat(it));
        }
        return counter_stats;
    }

    for (const auto& stat : policer_stat_ids)
    {
        if (std::find(supported_stats.begin(), supported_stats.end(), stat) != supported_stats.end())
        {
            counter_stats.emplace(sai_serialize_policer_stat(stat));
        }
        else
        {
            SWSS_LOG_WARN("Unsupported policer stat: %d", stat);
        }
    }

    return counter_stats;
}


void PolicerOrch::generatePolicerCounterMap()
{
    if (m_isPolicerCounterMapGenerated)
    {
        return;
    }

    m_FlexCounterUpdTimer->start();

    m_isPolicerCounterMapGenerated = true;
}

void PolicerOrch::addPolicerToFlexCounter(sai_object_id_t oid, const string &name)
{
    m_pendingPcAddToFlexCntr[oid] = name;
}


void PolicerOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    task_process_status storm_status = task_success;

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto tuple = it->second;

        auto key = kfvKey(tuple);
        auto op = kfvOp(tuple);
        auto table_name = consumer.getTableName();

        // Special handling for storm-control configuration.
        if (table_name == CFG_PORT_STORM_CONTROL_TABLE_NAME)
        {
            storm_status = handlePortStormControlTable(tuple);
            if ((storm_status == task_process_status::task_success) ||
                    (storm_status == task_process_status::task_failed))
            {
                it = consumer.m_toSync.erase(it);
            }
            else
            {
                it++;
            }
            continue;
        }
        if (op == SET_COMMAND)
        {
            // Mark the operation as an 'update', if the policer exists.
            bool update = m_syncdPolicers.find(key) != m_syncdPolicers.end();

            vector<sai_attribute_t> attrs;
            bool meter_type = false, mode = false;

            for (auto i = kfvFieldsValues(tuple).begin();
                    i != kfvFieldsValues(tuple).end(); ++i)
            {
                auto field = to_upper(fvField(*i));
                auto value = to_upper(fvValue(*i));

                SWSS_LOG_DEBUG("attribute: %s value: %s", field.c_str(), value.c_str());

                sai_attribute_t attr;

                if (field == meter_type_field)
                {
                    attr.id = SAI_POLICER_ATTR_METER_TYPE;
                    attr.value.s32 = (sai_meter_type_t) meter_type_map.at(value);
                    meter_type = true;
                }
                else if (field == mode_field)
                {
                    attr.id = SAI_POLICER_ATTR_MODE;
                    attr.value.s32 = (sai_policer_mode_t) policer_mode_map.at(value);
                    mode = true;
                }
                else if (field == color_source_field)
                {
                    attr.id = SAI_POLICER_ATTR_COLOR_SOURCE;
                    attr.value.s32 = policer_color_source_map.at(value);
                }
                else if (field == cbs_field)
                {
                    attr.id = SAI_POLICER_ATTR_CBS;
                    attr.value.u64 = stoul(value);
                }
                else if (field == cir_field)
                {
                    attr.id = SAI_POLICER_ATTR_CIR;
                    attr.value.u64 = stoul(value);
                }
                else if (field == pbs_field)
                {
                    attr.id = SAI_POLICER_ATTR_PBS;
                    attr.value.u64 = stoul(value);
                }
                else if (field == pir_field)
                {
                    attr.id = SAI_POLICER_ATTR_PIR;
                    attr.value.u64 = stoul(value);
                }
                else if (field == red_packet_action_field)
                {
                    attr.id = SAI_POLICER_ATTR_RED_PACKET_ACTION;
                    attr.value.s32 = packet_action_map.at(value);
                }
                else if (field == green_packet_action_field)
                {
                    attr.id = SAI_POLICER_ATTR_GREEN_PACKET_ACTION;
                    attr.value.s32 = packet_action_map.at(value);
                }
                else if (field == yellow_packet_action_field)
                {
                    attr.id = SAI_POLICER_ATTR_YELLOW_PACKET_ACTION;
                    attr.value.s32 = packet_action_map.at(value);
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown policer attribute %s specified",
                            field.c_str());
                    continue;
                }

                attrs.push_back(attr);
            }

            // Create a new policer
            if (!update)
            {
                if (!meter_type || !mode)
                {
                    SWSS_LOG_ERROR("Failed to create policer %s,\
                            missing mandatory fields", key.c_str());
                }

                sai_object_id_t policer_id;
                sai_status_t status = sai_policer_api->create_policer(
                    &policer_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to create policer %s, rv:%d",
                            key.c_str(), status);
                    if (handleSaiCreateStatus(SAI_API_POLICER, status) == task_need_retry)
                    {
                        it++;
                        continue;
                    }
                }

                SWSS_LOG_NOTICE("Created policer %s", key.c_str());
                m_syncdPolicers[key] = policer_id;
                m_policerRefCounts[key] = 0;

                addPolicerToFlexCounter(policer_id, key.c_str());
            }
            // Update an existing policer
            else
            {
                auto policer_id = m_syncdPolicers[key];

                // The update operation has limitations that it could only update
                // the rate and the size accordingly.
                // SR_TCM: CIR, CBS, PBS
                // TR_TCM: CIR, CBS, PIR, PBS
                // STORM_CONTROL: CIR, CBS
                for (auto & attr: attrs)
                {
                    if (attr.id != SAI_POLICER_ATTR_CBS &&
                            attr.id != SAI_POLICER_ATTR_CIR &&
                            attr.id != SAI_POLICER_ATTR_PBS &&
                            attr.id != SAI_POLICER_ATTR_PIR)
                    {
                        continue;
                    }

                    sai_status_t status = sai_policer_api->set_policer_attribute(
                            policer_id, &attr);
                    if (status != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("Failed to update policer %s attribute, rv:%d",
                                key.c_str(), status);
                        if (handleSaiSetStatus(SAI_API_POLICER, status) == task_need_retry)
                        {
                            it++;
                            continue;
                        }
                    }
                }

                SWSS_LOG_NOTICE("Update policer %s attributes", key.c_str());
            }

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (m_syncdPolicers.find(key) == m_syncdPolicers.end())
            {
                SWSS_LOG_ERROR("Policer %s does not exists", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (m_policerRefCounts[key] > 0)
            {
                SWSS_LOG_INFO("Policer %s is still referenced", key.c_str());
                it++;
                continue;
            }

            removePCFromFlexCounter(m_syncdPolicers[key], key);

            sai_status_t status = sai_policer_api->remove_policer(
                    m_syncdPolicers.at(key));
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove policer %s, rv:%d",
                        key.c_str(), status);
                if (handleSaiRemoveStatus(SAI_API_POLICER, status) == task_need_retry)
                {
                    it++;
                    continue;
                }
            }

            SWSS_LOG_NOTICE("Removed policer %s", key.c_str());
            m_syncdPolicers.erase(key);
            m_policerRefCounts.erase(key);
            it = consumer.m_toSync.erase(it);
        }
    }
}
