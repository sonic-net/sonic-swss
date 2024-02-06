#include "fabricportsorch.h"

#include <inttypes.h>
#include <fstream>
#include <sstream>
#include <tuple>

#include "logger.h"
#include "schema.h"
#include "sai_serialize.h"
#include "timer.h"
#include "saihelper.h"

#define FABRIC_POLLING_INTERVAL_DEFAULT   (30)
#define FABRIC_PORT_PREFIX    "PORT"
#define FABRIC_PORT_ERROR     0
#define FABRIC_PORT_SUCCESS   1
#define FABRIC_PORT_STAT_COUNTER_FLEX_COUNTER_GROUP         "FABRIC_PORT_STAT_COUNTER"
#define FABRIC_PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS   10000
#define FABRIC_QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP        "FABRIC_QUEUE_STAT_COUNTER"
#define FABRIC_QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS  100000

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;
extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;

const vector<sai_port_stat_t> port_stat_ids =
{
    SAI_PORT_STAT_IF_IN_OCTETS,
    SAI_PORT_STAT_IF_IN_ERRORS,
    SAI_PORT_STAT_IF_IN_FABRIC_DATA_UNITS,
    SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES,
    SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES,
    SAI_PORT_STAT_IF_IN_FEC_SYMBOL_ERRORS,
    SAI_PORT_STAT_IF_OUT_OCTETS,
    SAI_PORT_STAT_IF_OUT_FABRIC_DATA_UNITS,
};

static const vector<sai_queue_stat_t> queue_stat_ids =
{
    SAI_QUEUE_STAT_WATERMARK_LEVEL,
    SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES,
    SAI_QUEUE_STAT_CURR_OCCUPANCY_LEVEL,
};

FabricPortsOrch::FabricPortsOrch(DBConnector *appl_db, vector<table_name_with_pri_t> &tableNames,
                                 bool fabricPortStatEnabled, bool fabricQueueStatEnabled) :
        Orch(appl_db, tableNames),
        port_stat_manager(FABRIC_PORT_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                          FABRIC_PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, true),
        queue_stat_manager(FABRIC_QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                           FABRIC_QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, true),
        m_timer(new SelectableTimer(timespec { .tv_sec = FABRIC_POLLING_INTERVAL_DEFAULT, .tv_nsec = 0 }))
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE( "FabricPortsOrch constructor" );

    m_state_db = shared_ptr<DBConnector>(new DBConnector("STATE_DB", 0));
    m_stateTable = unique_ptr<Table>(new Table(m_state_db.get(), APP_FABRIC_PORT_TABLE_NAME));

    m_counter_db = shared_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0));
    m_portNameQueueCounterTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_FABRIC_QUEUE_NAME_MAP));
    m_portNamePortCounterTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_FABRIC_PORT_NAME_MAP));

    m_flex_db = shared_ptr<DBConnector>(new DBConnector("FLEX_COUNTER_DB", 0));
    m_flexCounterTable = unique_ptr<ProducerTable>(new ProducerTable(m_flex_db.get(), APP_FABRIC_PORT_TABLE_NAME));

    m_fabricPortStatEnabled = fabricPortStatEnabled;
    m_fabricQueueStatEnabled = fabricQueueStatEnabled;

    getFabricPortList();

    auto executor = new ExecutableTimer(m_timer, this, "FABRIC_POLL");
    Orch::addExecutor(executor);
    m_timer->start();
}

int FabricPortsOrch::getFabricPortList()
{
    SWSS_LOG_ENTER();

    if (m_getFabricPortListDone) {
        return FABRIC_PORT_SUCCESS;
    }

    uint32_t i;
    sai_status_t status;
    sai_attribute_t attr;

    attr.id = SAI_SWITCH_ATTR_NUMBER_OF_FABRIC_PORTS;
    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get fabric port number, rv:%d", status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            return FABRIC_PORT_ERROR;
        }
    }
    m_fabricPortCount = attr.value.u32;
    SWSS_LOG_NOTICE("Get %d fabric ports", m_fabricPortCount);

    vector<sai_object_id_t> fabric_port_list;
    fabric_port_list.resize(m_fabricPortCount);
    attr.id = SAI_SWITCH_ATTR_FABRIC_PORT_LIST;
    attr.value.objlist.count = (uint32_t)fabric_port_list.size();
    attr.value.objlist.list = fabric_port_list.data();
    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("FabricPortsOrch get port list failure");
        }
    }

    for (i = 0; i < m_fabricPortCount; i++)
    {
        sai_uint32_t lanes[1] = { 0 };
        attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
        attr.value.u32list.count = 1;
        attr.value.u32list.list = lanes;
        status = sai_port_api->get_port_attribute(fabric_port_list[i], 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
            if (handle_status != task_process_status::task_success)
            {
                throw runtime_error("FabricPortsOrch get port lane failure");
            }
        }
        int lane = attr.value.u32list.list[0];
        m_fabricLanePortMap[lane] = fabric_port_list[i];
    }

    generatePortStats();

    m_getFabricPortListDone = true;

    return FABRIC_PORT_SUCCESS;
}

bool FabricPortsOrch::allPortsReady()
{
    return m_getFabricPortListDone;
}

void FabricPortsOrch::generatePortStats()
{
    if (!m_fabricPortStatEnabled) return;

    SWSS_LOG_NOTICE("Generate fabric port stats");

    vector<FieldValueTuple> portNamePortCounterMap;
    for (auto p : m_fabricLanePortMap)
    {
        int lane = p.first;
        sai_object_id_t port = p.second;

        std::ostringstream portName;
        portName << FABRIC_PORT_PREFIX << lane;
        portNamePortCounterMap.emplace_back(portName.str(), sai_serialize_object_id(port));

        // Install flex counters for port stats
        std::unordered_set<std::string> counter_stats;
        for (const auto& it: port_stat_ids)
        {
            counter_stats.emplace(sai_serialize_port_stat(it));
        }
        port_stat_manager.setCounterIdList(port, CounterType::PORT, counter_stats);
    }
    m_portNamePortCounterTable->set("", portNamePortCounterMap);
}

void FabricPortsOrch::generateQueueStats()
{
    if (!m_fabricQueueStatEnabled) return;
    if (m_isQueueStatsGenerated) return;
    if (!m_getFabricPortListDone) return;

    SWSS_LOG_NOTICE("Generate queue map for fabric ports");

    sai_status_t status;
    sai_attribute_t attr;

    for (auto p : m_fabricLanePortMap)
    {
        int lane = p.first;
        sai_object_id_t port = p.second;

        // Each serdes has some pipes (queues) for unicast and multicast.
        // But normally fabric serdes uses only one pipe.
        attr.id = SAI_PORT_ATTR_QOS_NUMBER_OF_QUEUES;
        status = sai_port_api->get_port_attribute(port, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            throw runtime_error("FabricPortsOrch get port queue number failure");
        }
        int num_queues = attr.value.u32;

        if (num_queues > 0)
        {
            vector<sai_object_id_t> m_queue_ids;
            m_queue_ids.resize(num_queues);

            attr.id = SAI_PORT_ATTR_QOS_QUEUE_LIST;
            attr.value.objlist.count = (uint32_t) num_queues;
            attr.value.objlist.list = m_queue_ids.data();

            status = sai_port_api->get_port_attribute(port, 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                throw runtime_error("FabricPortsOrch get port queue list failure");
            }

            // Maintain queue map and install flex counters for queue stats
            vector<FieldValueTuple> portNameQueueMap;

            // Fabric serdes queue type is SAI_QUEUE_TYPE_FABRIC_TX. Since we always
            // maintain only one queue for fabric serdes, m_queue_ids size is 1.
            // And so, there is no need to query  SAI_QUEUE_ATTR_TYPE and SAI_QUEUE_ATTR_INDEX
            // for queue. Actually, SAI does not support query these attributes on fabric serdes.
            int queueIndex = 0;
            std::ostringstream portName;
            portName << FABRIC_PORT_PREFIX << lane << ":" << queueIndex;
            const auto queue = sai_serialize_object_id(m_queue_ids[queueIndex]);
            portNameQueueMap.emplace_back(portName.str(), queue);

            // We collect queue counters like occupancy level
            std::unordered_set<string> counter_stats;
            for (const auto& it: queue_stat_ids)
            {
                counter_stats.emplace(sai_serialize_queue_stat(it));
            }
            queue_stat_manager.setCounterIdList(m_queue_ids[queueIndex], CounterType::QUEUE, counter_stats);

            m_portNameQueueCounterTable->set("", portNameQueueMap);
        }
    }

    m_isQueueStatsGenerated = true;
}

void FabricPortsOrch::updateFabricPortState()
{
    if (!m_getFabricPortListDone) return;

    SWSS_LOG_ENTER();

    sai_status_t status;
    sai_attribute_t attr;

    time_t now;
    struct timespec time_now;
    if (clock_gettime(CLOCK_MONOTONIC, &time_now) < 0)
    {
        return;
    }
    now = time_now.tv_sec;

    for (auto p : m_fabricLanePortMap)
    {
        int lane = p.first;
        sai_object_id_t port = p.second;

        string key = FABRIC_PORT_PREFIX + to_string(lane);
        std::vector<FieldValueTuple> values;
        uint32_t remote_peer = 0;
        uint32_t remote_port = 0;

        attr.id = SAI_PORT_ATTR_FABRIC_ATTACHED;
        status = sai_port_api->get_port_attribute(port, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            // Port may not be ready for query
            SWSS_LOG_ERROR("Failed to get fabric port (%d) status, rv:%d", lane, status);
            task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
            if (handle_status != task_process_status::task_success)
            {
                return;
            }
        }

        if (m_portStatus.find(lane) != m_portStatus.end() &&
            m_portStatus[lane] && !attr.value.booldata)
        {
            m_portDownCount[lane] ++;
            m_portDownSeenLastTime[lane] = now;
        }
        m_portStatus[lane] = attr.value.booldata;

        if (m_portStatus[lane])
        {
            attr.id = SAI_PORT_ATTR_FABRIC_ATTACHED_SWITCH_ID;
            status = sai_port_api->get_port_attribute(port, 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
                if (handle_status != task_process_status::task_success)
                {
                    throw runtime_error("FabricPortsOrch get remote id failure");
                }
            }
            remote_peer = attr.value.u32;

            attr.id = SAI_PORT_ATTR_FABRIC_ATTACHED_PORT_INDEX;
            status = sai_port_api->get_port_attribute(port, 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
                if (handle_status != task_process_status::task_success)
                {
                    throw runtime_error("FabricPortsOrch get remote port index failure");
                }
            }
            remote_port = attr.value.u32;
        }

        values.emplace_back("STATUS", m_portStatus[lane] ? "up" : "down");
        if (m_portStatus[lane])
        {
            values.emplace_back("REMOTE_MOD", to_string(remote_peer));
            values.emplace_back("REMOTE_PORT", to_string(remote_port));
        }
        if (m_portDownCount[lane] > 0)
        {
            values.emplace_back("PORT_DOWN_COUNT", to_string(m_portDownCount[lane]));
            values.emplace_back("PORT_DOWN_SEEN_LAST_TIME",
                                to_string(m_portDownSeenLastTime[lane]));
        }
        m_stateTable->set(key, values);
    }
}

void FabricPortsOrch::doTask()
{
}

void FabricPortsOrch::doTask(Consumer &consumer)
{
}

void FabricPortsOrch::doTask(swss::SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    if (!m_getFabricPortListDone)
    {
        getFabricPortList();
    }

    if (m_getFabricPortListDone)
    {
        updateFabricPortState();
    }
}
